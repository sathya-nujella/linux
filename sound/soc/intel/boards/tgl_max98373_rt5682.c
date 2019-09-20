// SPDX-License-Identifier: GPL-2.0
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
//

/*
 * tgl_max98373_rt5682.c - ASoc Machine driver for Intel platforms
 * with RT1308 codec.
 */



#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/soc-acpi.h>
#include <linux/input.h>
#include <linux/delay.h>
#include "../../codecs/rt5682.h"
#include "../../codecs/hdac_hdmi.h"

#define MAX98373_CODEC_DAI	"max98373-aif1"
#define MAXIM_DEV0_NAME		"i2c-MX98373:00"
#define MAXIM_DEV1_NAME		"i2c-MX98373:01"

/* The platform clock outputs 19.2Mhz clock to codec as I2S MCLK */
#define TGL_PLAT_CLK_FREQ 19200000
#define RT5682_PLL_FREQ (48000 * 512)
#define TGL_REALTEK_CODEC_DAI "rt5682-aif1"

struct tgl_card_private {
	struct list_head hdmi_pcm_list;
        struct snd_soc_jack tgl_headset;
};

#if IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)
static struct snd_soc_jack tgl_hdmi[4];

struct tgl_hdmi_pcm {
	struct list_head head;
	struct snd_soc_dai *codec_dai;
	int device;
};

static int tgl_hdmi_init(struct snd_soc_pcm_runtime *rtd)
{
	struct tgl_card_private *ctx = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *dai = rtd->codec_dai;
	struct tgl_hdmi_pcm *pcm;

	pcm = devm_kzalloc(rtd->card->dev, sizeof(*pcm), GFP_KERNEL);
	if (!pcm)
		return -ENOMEM;

	/* dai_link id is 1:1 mapped to the PCM device */
	pcm->device = rtd->dai_link->id;
	pcm->codec_dai = dai;

	list_add_tail(&pcm->head, &ctx->hdmi_pcm_list);

	return 0;
}

#define NAME_SIZE	32
static int tgl_card_late_probe(struct snd_soc_card *card)
{
	struct tgl_card_private *ctx = snd_soc_card_get_drvdata(card);
	struct tgl_hdmi_pcm *pcm;
	struct snd_soc_component *component = NULL;
	int err, i = 0;
	char jack_name[NAME_SIZE];

	list_for_each_entry(pcm, &ctx->hdmi_pcm_list, head) {
		component = pcm->codec_dai->component;
		snprintf(jack_name, sizeof(jack_name),
			 "HDMI/DP, pcm=%d Jack", pcm->device);
		err = snd_soc_card_jack_new(card, jack_name,
					    SND_JACK_AVOUT, &tgl_hdmi[i],
					    NULL, 0);

		if (err)
			return err;

		err = hdac_hdmi_jack_init(pcm->codec_dai, pcm->device,
					  &tgl_hdmi[i]);
		if (err < 0)
			return err;

		i++;
	}

	if (!component)
		return -EINVAL;

	return hdac_hdmi_jack_port_init(component, &card->dapm);
}
#else
static int tgl_card_late_probe(struct snd_soc_card *card)
{
	return 0;
}
#endif

static int tgl_rt5682_init(struct snd_soc_pcm_runtime *rtd)
{
        struct tgl_card_private *ctx = snd_soc_card_get_drvdata(rtd->card);
        struct snd_soc_component *component = rtd->codec_dai->component;
        struct snd_soc_dai *codec_dai = rtd->codec_dai;
        struct snd_soc_jack *jack;
        int ret;
        ret = snd_soc_dai_set_pll(codec_dai, 0, RT5682_PLL1_S_MCLK,
                                        TGL_PLAT_CLK_FREQ, RT5682_PLL_FREQ);
        if (ret < 0) {
                dev_err(rtd->dev, "can't set codec pll: %d\n", ret);
                return ret;
        }

        /* Configure sysclk for codec */
        ret = snd_soc_dai_set_sysclk(codec_dai, RT5682_SCLK_S_PLL1,
                                        RT5682_PLL_FREQ, SND_SOC_CLOCK_IN);
        if (ret < 0)
                dev_err(rtd->dev, "snd_soc_dai_set_sysclk err = %d\n", ret);

        /*
         * Headset buttons map to the google Reference headset.
         * These can be configured by userspace.
         */
        ret = snd_soc_card_jack_new(rtd->card, "Headset Jack",
                        SND_JACK_HEADSET | SND_JACK_BTN_0 | SND_JACK_BTN_1 |
                        SND_JACK_BTN_2 | SND_JACK_BTN_3 | SND_JACK_LINEOUT,
                        &ctx->tgl_headset, NULL, 0);
        if (ret) {
                dev_err(rtd->dev, "Headset Jack creation failed: %d\n", ret);
                return ret;
        }

        jack = &ctx->tgl_headset;
        snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
        snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_VOICECOMMAND);
        snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
        snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOLUMEDOWN);
        ret = snd_soc_component_set_jack(component, jack, NULL);

        if (ret) {
                dev_err(rtd->dev, "Headset Jack call-back failed: %d\n", ret);
                return ret;
        }
        return ret;
};

