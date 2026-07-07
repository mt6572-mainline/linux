// SPDX-License-Identifier: GPL-2.0
/*
 * MT6323 PMIC analog audio codec driver
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

#define MT6323_CODEC_RATES	SNDRV_PCM_RATE_8000_48000
#define MT6323_CODEC_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | \
				 SNDRV_PCM_FMTBIT_S24_LE | \
				 SNDRV_PCM_FMTBIT_S32_LE)

#define ABB_AFE_CON(n)		(0x4000 + (n) * 2)
#define AUDTOP_CON(n)		(0x0700 + (n) * 2)
#define SPK_CON(n)		(0x0052 + (n) * 2)
#define PMIC_RG_AUD_SPK_PDN	0x000e
#define AUDTOP_CON7_VBUF_GAIN	GENMASK(7, 4)	/* voice-buffer gain: 0x5=-11dB .. 0xb=+1dB */
#define SPK_VBUF_GAIN_START	0x6
#define SPK_VBUF_GAIN_END	0xb		/* 0x35b0, final +1dB */
#define ABB_AFE_UP8X_FIFO_CFG0	0x401e
#define ABB_AFE_PMIC_NEWIF_CFG0	0x4024
#define ABB_AFE_PMIC_NEWIF_CFG1	0x4026
#define ABB_AFE_PMIC_NEWIF_CFG2	0x4028
#define ABB_AFE_PMIC_NEWIF_CFG3	0x402a

/* MT6323 audio clocks (TOP_CKPDN). Atomic SET/CLR aliases. */
#define MT6323_TOP_CKPDN0_SET	0x0104
#define MT6323_TOP_CKPDN0_CLR	0x0106
#define PMIC_RG_CLKSQ_EN_AUD	BIT(0)

#define MT6323_TOP_CKPDN1_SET	0x010a
#define MT6323_TOP_CKPDN1_CLR	0x010c
#define PMIC_RG_AUD_26M_PDN	BIT(8)

#define ZCD_CON1		0x0802		/* lineout L/R gain */
#define ZCD_CON2		0x0804		/* headphone L/R gain */
#define ZCD_GAIN_0DB		8
#define ZCD_GAIN_N10DB		18
#define ZCD_GAIN_CTL_MAX	0x12		/* +8dB(0) .. 0dB(8) .. -10dB(18), -1dB/step */
#define ZCD_GAIN_REG(g)		(((g) << 7) | (g))	/* L at [4:0], R at [11:7] */

struct mt6323_codec_priv {
	struct device *dev;
	/* borrowed from the parent MT6323 MFD */
	struct regmap *regmap;
};

static const struct reg_sequence mt6323_codec_init[] = {
	{ ABB_AFE_CON(1),  0x0009 },
	{ ABB_AFE_CON(3),  0x0221 },
	{ ABB_AFE_CON(4),  0x0255 },
	{ ABB_AFE_CON(5),  0x0028 },
	{ ABB_AFE_CON(6),  0x0218 },
	{ ABB_AFE_CON(7),  0x0204 },
	{ ABB_AFE_CON(10), 0x0001 },
	{ AUDTOP_CON(0),   0x6010 },
	{ AUDTOP_CON(1),   0x0140 },
	{ AUDTOP_CON(2),   0x00c0 },
	{ AUDTOP_CON(3),   0x0200 },
	{ AUDTOP_CON(5),   0x0014 },
	{ AUDTOP_CON(6),   0x37e2 },
	{ AUDTOP_CON(8),   0x0200 },
	{ AUDTOP_CON(9),   0x0008 },
	{ ABB_AFE_UP8X_FIFO_CFG0,  0x0001 },
	{ ABB_AFE_PMIC_NEWIF_CFG0, 0x7330 },
	{ ABB_AFE_PMIC_NEWIF_CFG1, 0x0018 },
	{ ABB_AFE_PMIC_NEWIF_CFG2, 0x302f },
	{ ABB_AFE_PMIC_NEWIF_CFG3, 0xf872 },
	{ ZCD_CON2, ZCD_GAIN_REG(ZCD_GAIN_0DB) },
	{ ZCD_CON1, ZCD_GAIN_REG(ZCD_GAIN_N10DB) },
	{ SPK_CON(9), 0x0400 },
};

/* Audio clock supply (CLKSQ + 26 MHz), enabled while the stream is active. */
static int mt6323_clk_event(struct snd_soc_dapm_widget *w,
			    struct snd_kcontrol *kcontrol, int event)
{
	struct mt6323_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		regmap_write(priv->regmap, MT6323_TOP_CKPDN0_SET, PMIC_RG_CLKSQ_EN_AUD);
		regmap_write(priv->regmap, MT6323_TOP_CKPDN1_CLR, PMIC_RG_AUD_26M_PDN);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_write(priv->regmap, MT6323_TOP_CKPDN1_SET, PMIC_RG_AUD_26M_PDN);
		regmap_write(priv->regmap, MT6323_TOP_CKPDN0_CLR, PMIC_RG_CLKSQ_EN_AUD);
		break;
	}
	return 0;
}

