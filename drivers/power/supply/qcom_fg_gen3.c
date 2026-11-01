// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm PM660/PM660L fuel gauge (qpnp-fg-gen3)
 *
 * The PM660 fuel gauge is a SPMI peripheral made of three register
 * blocks:
 *
 *   0x4000  "qcom,fg-batt-soc"   state of charge
 *   0x4100  "qcom,fg-batt-info"  VBATT/IBATT/temperature ADC results
 *   0x4400  "qcom,fg-memif"      interleaved memory (SRAM) access engine
 *
 * The gauge keeps its decoded parameters (filtered battery voltage,
 * learned capacity, ...) in a 32 bit wide SRAM which is not memory
 * mapped.  It is reached through the interleaved memory access (IMA)
 * engine of the memif block: a 16 bit shadow address latch selects the
 * word, a 4 byte window is used to move the data and a byte mask
 * selects which bytes of a word a write affects.
 *
 * The register layout and the access sequence below were recovered from
 * the vendor kernel (fg_sram_read(), fg_sram_write(),
 * fg_interleaved_mem_read(), fg_interleaved_mem_write(),
 * fg_interleaved_mem_config(), fg_set_address(),
 * fg_config_access_mode(), fg_check_iacs_ready()) and confirmed on
 * hardware: after a vendor debugfs SRAM read of word 0x61 the shadow
 * latch at 0x4461 reads back 0x0061 and the read window at
 * 0x4467..0x446a holds exactly the 4 bytes the read returned.
 *
 * All register values here are absolute SPMI addresses: the fuel gauge
 * DT node itself has no "reg" property, its children own the blocks.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/spmi.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define FG_SOC_BASE			0x4000
#define FG_INFO_BASE			0x4100
#define FG_MEMIF_BASE			0x4400

/* fg-batt-soc block */
#define FG_MSOC				0x09

/* fg-batt-info block */
#define FG_BATT_TEMP			0x50
#define FG_BATT_VOLT			0xa0
#define FG_BATT_CURR			0xa2

/* fg-memif block */
#define FG_MEMIF_IACS_CTRL		0x50
#define FG_MEMIF_IACS_MODE		0x51
#define FG_MEMIF_IACS_STATUS		0x54
#define FG_MEMIF_IACS_HANDSHAKE		0x57
#define FG_MEMIF_WMASK			0x60
#define FG_MEMIF_ADDR			0x61
#define FG_MEMIF_WDATA			0x63
#define FG_MEMIF_RDATA			0x67

/* fg-memif IACS control (0x4450) */
#define FG_IACS_BUSY			BIT(7)
#define FG_IACS_REQ			GENMASK(7, 5)

/* fg-memif IACS mode (0x4451) */
#define FG_IACS_MODE_MASK		GENMASK(7, 6)
#define FG_IACS_MODE_WRITE		BIT(6)
#define FG_IACS_MODE_BURST		BIT(7)

/* fg-memif IACS status (0x4454) */
#define FG_IACS_READY			BIT(1)

/*
 * The vendor driver polls the busy flag six times with a 4 ms sleep and
 * the ready flag 250 times with a 5..7 ms sleep (up to 1.75 s).  Keep
 * the first timeout identical and bound the second one much tighter:
 * a single SRAM word transfer takes microseconds and this runs from
 * ->get_property().
 */
#define FG_IACS_IDLE_POLLS		6
#define FG_IACS_IDLE_US			4000
#define FG_IACS_READY_POLLS		50
#define FG_IACS_READY_US		1000

#define FG_SRAM_RETRIES			3
#define FG_MSOC_RETRIES			5

/* raw * 5 / 2 - 2730, in 0.1 degrees Celsius */
#define FG_TEMP_SCALE_NUM		5
#define FG_TEMP_SCALE_DEN		2
#define FG_TEMP_OFFSET			2730

/* 488281 uA per LSB, i.e. 1 / 2048 of a volt across a 10 mOhm sense */
#define FG_CURR_LSB_NUM			488281
#define FG_CURR_LSB_DEN			1000

#define FG_MSOC_FULL			0xff

enum fg_gen3_param {
	FG_PARAM_BATT_VOLT,
	FG_PARAM_FCC,
	FG_PARAM_MAX,
};

