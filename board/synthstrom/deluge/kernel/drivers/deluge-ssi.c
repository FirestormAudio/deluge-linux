// SPDX-License-Identifier: GPL-2.0
//
// Synthstrom Deluge RZ/A1 SSIF ASoC CPU DAI.
//
// Forked from the mainline RZ/G2L SSI driver (sound/soc/sh/rz-ssi.c) and
// adapted for the RZ/A1L (r7s72100) SSIF: single external AUDIO_X1 master
// clock (CKS=0, CKDV=2 -> 128 x fs), 24-bit S24_LE, FIFO DMA trigger levels in
// SSIFCR (no SSISCR), TFRST/RFRST reset (no SSIRST/AUCKE), optional SFR clock /
// audio_clk2 / reset / error-IRQ. Register sequences from deluge-sdk
// rza1l-hal/src/ssi.rs + deluge-bsp/src/audio.rs.
//
// Copyright (C) 2021 Renesas Electronics Corp.
// Copyright (C) 2019 Chris Brandt.
//

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <sound/soc.h>

/* REGISTER OFFSET */
#define SSICR			0x000
#define SSISR			0x004
#define SSIFCR			0x010
#define SSIFSR			0x014
#define SSIFTDR			0x018
#define SSIFRDR			0x01c
#define SSIOFR			0x020
#define SSISCR			0x024

/* SSI REGISTER BITS */
#define SSICR_DWL(x)		(((x) & 0x7) << 19)
#define SSICR_SWL(x)		(((x) & 0x7) << 16)

/*
 * Parallel Data Alignment (SSICR bit 9). For DWL 18/20/22/24, PDTA selects
 * where the sub-32-bit data sits in the 32-bit slot: 0 = left-aligned (valid
 * [31:8]), 1 = right-aligned (valid [23:0]). ALSA S24_LE stores the 24-bit
 * sample right-aligned in [23:0], so we need PDTA=1. (The SDK uses PDTA=0
 * because its synth writes MSB-aligned i32 samples.)
 */
#define SSICR_PDTA		BIT(9)

#define SSICR_CKS		BIT(30)
#define SSICR_TUIEN		BIT(29)
#define SSICR_TOIEN		BIT(28)
#define SSICR_RUIEN		BIT(27)
#define SSICR_ROIEN		BIT(26)
/*
 * Master mode. Unlike RZ/G2L (a single MST bit), the RZ/A1 SSIF has separate
 * direction bits for the bit clock and word select, and BOTH must be set for
 * the SSI to drive them: SCKD (bit15, serial bit-clock direction = output) and
 * SWSD (bit14, word-select direction = output). Matches the SDK's SSICR value
 * 0x002B_C020 (rza1l-hal ssi.rs); setting only SWSD leaves BCLK an input, so no
 * clock reaches the codec and the TX FIFO never drains.
 */
#define SSICR_SCKD		BIT(15)
#define SSICR_SWSD		BIT(14)
#define SSICR_MST		(SSICR_SCKD | SSICR_SWSD)
#define SSICR_BCKP		BIT(13)
#define SSICR_LRCKP		BIT(12)
#define SSICR_CKDV(x)		(((x) & 0xf) << 4)
#define SSICR_TEN		BIT(1)
#define SSICR_REN		BIT(0)

#define SSISR_TUIRQ		BIT(29)
#define SSISR_TOIRQ		BIT(28)
#define SSISR_RUIRQ		BIT(27)
#define SSISR_ROIRQ		BIT(26)
#define SSISR_IIRQ		BIT(25)

#define SSIFCR_AUCKE		BIT(31)
#define SSIFCR_SSIRST		BIT(16)
#define SSIFCR_TIE		BIT(3)
#define SSIFCR_RIE		BIT(2)
#define SSIFCR_TFRST		BIT(1)
#define SSIFCR_RFRST		BIT(0)
/* RZ/A1: FIFO DMA-request trigger levels (SDK SSIFCR_INIT) — request when
 * >=4 FIFO stages are ready. RZ/G2L uses SSISCR for this; RZ/A1 has no 0x24. */
#define SSIFCR_TTRG_4		(0x2 << 6)	/* bits [7:6] */
#define SSIFCR_RTRG_4		(0x2 << 4)	/* bits [5:4] */

/* Samples to pre-load into the 32-stage TX FIFO to prime the first DMA-request
 * edge (must exceed the TTRG=4 trigger level; stays well under full). */
#define DELUGE_SSI_TX_FIFO_PRIME	16

#define SSIFSR_TDC_MASK		0x3f
#define SSIFSR_TDC_SHIFT	24
#define SSIFSR_RDC_MASK		0x3f
#define SSIFSR_RDC_SHIFT	8

#define SSIFSR_TDE		BIT(16)
#define SSIFSR_RDF		BIT(0)

#define SSIOFR_LRCONT		BIT(8)

#define SSISCR_TDES(x)		(((x) & 0x1f) << 8)
#define SSISCR_RDFS(x)		(((x) & 0x1f) << 0)

/* ---- RZ/A1 DMAC (raw), SSI0 TX = channel 6 --------------------------------
 * The SSI TX FIFO request does not drive an nbpf dmaengine slave transfer on
 * RZ/A1 (verified on-device); raw link-descriptor DMA — as the bare-metal SDK
 * does it — works. nbpf keeps ch0-5; we own ch6.  Register facts: TRM section 9.
 */
/* The DMAC + DMARS regions are owned by the nbpf driver; we ioremap them at
 * their fixed physical addresses rather than declaring overlapping DT reg
 * entries (platform_device_add would fail to claim the shared iomem). */
#define DMAC_BASE_PHYS		0xe8200000
#define DMAC_REGION_SZ		0x400
#define DMARS_BASE_PHYS		0xfcfe1000
#define DMARS_REGION_SZ		0x10

#define DMAC_TX_CH		6
#define DMAC_RX_CH		7
#define DMAC_CH_OFF(n)		((n) * 0x40)		/* channel register block */
#define DMAC_N0SA		0x00			/* Next0 source addr */
#define DMAC_N0DA		0x04			/* Next0 dest addr */
#define DMAC_N0TB		0x08			/* Next0 transaction bytes */
#define DMAC_CRSA		0x18			/* current source addr */
#define DMAC_CRDA		0x1c			/* current dest addr */
#define DMAC_CRTB		0x20			/* current transfer bytes */
#define DMAC_CHSTAT		0x24
#define DMAC_CHCTRL		0x28
#define DMAC_CHCFG		0x2c
#define DMAC_NXLA		0x38			/* next link-descriptor addr */
#define DMAC_DCTRL		0x300			/* group 0-7 control */

#define DMAC_SETEN		BIT(0)
#define DMAC_CLREN		BIT(1)
#define DMAC_SWRST		BIT(3)
#define DMAC_CLREND		BIT(5)
#define DMAC_CLRTC		BIT(6)
#define DMAC_CLRSUS		BIT(9)	/* clear suspend (resume the ring) */

#define DMAC_STAT_EN		BIT(0)
#define DMAC_STAT_END		BIT(5)
#define DMAC_STAT_TC		BIT(6)

#define DMAC_CHCFG_DMS		BIT(31)			/* 0=register mode, 1=link */
#define DMAC_CHCFG_DEM		BIT(24)			/* end-IRQ mask */
#define DMAC_CHCFG_DAD		BIT(21)			/* fixed dest (TX FIFO) */
#define DMAC_CHCFG_SAD		BIT(20)			/* fixed source (RX FIFO) */
#define DMAC_CHCFG_DDS(x)	(((x) & 0xf) << 16)
#define DMAC_CHCFG_SDS(x)	(((x) & 0xf) << 12)
#define DMAC_CHCFG_AM(x)	(((x) & 0x7) << 8)
#define DMAC_CHCFG_LVL		BIT(6)
#define DMAC_CHCFG_HIEN		BIT(5)
#define DMAC_CHCFG_REQD		BIT(3)
#define DMAC_SIZE_32BIT		2
#define DMAC_AM_BURST		2

