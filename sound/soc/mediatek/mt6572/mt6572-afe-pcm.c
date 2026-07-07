// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6572 AFE platform driver
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define AUDIO_TOP_CON0		0x0000
#define AUDIO_TOP_CON0_AFE_ON	0x60004000

#define AFE_DAC_CON0		0x0010
#define AFE_DAC_CON0_AFE_ON	BIT(0)
#define AFE_DAC_CON0_DL1_ON	BIT(1)
#define AFE_DAC_CON0_DL1_OUT	BIT(10)

#define AFE_DAC_CON1		0x0014
#define AFE_DAC_CON1_DL1_RATE	GENMASK(3, 0)

#define AFE_DL1_BASE		0x0040
#define AFE_DL1_CUR		0x0044
/* ring end, inclusive */
#define AFE_DL1_END		0x0048

#define AFE_MEMIF_MAXLEN	0x03d4
#define AFE_MEMIF_MAXLEN_DL1	GENMASK(3, 0)

#define AFE_MEMIF_PBUF_SIZE	0x03d8
#define AFE_MEMIF_PBUF_SIZE_DL1	GENMASK(17, 16)

#define AFE_IRQ_MCU_CON		0x03a0
#define AFE_IRQ_MCU_CON_IRQ1_ON		BIT(0)
#define AFE_IRQ_MCU_CON_IRQ1_RATE	GENMASK(7, 4)

#define AFE_IRQ_MCU_STATUS	0x03a4
#define AFE_IRQ_MCU_STATUS_IRQ1	BIT(0)
#define AFE_IRQ_MCU_STATUS_MASK	GENMASK(3, 0)

#define AFE_IRQ_MCU_CLR		0x03a8
#define AFE_IRQ_MCU_CLR_NOSTATUS (BIT(6) | BIT(1) | BIT(0))

#define AFE_IRQ_MCU_CNT1	0x03ac

/* DL1 -> interconnect -> ADDA downlink SRC -> AFE<->PMIC link. */
#define AFE_I2S_CON1		0x0034
#define AFE_I2S_CON1_DAC_FORMAT	0x00000008
#define AFE_I2S_CON1_RATE	GENMASK(11, 8)
#define AFE_I2S_CON1_ON		BIT(0)

#define AFE_CONN1		0x0024
#define AFE_CONN1_DL1_O3	BIT(21)		/* DL1 ch1 -> interconnect out O3 */

#define AFE_CONN2		0x0028
#define AFE_CONN2_DL1_O4	BIT(6)		/* DL1 ch2 -> interconnect out O4 */

#define AFE_ADDA_DL_SRC2_CON0	0x0108
#define AFE_ADDA_DL_SRC2_CON0_BASE 0x03001802	/* SRC-disabled base */
#define AFE_ADDA_DL_SRC2_CON0_RATE GENMASK(31, 28)
#define AFE_ADDA_DL_SRC2_CON0_ON   BIT(0)

#define AFE_ADDA_DL_SRC2_CON1	0x010c
#define AFE_ADDA_DL_SRC2_CON1_GAIN GENMASK(31, 16)
#define AFE_DL_GAIN_DEFAULT	0x203b		/* ~-18dB */

#define AFE_ADDA_UL_DL_CON0	0x0124
#define AFE_ADDA_UL_DL_CON0_ON	BIT(0)

#define AFE_ADDA_PREDIS_CON0	0x0260		/* ADDA downlink pre-distortion */
#define AFE_ADDA_PREDIS_CON1	0x0264

#define AFE_ADDA_NEWIF_CFG0	0x0138		/* AFE<->PMIC serial link (NEWIF) */
#define AFE_ADDA_NEWIF_CFG0_VAL	0x03f87200

#define AFE_ADDA_NEWIF_CFG1	0x013c
#define AFE_ADDA_NEWIF_CFG1_VAL	0x03117180

static const struct regmap_config mt6572_afe_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.fast_io = true,
	.max_register = 0x0ffc,
};

struct mt6572_afe {
	struct device *dev;
	struct regmap *regmap;
	struct clk *clk;
	struct snd_pcm_substream *dl1_substream;	/* active DL1 stream */
	unsigned int dl_gain;				/* "Playback Volume" */
};