/*
 * Parameter table mirroring the subset of the vendor fg_sram_params[]
 * array that was verified against the hardware:
 *
 *	value = raw * multiplier / denominator
 *
 * The vendor descriptor is 36 bytes long and stores the word as u16 at
 * +0x00, the byte offset as u8 at +0x04, the byte count as u8 at +0x08,
 * the denominator as u32 at +0x10, the multiplier as u32 at +0x14 and
 * the encode/decode callbacks at +0x1c/+0x20.
 *
 * Verified entries:
 *
 *  BATT_VOLT	word 0x61 bytes 2..3, multiplier 244141, denominator
 *		1000.  Raw 15587 and 15583 were observed while
 *		voltage_now read 3806142 uV and 3806875 uV, giving
 *		3805425 uV and 3804449 uV.
 *  BATT_FCC	word 0x4a bytes 0..1, the learned capacity in mAh.
 *		Raw 772 while charge_full read 772000 uAh.
 *
 * Not included, because they could not be verified against a userspace
 * property: the two ESR terms (words 0x63 and 0x65, summed by the
 * vendor's fg_get_battery_resistance()) and the Q30 coulomb
 * accumulator (word 0x60) used by fg_get_charge_counter().
 */
static const struct fg_gen3_sram_param {
	u16 word;
	u8 offset;
	u8 len;
	u32 multiplier;
	u32 denominator;
	const char *name;
} fg_gen3_sram_params[FG_PARAM_MAX] = {
	[FG_PARAM_BATT_VOLT] = {
		.word		= 0x61,
		.offset		= 2,
		.len		= 2,
		.multiplier	= 244141,
		.denominator	= 1000,
		.name		= "BATT_VOLT",
	},
	/* The vendor table stores this one in mAh, CHARGE_FULL is in uAh. */
	[FG_PARAM_FCC] = {
		.word		= 0x4a,
		.offset		= 0,
		.len		= 2,
		.multiplier	= 1000,
		.denominator	= 1,
		.name		= "BATT_FCC",
	},
};

struct fg_gen3_chip {
	struct device *dev;
	struct regmap *regmap;
	struct mutex sram_lock;	/* serialises the SRAM access engine */
	struct power_supply *psy;
	struct power_supply_desc desc;
};

static const struct regmap_config fg_gen3_regmap_config = {
	.reg_bits	= 16,
	.val_bits	= 8,
	.max_register	= 0xffff,
};

static int fg_gen3_memif_poll(struct fg_gen3_chip *chip, u16 reg, u8 bit,
			      bool set, int polls, unsigned int usec)
{
	unsigned int val;
	int i, ret;

	for (i = 0; i < polls; i++) {
		ret = regmap_read(chip->regmap, reg, &val);
		if (ret)
			return ret;

		if (set ? (val & bit) : !(val & bit))
			return 0;

		usleep_range(usec, usec + 1000);
	}

	return -ETIMEDOUT;
}

static int fg_gen3_memif_wait_idle(struct fg_gen3_chip *chip)
{
	return fg_gen3_memif_poll(chip, FG_MEMIF_BASE + FG_MEMIF_IACS_CTRL,
				  FG_IACS_BUSY, false, FG_IACS_IDLE_POLLS,
				  FG_IACS_IDLE_US);
}

static int fg_gen3_memif_wait_ready(struct fg_gen3_chip *chip)
{
	return fg_gen3_memif_poll(chip, FG_MEMIF_BASE + FG_MEMIF_IACS_STATUS,
				  FG_IACS_READY, true, FG_IACS_READY_POLLS,
				  FG_IACS_READY_US);
}

/*
 * Read one 32 bit SRAM word.  The sequence is the one used by
 * fg_interleaved_mem_config() followed by fg_interleaved_mem_read():
 *
 *   1. wait until the access engine is not busy (0x4450 bit 7)
 *   2. request access		(0x4450 |= 0xa0)
 *   3. select a single word read	(0x4451 bits 7:6 = 0)
 *   4. wait for IACS ready		(0x4454 bit 1)
 *   5. latch the word address	(0x4461..0x4462, little endian)
 *   6. wait for IACS ready again	(the word is now in the window)
 *   7. read the window		(0x4467..0x446a)
 *   8. release access		(0x4450 &= ~0xa0)
 */