/*
 * TX channel config — LINK-descriptor mode (DMS=1), the SDK's proven audio DMA
 * path (rza1l-hal ssi.rs tx_chcfg). Source increments through the buffer, dest
 * is the fixed SSIFTDR FIFO (DAD), 32-bit both sides, burst ACK, high-level
 * request, dest-select trigger (REQD — the SSI TX FIFO drives the request).
 * DEM cleared. The channel number goes in the low 3 bits. For ch6 this is
 * 0x8022226e. Continuous playback comes from a single self-referential
 * descriptor whose header sets WBD (see DMAC_DESC_HEADER) so the DMAC never
 * invalidates it — the request is serviced forever with no wrap gap.
 */
#define DMAC_TX_CHCFG		(DMAC_CHCFG_DMS | DMAC_CHCFG_DAD | \
				 DMAC_CHCFG_DDS(DMAC_SIZE_32BIT) | \
				 DMAC_CHCFG_SDS(DMAC_SIZE_32BIT) | \
				 DMAC_CHCFG_AM(DMAC_AM_BURST) | \
				 DMAC_CHCFG_HIEN | DMAC_CHCFG_REQD | \
				 DMAC_CHCFG_LVL | (DMAC_TX_CH & 7))

/*
 * RX channel config — LINK-descriptor mode, mirror of TX with the direction
 * reversed: fixed SOURCE = SSIFRDR (SAD), dest increments through the buffer,
 * source-select trigger (no REQD — the SSI RX FIFO drives the request). For
 * ch7 this is 0x80122267.
 */
#define DMAC_RX_CHCFG		(DMAC_CHCFG_DMS | DMAC_CHCFG_SAD | \
				 DMAC_CHCFG_DDS(DMAC_SIZE_32BIT) | \
				 DMAC_CHCFG_SDS(DMAC_SIZE_32BIT) | \
				 DMAC_CHCFG_AM(DMAC_AM_BURST) | \
				 DMAC_CHCFG_HIEN | DMAC_CHCFG_LVL | (DMAC_RX_CH & 7))

/*
 * Link-descriptor HEADER for our single self-looping node. TRM Table 9.15
 * header bits: LV(0) LE(1) WBD(2) DIM(3). We set LV|WBD|DIM = 0xd (the SDK's
 * DESC_HEADER). WBD=1 is the crux: it disables descriptor write-back, so the
 * DMAC never clears LV=0 in memory after a pass. The self-referential NXLA
 * then reloads a descriptor that is still valid, forever — continuous cyclic
 * playback with no wrap gap and no self-corruption.
 */
#define DMAC_DESC_HEADER	0xd

#define DMAC_DMARS_SSI0_TX	0xe1
#define DMAC_DMARS_SSI0_RX	0xe2
#define DMAC_DMARS_REG(n)	(((n) / 2) * 4)
#define DMAC_DMARS_SHIFT(n)	(((n) % 2) * 16)

/* RZ/A1 DMAC hardware link descriptor (8 x u32; TRM section 9). */
struct deluge_ssi_lldesc {
	u32 header;
	u32 src;
	u32 dst;
	u32 tb;			/* transaction byte count */
	u32 chcfg;
	u32 interval;
	u32 ext;
	u32 next;		/* physical address of the next descriptor */
} __packed;

/* Per-stream raw-DMA state: one self-referential link descriptor + the ALSA
 * buffer it moves, on a fixed DMAC channel. TX fixes the destination (SSIFTDR),
 * RX fixes the source (SSIFRDR); the other endpoint walks the buffer. */
struct deluge_ssi_dma {
	struct deluge_ssi_lldesc *desc;	/* ring of `periods` link descriptors */
	dma_addr_t desc_dma;		/* bus addr of desc[0] */
	dma_addr_t buf_dma;		/* ALSA buffer bus addr */
	unsigned int buf_bytes;		/* ALSA buffer size */
	unsigned int periods;		/* descriptors in the ring (1 = self-loop) */
	unsigned int period_bytes;	/* bytes per descriptor (0 = whole buf) */
	u8   ch;			/* DMAC channel: TX=6, RX=7 */
	bool dst_fixed;			/* true=TX (dst=FIFO), false=RX (src=FIFO) */
	bool mask_end_irq;		/* DEM=1: no transfer-end IRQ (dither only) */
};

#define DMAC_MAX_PERIODS	32	/* deluge_ssi_pcm_hardware.periods_max */
#define DMAC_CHSTAT_ER		BIT(4)	/* transfer error */
/* ---- end RZ/A1 DMAC (raw) ------------------------------------------------ */

/* TX keep-alive dither: one period of alternating +/- a few LSB (S24 right-
 * aligned) keeps the codec's I2S bus warm so it never emits idle static. */
#define DELUGE_SSI_DITHER_FRAMES	1024
#define DELUGE_SSI_DITHER_LEVEL	4	/* +/- LSB in the S24 field (inaudible) */

/* Pre allocated buffers sizes */
#define PREALLOC_BUFFER		(SZ_32K)
#define PREALLOC_BUFFER_MAX	(SZ_32K)

#define SSI_RATES		SNDRV_PCM_RATE_8000_48000 /* 8k-44.1kHz */
#define SSI_FMTS		SNDRV_PCM_FMTBIT_S24_LE	/* 24-bit data in 32-bit slot */
#define SSI_CHAN_MIN		2
#define SSI_CHAN_MAX		2
#define SSI_FIFO_DEPTH		32

struct deluge_ssi_priv;

struct deluge_ssi_stream {
	struct deluge_ssi_priv *priv;
	struct snd_pcm_substream *substream;
	int fifo_sample_size;	/* sample capacity of SSI FIFO */
	int dma_buffer_pos;	/* The address for the next DMA descriptor */
	int period_counter;	/* for keeping track of periods transferred */
	int sample_width;
	int buffer_pos;		/* current frame position in the buffer */
	int running;		/* 0=stopped, 1=running */

	struct deluge_ssi_dma dma;	/* raw DMAC channel for this direction */

	/* Preallocated on-chip SRAM audio buffer for this direction (NULL when
	 * falling back to the ALSA managed buffer in SDRAM). Non-cached; the CPU
	 * pointer and its bus/phys address are pointed to by runtime->dma_* at
	 * hw_params and mapped non-cached to userspace by the component .mmap. */
	void *sram_buf;
	dma_addr_t sram_buf_dma;

	int uerr_num;
	int oerr_num;
};

struct deluge_ssi_priv {
	void __iomem *base;
	struct platform_device *pdev;
	struct reset_control *rstc;
	struct device *dev;
	struct clk *sfr_clk;
	struct clk *clk;

	phys_addr_t phys;

	/* Raw DMAC (SSI0 TX = ch6): we drive the channel directly. */
	void __iomem *dmac;		/* DMAC register region (ch6 at +0x180) */
	void __iomem *dmars;		/* DMARS register region */
	/* Period advance is interrupt-driven: each ch6/ch7 descriptor covers one
	 * ALSA period with DEM=0, so its transfer-end (DMAINT) fires per period. */

	/* On-chip SRAM pool for DMA descriptors/buffers (NULL = fall back to the
	 * default coherent allocator in SDRAM). */
	struct gen_pool *sram;
	void *dither_buf;		/* kept for the SRAM free path */

