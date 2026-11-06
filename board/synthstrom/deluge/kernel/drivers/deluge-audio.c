// SPDX-License-Identifier: GPL-2.0
/*
 * Synthstrom Deluge audio machine (sound card): SSI0 <-> dummy codec, I2S
 * 44.1 kHz S24_LE stereo, SSI as master. The codec is hardware-strapped with no
 * control bus; this driver owns the codec power-up sequence from
 * deluge-bsp/audio.rs: enable the audio master clock, wait 5 ms for it to
 * stabilise, then raise CODEC_POWER (P6.12). The mclk stays enabled for the
 * card's lifetime (the codec's system clock must never stop).
 *
 * Speaker amplifier (P4.1, high = on): gated exactly as the SDK's jack policy
 * (deluge-bsp/jacks.rs) — "enable only when no headphone or line-out is
 * inserted" — combined with a "Speaker Playback Switch" mixer control for
 * manual override. The jack-detect pins are plain GPIO inputs with no interrupt
 * capability on RZ/A1, so (as the SDK does) they are polled.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <sound/soc.h>

#define DELUGE_MCLK_DELAY_MS	5
#define DELUGE_JACK_POLL_MS	500

struct deluge_audio {
	struct snd_soc_card card;
	struct snd_soc_dai_link link;
	struct snd_soc_dai_link_component cpu, platform;
	struct snd_soc_aux_dev midi_aux;	/* DIN MIDI (deluge-midi) component */
	struct clk *mclk;
	struct gpio_desc *codec_power;
	struct gpio_desc *speaker_enable;
	struct gpio_desc *headphone_det;	/* high = headphone inserted */
	struct gpio_descs *lineout_det;		/* high = line-out inserted (L/R) */
	/* Input jacks: read-only, reported to userspace but NOT part of the
	 * speaker policy — plugging a mic in must not mute the speaker. */
	struct gpio_desc *linein_det;		/* high = line-in inserted */
	struct gpio_desc *mic_det;		/* high = mic inserted */
	struct delayed_work jack_work;
	struct mutex lock;			/* serialises speaker state */
	bool speaker_switch;			/* user control (default on) */
};

/* Any output jack inserted mutes the on-board speaker (SDK jack policy). */
static bool deluge_jack_inserted(struct deluge_audio *priv)
{
	unsigned int i;

	if (priv->headphone_det && gpiod_get_value_cansleep(priv->headphone_det))
		return true;

	if (priv->lineout_det) {
		for (i = 0; i < priv->lineout_det->ndescs; i++)
			if (gpiod_get_value_cansleep(priv->lineout_det->desc[i]))
				return true;
	}
	return false;
}

/* Speaker on = user switch AND no output jack inserted. */
static void deluge_speaker_update(struct deluge_audio *priv)
{
	bool on;

	mutex_lock(&priv->lock);
	on = priv->speaker_switch && !deluge_jack_inserted(priv);
	gpiod_set_value_cansleep(priv->speaker_enable, on);
	mutex_unlock(&priv->lock);
}

static void deluge_jack_poll(struct work_struct *work)
{
	struct deluge_audio *priv =
		container_of(work, struct deluge_audio, jack_work.work);

	deluge_speaker_update(priv);
	schedule_delayed_work(&priv->jack_work,
			      msecs_to_jiffies(DELUGE_JACK_POLL_MS));
}

static int deluge_spk_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct deluge_audio *priv = snd_soc_card_get_drvdata(card);

	ucontrol->value.integer.value[0] = priv->speaker_switch;
	return 0;
}

static int deluge_spk_put(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct deluge_audio *priv = snd_soc_card_get_drvdata(card);
	bool val = !!ucontrol->value.integer.value[0];

	if (val == priv->speaker_switch)
		return 0;

	priv->speaker_switch = val;
	deluge_speaker_update(priv);
	return 1;
}

/* Read-only jack-detect controls.
 *
 * Exported as kcontrols rather than sysfs so the whole audio-jack domain lives
 * behind one mechanism (this card already owns "Speaker Playback Switch"), and
 * so userspace can read them with plain ALSA rather than a private interface.
 *
 * `private_value` selects which jack; the GPIOs are plain inputs with no
 * interrupt, so these read the pin on demand. Absent GPIO reads as 0 (not
 * inserted) rather than failing — a kernel without the optional pin declared
 * should not make the control unreadable.
 */
