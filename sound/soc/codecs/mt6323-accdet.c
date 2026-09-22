// SPDX-License-Identifier: GPL-2.0

/* mt6323-accdet.c -- ALSA SoC mt6323 accdet driver
 *
 * Copyright (c) 2026 Roman Vivchar <rva333@protonmail.com>
 */

#include <linux/of.h>
#include <linux/bitfield.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/io.h>
#include <linux/sched/clock.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/devm-helpers.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/regmap.h>
#include <linux/iio/consumer.h>
#include <sound/soc.h>
#include <sound/jack.h>

#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6323/registers.h>

#include "mt6323-accdet.h"

#define REGISTER_VAL(x)	((x) - 1)

#define TOP_CKPDN1_ACCDET		BIT(9)

#define TOP_RST_CON_ACCDET		BIT(4)

#define INT_CON1_ACCDET			BIT(2)

#define ACCDET_CON0_1V9_MODE_OFF	0x1a10
#define ACCDET_CON0_1V9_MODE_ON		0x1e10

#define ACCDET_CON1_ENABLE		BIT(0)

#define ACCDET_CON2_SWCTRL		GENMASK(2, 0)

#define ACCDET_CON3_PWM_WIDTH		REGISTER_VAL(0x500)

#define ACCDET_CON4_PWM_THRESHOLD	REGISTER_VAL(0x400)

#define ACCDET_CON5_FALL_DELAY		BIT(15)
#define ACCDET_CON5_RISE_DELAY		GENMASK(14, 0)
#define ACCDET_CON5_RISE_DELAY_DEFAULT	0x03f0

/* DEBOUNCE0 (state 00): long while detecting a plug, short for button taps */
#define ACCDET_CON6_DEBOUNCE_DETECT	0x3000
#define ACCDET_CON6_DEBOUNCE_BUTTON	0x0400
/* DEBOUNCE1 (state 01): gates press detection */
#define ACCDET_CON7_DEBOUNCE_DEFAULT	0x0800
/* DEBOUNCE3 (state 11): long enough to ride out contacts breaking mid-insertion */
#define ACCDET_CON9_DEBOUNCE_DEFAULT	0x0a00

/* Power down timeout for spurious wakeups */
#define ACCDET_SHUTDOWN_MS		1000

/* A plug being pushed in or wiggled breaks contact; confirm removals after this */
#define ACCDET_UNPLUG_CONFIRM_US	50000

/* Button releases are polled rather than waited on as an interrupt */
#define ACCDET_KEY_POLL_MS		60

/* Settling time before the key voltage is sampled */
#define ACCDET_KEY_SETTLE_US		400

/* The block latches the clear on its own slow clock, so hold the bit */
#define ACCDET_IRQ_CLR_US		100

#define ACCDET_CON11_IRQ_CLR		BIT(8)
#define ACCDET_CON11_IRQ_STA		BIT(0)

#define ACCDET_CON13_STATE_MASK		GENMASK(7, 6)

#define ACCDET_KEY_MEDIA_MAX_MV		90
#define ACCDET_KEY_VOLUMEUP_MAX_MV	170
#define ACCDET_KEY_VOLUMEDOWN_MAX_MV	560

#define MT6323_ACCDET_JACK_MASK (SND_JACK_HEADPHONE | \
				SND_JACK_HEADSET | \
				SND_JACK_BTN_0 | \
				SND_JACK_BTN_1 | \
				SND_JACK_BTN_2)

static const struct snd_soc_component_driver mt6323_accdet_soc_driver;

enum mt6323_accdet_jack_type {
	ACCDET_HEADPHONE,
	ACCDET_HEADSET,
	ACCDET_NO_DEVICE = 3
};

struct mt6323_accdet {
	struct snd_soc_jack *jack;
	struct device *dev;
	struct regmap *regmap;
	/* power status lock */
	struct mutex lock;

	int jack_type;
	int btn_type;
	bool powered;
	struct delayed_work shutdown_work;
	struct delayed_work key_work;
	struct iio_channel *adc;
};

static int mt6323_accdet_get_jack_type(struct mt6323_accdet *accdet,
                                       enum mt6323_accdet_jack_type *jack) {
	int ret;
	u32 val;

	ret = regmap_read(accdet->regmap, MT6323_ACCDET_CON13, &val);
	if (ret)
		return ret;

	*jack = FIELD_GET(ACCDET_CON13_STATE_MASK, val);
	return 0;
}