	spinlock_t lock;

	/*
	 * The SSI supports full-duplex transmission and reception.
	 * However, if an error occurs, channel reset (both transmission
	 * and reception reset) is required.
	 * So it is better to use as half-duplex (playing and recording
	 * should be done on separate channels).
	 */
	struct deluge_ssi_stream playback;
	struct deluge_ssi_stream capture;

	/* clock */
	unsigned long audio_mck;
	unsigned long audio_clk_1;
	unsigned long audio_clk_2;
	unsigned int cur_rate;	/* rate of the running stream(s) (duplex guard) */
	struct deluge_ssi_dma dither;	/* TX keep-alive: continuous low-level dither */
	bool tx_keepalive;		/* dither TX running (codec bus kept warm) */

	bool lrckp_fsync_fall;	/* LR clock polarity (SSICR.LRCKP) */
	bool bckp_rise;	/* Bit clock polarity (SSICR.BCKP) */
};

static void deluge_ssi_reg_writel(struct deluge_ssi_priv *priv, uint reg, u32 data)
{
	writel(data, (priv->base + reg));
}

static u32 deluge_ssi_reg_readl(struct deluge_ssi_priv *priv, uint reg)
{
	return readl(priv->base + reg);
}

static void deluge_ssi_reg_mask_setl(struct deluge_ssi_priv *priv, uint reg,
				 u32 bclr, u32 bset)
{
	u32 val;

	val = readl(priv->base + reg);
	val = (val & ~bclr) | bset;
	writel(val, (priv->base + reg));
}

static inline struct snd_soc_dai *
deluge_ssi_get_dai(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);

	return snd_soc_rtd_to_cpu(rtd, 0);
}

static inline bool deluge_ssi_stream_is_play(struct deluge_ssi_priv *ssi,
					 struct snd_pcm_substream *substream)
{
	return substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
}

static inline struct deluge_ssi_stream *
deluge_ssi_stream_get(struct deluge_ssi_priv *ssi, struct snd_pcm_substream *substream)
{
	struct deluge_ssi_stream *stream = &ssi->playback;

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		stream = &ssi->capture;

	return stream;
}

/* The opposite-direction stream (for full-duplex first/last-stream logic). */
static inline struct deluge_ssi_stream *
deluge_ssi_other_stream(struct deluge_ssi_priv *ssi, struct deluge_ssi_stream *strm)
{
	return strm == &ssi->playback ? &ssi->capture : &ssi->playback;
}

static void deluge_ssi_set_substream(struct deluge_ssi_stream *strm,
				 struct snd_pcm_substream *substream)
{
	struct deluge_ssi_priv *ssi = strm->priv;
	unsigned long flags;

	spin_lock_irqsave(&ssi->lock, flags);
	strm->substream = substream;
	spin_unlock_irqrestore(&ssi->lock, flags);
}

static void deluge_ssi_stream_init(struct deluge_ssi_stream *strm,
			       struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	deluge_ssi_set_substream(strm, substream);
	strm->sample_width = samples_to_bytes(runtime, 1);
	strm->dma_buffer_pos = 0;
	strm->period_counter = 0;
	strm->buffer_pos = 0;

	strm->oerr_num = 0;
	strm->uerr_num = 0;
	strm->running = 0;

	/* fifo init */
	strm->fifo_sample_size = SSI_FIFO_DEPTH;
}

static void deluge_ssi_stream_quit(struct deluge_ssi_priv *ssi,
			       struct deluge_ssi_stream *strm)
{
	struct snd_soc_dai *dai = deluge_ssi_get_dai(strm->substream);

	deluge_ssi_set_substream(strm, NULL);

	if (strm->oerr_num > 0)
		dev_info(dai->dev, "overrun = %d\n", strm->oerr_num);

	if (strm->uerr_num > 0)
		dev_info(dai->dev, "underrun = %d\n", strm->uerr_num);
}

static int deluge_ssi_clk_setup(struct deluge_ssi_priv *ssi, unsigned int rate,
			    unsigned int channels)
{
	static s8 ckdv[16] = { 1,  2,  4,  8, 16, 32, 64, 128,
			       6, 12, 24, 48, 96, -1, -1, -1 };
	/*
	 * RZ/A1 Deluge: BCLK = 128 x fs (AUDIO_X1 22.5792 MHz / 4, CKDV=2),
	 * matching the SDK (deluge-bsp/audio.rs). This multiplier only feeds the
	 * CKDV computation below; the SSICR System Word Length is 32-bit (SWL=3).
	 */
	unsigned int channel_bits = 64;
	unsigned long bclk_rate = rate * channels * channel_bits;
	unsigned int div;
	unsigned int i;
	u32 ssicr = 0;
	u32 clk_ckdv;

	/* Clear AUCKE so we can set MST */
	deluge_ssi_reg_writel(ssi, SSIFCR, 0);

	/* Continue to output LRCK pin even when idle */
	deluge_ssi_reg_writel(ssi, SSIOFR, SSIOFR_LRCONT);
	if (ssi->audio_clk_1 && ssi->audio_clk_2) {
		if (ssi->audio_clk_1 % bclk_rate)
			ssi->audio_mck = ssi->audio_clk_2;
		else
			ssi->audio_mck = ssi->audio_clk_1;
	}

	/* Clock setting */
	ssicr |= SSICR_MST;
	/* RZ/A1: CKS=0 selects the external AUDIO_X1 crystal (our only source). */
	if (ssi->bckp_rise)
		ssicr |= SSICR_BCKP;
	if (ssi->lrckp_fsync_fall)
		ssicr |= SSICR_LRCKP;

	/* Determine the clock divider */
	clk_ckdv = 0;
	div = ssi->audio_mck / bclk_rate;
	/* try to find an match */
	for (i = 0; i < ARRAY_SIZE(ckdv); i++) {
		if (ckdv[i] == div) {
			clk_ckdv = i;
			break;
		}
	}

	if (i == ARRAY_SIZE(ckdv)) {
		dev_err(ssi->dev, "Rate not divisible by audio clock source\n");
		return -EINVAL;
	}

	/*
	 * DWL: Data Word Length = 24 bits (S24_LE) — RZ/A1 Deluge codec.
	 * SWL: System Word Length = 32 bits.
	 * PDTA=1: right-align the 24-bit data in the 32-bit slot to match the
	 * ALSA S24_LE in-memory layout (value in bits [23:0]).
	 */
	ssicr |= SSICR_CKDV(clk_ckdv);
	ssicr |= SSICR_DWL(5) | SSICR_SWL(3) | SSICR_PDTA;
	deluge_ssi_reg_writel(ssi, SSICR, ssicr);
	/*
	 * RZ/A1: no AUCKE (external always-on crystal); hold both FIFOs in reset
	 * and set the DMA trigger levels (SDK SSIFCR_INIT).
	 */
	deluge_ssi_reg_writel(ssi, SSIFCR,
			  (SSIFCR_TFRST | SSIFCR_RFRST |
			   SSIFCR_TTRG_4 | SSIFCR_RTRG_4));

	return 0;
}

