// SPDX-License-Identifier: GPL-2.0
/*
 * Synthstrom Deluge DIN MIDI over SCIF0 — self-contained rawmidi driver.
 *
 * The 5-pin DIN MIDI IN/OUT jacks are wired to SCIF0 @ 31250 bps. At this low
 * rate the SCIF's own GIC interrupts are the natural transport (no DMA): the
 * receive-FIFO interrupt (RXI) drains bytes into ALSA rawmidi, and the
 * transmit-FIFO-empty interrupt (TXI) feeds the rawmidi output buffer to the
 * TX FIFO. This is interrupt-driven end to end — no cyclic DMA ring and no
 * hrtimer poll (unlike the PIC on SCIF1, whose 200000-baud stream needs DMA).
 * The port is exposed as an ALSA rawmidi on the shared "Deluge" sound card via
 * an ASoC auxiliary component, so one card carries PCM + MIDI.
 *
 * Single-core SoC: TX is shared between the output .trigger (process context,
 * rawmidi runtime lock held) and the TXI handler (hard IRQ); tx_lock serialises
 * them. RX is touched only by the RXI handler.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <sound/core.h>
#include <sound/rawmidi.h>
#include <sound/soc.h>
#include <linux/deluge-scif.h>

#define SCSCR_MIDI	0x0070	/* RE|RIE|TE (RIE enables the RXI interrupt) */
#define SCSCR_MIDI_TX	0x00f0	/* + TIE: enable the TXI interrupt while sending */
#define SCFSR_TXBITS	(SCFSR_TDFE | SCFSR_TEND)	/* preserved by the RX handler */

#define MIDI_BAUD	31250

struct deluge_midi {
	struct device *dev;
	void __iomem *scif;
	struct clk *clk;
	spinlock_t tx_lock;			/* serialises TX trigger vs TXI */
	struct snd_rawmidi *rmidi;
	struct snd_rawmidi_substream *in;	/* set by input .open */
	struct snd_rawmidi_substream *out;	/* set by output .open */
	bool in_running;
	bool out_running;
};

/*
 * Naming: deluge_midi_* = driver / ASoC entry points (probe, remove, component
 * ops); midi_* = SCIF and rawmidi transport helpers (drain, rxi, open/close).
 */

/*
 * Feed the rawmidi output buffer into the TX FIFO until the FIFO is full or the
 * buffer is drained; when nothing remains, drop TIE so the TXI stops firing.
 * Caller holds tx_lock.
 */
static void midi_drain_tx(struct deluge_midi *m)
{
	struct snd_rawmidi_substream *out = READ_ONCE(m->out);
	unsigned char b;

	if (!out || !READ_ONCE(m->out_running)) {
		writew(SCSCR_MIDI, m->scif + SCIF_SCSCR);	/* TIE off */
		return;
	}
	while (readw(m->scif + SCIF_SCFSR) & SCFSR_TDFE) {
		if (snd_rawmidi_transmit(out, &b, 1) != 1) {
			writew(SCSCR_MIDI, m->scif + SCIF_SCSCR);	/* drained: TIE off */
			return;
		}
		scif_tx_byte(m->scif, b);	/* TDFE already set: writes + clears, no spin */
	}
	/* FIFO full with data still pending: leave TIE on, TXI resumes us. */
}

/* RX FIFO interrupt: pull every byte the FIFO holds into rawmidi. */
static irqreturn_t midi_rxi(int irq, void *dev_id)
{
	struct deluge_midi *m = dev_id;
	struct snd_rawmidi_substream *in = READ_ONCE(m->in);
	u16 fsr, lsr;
	int count;
	u8 b;

	/* An overrun latches and blocks further reception until cleared. */
	lsr = readw(m->scif + SCIF_SCLSR);
	if (lsr & SCLSR_ORER) {
		writew(lsr & ~SCLSR_ORER, m->scif + SCIF_SCLSR);
		dev_warn_ratelimited(m->dev, "MIDI RX overrun\n");
	}

	fsr = readw(m->scif + SCIF_SCFSR);
	if (!(fsr & (SCFSR_RDF | SCFSR_DR | SCFSR_ER | SCFSR_BRK)))
		return IRQ_NONE;

	count = readw(m->scif + SCIF_SCFDR) & SCFDR_RXCNT;
	while (count-- > 0) {
		b = readb(m->scif + SCIF_SCFRDR);
		if (in && READ_ONCE(m->in_running))
			snd_rawmidi_receive(in, &b, 1);
	}

	/* Clear the RX status (RDF/DR/ER/BRK) after reading; preserve TX flags. */
	writew(readw(m->scif + SCIF_SCFSR) & SCFSR_TXBITS, m->scif + SCIF_SCFSR);
	return IRQ_HANDLED;
}