static int tigerlake_rt5682_hw_params(struct snd_pcm_substream *substream,
        struct snd_pcm_hw_params *params)
{
        struct snd_soc_pcm_runtime *rtd = substream->private_data;
        struct snd_soc_dai *codec_dai = rtd->codec_dai;
        int ret;

        /* Set valid bitmask & configuration for I2S in 24 bit */
        ret = snd_soc_dai_set_tdm_slot(codec_dai, 0x0, 0x0, 2, 24);
        if (ret < 0) {
                dev_err(rtd->dev, "set TDM slot err:%d\n", ret);
                return ret;
        }

        return ret;
}

static struct snd_soc_ops tigerlake_rt5682_ops = {
        .hw_params = tigerlake_rt5682_hw_params,
};

static const struct snd_soc_dapm_widget tgl_max98373_rt5682_dapm_widgets[] = {
        SND_SOC_DAPM_HP("Headphone Jack", NULL),
        SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_SPK("Left Spk", NULL),
	SND_SOC_DAPM_SPK("Right Spk", NULL),
};

static const struct snd_kcontrol_new tgl_max98373_rt5682_controls[] = {
        SOC_DAPM_PIN_SWITCH("Headphone Jack"),
        SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Left Spk"),
	SOC_DAPM_PIN_SWITCH("Right Spk"),
};

static const struct snd_soc_dapm_route tgl_max98373_rt5682_dapm_routes[] = {
	/* speaker */
	{ "Left Spk", NULL, "Left BE_OUT" },
	{ "Right Spk", NULL, "Right BE_OUT" },

        /* HP jack connectors */
        {"Headphone Jack", NULL, "HPOL" },
        {"Headphone Jack", NULL, "HPOR" },
};

static int icl_ssp1_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *runtime = substream->private_data;
	int ret, j;

	for (j = 0; j < runtime->num_codecs; j++) {
		struct snd_soc_dai *codec_dai = runtime->codec_dais[j];

		/* Note: the speaker still work without ref capture FE */
		if (!strcmp(codec_dai->component->name, MAXIM_DEV0_NAME)) {
			/* tx_mask, rx_mask, slots, slot_width */
			ret = snd_soc_dai_set_tdm_slot(codec_dai, 0x30, 3, 8, 16);
			if (ret < 0) {
				dev_err(runtime->dev, "DEV0 TDM slot err:%d\n", ret);
				return ret;
			}
		}
		if (!strcmp(codec_dai->component->name, MAXIM_DEV1_NAME)) {
			ret = snd_soc_dai_set_tdm_slot(codec_dai, 0xC0, 3, 8, 16);
			if (ret < 0) {
				dev_err(runtime->dev, "DEV1 TDM slot err:%d\n", ret);
				return ret;
			}
		}
	}

	return 0;
}

static struct snd_soc_ops ssp1_ops = {
	.hw_params = icl_ssp1_hw_params,
};
static struct snd_soc_codec_conf max98373_codec_conf[] = {
	{
		.dev_name = MAXIM_DEV0_NAME,
		.name_prefix = "Right",
	},

	{
		.dev_name = MAXIM_DEV1_NAME,
		.name_prefix = "Left",
	},
};

static int tgl_max98373_rt5682_init(struct snd_soc_pcm_runtime *rtd)
{
	int ret = 0;

	return ret;
}

static struct snd_soc_dai_link_component platform_component[] = {
	{
		.name = "0000:00:1f.3"
	}
};

SND_SOC_DAILINK_DEF(ssp2_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("SSP2 Pin")));

SND_SOC_DAILINK_DEF(ssp2_codec,
	DAILINK_COMP_ARRAY(
       /* Left */      COMP_CODEC(MAXIM_DEV0_NAME, MAX98373_CODEC_DAI),
        /* Right */ COMP_CODEC(MAXIM_DEV1_NAME, MAX98373_CODEC_DAI)));

SND_SOC_DAILINK_DEF(ssp0_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("SSP0 Pin")));

SND_SOC_DAILINK_DEF(ssp0_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("i2c-10EC5682:00", "rt5682-aif1")));

SND_SOC_DAILINK_DEF(platform,
	DAILINK_COMP_ARRAY(COMP_PLATFORM("0000:00:1f.3")));

SND_SOC_DAILINK_DEF(dmic_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("DMIC01 Pin")));
SND_SOC_DAILINK_DEF(dmic_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("dmic-codec", "dmic-hifi")));

SND_SOC_DAILINK_DEF(idisp1_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("iDisp1 Pin")));
SND_SOC_DAILINK_DEF(idisp1_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("ehdaudio0D2", "intel-hdmi-hifi1")));