static int deluge_ssi_start(struct deluge_ssi_priv *ssi, struct deluge_ssi_stream *strm)
{
	bool is_play = deluge_ssi_stream_is_play(ssi, strm->substream);
	u32 ssicr;

	/*
	 * Start sequence mirrors the SDK ssiStart(). Each step touches only this
	 * direction's bits (mask-set), so a running opposite-direction stream is
	 * left undisturbed — required for full duplex.
	 */

	/* 1. Error-status IRQ enables (SSICR); TEN/REN come last. */
	ssicr = deluge_ssi_reg_readl(ssi, SSICR);
	if (is_play)
		ssicr |= SSICR_TUIEN | SSICR_TOIEN;
	else
		ssicr |= SSICR_RUIEN | SSICR_ROIEN;
	deluge_ssi_reg_writel(ssi, SSICR, ssicr);

	/*
	 * 2. Release ONLY this direction's FIFO from reset (leave the other
	 * direction's FIFO/enable bits untouched so a running opposite stream is
	 * not disturbed). Separate write, before enabling the DMA request.
	 */
	deluge_ssi_reg_mask_setl(ssi, SSIFCR,
			     is_play ? SSIFCR_TFRST : SSIFCR_RFRST, 0);

	/*
	 * 3. RZ/A1 + nbpf DMA priming: seed the TX FIFO (now released, TEN still
	 * off) so the first drain past the trigger level generates the initial
	 * request edge for the armed nbpf DMA. The bare-metal SDK avoids this
	 * because its link-descriptor DMA fills the FIFO unconditionally.
	 */
	if (is_play) {
		int i;

		for (i = 0; i < DELUGE_SSI_TX_FIFO_PRIME; i++)
			deluge_ssi_reg_writel(ssi, SSIFTDR, 0);
	}

	/* 4. Enable ONLY this direction's DMA request (separate write). */
	deluge_ssi_reg_mask_setl(ssi, SSIFCR, 0,
			     is_play ? SSIFCR_TIE : SSIFCR_RIE);

	/* 5. Clear all error flags. */
	deluge_ssi_reg_mask_setl(ssi, SSISR,
			     (SSISR_TOIRQ | SSISR_TUIRQ | SSISR_ROIRQ |
			      SSISR_RUIRQ), 0);

	/* 6. Enable transmit/receive. */
	strm->running = 1;
	ssicr |= is_play ? SSICR_TEN : SSICR_REN;
	deluge_ssi_reg_writel(ssi, SSICR, ssicr);

	return 0;
}

/* Full-SSI quiesce: wait for idle, then hold BOTH FIFOs in reset. Only safe
 * when no direction is still running (last stop, or a fatal SSI error). */
static void deluge_ssi_quiesce(struct deluge_ssi_priv *ssi)
{
	int timeout = 100;

	while (--timeout) {
		if (deluge_ssi_reg_readl(ssi, SSISR) & SSISR_IIRQ)
			break;
		udelay(1);
	}
	if (!timeout)
		dev_info(ssi->dev, "timeout waiting for SSI idle\n");

	deluge_ssi_reg_mask_setl(ssi, SSIFCR, 0, SSIFCR_TFRST | SSIFCR_RFRST);
}

static int deluge_ssi_stop(struct deluge_ssi_priv *ssi, struct deluge_ssi_stream *strm)
{
	bool is_play = deluge_ssi_stream_is_play(ssi, strm->substream);

	strm->running = 0;

	/* Disable only this direction: enable, error IRQs, DMA request. The raw
	 * DMAC channel is halted by deluge_ssi_dmac_stop() in the trigger. */
	deluge_ssi_reg_mask_setl(ssi, SSICR, is_play ? SSICR_TEN : SSICR_REN, 0);
	deluge_ssi_reg_mask_setl(ssi, SSICR,
			     is_play ? (SSICR_TUIEN | SSICR_TOIEN)
				     : (SSICR_RUIEN | SSICR_ROIEN), 0);
	deluge_ssi_reg_mask_setl(ssi, SSIFCR, is_play ? SSIFCR_TIE : SSIFCR_RIE, 0);

	/* Clear only this direction's error flags. */
	deluge_ssi_reg_mask_setl(ssi, SSISR,
			     is_play ? (SSISR_TOIRQ | SSISR_TUIRQ)
				     : (SSISR_ROIRQ | SSISR_RUIRQ), 0);
	return 0;
}

/* ==========================================================================
 * Raw RZ/A1 DMAC engine (SSI0 TX=ch6, RX=ch7): link-descriptor ring, arm/stop/
 * pos/irq. Self-contained below; the DAI ops above call these deluge_ssi_dmac_*
 * entry points. (Kept in this file per the tier-4 in-file-reorg decision.)
 * ========================================================================== */
static inline u32 deluge_ssi_dmac_read(struct deluge_ssi_priv *ssi, u8 ch, u32 off)
{
	return ioread32(ssi->dmac + DMAC_CH_OFF(ch) + off);
}

static inline void deluge_ssi_dmac_write(struct deluge_ssi_priv *ssi, u8 ch,
				     u32 off, u32 val)
{
	iowrite32(val, ssi->dmac + DMAC_CH_OFF(ch) + off);
}

/* Halt a DMAC channel: disable it, then soft-reset. */
static void deluge_ssi_dmac_ch_halt(struct deluge_ssi_priv *ssi, u8 ch)
{
	deluge_ssi_dmac_write(ssi, ch, DMAC_CHCTRL, DMAC_CLREN);
	deluge_ssi_dmac_write(ssi, ch, DMAC_CHCTRL, DMAC_SWRST);
}

/* Allocate DMA-coherent memory, preferring the on-chip SRAM pool (0-wait, off
 * the SDRAM bus) when present, else the default coherent allocator in SDRAM. */
static void *deluge_ssi_dma_alloc(struct deluge_ssi_priv *ssi, size_t size,
			      dma_addr_t *dma)
{
	if (ssi->sram)
		return gen_pool_dma_alloc(ssi->sram, size, dma);
	return dmam_alloc_coherent(ssi->dev, size, dma, GFP_KERNEL);
}

/*
 * Like deluge_ssi_dma_alloc, but page-aligned — for buffers that are mmap'd to
 * userspace. .mmap remaps whole pages (remap_pfn_range on dma_addr >>
 * PAGE_SHIFT), silently dropping any intra-page offset. A buffer starting
 * mid-page would place the userspace mapping a page-offset too low, so the
 * app's writes land on whatever precedes the buffer in its first page — here
 * the DMA descriptor ring. The app zeroing the live descriptor faults the
 * channel (DER: LV=0 on the self-loop reload). Page-aligning the buffer makes
 * the mmap offset zero and isolates it from the descriptors.
 */
static void *deluge_ssi_dma_alloc_page(struct deluge_ssi_priv *ssi, size_t size,
				   dma_addr_t *dma)
{
	if (ssi->sram)
		return gen_pool_dma_alloc_align(ssi->sram, size, dma, PAGE_SIZE);
	return dmam_alloc_coherent(ssi->dev, size, dma, GFP_KERNEL);
}

/* Record the ALSA buffer for the DMA transfer (called at hw_params). */
static int deluge_ssi_dmac_setup_ring(struct deluge_ssi_priv *ssi,
				  struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	struct deluge_ssi_stream *strm = deluge_ssi_stream_get(ssi, substream);

	/*
	 * runtime->buffer_size is not committed until after this hw_params
	 * callback returns, so snd_pcm_lib_buffer_bytes() would read 0 here.
	 * Take the size straight from the params being applied. dma_addr is
	 * valid (the managed buffer is preallocated).
	 */
	strm->dma.buf_dma = substream->runtime->dma_addr;
	strm->dma.buf_bytes = params_buffer_bytes(params);
	/* One link descriptor per ALSA period, so each descriptor's transfer-end
	 * (DMAINT) marks a period boundary. Bounded by periods_max. */
	strm->dma.periods = params_periods(params);
	if (!strm->dma.periods || strm->dma.periods > DMAC_MAX_PERIODS)
		return -EINVAL;
	strm->dma.period_bytes = strm->dma.buf_bytes / strm->dma.periods;
	return 0;
}