/* AFE<->PMIC serial bridge (NEWIF) downlink enable. */
static int mt6323_newif_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	struct mt6323_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* enable the ABB AFE bridge (CON0) and its clock/format config (CON11) */
		regmap_write(priv->regmap, ABB_AFE_CON(0),  0x0001);
		regmap_write(priv->regmap, ABB_AFE_CON(11), 0x0303);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_write(priv->regmap, ABB_AFE_CON(11), 0x0000);
		regmap_write(priv->regmap, ABB_AFE_CON(0),  0x0000);
		break;
	}
	return 0;
}

static int mt6323_dac_event(struct snd_soc_dapm_widget *w,
			    struct snd_kcontrol *kcontrol, int event)
{
	struct mt6323_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* DAC output level */
		regmap_write(priv->regmap, AUDTOP_CON(5), 0x0014);
		/* DAC enable */
		regmap_write(priv->regmap, AUDTOP_CON(0), 0x7010);
		break;
	case SND_SOC_DAPM_POST_PMD:
		/* restore idle baseline */
		regmap_write(priv->regmap, AUDTOP_CON(0), 0x6010);
		regmap_write(priv->regmap, AUDTOP_CON(5), 0x0014);
		break;
	}
	return 0;
}

static int mt6323_hp_event(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *kcontrol, int event)
{
	struct mt6323_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* HP drv + depop */
		regmap_write(priv->regmap, AUDTOP_CON(6), 0xf5ba);
		/* HP enable */
		regmap_write(priv->regmap, AUDTOP_CON(4), 0x007c);
		break;
	case SND_SOC_DAPM_POST_PMD:
		/* HP off */
		regmap_write(priv->regmap, AUDTOP_CON(4), 0x0000);
		/* baseline */
		regmap_write(priv->regmap, AUDTOP_CON(6), 0x37e2);
		break;
	}
	return 0;
}

/*
 * Internal class-D speaker (SPK_CON), fed by the AUDTOP voice/LCH DAC; the
 * 1.35V CM buffer comes up with the shared "DAC" widget.
 */
static int mt6323_speaker_event(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kcontrol, int event)
{
	struct mt6323_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	int i;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* voice buffer, min gain */
		regmap_write(priv->regmap, AUDTOP_CON(7), 0x2400);
		/* HP input short, 2.4V, audio clk */
		regmap_write(priv->regmap, AUDTOP_CON(6), 0xb7f6);
		/* audio bias + LCH DAC */
		regmap_write(priv->regmap, AUDTOP_CON(4), 0x0014);
		/* bias/DAC settle */
		fsleep(10000);
		/* connect voice buffer -> SPK amp */
		regmap_write(priv->regmap, AUDTOP_CON(7), 0x3550);
		/* speaker clock on */
		regmap_write(priv->regmap, MT6323_TOP_CKPDN1_CLR, PMIC_RG_AUD_SPK_PDN);
		/* class-AB OC protection */
		regmap_write(priv->regmap, SPK_CON(2), 0x0214);
		/* enable amp, offset trim, class-D */
		regmap_write(priv->regmap, SPK_CON(0), 0x3008);
		regmap_write(priv->regmap, SPK_CON(0), 0x3009);
		/* amp power-up settle */
		fsleep(5000);
		/* class-D, amp enable */
		regmap_write(priv->regmap, SPK_CON(0), 0x3001);
		/* output stage enable */
		regmap_write(priv->regmap, SPK_CON(12), 0x0a00);
		/* ramp the voice-buffer gain up to its final level, 1ms/step, to avoid a turn-on pop */
		for (i = SPK_VBUF_GAIN_START; i <= SPK_VBUF_GAIN_END; i++) {
			fsleep(1000);
			regmap_write(priv->regmap, AUDTOP_CON(7),
				     0x3500 | FIELD_PREP(AUDTOP_CON7_VBUF_GAIN, i));
		}
		break;
	case SND_SOC_DAPM_POST_PMD:
		/* mute + disable class-D amp */
		regmap_write(priv->regmap, SPK_CON(0), 0x0004);
		/* output stage off */
		regmap_write(priv->regmap, SPK_CON(12), 0x0000);
		/* speaker clock off */
		regmap_write(priv->regmap, MT6323_TOP_CKPDN1_SET, PMIC_RG_AUD_SPK_PDN);
		/* voice buffer off */
		regmap_write(priv->regmap, AUDTOP_CON(7), 0x2400);
		/* LCH DAC off */
		regmap_write(priv->regmap, AUDTOP_CON(4), 0x0000);
		/* baseline */
		regmap_write(priv->regmap, AUDTOP_CON(6), 0x37e2);
		break;
	}
	return 0;
}