static int fg_gen3_sram_read_word(struct fg_gen3_chip *chip, u16 word,
				  u32 *val)
{
	u8 addr[2] = { word & 0xff, word >> 8 };
	u8 data[4];
	int ret;

	mutex_lock(&chip->sram_lock);

	ret = fg_gen3_memif_wait_idle(chip);
	if (ret)
		goto out_unlock;

	ret = regmap_update_bits(chip->regmap,
				 FG_MEMIF_BASE + FG_MEMIF_IACS_CTRL,
				 FG_IACS_REQ, FG_IACS_REQ);
	if (ret)
		goto out_release;

	ret = regmap_update_bits(chip->regmap,
				 FG_MEMIF_BASE + FG_MEMIF_IACS_MODE,
				 FG_IACS_MODE_MASK, 0);
	if (ret)
		goto out_release;

	ret = fg_gen3_memif_wait_ready(chip);
	if (ret)
		goto out_release;

	ret = regmap_bulk_write(chip->regmap,
				FG_MEMIF_BASE + FG_MEMIF_ADDR, addr,
				sizeof(addr));
	if (ret)
		goto out_release;

	ret = fg_gen3_memif_wait_ready(chip);
	if (ret)
		goto out_release;

	ret = regmap_bulk_read(chip->regmap,
			       FG_MEMIF_BASE + FG_MEMIF_RDATA, data,
			       sizeof(data));
	if (ret)
		goto out_release;

	*val = get_unaligned_le32(data);

out_release:
	regmap_update_bits(chip->regmap,
			   FG_MEMIF_BASE + FG_MEMIF_IACS_CTRL,
			   FG_IACS_REQ, 0);
out_unlock:
	mutex_unlock(&chip->sram_lock);

	return ret;
}

static int fg_gen3_read_param(struct fg_gen3_chip *chip,
			      enum fg_gen3_param id, s64 *value)
{
	const struct fg_gen3_sram_param *p = &fg_gen3_sram_params[id];
	u32 word, raw;
	int i, ret = -EIO;

	for (i = 0; i < FG_SRAM_RETRIES; i++) {
		ret = fg_gen3_sram_read_word(chip, p->word, &word);
		if (!ret)
			break;
	}

	if (ret) {
		dev_dbg(chip->dev, "failed to read %s: %d\n", p->name, ret);
		return ret;
	}

	if (p->len >= 4)
		raw = word;
	else
		raw = (word >> (8 * p->offset)) &
		      ((1U << (8 * p->len)) - 1);

	*value = div_s64((s64)raw * p->multiplier, p->denominator);

	return 0;
}

/*
 * MSOC is exposed as two copies that must agree; the vendor driver
 * retries until they do and rejects the sample otherwise.
 */
static int fg_gen3_get_msoc_raw(struct fg_gen3_chip *chip, u8 *msoc)
{
	u8 buf[2];
	int i, ret;

	for (i = 0; i < FG_MSOC_RETRIES; i++) {
		ret = regmap_bulk_read(chip->regmap, FG_SOC_BASE + FG_MSOC,
				       buf, sizeof(buf));
		if (ret)
			return ret;

		if (buf[0] == buf[1]) {
			*msoc = buf[0];
			return 0;
		}
	}

	return -EAGAIN;
}

/*
 * Same transfer function as the vendor fg_get_msoc(): 0 and 0xff are
 * the end points, everything else is linearised with a 98/255 slope.
 */
static int fg_gen3_msoc_to_percent(u8 raw)
{
	s32 val = 98 * (raw - 1);

	if (raw == 0)
		return 0;

	if (raw == FG_MSOC_FULL)
		return 100;

	if (val > 0)
		val += 126;
	else
		val -= 126;

	return div_s64(val, 255) + 1;
}

static int fg_gen3_get_current(struct fg_gen3_chip *chip, int *curr)
{
	u8 buf[2];
	int ret;

	/* Verified: raw 360 / -138 gave 175781 uA / -67383 uA. */
	ret = regmap_bulk_read(chip->regmap, FG_INFO_BASE + FG_BATT_CURR,
			       buf, sizeof(buf));
	if (ret)
		return ret;

	*curr = div_s64((s64)(s16)get_unaligned_le16(buf) *
			FG_CURR_LSB_NUM, FG_CURR_LSB_DEN);

	return 0;
}