enum deluge_jack_id {
	DELUGE_JACK_HEADPHONE,
	DELUGE_JACK_LINEOUT_L,
	DELUGE_JACK_LINEOUT_R,
	DELUGE_JACK_LINEIN,
	DELUGE_JACK_MIC,
};

static int deluge_jack_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct deluge_audio *priv = snd_soc_card_get_drvdata(card);
	struct gpio_desc *d = NULL;

	switch (kcontrol->private_value) {
	case DELUGE_JACK_HEADPHONE:
		d = priv->headphone_det;
		break;
	case DELUGE_JACK_LINEOUT_L:
		if (priv->lineout_det && priv->lineout_det->ndescs > 0)
			d = priv->lineout_det->desc[0];
		break;
	case DELUGE_JACK_LINEOUT_R:
		if (priv->lineout_det && priv->lineout_det->ndescs > 1)
			d = priv->lineout_det->desc[1];
		break;
	case DELUGE_JACK_LINEIN:
		d = priv->linein_det;
		break;
	case DELUGE_JACK_MIC:
		d = priv->mic_det;
		break;
	}

	ucontrol->value.integer.value[0] =
		d ? !!gpiod_get_value_cansleep(d) : 0;
	return 0;
}

#define DELUGE_JACK_CTL(xname, xid) {					\
	.iface = SNDRV_CTL_ELEM_IFACE_CARD,				\
	.name = xname,							\
	.access = SNDRV_CTL_ELEM_ACCESS_READ |				\
		  SNDRV_CTL_ELEM_ACCESS_VOLATILE,			\
	.info = snd_ctl_boolean_mono_info,				\
	.get = deluge_jack_get,						\
	.private_value = (xid),						\
}

static const struct snd_kcontrol_new deluge_controls[] = {
	SOC_SINGLE_BOOL_EXT("Speaker Playback Switch", 0,
			    deluge_spk_get, deluge_spk_put),
	DELUGE_JACK_CTL("Headphone Jack", DELUGE_JACK_HEADPHONE),
	DELUGE_JACK_CTL("Line Out L Jack", DELUGE_JACK_LINEOUT_L),
	DELUGE_JACK_CTL("Line Out R Jack", DELUGE_JACK_LINEOUT_R),
	DELUGE_JACK_CTL("Line In Jack", DELUGE_JACK_LINEIN),
	DELUGE_JACK_CTL("Mic Jack", DELUGE_JACK_MIC),
};

/* Card lifetime teardown, registered before the card so devm LIFO runs it
 * AFTER the card is unregistered (mclk must outlive the card). */
static void deluge_audio_powerdown(void *data)
{
	struct deluge_audio *priv = data;

	cancel_delayed_work_sync(&priv->jack_work);
	gpiod_set_value_cansleep(priv->speaker_enable, 0);
	gpiod_set_value_cansleep(priv->codec_power, 0);
	clk_disable_unprepare(priv->mclk);
}