static const struct snd_soc_dapm_widget mt6323_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY("AUDCLK", SND_SOC_NOPM, 0, 0, mt6323_clk_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("NEWIF", SND_SOC_NOPM, 0, 0, mt6323_newif_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC_E("DAC", NULL, SND_SOC_NOPM, 0, 0, mt6323_dac_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUT_DRV_E("HP Driver", SND_SOC_NOPM, 0, 0, NULL, 0,
			       mt6323_hp_event,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("Headphone"),
	SND_SOC_DAPM_OUT_DRV_E("Speaker Driver", SND_SOC_NOPM, 0, 0, NULL, 0,
			       mt6323_speaker_event,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
};

static const struct snd_soc_dapm_route mt6323_dapm_routes[] = {
	{ "DAC", NULL, "AIF1 Playback" },
	{ "DAC", NULL, "AUDCLK" },
	{ "DAC", NULL, "NEWIF" },
	{ "HP Driver", NULL, "DAC" },
	{ "Headphone", NULL, "HP Driver" },
	{ "Speaker Driver", NULL, "DAC" },
};

/* Per-output mute switches. */
#define MT6323_PIN_SWITCH(xname) { \
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname " Switch", \
	.info = snd_soc_dapm_info_pin_switch, \
	.get = snd_soc_dapm_get_component_pin_switch, \
	.put = snd_soc_dapm_put_component_pin_switch, \
	.private_value = (unsigned long)xname }

/* Output volume: -10dB .. +8dB in 1dB steps (the inverted ZCD gain field). */
static const DECLARE_TLV_DB_SCALE(mt6323_dl_tlv, -1000, 100, 0);

/* Class-D speaker PGA gain: analog, scales after the DAC. */
static const DECLARE_TLV_DB_RANGE(mt6323_spk_tlv,
	0, 0, TLV_DB_SCALE_ITEM(TLV_DB_GAIN_MUTE, 0, 1),
	1, 1, TLV_DB_SCALE_ITEM(0, 0, 0),
	2, 15, TLV_DB_SCALE_ITEM(400, 100, 0));

static const struct snd_kcontrol_new mt6323_snd_controls[] = {
	MT6323_PIN_SWITCH("Headphone"),
	SOC_DOUBLE_TLV("Headphone Volume", ZCD_CON2, 0, 7, ZCD_GAIN_CTL_MAX, 1,
		       mt6323_dl_tlv),
	SOC_DOUBLE_TLV("Lineout Volume", ZCD_CON1, 0, 7, ZCD_GAIN_CTL_MAX, 1,
		       mt6323_dl_tlv),
	SOC_SINGLE_TLV("Speaker Volume", SPK_CON(9), 8, 0x0f, 0, mt6323_spk_tlv),
};

static int mt6323_component_probe(struct snd_soc_component *component)
{
	struct mt6323_codec_priv *priv = snd_soc_component_get_drvdata(component);

	/* init the component regmap so mixer control writes reach the PMIC */
	snd_soc_component_init_regmap(component, priv->regmap);
	return 0;
}

static const struct snd_soc_component_driver mt6323_soc_component_driver = {
	.probe			= mt6323_component_probe,
	.controls		= mt6323_snd_controls,
	.num_controls		= ARRAY_SIZE(mt6323_snd_controls),
	.dapm_widgets		= mt6323_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(mt6323_dapm_widgets),
	.dapm_routes		= mt6323_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(mt6323_dapm_routes),
	.endianness		= 1,
};

static struct snd_soc_dai_driver mt6323_dai_driver[] = {
	{
		.name = "mt6323-snd-codec-aif1",
		.playback = {
			.stream_name = "AIF1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MT6323_CODEC_RATES,
			.formats = MT6323_CODEC_FORMATS,
		},
	},
};

static int mt6323_codec_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6397 = dev_get_drvdata(pdev->dev.parent);
	struct mt6323_codec_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	priv->regmap = mt6397->regmap;
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	platform_set_drvdata(pdev, priv);

	/* Analog + NEWIF idle baseline; DAPM powers the output path per-stream. */
	ret = regmap_multi_reg_write(priv->regmap, mt6323_codec_init,
				     ARRAY_SIZE(mt6323_codec_init));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to init analog codec\n");

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &mt6323_soc_component_driver,
					      mt6323_dai_driver,
					      ARRAY_SIZE(mt6323_dai_driver));
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id mt6323_codec_of_match[] = {
	{ .compatible = "mediatek,mt6323-sound", },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6323_codec_of_match);

static struct platform_driver mt6323_codec_driver = {
	.driver = {
		.name = "mt6323-sound",
		.of_match_table = mt6323_codec_of_match,
	},
	.probe = mt6323_codec_probe,
};
module_platform_driver(mt6323_codec_driver);

MODULE_DESCRIPTION("MediaTek MT6323 PMIC audio codec driver");
MODULE_LICENSE("GPL");