static int fg_gen3_get_temp(struct fg_gen3_chip *chip, int *temp)
{
	u8 buf[2];
	int ret;

	/*
	 * Verified: raw 1250 and 1255 were read while temp reported 395
	 * and 407 (0.1 degrees Celsius).
	 */
	ret = regmap_bulk_read(chip->regmap, FG_INFO_BASE + FG_BATT_TEMP,
			       buf, sizeof(buf));
	if (ret)
		return ret;

	*temp = div_s64((s64)(buf[0] | ((buf[1] & GENMASK(2, 0)) << 8)) *
			FG_TEMP_SCALE_NUM, FG_TEMP_SCALE_DEN) - FG_TEMP_OFFSET;

	return 0;
}

static int fg_gen3_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct fg_gen3_chip *chip = power_supply_get_drvdata(psy);
	s64 value;
	u8 msoc;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = fg_gen3_read_param(chip, FG_PARAM_BATT_VOLT, &value);
		if (ret)
			return ret;

		val->intval = value;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return fg_gen3_get_current(chip, &val->intval);
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = fg_gen3_get_msoc_raw(chip, &msoc);
		if (ret)
			return ret;

		val->intval = fg_gen3_msoc_to_percent(msoc);
		return 0;
	case POWER_SUPPLY_PROP_TEMP:
		return fg_gen3_get_temp(chip, &val->intval);
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = fg_gen3_read_param(chip, FG_PARAM_FCC, &value);
		if (ret)
			return ret;

		val->intval = value;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		ret = fg_gen3_get_current(chip, &val->intval);
		if (ret)
			return ret;

		/*
		 * The gauge cannot see VBUS; derive the direction from the
		 * battery current.  A charger driver is expected to
		 * override this through the power supply notifier API.
		 */
		if (val->intval > 0)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (val->intval < 0)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;

		return 0;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = fg_gen3_get_temp(chip, &val->intval);
		if (ret)
			return ret;

		/*
		 * Temperature based, like the vendor charger driver.  The
		 * Warm/Good boundary at 40.0 degrees Celsius was confirmed
		 * on hardware (395 -> Good, 407 -> Warm).
		 */
		if (val->intval >= 600)
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		else if (val->intval >= 400)
			val->intval = POWER_SUPPLY_HEALTH_WARM;
		else if (val->intval <= 100)
			val->intval = POWER_SUPPLY_HEALTH_COOL;
		else
			val->intval = POWER_SUPPLY_HEALTH_GOOD;

		return 0;
	default:
		return -EINVAL;
	}
}

static enum power_supply_property fg_gen3_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_FULL,
};

static int fg_gen3_probe(struct spmi_device *sdev)
{
	struct power_supply_config psy_cfg = {};
	struct fg_gen3_chip *chip;
	struct device *dev = &sdev->dev;
	unsigned int val;
	int ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = dev;
	mutex_init(&chip->sram_lock);

	chip->regmap = devm_regmap_init_spmi_ext(sdev,
						 &fg_gen3_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(dev, PTR_ERR(chip->regmap),
				     "failed to initialise regmap\n");

	/* Fail early instead of at the first property read. */
	ret = regmap_read(chip->regmap,
			  FG_MEMIF_BASE + FG_MEMIF_IACS_STATUS, &val);
	if (ret)
		return dev_err_probe(dev, ret, "memif block not accessible\n");

	chip->desc.name		= "pm660-fg";
	chip->desc.type		= POWER_SUPPLY_TYPE_BATTERY;
	chip->desc.properties	= fg_gen3_props;
	chip->desc.num_properties = ARRAY_SIZE(fg_gen3_props);
	chip->desc.get_property	= fg_gen3_get_property;

	psy_cfg.drv_data = chip;
	psy_cfg.fwnode = dev_fwnode(dev);

	chip->psy = devm_power_supply_register(dev, &chip->desc, &psy_cfg);
	if (IS_ERR(chip->psy))
		return dev_err_probe(dev, PTR_ERR(chip->psy),
				     "failed to register power supply\n");

	spmi_device_set_drvdata(sdev, chip);

	return 0;
}

static const struct of_device_id fg_gen3_match_table[] = {
	{ .compatible = "qcom,fg-gen3" },
	{ }
};
MODULE_DEVICE_TABLE(of, fg_gen3_match_table);

static struct spmi_driver fg_gen3_driver = {
	.driver = {
		.name		= "qcom-fg-gen3",
		.of_match_table	= fg_gen3_match_table,
	},
	.probe	= fg_gen3_probe,
};
module_spmi_driver(fg_gen3_driver);

MODULE_DESCRIPTION("Qualcomm PM660 (qpnp-fg-gen3) fuel gauge");
MODULE_LICENSE("GPL");