static int mt6572_afe_rate_code(unsigned int rate)
{
	switch (rate) {
	case 8000:	return 0;
	case 11025:	return 1;
	case 12000:	return 2;
	case 16000:	return 4;
	case 22050:	return 5;
	case 24000:	return 6;
	case 32000:	return 8;
	case 44100:	return 9;
	case 48000:	return 10;
	default:	return -EINVAL;
	}
}

static int mt6572_afe_adda_rate_code(unsigned int rate)
{
	switch (rate) {
	case 8000:	return 0;
	case 11025:	return 1;
	case 12000:	return 2;
	case 16000:	return 3;
	case 22050:	return 4;
	case 24000:	return 5;
	case 32000:	return 6;
	case 44100:	return 7;
	case 48000:	return 8;
	default:	return -EINVAL;
	}
}

static struct snd_soc_dai_driver mt6572_afe_dais[] = {
	{
		.name = "mt6572-afe-dl1",
		.playback = {
			.stream_name = "DL1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
	},
};

static const struct snd_pcm_hardware mt6572_afe_hardware = {
	/* on-chip SRAM buffer, no mmap */
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_8000_48000,
	.rate_min = 8000,
	.rate_max = 48000,
	.channels_min = 1,
	.channels_max = 2,
	.period_bytes_min = 1024,
	.period_bytes_max = 8192,
	.periods_min = 2,
	.periods_max = 16,
	.buffer_bytes_max = 16 * 1024,
};

static int mt6572_afe_pcm_open(struct snd_soc_component *comp,
			       struct snd_pcm_substream *substream)
{
	snd_soc_set_runtime_hwparams(substream, &mt6572_afe_hardware);
	/* AFE_DL1_END[2:0] must be 7: keep the period (so the buffer) 8-byte aligned. */
	return snd_pcm_hw_constraint_step(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 8);
}

static int mt6572_afe_pcm_hw_params(struct snd_soc_component *comp,
				    struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params)
{
	struct mt6572_afe *afe = snd_soc_component_get_drvdata(comp);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int bytes = params_buffer_bytes(params);
	u32 base = lower_32_bits(runtime->dma_addr);

	/* program the DL1 memif DMA ring (in the AFE on-chip SRAM) */
	regmap_write(afe->regmap, AFE_DL1_BASE, base);
	regmap_write(afe->regmap, AFE_DL1_END, base + bytes - 1);
	regmap_clear_bits(afe->regmap, AFE_MEMIF_MAXLEN, AFE_MEMIF_MAXLEN_DL1);
	regmap_clear_bits(afe->regmap, AFE_MEMIF_PBUF_SIZE, AFE_MEMIF_PBUF_SIZE_DL1);
	return 0;
}

static int mt6572_afe_pcm_prepare(struct snd_soc_component *comp,
				  struct snd_pcm_substream *substream)
{
	struct mt6572_afe *afe = snd_soc_component_get_drvdata(comp);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int adda_code = mt6572_afe_adda_rate_code(runtime->rate);
	int rate_code = mt6572_afe_rate_code(runtime->rate);

	if (adda_code < 0 || rate_code < 0)
		return -EINVAL;

	/* IRQ1 rate + per-period frame count (enabled in the trigger) */
	regmap_update_bits(afe->regmap, AFE_IRQ_MCU_CON, AFE_IRQ_MCU_CON_IRQ1_RATE,
			   FIELD_PREP(AFE_IRQ_MCU_CON_IRQ1_RATE, rate_code));
	regmap_write(afe->regmap, AFE_IRQ_MCU_CNT1, runtime->period_size);

	/* interconnect: DL1 ch1/ch2 -> O3/O4 */
	regmap_set_bits(afe->regmap, AFE_CONN1, AFE_CONN1_DL1_O3);
	regmap_set_bits(afe->regmap, AFE_CONN2, AFE_CONN2_DL1_O4);