/* Current position (frames) from the DMAC's moving address: source for TX
 * (dst is the fixed FIFO), destination for RX (src is the fixed FIFO). */
static snd_pcm_uframes_t deluge_ssi_dmac_pos(struct deluge_ssi_stream *strm)
{
	struct deluge_ssi_priv *ssi = strm->priv;
	struct deluge_ssi_dma *dma = &strm->dma;
	u32 cur = deluge_ssi_dmac_read(ssi, dma->ch,
				   dma->dst_fixed ? DMAC_CRSA : DMAC_CRDA);
	u32 off = cur - (u32)dma->buf_dma;

	if (off >= dma->buf_bytes)		/* not yet loaded / at wrap */
		return 0;
	return bytes_to_frames(strm->substream->runtime, off);
}

/*
 * Arm a channel in LINK-descriptor mode with a ring of `periods` descriptors
 * (SDK init_with_link_descriptor + channel_start). WBD=1 in each header keeps
 * every descriptor permanently valid, so the DMAC walks the ring forever
 * (each .next -> the next, last -> first) servicing the SSI request. Each
 * descriptor covers one ALSA period; with DEM=0 its completion raises the
 * transfer-end (DMAINT) interrupt, so deluge_ssi_dmac_irq() gets one interrupt per
 * period. The dither keep-alive uses one self-looping descriptor with DEM=1
 * (masked: no interrupts while idle). TX moves buffer -> SSIFTDR (DAD); RX
 * moves SSIFRDR -> buffer (SAD). Descriptors are in coherent memory.
 */
static void deluge_ssi_dmac_arm(struct deluge_ssi_priv *ssi, struct deluge_ssi_dma *dma)
{
	u32 reg = DMAC_DMARS_REG(dma->ch);
	u32 shift = DMAC_DMARS_SHIFT(dma->ch);
	u32 fifo = (u32)ssi->phys + (dma->dst_fixed ? SSIFTDR : SSIFRDR);
	u32 dmars = dma->dst_fixed ? DMAC_DMARS_SSI0_TX : DMAC_DMARS_SSI0_RX;
	u32 chcfg = (dma->dst_fixed ? DMAC_TX_CHCFG : DMAC_RX_CHCFG) |
		    (dma->mask_end_irq ? DMAC_CHCFG_DEM : 0);
	unsigned int n = dma->periods ? dma->periods : 1;
	unsigned int pb = dma->period_bytes ? dma->period_bytes : dma->buf_bytes;
	unsigned int i;
	u32 v;

	for (i = 0; i < n; i++) {
		struct deluge_ssi_lldesc *d = &dma->desc[i];
		u32 buf = (u32)dma->buf_dma + i * pb;

		d->header   = DMAC_DESC_HEADER;
		d->src      = dma->dst_fixed ? buf : fifo;
		d->dst      = dma->dst_fixed ? fifo : buf;
		d->tb       = pb;
		d->chcfg    = chcfg;
		d->interval = 0;
		d->ext      = 0;
		d->next     = (u32)dma->desc_dma + ((i + 1) % n) * sizeof(*d);
	}
	wmb();

	/* init_with_link_descriptor: DCTRL=0, CHCFG=desc[0], DMARS, NXLA=desc[0]. */
	iowrite32(0, ssi->dmac + DMAC_DCTRL);
	deluge_ssi_dmac_write(ssi, dma->ch, DMAC_CHCFG, chcfg);

	v = ioread32(ssi->dmars + reg);
	v = (v & ~(0xffffu << shift)) | (dmars << shift);
	iowrite32(v, ssi->dmars + reg);

	deluge_ssi_dmac_write(ssi, dma->ch, DMAC_NXLA, (u32)dma->desc_dma);

	/* channel_start: SWRST, barrier, SETEN. */
	deluge_ssi_dmac_write(ssi, dma->ch, DMAC_CHCTRL, DMAC_SWRST);
	wmb();
	deluge_ssi_dmac_write(ssi, dma->ch, DMAC_CHCTRL, DMAC_SETEN);
}

/*
 * Transfer-end (DMAINT) interrupt: one per period (each ring descriptor has
 * DEM=0). Acknowledge it and advance the ALSA pointer. dev_id is the stream
 * whose channel raised it (ch6 = playback, ch7 = capture). The dither runs
 * with DEM=1, so ch6 never interrupts while idle.
 */
static irqreturn_t deluge_ssi_dmac_irq(int irq, void *dev_id)
{
	struct deluge_ssi_stream *strm = dev_id;
	struct deluge_ssi_priv *ssi = strm->priv;
	u32 stat = deluge_ssi_dmac_read(ssi, strm->dma.ch, DMAC_CHSTAT);

	deluge_ssi_dmac_write(ssi, strm->dma.ch, DMAC_CHCTRL, DMAC_CLREND);

	if (stat & DMAC_CHSTAT_ER) {
		dev_err_ratelimited(ssi->dev, "DMAC ch%u transfer error %#x\n",
				    strm->dma.ch, stat);
		return IRQ_HANDLED;
	}
	if (READ_ONCE(strm->running) && strm->substream)
		snd_pcm_period_elapsed(strm->substream);
	return IRQ_HANDLED;
}

static void deluge_ssi_dmac_start(struct deluge_ssi_priv *ssi, struct deluge_ssi_stream *strm)
{
	deluge_ssi_dmac_arm(ssi, &strm->dma);
}

static void deluge_ssi_dmac_stop(struct deluge_ssi_priv *ssi, struct deluge_ssi_stream *strm)
{
	deluge_ssi_dmac_ch_halt(ssi, strm->dma.ch);
}
/* ==================== end raw DMAC engine ================================== */

/*
 * Start the TX dither keep-alive: configure the clock at the default rate, arm
 * ch6 with the dither buffer, and enable TX. Keeps the codec's I2S bus warm so
 * it never emits idle static. Called at card bind (component .probe) and resume.
 */
static int deluge_ssi_tx_keepalive(struct deluge_ssi_priv *ssi)
{
	u32 ssicr;
	int i, ret;

	ret = deluge_ssi_clk_setup(ssi, 44100, 2);	/* SSICR + both-FIFO reset */
	if (ret)
		return ret;
	ssi->cur_rate = 44100;

	deluge_ssi_dmac_arm(ssi, &ssi->dither);	/* ch6 -> dither buffer */

	/* TX enable (mirrors deluge_ssi_start's TX path; no substream needed). */
	ssicr = deluge_ssi_reg_readl(ssi, SSICR);
	ssicr |= SSICR_TUIEN | SSICR_TOIEN;
	deluge_ssi_reg_writel(ssi, SSICR, ssicr);
	deluge_ssi_reg_mask_setl(ssi, SSIFCR, SSIFCR_TFRST, 0);	/* release TX FIFO */
	for (i = 0; i < DELUGE_SSI_TX_FIFO_PRIME; i++)
		deluge_ssi_reg_writel(ssi, SSIFTDR, 0);		/* prime request edge */
	deluge_ssi_reg_mask_setl(ssi, SSIFCR, 0, SSIFCR_TIE);	/* TX DMA request */
	deluge_ssi_reg_mask_setl(ssi, SSISR, SSISR_TOIRQ | SSISR_TUIRQ, 0);
	ssicr |= SSICR_TEN;
	deluge_ssi_reg_writel(ssi, SSICR, ssicr);

	ssi->tx_keepalive = true;
	return 0;
}