static int mt6323_accdet_get_btn_type(struct mt6323_accdet *accdet)
{
	int ret, mv;

	if (!accdet->adc)
		return SND_JACK_BTN_0;

	ret = regmap_write(accdet->regmap, MT6323_ACCDET_CON4,
			   ACCDET_CON3_PWM_WIDTH);
	if (ret)
		return SND_JACK_BTN_0;

	ret = regmap_write(accdet->regmap, MT6323_ACCDET_CON0,
			   ACCDET_CON0_1V9_MODE_ON);
	if (!ret) {
		udelay(ACCDET_KEY_SETTLE_US);
		ret = iio_read_channel_processed(accdet->adc, &mv);
	}

	regmap_write(accdet->regmap, MT6323_ACCDET_CON0,
		     ACCDET_CON0_1V9_MODE_OFF);
	regmap_write(accdet->regmap, MT6323_ACCDET_CON4,
		     ACCDET_CON4_PWM_THRESHOLD);

	if (ret) {
		dev_warn(accdet->dev, "failed to read key voltage: %d\n", ret);
		return SND_JACK_BTN_0;
	}

	dev_dbg(accdet->dev, "key voltage %d mV\n", mv);

	if (mv < ACCDET_KEY_MEDIA_MAX_MV)
		return SND_JACK_BTN_0;

	if (mv < ACCDET_KEY_VOLUMEUP_MAX_MV)
		return SND_JACK_BTN_2;

	if (mv < ACCDET_KEY_VOLUMEDOWN_MAX_MV)
		return SND_JACK_BTN_1;

	return 0;
}

static void mt6323_accdet_jack_report(struct mt6323_accdet *accdet)
{
	if (!accdet->jack)
		return;

	snd_soc_jack_report(accdet->jack, accdet->jack_type | accdet->btn_type,
	                    MT6323_ACCDET_JACK_MASK);
}

int mt6323_accdet_enable_jack_detect(struct snd_soc_component *component,
				     struct snd_soc_jack *jack)
{
	struct mt6323_accdet *accdet =
		snd_soc_component_get_drvdata(component);

	snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_VOLUMEDOWN);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEUP);

	accdet->jack = jack;

	mt6323_accdet_jack_report(accdet);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6323_accdet_enable_jack_detect);

static int mt6323_accdet_enable(struct mt6323_accdet *accdet) {
	struct regmap *map = accdet->regmap;
	int ret;

	/* ungate clock */
	ret = regmap_write(map, MT6323_TOP_CKPDN1_CLR, TOP_CKPDN1_ACCDET);
	if (ret)
		return ret;

	/* enable software control */
	ret = regmap_write(map, MT6323_ACCDET_CON2, ACCDET_CON2_SWCTRL);
	if (ret)
		return ret;

	/* enable accdet */
	ret = regmap_write(map, MT6323_ACCDET_CON1, ACCDET_CON1_ENABLE);
	if (ret)
		return ret;

	accdet->powered = true;
	return 0;
}

static int mt6323_accdet_disable(struct mt6323_accdet *accdet)
{
	struct regmap *map = accdet->regmap;
	int ret;

	/* disable accdet */
	ret = regmap_write(map, MT6323_ACCDET_CON1, 0);
	if (ret)
		return ret;

	/* disable software control */
	ret = regmap_write(map, MT6323_ACCDET_CON2, 0);
	if (ret)
		return ret;

	/* gate the clock */
	ret = regmap_write(map, MT6323_TOP_CKPDN1_SET, TOP_CKPDN1_ACCDET);
	if (ret)
		return ret;

	accdet->powered = false;
	return 0;
}

/* Power up the accdet and schedule shutdown work. Caller must hold the mutex. */
static void mt6323_accdet_wake(struct mt6323_accdet *accdet)
{
	if (mt6323_accdet_enable(accdet))
		return;

	schedule_delayed_work(&accdet->shutdown_work,
			      msecs_to_jiffies(ACCDET_SHUTDOWN_MS));
}

static void mt6323_accdet_key_work(struct work_struct *work)
{
	struct mt6323_accdet *accdet =
		container_of(work, struct mt6323_accdet, key_work.work);
	enum mt6323_accdet_jack_type jack;

	guard(mutex)(&accdet->lock);

	if (!accdet->btn_type)
		return;

	if (mt6323_accdet_get_jack_type(accdet, &jack))
		return;

	if (jack == ACCDET_HEADPHONE) {
		schedule_delayed_work(&accdet->key_work,
				      msecs_to_jiffies(ACCDET_KEY_POLL_MS));
		return;
	}

	accdet->btn_type = 0;
	mt6323_accdet_jack_report(accdet);
}