	regmap_set_bits(afe->regmap, AFE_DAC_CON0, AFE_DAC_CON0_DL1_OUT);
	regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON0, 0);
	regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON1, 0);

	/* enable ADDA downlink SRC + I2S */
	regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON0,
		     AFE_ADDA_DL_SRC2_CON0_BASE |
		     FIELD_PREP(AFE_ADDA_DL_SRC2_CON0_RATE, adda_code) |
		     AFE_ADDA_DL_SRC2_CON0_ON);
	regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON1,
		     FIELD_PREP(AFE_ADDA_DL_SRC2_CON1_GAIN, afe->dl_gain));
	regmap_write(afe->regmap, AFE_I2S_CON1,
		     AFE_I2S_CON1_DAC_FORMAT | FIELD_PREP(AFE_I2S_CON1_RATE, rate_code));
	regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON0,
		     AFE_ADDA_DL_SRC2_CON0_BASE |
		     FIELD_PREP(AFE_ADDA_DL_SRC2_CON0_RATE, adda_code) |
		     AFE_ADDA_DL_SRC2_CON0_ON);
	regmap_set_bits(afe->regmap, AFE_I2S_CON1, AFE_I2S_CON1_ON);
	regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON0,
		     AFE_ADDA_DL_SRC2_CON0_BASE |
		     FIELD_PREP(AFE_ADDA_DL_SRC2_CON0_RATE, adda_code) |
		     AFE_ADDA_DL_SRC2_CON0_ON);
	regmap_set_bits(afe->regmap, AFE_ADDA_UL_DL_CON0, AFE_ADDA_UL_DL_CON0_ON);

	regmap_set_bits(afe->regmap, AFE_DAC_CON0, AFE_DAC_CON0_AFE_ON);
	regmap_update_bits(afe->regmap, AFE_DAC_CON1, AFE_DAC_CON1_DL1_RATE,
			   FIELD_PREP(AFE_DAC_CON1_DL1_RATE, rate_code));

	return 0;
}

static int mt6572_afe_pcm_trigger(struct snd_soc_component *comp,
				  struct snd_pcm_substream *substream, int cmd)
{
	struct mt6572_afe *afe = snd_soc_component_get_drvdata(comp);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		afe->dl1_substream = substream;
		/* atomic: memif start + period IRQ only (HW-IRQ driven) */
		regmap_set_bits(afe->regmap, AFE_IRQ_MCU_CON, AFE_IRQ_MCU_CON_IRQ1_ON);
		regmap_set_bits(afe->regmap, AFE_DAC_CON0, AFE_DAC_CON0_DL1_ON);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		/* minimal stop: DL1 memif + period IRQ only; .prepare re-asserts the rest */
		regmap_clear_bits(afe->regmap, AFE_IRQ_MCU_CON, AFE_IRQ_MCU_CON_IRQ1_ON);
		regmap_clear_bits(afe->regmap, AFE_DAC_CON0, AFE_DAC_CON0_DL1_ON);
		afe->dl1_substream = NULL;
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t mt6572_afe_pcm_pointer(struct snd_soc_component *comp,
						struct snd_pcm_substream *substream)
{
	struct mt6572_afe *afe = snd_soc_component_get_drvdata(comp);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u32 base = lower_32_bits(runtime->dma_addr);
	unsigned int cur = 0;

	regmap_read(afe->regmap, AFE_DL1_CUR, &cur);
	if (cur < base || cur >= base + runtime->dma_bytes)
		return 0;
	return bytes_to_frames(runtime, cur - base);
}

static int mt6572_afe_pcm_construct(struct snd_soc_component *comp,
				    struct snd_soc_pcm_runtime *rtd)
{
	size_t size = mt6572_afe_hardware.buffer_bytes_max;

	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV_IRAM, comp->dev,
				       size, size);
	return 0;
}

static const DECLARE_TLV_DB_LINEAR(dl_gain_tlv, TLV_DB_GAIN_MUTE, 0);

static int mt6572_dl_gain_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_kcontrol_chip(kcontrol);
	struct mt6572_afe *afe = snd_soc_component_get_drvdata(comp);

	ucontrol->value.integer.value[0] = afe->dl_gain;
	return 0;
}

static int mt6572_dl_gain_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_kcontrol_chip(kcontrol);
	struct mt6572_afe *afe = snd_soc_component_get_drvdata(comp);
	unsigned int gain = ucontrol->value.integer.value[0];

	if (gain > 0xffff)
		return -EINVAL;
	if (gain == afe->dl_gain)
		return 0;

	afe->dl_gain = gain;
	regmap_update_bits(afe->regmap, AFE_ADDA_DL_SRC2_CON1,
			   AFE_ADDA_DL_SRC2_CON1_GAIN,
			   FIELD_PREP(AFE_ADDA_DL_SRC2_CON1_GAIN, gain));
	return 1;
}

static const struct snd_kcontrol_new mt6572_afe_controls[] = {
	SOC_SINGLE_EXT_TLV("Playback Volume", SND_SOC_NOPM, 0, 0xffff, 0,
			   mt6572_dl_gain_get, mt6572_dl_gain_put, dl_gain_tlv),
};