/* TX FIFO-empty interrupt: refill from rawmidi. */
static irqreturn_t midi_txi(int irq, void *dev_id)
{
	struct deluge_midi *m = dev_id;

	spin_lock(&m->tx_lock);
	midi_drain_tx(m);
	spin_unlock(&m->tx_lock);
	return IRQ_HANDLED;
}

/* --- rawmidi ops --- */

static int midi_in_open(struct snd_rawmidi_substream *s)
{
	struct deluge_midi *m = s->rmidi->private_data;

	WRITE_ONCE(m->in, s);
	return 0;
}

static int midi_in_close(struct snd_rawmidi_substream *s)
{
	struct deluge_midi *m = s->rmidi->private_data;

	WRITE_ONCE(m->in_running, false);
	WRITE_ONCE(m->in, NULL);
	return 0;
}

static void midi_in_trigger(struct snd_rawmidi_substream *s, int up)
{
	struct deluge_midi *m = s->rmidi->private_data;

	WRITE_ONCE(m->in_running, !!up);
}

static int midi_out_open(struct snd_rawmidi_substream *s)
{
	struct deluge_midi *m = s->rmidi->private_data;

	WRITE_ONCE(m->out, s);
	return 0;
}

static int midi_out_close(struct snd_rawmidi_substream *s)
{
	struct deluge_midi *m = s->rmidi->private_data;

	WRITE_ONCE(m->out_running, false);
	WRITE_ONCE(m->out, NULL);
	return 0;
}

static void midi_out_trigger(struct snd_rawmidi_substream *s, int up)
{
	struct deluge_midi *m = s->rmidi->private_data;
	unsigned long flags;

	WRITE_ONCE(m->out_running, !!up);
	spin_lock_irqsave(&m->tx_lock, flags);
	if (up) {
		writew(SCSCR_MIDI_TX, m->scif + SCIF_SCSCR);	/* arm TIE */
		midi_drain_tx(m);				/* and kick it now */
	} else {
		writew(SCSCR_MIDI, m->scif + SCIF_SCSCR);	/* TIE off */
	}
	spin_unlock_irqrestore(&m->tx_lock, flags);
}

static void midi_out_drain(struct snd_rawmidi_substream *s)
{
	struct deluge_midi *m = s->rmidi->private_data;
	int i;

	for (i = 0; i < 1000; i++) {
		if (snd_rawmidi_transmit_empty(s) &&
		    (readw(m->scif + SCIF_SCFSR) & SCFSR_TEND))
			return;
		msleep(1);
	}
}

static const struct snd_rawmidi_ops midi_input_ops = {
	.open = midi_in_open,
	.close = midi_in_close,
	.trigger = midi_in_trigger,
};

static const struct snd_rawmidi_ops midi_output_ops = {
	.open = midi_out_open,
	.close = midi_out_close,
	.trigger = midi_out_trigger,
	.drain = midi_out_drain,
};

/* --- ASoC component: creates the rawmidi on the shared card --- */