/* Report jack or key events. Caller holds the lock. */
static void mt6323_accdet_sync(struct mt6323_accdet *accdet)
{
	enum mt6323_accdet_jack_type jack;

	if (mt6323_accdet_get_jack_type(accdet, &jack))
		return;

	if (jack == ACCDET_NO_DEVICE && accdet->jack_type) {
		fsleep(ACCDET_UNPLUG_CONFIRM_US);

		if (mt6323_accdet_get_jack_type(accdet, &jack))
			return;
	}

	if (jack == ACCDET_NO_DEVICE) {
		/*
		 * If jack_type is not set to a real state, this is a spurious wakeup
		 * and shutdown work will cleanup accdet state.
		 */
		if (!accdet->jack_type)
			return;

		accdet->jack_type = 0;
		accdet->btn_type = 0;
		mt6323_accdet_jack_report(accdet);

		/* Set long debounce for the next jack insertion event */
		regmap_write(accdet->regmap, MT6323_ACCDET_CON6,
			     ACCDET_CON6_DEBOUNCE_DETECT);

		schedule_delayed_work(&accdet->shutdown_work,
				      msecs_to_jiffies(ACCDET_SHUTDOWN_MS));
		return;
	}

	/* Cancel the work to prevent shutdown */
	cancel_delayed_work(&accdet->shutdown_work);

	if (accdet->jack_type == SND_JACK_HEADSET) {
		if (jack != ACCDET_HEADPHONE) {
			accdet->btn_type = 0;
		} else if (!accdet->btn_type) {
			accdet->btn_type = mt6323_accdet_get_btn_type(accdet);

			if (accdet->btn_type)
				schedule_delayed_work(&accdet->key_work,
						      msecs_to_jiffies(ACCDET_KEY_POLL_MS));
		}
	} else if (jack == ACCDET_HEADSET) {
		accdet->jack_type = SND_JACK_HEADSET;
		/* Set short debounce for key events */
		regmap_write(accdet->regmap, MT6323_ACCDET_CON6,
			     ACCDET_CON6_DEBOUNCE_BUTTON);
	} else {
		accdet->jack_type = SND_JACK_HEADPHONE;
	}

	mt6323_accdet_jack_report(accdet);
}

static void mt6323_accdet_shutdown_work(struct work_struct *work)
{
	struct mt6323_accdet *accdet =
		container_of(work, struct mt6323_accdet, shutdown_work.work);

	guard(mutex)(&accdet->lock);

	/* Abort if already powered down */
	if (accdet->jack_type || !accdet->powered)
		return;

	mt6323_accdet_sync(accdet);
	if (accdet->jack_type)
		return;

	mt6323_accdet_disable(accdet);
}

static int mt6323_accdet_init(struct mt6323_accdet *accdet) {
	struct regmap *map = accdet->regmap;
	int ret;

	ret = regmap_write(map, MT6323_ACCDET_CON0, ACCDET_CON0_1V9_MODE_OFF);
	if (ret)
		return ret;

	/* ungate clock */
	ret = regmap_write(map, MT6323_TOP_CKPDN1_CLR, TOP_CKPDN1_ACCDET);
	if (ret)
		return ret;

	/* reset */
	ret = regmap_write(map, MT6323_TOP_RST_CON_SET, TOP_RST_CON_ACCDET);
	if (ret)
		return ret;

	ret = regmap_write(map, MT6323_TOP_RST_CON_CLR, TOP_RST_CON_ACCDET);
	if (ret)
		return ret;

	/* setup pwm */
	ret = regmap_write(map, MT6323_ACCDET_CON3, ACCDET_CON3_PWM_WIDTH);
	if (ret)
		return ret;

	ret = regmap_write(map, MT6323_ACCDET_CON4, ACCDET_CON4_PWM_THRESHOLD);
	if (ret)
		return ret;

	ret = regmap_write(map, MT6323_ACCDET_CON2, ACCDET_CON2_SWCTRL);
	if (ret)
		return ret;

	/* setup delay */
	ret = regmap_write(map, MT6323_ACCDET_CON5,
			   ACCDET_CON5_FALL_DELAY |
			   FIELD_PREP(ACCDET_CON5_RISE_DELAY,
				      ACCDET_CON5_RISE_DELAY_DEFAULT));
	if (ret)
		return ret;

	/* setup debounce time */
	ret = regmap_write(map, MT6323_ACCDET_CON6, ACCDET_CON6_DEBOUNCE_DETECT);
	if (ret)
		return ret;

	ret = regmap_write(map, MT6323_ACCDET_CON7, ACCDET_CON7_DEBOUNCE_DEFAULT);
	if (ret)
		return ret;

	ret = regmap_write(map, MT6323_ACCDET_CON9, ACCDET_CON9_DEBOUNCE_DEFAULT);
	if (ret)
		return ret;

	/* clear irq if any: w1c */
	ret = regmap_write(map, MT6323_ACCDET_CON11, ACCDET_CON11_IRQ_CLR);
	if (ret)
		return ret;

	udelay(ACCDET_IRQ_CLR_US);

	ret = regmap_write(map, MT6323_ACCDET_CON11, 0);
	if (ret)
		return ret;

	/* enable interrupt */
	return regmap_write(map, MT6323_INT_CON1_SET, INT_CON1_ACCDET);
}