/* Stop the dither keep-alive: halt ch6, disable TX, quiesce the SSI. */
static void deluge_ssi_tx_keepalive_stop(struct deluge_ssi_priv *ssi)
{
	if (!ssi->tx_keepalive)
		return;
	deluge_ssi_reg_mask_setl(ssi, SSICR, SSICR_TEN, 0);
	deluge_ssi_dmac_ch_halt(ssi, DMAC_TX_CH);
	deluge_ssi_quiesce(ssi);
	ssi->tx_keepalive = false;
}

static int deluge_ssi_dai_trigger(struct snd_pcm_substream *substream, int cmd,
			      struct snd_soc_dai *dai)
{
	struct deluge_ssi_priv *ssi = snd_soc_dai_get_drvdata(dai);
	struct deluge_ssi_stream *strm = deluge_ssi_stream_get(ssi, substream);
	bool is_play = deluge_ssi_stream_is_play(ssi, substream);
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		deluge_ssi_stream_init(strm, substream);
		if (is_play) {
			/* Re-point TX from the dither keep-alive to the ALSA buffer;
			 * TEN/TIE stay on (no cold start, no FIFO reset). */
			deluge_ssi_dmac_arm(ssi, &strm->dma);
			strm->running = 1;
		} else {
			/* Capture: arm ch7 and enable RX. The TX dither keeps
			 * BCLK/WS alive, so no both-FIFO reset is needed. */
			deluge_ssi_dmac_start(ssi, strm);
			ret = deluge_ssi_start(ssi, strm);
		}
		/* Period advance is now driven by the per-period DMAINT
		 * (deluge_ssi_dmac_irq); no poll timer to start. */
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		if (is_play) {
			/* Re-point TX back to the dither keep-alive; leave TEN on so
			 * the codec bus stays warm (no idle static). */
			strm->running = 0;
			deluge_ssi_dmac_arm(ssi, &ssi->dither);
		} else {
			deluge_ssi_stop(ssi, strm);		/* RX disable */
			deluge_ssi_dmac_stop(ssi, strm);	/* halt ch7 */
		}
		deluge_ssi_stream_quit(ssi, strm);
		/* The poll self-stops (returns NORESTART) once no stream runs. */
		break;
	}

	return ret;
}

static int deluge_ssi_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct deluge_ssi_priv *ssi = snd_soc_dai_get_drvdata(dai);

	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_BP_FP:
		break;
	default:
		dev_err(ssi->dev, "Codec should be clk and frame consumer\n");
		return -EINVAL;
	}

	/*
	 * set clock polarity
	 *
	 * "normal" BCLK = Signal is available at rising edge of BCLK
	 * "normal" FSYNC = (I2S) Left ch starts with falling FSYNC edge
	 */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		ssi->bckp_rise = false;
		ssi->lrckp_fsync_fall = false;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		ssi->bckp_rise = false;
		ssi->lrckp_fsync_fall = true;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		ssi->bckp_rise = true;
		ssi->lrckp_fsync_fall = false;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		ssi->bckp_rise = true;
		ssi->lrckp_fsync_fall = true;
		break;
	default:
		return -EINVAL;
	}

	/* only i2s support */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		break;
	default:
		dev_err(ssi->dev, "Only I2S mode is supported.\n");
		return -EINVAL;
	}

	return 0;
}

static int deluge_ssi_dai_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct deluge_ssi_priv *ssi = snd_soc_dai_get_drvdata(dai);
	unsigned int sample_bits = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_SAMPLE_BITS)->min;
	unsigned int channels = params_channels(params);
	int ret;

	/* S24_LE carries 24 data bits in a 32-bit physical slot (SWL=32), so the
	 * ALSA sample (physical) width is 32. This is the only format the fixed
	 * SSICR word geometry (DWL=24/SWL=32) supports. */
	if (sample_bits != 32) {
		dev_err(ssi->dev, "Unsupported sample width: %d\n",
			sample_bits);
		return -EINVAL;
	}

	if (channels != 2) {
		dev_err(ssi->dev, "Number of channels not matched: %d\n",
			channels);
		return -EINVAL;
	}

	/*
	 * When the on-chip SRAM pool is present, run the audio data stream from
	 * there instead of the ALSA managed buffer in SDRAM: point runtime->dma_*
	 * at this direction's preallocated non-cached SRAM buffer before
	 * setup_ring reads runtime->dma_addr. The DMA stream then never stalls on
	 * SDRAM refresh / bus contention — deterministic for RT audio. (The buffer
	 * is preallocated once at probe and reused, so hw_free need not release it.)
	 */
	if (ssi->sram) {
		struct deluge_ssi_stream *strm = deluge_ssi_stream_get(ssi, substream);
		struct snd_pcm_runtime *runtime = substream->runtime;

		runtime->dma_area = strm->sram_buf;
		runtime->dma_addr = strm->sram_buf_dma;
		runtime->dma_bytes = params_buffer_bytes(params);
	}

	/* Store the buffer for this direction's DMA channel. */
	ret = deluge_ssi_dmac_setup_ring(ssi, substream, params);
	if (ret)
		return ret;

	/*
	 * Clock/SSICR + both-FIFO-reset (deluge_ssi_clk_setup) is a shared,
	 * destructive operation: run it only for the first stream. If the other
	 * direction is already running it is already configured, and full duplex
	 * requires the same rate (they share BCLK/WS) — reject a mismatch.
	 */
	if (ssi->tx_keepalive ||
	    deluge_ssi_other_stream(ssi, deluge_ssi_stream_get(ssi, substream))->running) {
		if (params_rate(params) != ssi->cur_rate) {
			dev_err(ssi->dev,
				"duplex rate mismatch: %u vs running %u\n",
				params_rate(params), ssi->cur_rate);
			return -EINVAL;
		}
		return 0;
	}

	ssi->cur_rate = params_rate(params);
	return deluge_ssi_clk_setup(ssi, params_rate(params),
				params_channels(params));
}

static const struct snd_soc_dai_ops deluge_ssi_dai_ops = {
	.trigger	= deluge_ssi_dai_trigger,
	.set_fmt	= deluge_ssi_dai_set_fmt,
	.hw_params	= deluge_ssi_dai_hw_params,
};

static const struct snd_pcm_hardware deluge_ssi_pcm_hardware = {
	.info			= SNDRV_PCM_INFO_INTERLEAVED	|
				  SNDRV_PCM_INFO_MMAP		|
				  SNDRV_PCM_INFO_MMAP_VALID,
	.buffer_bytes_max	= PREALLOC_BUFFER,
	.period_bytes_min	= 32,
	.period_bytes_max	= 8192,
	.channels_min		= SSI_CHAN_MIN,
	.channels_max		= SSI_CHAN_MAX,
	.periods_min		= 1,
	.periods_max		= 32,
	.fifo_size		= 32 * 2,
};

static int deluge_ssi_pcm_open(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream)
{
	snd_soc_set_runtime_hwparams(substream, &deluge_ssi_pcm_hardware);

	return snd_pcm_hw_constraint_integer(substream->runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
}

static snd_pcm_uframes_t deluge_ssi_pcm_pointer(struct snd_soc_component *component,
					    struct snd_pcm_substream *substream)
{
	struct snd_soc_dai *dai = deluge_ssi_get_dai(substream);
	struct deluge_ssi_priv *ssi = snd_soc_dai_get_drvdata(dai);
	struct deluge_ssi_stream *strm = deluge_ssi_stream_get(ssi, substream);

	return deluge_ssi_dmac_pos(strm);
}

static int deluge_ssi_pcm_new(struct snd_soc_component *component,
			  struct snd_soc_pcm_runtime *rtd)
{
	struct deluge_ssi_priv *ssi = dev_get_drvdata(component->dev);

	/*
	 * With the SRAM pool the audio buffers live in on-chip SRAM (preallocated
	 * at probe, wired up in hw_params, mapped by our .mmap) — do NOT register
	 * a managed SDRAM buffer, which would override runtime->dma_* on hw_params.
	 * Only fall back to a managed SDRAM buffer when there is no SRAM pool.
	 */
	if (!ssi->sram)
		snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
					       rtd->card->snd_card->dev,
					       PREALLOC_BUFFER, PREALLOC_BUFFER_MAX);
	return 0;
}