static const struct snd_soc_component_driver mt6572_afe_component = {
	.name = "mt6572-afe-pcm",
	.controls = mt6572_afe_controls,
	.num_controls = ARRAY_SIZE(mt6572_afe_controls),
	.open = mt6572_afe_pcm_open,
	.hw_params = mt6572_afe_pcm_hw_params,
	.prepare = mt6572_afe_pcm_prepare,
	.trigger = mt6572_afe_pcm_trigger,
	.pointer = mt6572_afe_pcm_pointer,
	.pcm_construct = mt6572_afe_pcm_construct,
};

/* IRQ1 marks a DL1 period; hardirq, atomic PCM (fast_io regmap). */
static irqreturn_t mt6572_afe_irq(int irq, void *dev_id)
{
	struct mt6572_afe *afe = dev_id;
	unsigned int status;

	regmap_read(afe->regmap, AFE_IRQ_MCU_STATUS, &status);
	status &= AFE_IRQ_MCU_STATUS_MASK;
	if (!status) {
		/* triggered with no status set: write the clear-mask (bit 6 clears all) to ack */
		regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, AFE_IRQ_MCU_CLR_NOSTATUS);
		return IRQ_HANDLED;
	}

	if ((status & AFE_IRQ_MCU_STATUS_IRQ1) && afe->dl1_substream)
		snd_pcm_period_elapsed(afe->dl1_substream);

	regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, status);
	return IRQ_HANDLED;
}

static int mt6572_afe_pcm_dev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6572_afe *afe;
	struct resource res;
	void __iomem *base;
	int ret, irq;

	afe = devm_kzalloc(dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;
	afe->dev = dev;
	afe->dl_gain = AFE_DL_GAIN_DEFAULT;
	platform_set_drvdata(pdev, afe);

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set DMA mask\n");

	afe->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(afe->clk))
		return dev_err_probe(dev, PTR_ERR(afe->clk),
				     "failed to get/enable the audio clock\n");

	/* The AFE registers are in the parent audsys syscon window. */
	ret = of_address_to_resource(dev->parent->of_node, 0, &res);
	if (ret)
		return dev_err_probe(dev, ret, "no AFE reg in parent syscon\n");
	base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!base)
		return dev_err_probe(dev, -ENOMEM, "failed to map AFE registers\n");
	afe->regmap = devm_regmap_init_mmio(dev, base, &mt6572_afe_regmap_config);
	if (IS_ERR(afe->regmap))
		return dev_err_probe(dev, PTR_ERR(afe->regmap),
				     "failed to init AFE regmap\n");

	/* power on the AFE top + the SoC side of the AFE<->PMIC link */
	regmap_write(afe->regmap, AUDIO_TOP_CON0, AUDIO_TOP_CON0_AFE_ON);
	regmap_write(afe->regmap, AFE_ADDA_NEWIF_CFG0, AFE_ADDA_NEWIF_CFG0_VAL);
	regmap_write(afe->regmap, AFE_ADDA_NEWIF_CFG1, AFE_ADDA_NEWIF_CFG1_VAL);

	/* mask all AFE IRQs + clear stale status before hooking the GIC */
	regmap_write(afe->regmap, AFE_IRQ_MCU_CON, 0);
	regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, AFE_IRQ_MCU_STATUS_MASK);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, mt6572_afe_irq, 0, "mt6572-afe", afe);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request AFE irq %d\n", irq);

	ret = devm_snd_soc_register_component(dev, &mt6572_afe_component,
					      mt6572_afe_dais,
					      ARRAY_SIZE(mt6572_afe_dais));
	if (ret)
		return dev_err_probe(dev, ret, "failed to register AFE component\n");

	return 0;
}

static const struct of_device_id mt6572_afe_pcm_dt_match[] = {
	{ .compatible = "mediatek,mt6572-audio" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6572_afe_pcm_dt_match);

static struct platform_driver mt6572_afe_pcm_driver = {
	.driver = {
		.name = "mt6572-afe-pcm",
		.of_match_table = mt6572_afe_pcm_dt_match,
	},
	.probe = mt6572_afe_pcm_dev_probe,
};
module_platform_driver(mt6572_afe_pcm_driver);

MODULE_DESCRIPTION("MediaTek MT6572 AFE platform driver");
MODULE_LICENSE("GPL");