SND_SOC_DAILINK_DEF(idisp2_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("iDisp2 Pin")));
SND_SOC_DAILINK_DEF(idisp2_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("ehdaudio0D2", "intel-hdmi-hifi2")));

SND_SOC_DAILINK_DEF(idisp3_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("iDisp3 Pin")));
SND_SOC_DAILINK_DEF(idisp3_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("ehdaudio0D2", "intel-hdmi-hifi3")));

SND_SOC_DAILINK_DEF(idisp4_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("iDisp4 Pin")));
SND_SOC_DAILINK_DEF(idisp4_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("ehdaudio0D2", "intel-hdmi-hifi4")));

static struct snd_soc_dai_link tgl_max98373_rt5682_dailink[] = {
	{
		.name		= "SSP2-Codec",
		.id 		= 0,
		.no_pcm		= 1,
		.ops = &ssp1_ops,
		.dai_fmt = SND_SOC_DAIFMT_DSP_B |
			SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(ssp2_pin, ssp2_codec, platform),
	},
        {
                .name           = "SSP0-Codec",
                .id             = 1,
                .no_pcm         = 1,
                .init           = tgl_rt5682_init,
                .dai_fmt        = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
                                  SND_SOC_DAIFMT_CBS_CFS,
                .ops=&tigerlake_rt5682_ops,
                .dpcm_playback = 1,
                .dpcm_capture = 1,
		SND_SOC_DAILINK_REG(ssp0_pin, ssp0_codec, platform),
        },
	{
		.name = "dmic01",
		.id = 2,
		.ignore_suspend = 1,
		.dpcm_capture = 1,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(dmic_pin, dmic_codec, platform),
	},
#if IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)
	{
		.name = "iDisp1",
		.id = 3,
		.init = tgl_hdmi_init,
		.dpcm_playback = 1,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(idisp1_pin, idisp1_codec, platform),
	},
	{
		.name = "iDisp2",
		.id = 4,
		.init = tgl_hdmi_init,
		.dpcm_playback = 1,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(idisp2_pin, idisp2_codec, platform),
	},
	{
		.name = "iDisp3",
		.id = 5,
		.init = tgl_hdmi_init,
		.dpcm_playback = 1,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(idisp3_pin, idisp3_codec, platform),
	},
	{
		.name = "iDisp4",
		.id = 6,
		.init = tgl_hdmi_init,
		.dpcm_playback = 1,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(idisp4_pin, idisp4_codec, platform),
	},
#endif
};

/* audio machine driver */
static struct snd_soc_card tgl_max98373_rt5682_card = {
	.name         = "tgl_max98373_rt5682",
	.owner        = THIS_MODULE,
	.dai_link     = tgl_max98373_rt5682_dailink,
	.num_links = ARRAY_SIZE(tgl_max98373_rt5682_dailink),
	.controls = tgl_max98373_rt5682_controls,
	.num_controls = ARRAY_SIZE(tgl_max98373_rt5682_controls),
	.dapm_widgets = tgl_max98373_rt5682_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tgl_max98373_rt5682_dapm_widgets),
	.dapm_routes = tgl_max98373_rt5682_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(tgl_max98373_rt5682_dapm_routes),
	.codec_conf = max98373_codec_conf,
	.num_configs = ARRAY_SIZE(max98373_codec_conf),
	.late_probe = tgl_card_late_probe,
	//.fully_routed = true,
};

static int tgl_max98373_rt5682_probe(struct platform_device *pdev)
{
	struct snd_soc_acpi_mach *mach;
	struct tgl_card_private *ctx;
	struct snd_soc_card *card;
	int ret = 0;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);

        if (!ctx)
                return -ENOMEM;

	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI))
		INIT_LIST_HEAD(&ctx->hdmi_pcm_list);
        card=&tgl_max98373_rt5682_card;
        card->dev = &pdev->dev;
        snd_soc_card_set_drvdata(card, ctx);

	mach = (&pdev->dev)->platform_data;
	ret = snd_soc_fixup_dai_links_platform_name(card, mach->mach_params.platform);
	if (ret)
		return ret;

	return devm_snd_soc_register_card(&pdev->dev,card);
}

static int tgl_max98373_rt5682_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&tgl_max98373_rt5682_card);
}

static struct platform_driver tgl_max98373_rt5682_driver = {
	.driver = {
		.name   = "tgl_max98373_rt5682",
		.owner  = THIS_MODULE,
	},
	.probe          = tgl_max98373_rt5682_probe,
	.remove         = tgl_max98373_rt5682_remove,
};

module_platform_driver(tgl_max98373_rt5682_driver);

MODULE_AUTHOR("Sathyanarayana Nujella");
MODULE_AUTHOR("Jairaj Arava");
MODULE_AUTHOR("Naveen Manohar");
MODULE_DESCRIPTION("ASoC Intel(R) Tigerlak + Max98373 and ALC5682 Machine driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:tgl_max98373_rt5682");