static int deluge_midi_comp_probe(struct snd_soc_component *component)
{
	struct deluge_midi *m = dev_get_drvdata(component->dev);
	struct snd_rawmidi *rmidi;
	int ret;

	ret = snd_rawmidi_new(component->card->snd_card, "Deluge MIDI DIN", 0,
			      1, 1, &rmidi);
	if (ret)
		return ret;
	strscpy(rmidi->name, "Deluge MIDI DIN", sizeof(rmidi->name));
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_OUTPUT |
			    SNDRV_RAWMIDI_INFO_DUPLEX;
	rmidi->private_data = m;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &midi_input_ops);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &midi_output_ops);
	m->rmidi = rmidi;
	return 0;
}

static const struct snd_soc_component_driver deluge_midi_component = {
	.name = "deluge-midi",
	.probe = deluge_midi_comp_probe,
};

/* --- SCIF0 bring-up --- */

static int midi_hw_init(struct deluge_midi *m)
{
	unsigned long rate = clk_get_rate(m->clk);
	unsigned int brr;

	brr = DIV_ROUND_CLOSEST(rate, 32 * MIDI_BAUD);
	if (brr == 0 || brr > 256)
		return dev_err_probe(m->dev, -EINVAL,
				     "SCIF clk %lu Hz: no 31250 divisor\n", rate);
	brr -= 1;

	writew(0, m->scif + SCIF_SCSCR);			/* disable during cfg */
	writew(SCFCR_RFRST | SCFCR_TFRST, m->scif + SCIF_SCFCR);	/* flush FIFOs */
	writew(readw(m->scif + SCIF_SCFSR) & SCFSR_KEEP, m->scif + SCIF_SCFSR);
	writew(0, m->scif + SCIF_SCSMR);			/* async 8N1, CKS=0 */
	writeb(brr, m->scif + SCIF_SCBRR);
	udelay(100);						/* >= 1 bit time */
	writew(0, m->scif + SCIF_SCFCR);			/* release FIFOs, RTRG=1 byte */
	writew(0, m->scif + SCIF_SCLSR);
	writew(SCSCR_MIDI, m->scif + SCIF_SCSCR);		/* RE|RIE|TE (RXI on) */
	return 0;
}

static int deluge_midi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct deluge_midi *m;
	int ret, rxi, txi;

	m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->dev = dev;
	spin_lock_init(&m->tx_lock);
	platform_set_drvdata(pdev, m);

	m->scif = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(m->scif))
		return PTR_ERR(m->scif);

	m->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(m->clk))
		return dev_err_probe(dev, PTR_ERR(m->clk), "no clk\n");

	rxi = platform_get_irq_byname(pdev, "rxi");
	if (rxi < 0)
		return rxi;
	txi = platform_get_irq_byname(pdev, "txi");
	if (txi < 0)
		return txi;

	ret = devm_request_irq(dev, rxi, midi_rxi, 0, "deluge-midi-rx", m);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get rxi\n");
	ret = devm_request_irq(dev, txi, midi_txi, 0, "deluge-midi-tx", m);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get txi\n");

	ret = midi_hw_init(m);
	if (ret)
		return ret;

	ret = devm_snd_soc_register_component(dev, &deluge_midi_component,
					      NULL, 0);
	if (ret)
		goto disable;

	dev_info(dev, "Deluge DIN MIDI ready (SCIF0, %d baud, RXI/TXI interrupt)\n",
		 MIDI_BAUD);
	return 0;

disable:
	writew(0, m->scif + SCIF_SCSCR);
	return ret;
}

static void deluge_midi_remove(struct platform_device *pdev)
{
	struct deluge_midi *m = platform_get_drvdata(pdev);

	writew(0, m->scif + SCIF_SCSCR);			/* disable RX/TX */
}

static const struct of_device_id deluge_midi_of[] = {
	{ .compatible = "synthstrom,deluge-midi" },
	{ }
};
MODULE_DEVICE_TABLE(of, deluge_midi_of);

static struct platform_driver deluge_midi_driver = {
	.probe = deluge_midi_probe,
	.remove = deluge_midi_remove,
	.driver = {
		.name = "deluge-midi",
		.of_match_table = deluge_midi_of,
	},
};
module_platform_driver(deluge_midi_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synthstrom Deluge DIN MIDI (SCIF0, RXI/TXI interrupt, rawmidi)");