static int deluge_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct deluge_audio *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	priv->speaker_switch = true;
	INIT_DELAYED_WORK(&priv->jack_work, deluge_jack_poll);

	priv->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(priv->mclk))
		return dev_err_probe(dev, PTR_ERR(priv->mclk), "no mclk\n");

	priv->codec_power = devm_gpiod_get(dev, "codec-power", GPIOD_OUT_LOW);
	if (IS_ERR(priv->codec_power))
		return dev_err_probe(dev, PTR_ERR(priv->codec_power),
				     "no codec-power gpio\n");

	/* Speaker amplifier enable (P4.1, high = on). Optional. Starts low. */
	priv->speaker_enable = devm_gpiod_get_optional(dev, "speaker-enable",
						       GPIOD_OUT_LOW);
	if (IS_ERR(priv->speaker_enable))
		return dev_err_probe(dev, PTR_ERR(priv->speaker_enable),
				     "bad speaker-enable gpio\n");

	/* Jack-detect inputs (high = inserted); both optional. */
	priv->headphone_det = devm_gpiod_get_optional(dev, "headphone-det",
						      GPIOD_IN);
	if (IS_ERR(priv->headphone_det))
		return dev_err_probe(dev, PTR_ERR(priv->headphone_det),
				     "bad headphone-det gpio\n");

	priv->lineout_det = devm_gpiod_get_array_optional(dev, "lineout-det",
							  GPIOD_IN);
	if (IS_ERR(priv->lineout_det))
		return dev_err_probe(dev, PTR_ERR(priv->lineout_det),
				     "bad lineout-det gpios\n");

	/* Input-jack detects: reported to userspace only, never consulted by
	 * deluge_jack_inserted() — a mic or line-in must not mute the speaker. */
	priv->linein_det = devm_gpiod_get_optional(dev, "linein-det", GPIOD_IN);
	if (IS_ERR(priv->linein_det))
		return dev_err_probe(dev, PTR_ERR(priv->linein_det),
				     "bad linein-det gpio\n");

	priv->mic_det = devm_gpiod_get_optional(dev, "mic-det", GPIOD_IN);
	if (IS_ERR(priv->mic_det))
		return dev_err_probe(dev, PTR_ERR(priv->mic_det),
				     "bad mic-det gpio\n");

	/* Standard sound-dai resolution: fills priv->cpu.of_node + dai_name.
	 * Returns -EPROBE_DEFER until the deluge-ssi component registers. */
	ret = snd_soc_of_get_dlc(dev->of_node, NULL, &priv->cpu, 0);
	if (ret)
		return dev_err_probe(dev, ret, "sound-dai\n");
	priv->platform.of_node = priv->cpu.of_node;

	priv->link.name = "Deluge SSI";
	priv->link.stream_name = "Deluge SSI";
	priv->link.cpus = &priv->cpu;
	priv->link.num_cpus = 1;
	priv->link.codecs = &snd_soc_dummy_dlc;
	priv->link.num_codecs = 1;
	priv->link.platforms = &priv->platform;
	priv->link.num_platforms = 1;
	priv->link.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			     SND_SOC_DAIFMT_CBC_CFC;

	priv->card.name = "Deluge";
	priv->card.owner = THIS_MODULE;
	priv->card.dev = dev;
	priv->card.dai_link = &priv->link;
	priv->card.num_links = 1;
	priv->card.controls = deluge_controls;
	priv->card.num_controls = ARRAY_SIZE(deluge_controls);

	/* Optional DIN MIDI (deluge-midi on SCIF0) as an aux component: its rawmidi
	 * lands on this card's snd_card. ASoC defers (-EPROBE_DEFER) until the
	 * component registers. */
	{
		struct device_node *midi_np;

		midi_np = of_parse_phandle(dev->of_node, "synthstrom,midi", 0);
		if (midi_np) {
			priv->midi_aux.dlc.of_node = midi_np;
			priv->card.aux_dev = &priv->midi_aux;
			priv->card.num_aux_devs = 1;
			of_node_put(midi_np);
		}
	}

	snd_soc_card_set_drvdata(&priv->card, priv);

	/* Codec power-up sequence (deluge-bsp/audio.rs): mclk on, 5 ms, power. */
	ret = clk_prepare_enable(priv->mclk);
	if (ret)
		return dev_err_probe(dev, ret, "mclk enable\n");
	msleep(DELUGE_MCLK_DELAY_MS);
	gpiod_set_value_cansleep(priv->codec_power, 1);

	/* Registered before the card: devm LIFO powers the codec down AFTER the
	 * card is torn down. On failure here, devm runs it immediately. */
	ret = devm_add_action_or_reset(dev, deluge_audio_powerdown, priv);
	if (ret)
		return ret;

	ret = devm_snd_soc_register_card(dev, &priv->card);
	if (ret)
		return dev_err_probe(dev, ret, "register card\n");

	/* Set the initial speaker state and start polling the jacks. */
	deluge_speaker_update(priv);
	schedule_delayed_work(&priv->jack_work,
			      msecs_to_jiffies(DELUGE_JACK_POLL_MS));

	return 0;
}

static const struct of_device_id deluge_audio_of[] = {
	{ .compatible = "synthstrom,deluge-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, deluge_audio_of);

static struct platform_driver deluge_audio_driver = {
	.probe = deluge_audio_probe,
	.driver = {
		.name = "deluge-audio",
		.of_match_table = deluge_audio_of,
	},
};
module_platform_driver(deluge_audio_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synthstrom Deluge audio machine (SSI0 + dummy codec)");