/*
 * Map the audio buffer to userspace. For the SRAM buffer we must map it
 * non-cached (the DMAC and CPU share it with no cache maintenance, exactly as
 * the descriptors), so remap the SRAM physical pages with pgprot_noncached.
 * Without the SRAM pool, defer to the managed-buffer default mmap (SDRAM).
 */
static int deluge_ssi_pcm_mmap(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream,
			   struct vm_area_struct *vma)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct deluge_ssi_priv *ssi = dev_get_drvdata(component->dev);
	size_t size = vma->vm_end - vma->vm_start;

	if (!ssi->sram)
		return snd_pcm_lib_default_mmap(substream, vma);

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start,
			       runtime->dma_addr >> PAGE_SHIFT, size,
			       vma->vm_page_prot);
}

static struct snd_soc_dai_driver deluge_ssi_soc_dai[] = {
	{
		.name			= "rz-ssi-dai",
		.playback = {
			.rates		= SSI_RATES,
			.formats	= SSI_FMTS,
			.channels_min	= SSI_CHAN_MIN,
			.channels_max	= SSI_CHAN_MAX,
		},
		.capture = {
			.rates		= SSI_RATES,
			.formats	= SSI_FMTS,
			.channels_min	= SSI_CHAN_MIN,
			.channels_max	= SSI_CHAN_MAX,
		},
		.ops = &deluge_ssi_dai_ops,
	},
};

static int deluge_ssi_component_probe(struct snd_soc_component *component)
{
	struct deluge_ssi_priv *ssi = dev_get_drvdata(component->dev);
	int ret = deluge_ssi_tx_keepalive(ssi);

	if (ret)
		dev_warn(ssi->dev, "TX keep-alive start failed: %d\n", ret);
	return 0;	/* non-fatal: the card still works, just may click at idle */
}

/* Gate the dither keep-alive off on system suspend (battery) and back on at
 * resume, so the codec bus is warm while awake, idle while suspended. */
static int deluge_ssi_component_suspend(struct snd_soc_component *component)
{
	deluge_ssi_tx_keepalive_stop(dev_get_drvdata(component->dev));
	return 0;
}

static int deluge_ssi_component_resume(struct snd_soc_component *component)
{
	struct deluge_ssi_priv *ssi = dev_get_drvdata(component->dev);

	if (!ssi->tx_keepalive)
		deluge_ssi_tx_keepalive(ssi);
	return 0;
}

static const struct snd_soc_component_driver deluge_ssi_soc_component = {
	.name			= "rz-ssi",
	.probe			= deluge_ssi_component_probe,
	.suspend		= deluge_ssi_component_suspend,
	.resume			= deluge_ssi_component_resume,
	.open			= deluge_ssi_pcm_open,
	.pointer		= deluge_ssi_pcm_pointer,
	.pcm_new		= deluge_ssi_pcm_new,
	.mmap			= deluge_ssi_pcm_mmap,
	.legacy_dai_naming	= 1,
};

/* Return the on-chip SRAM pool allocations (not devm-managed by gen_pool).
 * Registered only when ssi->sram is present, so every field is valid here. */
static void deluge_ssi_sram_free(void *data)
{
	struct deluge_ssi_priv *ssi = data;
	size_t dsz = DMAC_MAX_PERIODS * sizeof(struct deluge_ssi_lldesc);

	gen_pool_free(ssi->sram, (unsigned long)ssi->playback.dma.desc, dsz);
	gen_pool_free(ssi->sram, (unsigned long)ssi->capture.dma.desc, dsz);
	gen_pool_free(ssi->sram, (unsigned long)ssi->dither.desc,
		      sizeof(struct deluge_ssi_lldesc));
	gen_pool_free(ssi->sram, (unsigned long)ssi->dither_buf,
		      ssi->dither.buf_bytes);
	gen_pool_free(ssi->sram, (unsigned long)ssi->playback.sram_buf,
		      PREALLOC_BUFFER);
	gen_pool_free(ssi->sram, (unsigned long)ssi->capture.sram_buf,
		      PREALLOC_BUFFER);
}

