// SPDX-License-Identifier: GPL-2.0+
/*
 * Software fuel gauge for the MediaTek MT6323 PMIC.
 *
 * The MT6323 has no autonomous fuel-gauge and no usable battery-current ADC
 * channel, so state-of-charge cannot be derived from a current measurement.
 * The stock firmware instead runs a voltage-only model-based observer ("oam"):
 * it infers the battery current from the gap between the modeled open-circuit
 * voltage (OCV) and the measured terminal voltage divided by the battery's
 * internal resistance, integrates that current to track depth-of-discharge
 * (DOD), and re-anchors to the OCV table when the battery is at rest. This
 * driver reimplements that approach on top of the mt6323-auxadc BATSNS channel
 * and the simple-battery OCV table, with the per-cell internal-resistance curve
 * embedded below.
 *
 * Copyright (c) 2026 Custom Firmware <gabin278@gmail.com>
 */

#include <linux/cleanup.h>
#include <linux/devm-helpers.h>
#include <linux/iio/consumer.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/* Observer poll period; matches the stock fuel-gauge loop. */
#define MT6323_BAT_POLL_S		10

/* Below this magnitude of estimated current the battery is considered at rest
 * and the DOD is re-anchored towards the OCV-table reading.
 */
#define MT6323_BAT_REST_UA		40000

/* DOD is tracked in centi-percent (0 = full .. 10000 = empty). */
#define MT6323_DOD_MAX			10000

struct mt6323_r_point {
	/* Open-circuit voltage in mV */
	int ocv_uv;
	/* Cell internal resistance in mΩ */
	int r_mohm;
};

static const struct mt6323_r_point mt6323_r_table[] = {
	{ 4158000, 215 }, { 4080000, 215 }, { 4024000, 228 }, { 3974000, 235 },
	{ 3925000, 258 }, { 3897000, 270 }, { 3856000, 238 }, { 3819000, 208 },
	{ 3793000, 213 }, { 3768000, 215 }, { 3742000, 215 }, { 3719000, 223 },
	{ 3692000, 223 }, { 3665000, 233 }, { 3639000, 265 }, { 3589000, 270 },
	{ 3515000, 293 }, { 3407000, 345 }, { 3202000, 678 },
};

struct mt6323_battery {
	struct power_supply *psy;
	struct iio_channel *batsns_chan;
	struct iio_channel *isense_chan;
	struct delayed_work work;
	struct mutex lock;
	int qmax_uah;		/* full charge capacity, micro-amp-hours */
	int dod;		/* depth of discharge, centi-percent */
	int volt_uv;		/* last terminal voltage */
	int ocv_uv;		/* last IR-compensated OCV */
	int curr_ua;		/* last estimated current (>0 discharge) */
	int capacity;		/* percent */
	int status;
};

/* Linear interpolation helper for descending-x tables. */
static int mt6323_interp(int x, int x0, int y0, int x1, int y1)
{
	if (x >= x0)
		return y0;
	if (x <= x1)
		return y1;
	return y0 + (int)div_s64((s64)(y1 - y0) * (x0 - x), x0 - x1);
}

/* Battery internal resistance (mOhm) at a given OCV (uV). */
static int mt6323_r_by_ocv(int ocv_uv)
{
	int i;

	for (i = 1; i < ARRAY_SIZE(mt6323_r_table); i++)
		if (ocv_uv >= mt6323_r_table[i].ocv_uv)
			return mt6323_interp(ocv_uv,
					     mt6323_r_table[i - 1].ocv_uv,
					     mt6323_r_table[i - 1].r_mohm,
					     mt6323_r_table[i].ocv_uv,
					     mt6323_r_table[i].r_mohm);

	return mt6323_r_table[ARRAY_SIZE(mt6323_r_table) - 1].r_mohm;
}

/* OCV (uV) -> DOD (centi-percent) via the DT OCV table. */
static int mt6323_ocv_to_dod(struct mt6323_battery *bat, int ocv_uv)
{
	struct power_supply_battery_info *info = bat->psy->battery_info;
	const struct power_supply_battery_ocv_table *t = info->ocv_table[0];
	int n = info->ocv_table_size[0];
	int i, cap;

	if (ocv_uv >= t[0].ocv)
		cap = t[0].capacity * 100;
	else if (ocv_uv <= t[n - 1].ocv)
		cap = t[n - 1].capacity * 100;
	else {
		for (i = 1; i < n; i++)
			if (ocv_uv >= t[i].ocv)
				break;
		cap = mt6323_interp(ocv_uv, t[i - 1].ocv, t[i - 1].capacity * 100,
				    t[i].ocv, t[i].capacity * 100);
	}

	return MT6323_DOD_MAX - cap;
}

/* DOD (centi-percent) -> modeled OCV (uV) via the DT OCV table. */
static int mt6323_dod_to_ocv(struct mt6323_battery *bat, int dod)
{
	struct power_supply_battery_info *info = bat->psy->battery_info;
	const struct power_supply_battery_ocv_table *t = info->ocv_table[0];
	int n = info->ocv_table_size[0];
	int cap_cp = MT6323_DOD_MAX - clamp(dod, 0, MT6323_DOD_MAX);
	int i;

	if (cap_cp >= t[0].capacity * 100)
		return t[0].ocv;
	if (cap_cp <= t[n - 1].capacity * 100)
		return t[n - 1].ocv;

	for (i = 1; i < n; i++)
		if (cap_cp >= t[i].capacity * 100)
			break;

	return mt6323_interp(cap_cp, t[i - 1].capacity * 100, t[i - 1].ocv,
			     t[i].capacity * 100, t[i].ocv);
}