/* PMIC interrupt handles key and unplug events */
static irqreturn_t mt6323_accdet_pmic_irq(int irq, void *data)
{
	struct mt6323_accdet *accdet = data;

	guard(mutex)(&accdet->lock);

	/* ack the irq (write-1-to-clear pulse), else the PMIC line storms */
	regmap_write(accdet->regmap, MT6323_ACCDET_CON11, ACCDET_CON11_IRQ_CLR);
	udelay(ACCDET_IRQ_CLR_US);
	regmap_write(accdet->regmap, MT6323_ACCDET_CON11, 0);

	if (!accdet->powered)
		return IRQ_HANDLED;

	mt6323_accdet_sync(accdet);

	return IRQ_HANDLED;
}

/* EINT interrupt wakes up accdet to check jack state */
static irqreturn_t mt6323_accdet_eint_irq(int irq, void *data)
{
	struct mt6323_accdet *accdet = data;

	guard(mutex)(&accdet->lock);

	if (!accdet->powered)
		mt6323_accdet_wake(accdet);
	else
		mt6323_accdet_sync(accdet);

	return IRQ_HANDLED;
}

static int mt6323_accdet_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6397 = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct mt6323_accdet *accdet;
	int ret, irq;

	accdet = devm_kzalloc(&pdev->dev, sizeof(*accdet), GFP_KERNEL);
	if (!accdet)
		return -ENOMEM;

	accdet->dev = dev;
	accdet->regmap = mt6397->regmap;
	if (IS_ERR(accdet->regmap))
		return dev_err_probe(dev, PTR_ERR(accdet->regmap), "failed to get regmap\n");

	ret = devm_mutex_init(dev, &accdet->lock);
	if (ret)
		return ret;

	ret = devm_delayed_work_autocancel(dev, &accdet->shutdown_work,
					   mt6323_accdet_shutdown_work);
	if (ret)
		return ret;

	ret = devm_delayed_work_autocancel(dev, &accdet->key_work,
					   mt6323_accdet_key_work);
	if (ret)
		return ret;

	accdet->adc = devm_iio_channel_get(dev, "accdet");
	if (IS_ERR(accdet->adc)) {
		ret = PTR_ERR(accdet->adc);
		if (ret == -EPROBE_DEFER)
			return ret;

		/* only the media button can be reported without it */
		dev_warn(dev, "no accdet auxadc channel (%d), buttons limited\n",
			 ret);
		accdet->adc = NULL;
	}

	irq = platform_get_irq_byname(pdev, "eint");
	if (irq < 0)
		return dev_err_probe(dev, irq, "failed to get eint irq\n");

	ret = devm_request_threaded_irq(&pdev->dev, irq,
					NULL, mt6323_accdet_eint_irq,
					IRQF_ONESHOT,
					"accdet-eint", accdet);
	if (ret)
		return ret;

	irq = platform_get_irq_byname(pdev, "pmic");
	if (irq < 0)
		return dev_err_probe(dev, irq, "failed to get pmic irq\n");

	ret = devm_request_threaded_irq(&pdev->dev, irq,
					NULL, mt6323_accdet_pmic_irq,
					IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
					IRQF_ONESHOT, "accdet-pmic", accdet);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, accdet);

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &mt6323_accdet_soc_driver,
					      NULL, 0);
	if (ret) {
		dev_err(&pdev->dev, "failed to register component\n");
		return ret;
	}

	mt6323_accdet_init(accdet);

	guard(mutex)(&accdet->lock);
	mt6323_accdet_wake(accdet);

	return 0;
}

static const struct of_device_id mt6323_accdet_match[] = {
	{ .compatible = "mediatek,mt6323-accdet" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6323_accdet_match);

static struct platform_driver mt6323_accdet_driver = {
	.driver = {
		.name = "mt6323-accdet",
		.of_match_table = mt6323_accdet_match,
	},
	.probe = mt6323_accdet_probe,
};

module_platform_driver(mt6323_accdet_driver);

MODULE_DESCRIPTION("MT6323 ALSA SoC accdet driver");
MODULE_LICENSE("GPL");