static int deluge_ssi_probe(struct platform_device *pdev)
{
	struct deluge_ssi_priv *ssi;
	struct clk *audio_clk;
	struct resource *res;
	void *dither_buf;
	unsigned int i;
	int ret;

	ssi = devm_kzalloc(&pdev->dev, sizeof(*ssi), GFP_KERNEL);
	if (!ssi)
		return -ENOMEM;

	ssi->pdev = pdev;
	ssi->dev = &pdev->dev;
	ssi->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(ssi->base))
		return PTR_ERR(ssi->base);

	ssi->phys = res->start;
	ssi->clk = devm_clk_get(&pdev->dev, "ssi");
	if (IS_ERR(ssi->clk))
		return PTR_ERR(ssi->clk);

	/* RZ/A1 has no separate SFR bus clock — optional. */
	ssi->sfr_clk = devm_clk_get_optional(&pdev->dev, "ssi_sfr");
	if (IS_ERR(ssi->sfr_clk))
		return PTR_ERR(ssi->sfr_clk);

	audio_clk = devm_clk_get(&pdev->dev, "audio_clk1");
	if (IS_ERR(audio_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(audio_clk),
				     "no audio clk1");

	ssi->audio_clk_1 = clk_get_rate(audio_clk);
	/* RZ/A1 has a single audio master clock (AUDIO_X1) — clk2 optional. */
	audio_clk = devm_clk_get_optional(&pdev->dev, "audio_clk2");
	if (IS_ERR(audio_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(audio_clk),
				     "no audio clk2");

	ssi->audio_clk_2 = clk_get_rate(audio_clk);
	if (!(ssi->audio_clk_1 || ssi->audio_clk_2))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "no audio clk1 or audio clk2");

	ssi->audio_mck = ssi->audio_clk_1 ? ssi->audio_clk_1 : ssi->audio_clk_2;

	/*
	 * Raw DMAC (ch6 at +0x180, DCTRL at +0x300) + DMARS. nbpf owns these
	 * regions, so map them with plain devm_ioremap at their fixed addresses
	 * (no exclusive request_mem_region, no overlapping DT reg entry).
	 */
	ssi->dmac = devm_ioremap(&pdev->dev, DMAC_BASE_PHYS, DMAC_REGION_SZ);
	ssi->dmars = devm_ioremap(&pdev->dev, DMARS_BASE_PHYS, DMARS_REGION_SZ);
	if (!ssi->dmac || !ssi->dmars)
		return -ENOMEM;

	/*
	 * A ring of up to periods_max link descriptors per direction, in
	 * coherent memory: the DMAC reads them (walking the ring via the .next
	 * chain) directly, and WBD=1 means it never writes them back — a coherent
	 * CPU mapping is all we need. TX = ch6 (dst fixed = SSIFTDR); RX = ch7
	 * (src fixed = SSIFRDR).
	 */
	ssi->playback.dma.ch = DMAC_TX_CH;
	ssi->playback.dma.dst_fixed = true;
	ssi->capture.dma.ch = DMAC_RX_CH;
	ssi->capture.dma.dst_fixed = false;

	/*
	 * We drive DMAC ch6/7 raw; the mainline rz-dmac must stay capped at ch0-5
	 * (DT dma-channels <= DMAC_TX_CH) so it never claims them. Warn loudly if a
	 * DT change would collide — silent corruption otherwise.
	 */
	{
		struct device_node *dmac_np =
			of_find_compatible_node(NULL, NULL, "renesas,rz-dmac");
		u32 nch;

		if (dmac_np) {
			if (!of_property_read_u32(dmac_np, "dma-channels", &nch) &&
			    nch > DMAC_TX_CH)
				dev_warn(&pdev->dev,
					 "rz-dmac exposes %u channels but deluge-ssi drives ch%u/%u raw — cap DT dma-channels at %u\n",
					 nch, DMAC_TX_CH, DMAC_RX_CH, DMAC_TX_CH);
			of_node_put(dmac_np);
		}
	}

	/* On-chip SRAM pool for the DMA descriptors + dither (optional). */
	ssi->sram = of_gen_pool_get(pdev->dev.of_node, "sram", 0);

	ssi->playback.dma.desc = deluge_ssi_dma_alloc(ssi,
			DMAC_MAX_PERIODS * sizeof(*ssi->playback.dma.desc),
			&ssi->playback.dma.desc_dma);
	ssi->capture.dma.desc = deluge_ssi_dma_alloc(ssi,
			DMAC_MAX_PERIODS * sizeof(*ssi->capture.dma.desc),
			&ssi->capture.dma.desc_dma);
	if (!ssi->playback.dma.desc || !ssi->capture.dma.desc)
		return -ENOMEM;
	dev_info(&pdev->dev, "tx desc @ %pad, rx desc @ %pad (%s)\n",
		 &ssi->playback.dma.desc_dma, &ssi->capture.dma.desc_dma,
		 ssi->sram ? "on-chip SRAM" : "SDRAM");

	/*
	 * Audio data buffers in on-chip SRAM (one per direction, max size),
	 * preallocated and reused. hw_params points runtime->dma_* at these; the
	 * DMA stream then runs entirely off the SDRAM bus (deterministic for RT
	 * audio). Without the pool, the ALSA managed SDRAM buffer is used instead.
	 */
	if (ssi->sram) {
		ssi->playback.sram_buf = deluge_ssi_dma_alloc_page(ssi, PREALLOC_BUFFER,
							  &ssi->playback.sram_buf_dma);
		ssi->capture.sram_buf = deluge_ssi_dma_alloc_page(ssi, PREALLOC_BUFFER,
							 &ssi->capture.sram_buf_dma);
		if (!ssi->playback.sram_buf || !ssi->capture.sram_buf)
			return -ENOMEM;
		dev_info(&pdev->dev, "tx buf @ %pad, rx buf @ %pad (on-chip SRAM)\n",
			 &ssi->playback.sram_buf_dma, &ssi->capture.sram_buf_dma);
	}

	/* TX keep-alive dither: one self-looping descriptor, DEM=1 (no IRQ). */
	ssi->dither.ch = DMAC_TX_CH;
	ssi->dither.dst_fixed = true;
	ssi->dither.periods = 1;
	ssi->dither.mask_end_irq = true;
	ssi->dither.buf_bytes = DELUGE_SSI_DITHER_FRAMES * SSI_CHAN_MAX * sizeof(u32);
	dither_buf = deluge_ssi_dma_alloc(ssi, ssi->dither.buf_bytes,
				      &ssi->dither.buf_dma);
	ssi->dither_buf = dither_buf;
	ssi->dither.desc = deluge_ssi_dma_alloc(ssi, sizeof(*ssi->dither.desc),
					    &ssi->dither.desc_dma);
	if (!dither_buf || !ssi->dither.desc)
		return -ENOMEM;
	for (i = 0; i < ssi->dither.buf_bytes / sizeof(s32); i++)
		((s32 *)dither_buf)[i] = (i & 1) ? DELUGE_SSI_DITHER_LEVEL
						 : -DELUGE_SSI_DITHER_LEVEL;

	/* All SRAM-pool allocations exist now; free them on unbind or any later
	 * probe-failure unwind (the non-SRAM path uses dmam_, already managed). */
	if (ssi->sram) {
		ret = devm_add_action_or_reset(&pdev->dev, deluge_ssi_sram_free, ssi);
		if (ret)
			return ret;
	}

	ssi->playback.priv = ssi;
	ssi->capture.priv = ssi;

	/* Per-period transfer-end (DMAINT) interrupts: ch6 -> playback,
	 * ch7 -> capture. rz-dmac is capped to ch0-5 so these channels are ours. */
	{
		int irq;

		irq = platform_get_irq_byname(pdev, "tx");
		if (irq < 0)
			return irq;
		ret = devm_request_irq(&pdev->dev, irq, deluge_ssi_dmac_irq, 0,
				       "deluge-ssi-tx", &ssi->playback);
		if (ret)
			return dev_err_probe(&pdev->dev, ret, "cannot get tx DMAINT\n");

		irq = platform_get_irq_byname(pdev, "rx");
		if (irq < 0)
			return irq;
		ret = devm_request_irq(&pdev->dev, irq, deluge_ssi_dmac_irq, 0,
				       "deluge-ssi-rx", &ssi->capture);
		if (ret)
			return dev_err_probe(&pdev->dev, ret, "cannot get rx DMAINT\n");
	}

	spin_lock_init(&ssi->lock);
	dev_set_drvdata(&pdev->dev, ssi);

	/* RZ/A1 CPG has no per-SSI reset controller — optional (NULL is a no-op). */
	ssi->rstc = devm_reset_control_get_optional_exclusive(&pdev->dev, NULL);
	if (IS_ERR(ssi->rstc)) {
		ret = PTR_ERR(ssi->rstc);
		goto err_reset;
	}

	reset_control_deassert(ssi->rstc);
	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "pm_runtime_resume_and_get failed\n");
		goto err_pm;
	}

	ret = devm_snd_soc_register_component(&pdev->dev, &deluge_ssi_soc_component,
					      deluge_ssi_soc_dai,
					      ARRAY_SIZE(deluge_ssi_soc_dai));
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register snd component\n");
		goto err_snd_soc;
	}

	return 0;

err_snd_soc:
	pm_runtime_put(ssi->dev);
err_pm:
	pm_runtime_disable(ssi->dev);
	reset_control_assert(ssi->rstc);
err_reset:
	return ret;
}

static void deluge_ssi_remove(struct platform_device *pdev)
{
	struct deluge_ssi_priv *ssi = dev_get_drvdata(&pdev->dev);

	deluge_ssi_tx_keepalive_stop(ssi);
	pm_runtime_put(ssi->dev);
	pm_runtime_disable(ssi->dev);
	reset_control_assert(ssi->rstc);
}

static const struct of_device_id deluge_ssi_of_match[] = {
	{ .compatible = "synthstrom,deluge-ssi", },
	{/* Sentinel */},
};
MODULE_DEVICE_TABLE(of, deluge_ssi_of_match);

static struct platform_driver deluge_ssi_driver = {
	.driver	= {
		.name	= "deluge-ssi",
		.of_match_table = deluge_ssi_of_match,
	},
	.probe		= deluge_ssi_probe,
	.remove	= deluge_ssi_remove,
};

module_platform_driver(deluge_ssi_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synthstrom Deluge RZ/A1 SSIF ASoC CPU DAI (adapted from rz-ssi)");
MODULE_AUTHOR("Biju Das <biju.das.jz@bp.renesas.com>");        /* original rz-ssi */
MODULE_AUTHOR("Katherine Whitlock <kate@skylinesynths.nyc>");