static int mt6323_read_voltage(struct mt6323_battery *bat, int *volt_uv)
{
	int mv, ret;

	ret = iio_read_channel_processed(bat->batsns_chan, &mv);
	if (ret < 0)
		return ret;

	*volt_uv = mv * 1000;
	return 0;
}

static void mt6323_battery_update(struct mt6323_battery *bat)
{
	int v_uv, ocv_model, r_mohm, i_ua, ddod, dod_meas;

	if (mt6323_read_voltage(bat, &v_uv))
		return;

	/*
	 * Estimate current from the gap between the modeled OCV (the OCV the
	 * cell should rest at for the tracked DOD) and the measured terminal
	 * voltage. Positive == discharging (terminal sags below OCV).
	 */
	ocv_model = mt6323_dod_to_ocv(bat, bat->dod);
	r_mohm = mt6323_r_by_ocv(ocv_model);
	i_ua = div_s64((s64)(ocv_model - v_uv) * 1000, r_mohm);

	/* Coulomb-integrate estimated current into DOD */
	ddod = div64_s64((s64)i_ua * MT6323_BAT_POLL_S * MT6323_DOD_MAX,
			 3600LL * bat->qmax_uah);
	bat->dod += ddod;

	if (abs(i_ua) < MT6323_BAT_REST_UA) {
		dod_meas = mt6323_ocv_to_dod(bat, v_uv);
		bat->dod += (dod_meas - bat->dod) / 4;
	}

	bat->dod = clamp(bat->dod, 0, MT6323_DOD_MAX);

	bat->volt_uv = v_uv;
	bat->curr_ua = -i_ua;
	bat->ocv_uv = v_uv + (int)div_s64((s64)i_ua * r_mohm, 1000);
	bat->capacity = (MT6323_DOD_MAX - bat->dod) / 100;

	if (i_ua > MT6323_BAT_REST_UA)
		bat->status = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (i_ua < -MT6323_BAT_REST_UA)
		bat->status = POWER_SUPPLY_STATUS_CHARGING;
	else
		bat->status = POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static void mt6323_battery_work(struct work_struct *work)
{
	struct mt6323_battery *bat =
		container_of(work, struct mt6323_battery, work.work);

	scoped_guard(mutex, &bat->lock)
		mt6323_battery_update(bat);

	queue_delayed_work(system_percpu_wq, &bat->work,
			   MT6323_BAT_POLL_S * HZ);
}

static int mt6323_battery_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct mt6323_battery *bat = power_supply_get_drvdata(psy);

	guard(mutex)(&bat->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = bat->status;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = bat->volt_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = bat->ocv_uv;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = bat->capacity;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property mt6323_battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,
};

static const struct power_supply_desc mt6323_battery_desc = {
	.name		= "fuel-gauge",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= mt6323_battery_properties,
	.num_properties	= ARRAY_SIZE(mt6323_battery_properties),
	.get_property	= mt6323_battery_get_property,
};

static int mt6323_battery_probe(struct platform_device *pdev)
{
	struct power_supply_config psy_cfg = {};
	struct device *dev = &pdev->dev;
	struct power_supply_battery_info *info;
	struct mt6323_battery *bat;
	int ret, v_uv;

	bat = devm_kzalloc(dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;

	bat->batsns_chan = devm_iio_channel_get(dev, "batsns");
	if (IS_ERR(bat->batsns_chan))
		return dev_err_probe(dev, PTR_ERR(bat->batsns_chan),
				     "getting batsns channel\n");

	ret = devm_mutex_init(dev, &bat->lock);
	if (ret)
		return ret;

	psy_cfg.drv_data = bat;
	psy_cfg.fwnode = dev_fwnode(dev);
	bat->psy = devm_power_supply_register(dev, &mt6323_battery_desc, &psy_cfg);
	if (IS_ERR(bat->psy))
		return dev_err_probe(dev, PTR_ERR(bat->psy),
				     "registering power supply\n");

	info = bat->psy->battery_info;
	if (!info || !info->ocv_table[0])
		return dev_err_probe(dev, -ENODEV,
				     "monitored-battery OCV table is required\n");

	bat->qmax_uah = info->charge_full_design_uah > 0 ?
		info->charge_full_design_uah : 1499000;

	/* Seed DOD assuming the battery is near rest at boot. */
	ret = mt6323_read_voltage(bat, &v_uv);
	if (ret)
		return dev_err_probe(dev, ret, "reading initial voltage\n");
	bat->dod = mt6323_ocv_to_dod(bat, v_uv);

	ret = devm_delayed_work_autocancel(dev, &bat->work, mt6323_battery_work);
	if (ret)
		return ret;

	mt6323_battery_update(bat);
	queue_delayed_work(system_percpu_wq, &bat->work, MT6323_BAT_POLL_S * HZ);

	platform_set_drvdata(pdev, bat);
	return 0;
}

static const struct of_device_id mt6323_battery_of_match[] = {
	{ .compatible = "mediatek,mt6323-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6323_battery_of_match);

static struct platform_driver mt6323_battery_driver = {
	.driver = {
		.name = "mt6323-battery",
		.of_match_table = mt6323_battery_of_match,
	},
	.probe = mt6323_battery_probe,
};
module_platform_driver(mt6323_battery_driver);

MODULE_AUTHOR("Custom Firmware <gabin278@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6323 PMIC software fuel gauge");
MODULE_LICENSE("GPL");
