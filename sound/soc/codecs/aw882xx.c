// SPDX-License-Identifier: GPL-2.0-only
//
// aw882xx.c  --  Awinic AW882xx (AW88261) smart speaker amplifier
//
// The chip on this board answers on I2C address 0x34 (i2c bus 2) with
// chip ID 0x2113, i.e. an AW88261.  Nothing in this driver comes from a
// datasheet: the register map, the initialisation sequence and the power
// up/down ordering were recovered from the vendor module that ships on the
// device (/vendor/lib/modules/audio_awinic882xx.ko, "driver_ver: v1.11.0")
// and cross-checked against a live register dump of the running part.
//
// Notes on what the vendor driver does and this one does not:
//   * The vendor driver keeps the register initialisation in an ACF
//     firmware blob (/system/etc/firmware/aw882xx_acf.bin).  The 56 writes
//     below are the speaker profile of that blob as programmed on the
//     device; every value was verified against the live chip.
//   * The vendor driver programs VSNTM1 (0x50) from the on-chip efuse
//     calibration words; that is reproduced here (aw882xx_set_vcalb()).
//   * The vendor driver has no regulator consumer at all: the amplifier is
//     fed from always-on rails.  Do not add supplies that the hardware does
//     not describe.
//

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <sound/soc.h>

#include "aw882xx.h"

/*
 * The vendor I2C accessors send one address byte and then clock out a
 * 16-bit big-endian value, so the regmap must be 8/16 with a big-endian
 * value format.
 */
static const struct regmap_config aw882xx_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = AW882XX_REG_MAX,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

/*
 * Speaker profile recovered from the ACF blob.  The volume word (0x05) is
 * intentionally not part of the table: it is owned by the mixer control.
 */
static const struct aw882xx_reg_val {
	u8 reg;
	u16 val;
} aw882xx_init_seq[] = {
	{ AW882XX_SYSINTM_REG,	0xffff },	/* mask all interrupts */
	{ AW882XX_SYSCTRL_REG,	0x3240 },
	{ AW882XX_I2SCTRL1_REG,	0x3408 },
	{ AW882XX_I2SCTRL2_REG,	0x0010 },
	{ AW882XX_I2SCTRL3_REG,	0x007c },
	{ AW882XX_DACCFG1_REG,	0x626d },
	{ AW882XX_DACCFG2_REG,	0x0964 },
	{ AW882XX_DACCFG3_REG,	0x2590 },
	{ AW882XX_DACCFG4_REG,	0x1c64 },
	{ AW882XX_DACCFG5_REG,	0x005b },
	{ AW882XX_DACCFG6_REG,	0x4a9b },
	{ AW882XX_DACCFG7_REG,	0x5264 },
	{ AW882XX_DACCFG8_REG,	0x5008 },
	{ AW882XX_PWMCTRL1_REG,	0x1f20 },
	{ AW882XX_PWMCTRL2_REG,	0x002b },
	{ AW882XX_I2SCFG1_REG,	0x2102 },
	{ AW882XX_DBGCTRL_REG,	0xc003 },
	{ AW882XX_DACCFG9_REG,	0x0643 },
	{ AW882XX_DACCFG10_REG,	0x000a },
	{ AW882XX_DSMCFG1_REG,	0x460c },
	{ AW882XX_DSMCFG2_REG,	0xc989 },
	{ AW882XX_DSMCFG3_REG,	0x3541 },
	{ AW882XX_DSMCFG4_REG,	0x4db8 },
	{ AW882XX_DSMCFG5_REG,	0xd6f9 },
	{ AW882XX_DSMCFG6_REG,	0x7ace },
	{ AW882XX_DSMCFG7_REG,	0xec7c },
	{ AW882XX_DSMCFG8_REG,	0xf18c },
	{ AW882XX_TESTIN_REG,	0x0800 },
	{ AW882XX_SADCCTRL1_REG,	0x8884 },
	{ AW882XX_SADCCTRL2_REG,	0x0666 },
	{ AW882XX_SADCCTRL3_REG,	0x0ed2 },
	{ AW882XX_SADCCTRL4_REG,	0x0663 },
	{ AW882XX_SADCCTRL5_REG,	0x01ae },
	{ AW882XX_SADCCTRL6_REG,	0x0505 },
	{ AW882XX_SADCCTRL7_REG,	0xffa0 },
	{ AW882XX_VSNTM2_REG,	0x00f3 },
	{ AW882XX_ISNCTRL1_REG,	0x3ee8 },
	{ AW882XX_ISNCTRL2_REG,	0x8005 },
	{ AW882XX_PLLCTRL1_REG,	0x1708 },
	{ AW882XX_PLLCTRL2_REG,	0x0202 },
	{ AW882XX_PLLCTRL3_REG,	0x3800 },
	{ AW882XX_CDACTRL1_REG,	0x84c9 },
	{ AW882XX_CDACTRL2_REG,	0x41d6 },
	{ AW882XX_DITHERCFG1_REG,	0x0000 },
	{ AW882XX_DITHERCFG2_REG,	0x0000 },
	{ AW882XX_DITHERCFG3_REG,	0x01c0 },
	{ AW882XX_CPCTRL_REG,	0x0049 },
	{ AW882XX_BSTCTRL1_REG,	0x0fcd },
	{ AW882XX_BSTCTRL2_REG,	0x4940 },
	{ AW882XX_BSTCTRL3_REG,	0x4a40 },
	{ AW882XX_BSTCTRL4_REG,	0x6077 },
	{ AW882XX_BSTCTRL5_REG,	0x0053 },
	{ AW882XX_BSTCTRL6_REG,	0x5ca1 },
	{ AW882XX_BSTCTRL7_REG,	0x40f9 },
	{ AW882XX_BSTCTRL8_REG,	0x508e },
	{ AW882XX_BSTCTRL9_REG,	0x0048 },
};

static int aw882xx_set_volume(struct aw882xx *aw, unsigned int volume)
{
	unsigned int val;
	int ret;

	volume = min_t(unsigned int, volume, AW882XX_MUTE_VOL);
	val = ((volume / AW882XX_VOLUME_STEP) << AW882XX_VOL_COARSE_SHIFT) |
	      (volume % AW882XX_VOLUME_STEP);

	ret = regmap_update_bits(aw->regmap, AW882XX_SYSCTRL2_REG,
				 AW882XX_VOL_MASK, val);
	if (ret)
		return ret;

	aw->volume = volume;

	return 0;
}

static int aw882xx_mute(struct aw882xx *aw, bool mute)
{
	return regmap_update_bits(aw->regmap, AW882XX_SYSCTRL_REG, AW882XX_HMUTE,
				  mute ? AW882XX_HMUTE : 0);
}

static int aw882xx_pwd(struct aw882xx *aw, bool power_down)
{
	return regmap_update_bits(aw->regmap, AW882XX_SYSCTRL_REG, AW882XX_PWDN,
				  power_down ? AW882XX_PWDN : 0);
}

static int aw882xx_amppd(struct aw882xx *aw, bool power_down)
{
	return regmap_update_bits(aw->regmap, AW882XX_SYSCTRL_REG, AW882XX_AMPPD,
				  power_down ? AW882XX_AMPPD : 0);
}

static int aw882xx_uls_hmute(struct aw882xx *aw, bool enable)
{
	return regmap_update_bits(aw->regmap, AW882XX_SYSCTRL_REG,
				  AW882XX_ULS_HMUTE,
				  enable ? AW882XX_ULS_HMUTE : 0);
}

static int aw882xx_i2s_tx_enable(struct aw882xx *aw, bool enable)
{
	return regmap_update_bits(aw->regmap, AW882XX_I2SCTRL3_REG,
				  AW882XX_I2STXEN,
				  enable ? AW882XX_I2STXEN : 0);
}

static void aw882xx_clear_int_status(struct aw882xx *aw)
{
	unsigned int val;

	/* SYSINT is cleared by reading it */
	regmap_read(aw->regmap, AW882XX_SYSINT_REG, &val);
	regmap_read(aw->regmap, AW882XX_SYSINT_REG, &val);
}

/*
 * Poll SYSST until all bits in @want are set.  The vendor driver does the
 * same with a 2 ms period and 10 attempts, first for the PLL/clock lock and
 * then for the boost/DSP start-up.
 */
static int aw882xx_check_sysst(struct aw882xx *aw, unsigned int want)
{
	unsigned int val;
	int ret, i;

	for (i = 0; i < AW882XX_SYSST_CHECK_MAX; i++) {
		ret = regmap_read(aw->regmap, AW882XX_SYSST_REG, &val);
		if (ret)
			return ret;

		if ((val & want) == want)
			return 0;

		dev_dbg(aw->dev, "sysst 0x%04x, want 0x%04x (try %d)\n",
			val, want, i);
		usleep_range(2000, 2100);
	}

	return -ETIMEDOUT;
}

/*
 * VSNTM1 holds a calibration word derived from the on-chip efuse.  The
 * vendor driver computes it as (1 << 13) * icalk / vcalk with 10-bit signed
 * efuse fields (see aw_pid_2113 get_icalk/get_vcalk/set_vcalb).  On this
 * board the efuse reads icalk = +30, vcalk = -13, which yields 0x2164 - the
 * exact value the running vendor driver leaves in 0x50.
 */
static s16 aw882xx_efuse_field(u16 hi, u16 lo)
{
	u16 val = (hi & AW882XX_EF_ICALK_MASK) & (lo & AW882XX_EF_ICALK_MASK);

	if (val & AW882XX_EF_ICALK_SIGN)
		val |= AW882XX_EF_FIELD_SIGN_EXT;

	return (s16)val;
}

static int aw882xx_set_vcalb(struct aw882xx *aw)
{
	unsigned int icalk_h, icalk_l, vcalk_h, vcalk_l;
	int icalk, vcalk, vcalb;
	int ret;

	ret = regmap_read(aw->regmap, AW882XX_EFRH4_REG, &icalk_h);
	if (ret)
		return ret;
	ret = regmap_read(aw->regmap, AW882XX_EFRL4_REG, &icalk_l);
	if (ret)
		return ret;
	ret = regmap_read(aw->regmap, AW882XX_EFRH3_REG, &vcalk_h);
	if (ret)
		return ret;
	ret = regmap_read(aw->regmap, AW882XX_EFRL3_REG, &vcalk_l);
	if (ret)
		return ret;

	icalk = AW882XX_CABL_BASE_VALUE +
		aw882xx_efuse_field(icalk_h, icalk_l);
	vcalk = AW882XX_CABL_BASE_VALUE +
		aw882xx_efuse_field(vcalk_h, vcalk_l);

	if (!vcalk)
		return -EINVAL;

	vcalb = (AW882XX_VCAL_FACTOR * icalk) / vcalk;

	dev_dbg(aw->dev, "icalk=%d vcalk=%d vcalb=0x%04x\n",
		icalk, vcalk, vcalb);

	return regmap_write(aw->regmap, AW882XX_VSNTM1_REG, vcalb);
}

static void aw882xx_hw_reset(struct aw882xx *aw)
{
	if (!aw->reset_gpio) {
		dev_warn(aw->dev, "no reset gpio, cannot reset the device\n");
		return;
	}

	/*
	 * RESET# is active low.  Raw values are used on purpose so that the
	 * pulse is correct no matter which polarity the DT declares for the
	 * line.  The vendor driver busy-waits 1 ms low and then 2 ms high.
	 */
	gpiod_set_raw_value_cansleep(aw->reset_gpio, 0);
	usleep_range(1000, 1500);
	gpiod_set_raw_value_cansleep(aw->reset_gpio, 1);
	usleep_range(2000, 2500);
}

static int aw882xx_read_chipid(struct aw882xx *aw)
{
	unsigned int id = 0;
	int ret, i;

	/* the vendor accessor retries a few times before giving up */
	for (i = 0; i < AW882XX_CHIPID_RETRIES; i++) {
		ret = regmap_read(aw->regmap, AW882XX_ID_REG, &id);
		if (!ret && id != 0x0000 && id != 0xffff)
			break;
		usleep_range(2000, 2100);
	}

	if (ret)
		return dev_err_probe(aw->dev, ret, "failed to read chip ID\n");

	switch (id) {
	case AW882XX_CHIP_ID_88261:
		break;
	/*
	 * Other revisions the vendor module knows about (0x1852, 0x2013,
	 * 0x2032, 0x2055, 0x2071) use different init sequences and register
	 * layouts; refuse them instead of driving them with AW88261 values.
	 */
	case 0x1852:
	case 0x2013:
	case 0x2032:
	case 0x2055:
	case 0x2071:
		return dev_err_probe(aw->dev, -ENODEV,
				     "AW882xx revision 0x%04x is known but not supported\n",
				     id);
	default:
		return dev_err_probe(aw->dev, -ENODEV,
				     "unknown AW882xx revision 0x%04x\n", id);
	}

	aw->chip_id = id;
	dev_info(aw->dev, "AW88261 detected (chip ID 0x%04x)\n", id);

	return 0;
}

static int aw882xx_dev_init(struct aw882xx *aw)
{
	int ret, i;

	/* software reset, then let the part settle */
	ret = regmap_write(aw->regmap, AW882XX_ID_REG, AW882XX_SOFT_RESET_VALUE);
	if (ret)
		return ret;
	usleep_range(1000, 1500);

	for (i = 0; i < ARRAY_SIZE(aw882xx_init_seq); i++) {
		ret = regmap_write(aw->regmap, aw882xx_init_seq[i].reg,
				   aw882xx_init_seq[i].val);
		if (ret) {
			dev_err(aw->dev, "init write 0x%02x failed: %d\n",
				aw882xx_init_seq[i].reg, ret);
			return ret;
		}
	}

	ret = aw882xx_set_vcalb(aw);
	if (ret)
		dev_warn(aw->dev, "vcalb calibration failed: %d\n", ret);

	/*
	 * The profile enables the I2S feedback transmitter; the vendor driver
	 * turns it off during initialisation, so mirror that.
	 */
	ret = aw882xx_i2s_tx_enable(aw, false);
	if (ret)
		return ret;

	ret = aw882xx_set_volume(aw, aw->volume);
	if (ret)
		return ret;

	aw882xx_clear_int_status(aw);

	/* leave the device muted and in power-down until a stream starts */
	ret = aw882xx_uls_hmute(aw, true);
	if (ret)
		return ret;
	ret = aw882xx_mute(aw, true);
	if (ret)
		return ret;
	usleep_range(1000, 1100);
	ret = aw882xx_amppd(aw, true);
	if (ret)
		return ret;
	ret = aw882xx_pwd(aw, true);
	if (ret)
		return ret;

	aw->status = AW882XX_DEV_PW_OFF;

	return 0;
}

static int aw882xx_dev_start(struct aw882xx *aw)
{
	int ret;

	if (aw->status == AW882XX_DEV_PW_ON)
		return 0;

	ret = aw882xx_pwd(aw, false);
	if (ret)
		return ret;
	usleep_range(2000, 2100);

	ret = aw882xx_check_sysst(aw, AW882XX_SYSST_PLL_OK);
	if (ret) {
		dev_err(aw->dev, "PLL/clock check failed, cannot start\n");
		goto err_pwd;
	}

	ret = aw882xx_amppd(aw, false);
	if (ret)
		goto err_pwd;
	usleep_range(1000, 1100);

	ret = aw882xx_check_sysst(aw, AW882XX_SYSST_RUN_OK);
	if (ret) {
		dev_err(aw->dev, "SYSST check failed, cannot start\n");
		goto err_amppd;
	}

	ret = aw882xx_uls_hmute(aw, false);
	if (ret)
		goto err_amppd;

	if (aw->volume != AW882XX_MUTE_VOL) {
		ret = aw882xx_mute(aw, false);
		if (ret)
			goto err_amppd;
	}

	aw882xx_clear_int_status(aw);
	aw->status = AW882XX_DEV_PW_ON;

	dev_dbg(aw->dev, "start success\n");

	return 0;

err_amppd:
	aw882xx_amppd(aw, true);
err_pwd:
	aw882xx_pwd(aw, true);
	aw->status = AW882XX_DEV_PW_OFF;

	return ret;
}

static int aw882xx_dev_stop(struct aw882xx *aw)
{
	if (aw->status == AW882XX_DEV_PW_OFF)
		return 0;

	aw882xx_clear_int_status(aw);
	aw882xx_uls_hmute(aw, true);
	aw882xx_mute(aw, true);
	aw882xx_i2s_tx_enable(aw, false);
	usleep_range(1000, 1100);
	aw882xx_amppd(aw, true);
	aw882xx_pwd(aw, true);

	aw->status = AW882XX_DEV_PW_OFF;

	return 0;
}

static void aw882xx_start_pa(struct aw882xx *aw)
{
	int ret, i;

	for (i = 0; i < AW882XX_START_RETRIES; i++) {
		ret = aw882xx_dev_start(aw);
		if (!ret)
			return;

		dev_dbg(aw->dev, "start failed, retry %d\n", i);
		usleep_range(2000, 2100);
	}

	dev_err(aw->dev, "failed to start the PA\n");
}

static void aw882xx_startup_work(struct work_struct *work)
{
	struct aw882xx *aw = container_of(work, struct aw882xx, start_work.work);

	mutex_lock(&aw->lock);
	aw882xx_start_pa(aw);
	mutex_unlock(&aw->lock);
}

/* Queue the PA start right away; the DAI trigger below re-tries if needed. */
static void aw882xx_start(struct aw882xx *aw, enum aw882xx_start_mode mode)
{
	if (aw->status == AW882XX_DEV_PW_ON)
		return;

	if (mode == AW882XX_SYNC_START) {
		mutex_lock(&aw->lock);
		aw882xx_start_pa(aw);
		mutex_unlock(&aw->lock);
	} else {
		queue_delayed_work(system_dfl_wq, &aw->start_work, 0);
	}
}

static int aw882xx_playback_event(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *k, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct aw882xx *aw = snd_soc_component_get_drvdata(component);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		aw882xx_start(aw, AW882XX_ASYNC_START);
		break;
	case SND_SOC_DAPM_POST_PMD:
		cancel_delayed_work_sync(&aw->start_work);
		mutex_lock(&aw->lock);
		aw882xx_dev_stop(aw);
		mutex_unlock(&aw->lock);
		break;
	default:
		break;
	}

	return 0;
}

static int aw882xx_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		break;
	default:
		dev_err(dai->dev, "unsupported DAI format 0x%x\n", fmt);
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_CBC_CFC:
	case SND_SOC_DAIFMT_CBP_CFC:
	case SND_SOC_DAIFMT_CBC_CFP:
	case SND_SOC_DAIFMT_CBP_CFP:
		break;
	default:
		dev_err(dai->dev, "invalid clocking mode 0x%x\n", fmt);
		return -EINVAL;
	}

	/*
	 * The amplifier derives its internal clocks from the incoming I2S
	 * bit clock / frame clock (SYSST reports PLL lock and clock stability)
	 * and needs no register programming for rate or width.
	 */
	return 0;
}

static int aw882xx_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct aw882xx *aw = snd_soc_component_get_drvdata(dai->component);
	int ret;

	if (stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	mutex_lock(&aw->lock);
	ret = aw882xx_mute(aw, mute);
	mutex_unlock(&aw->lock);

	return ret;
}

/*
 * On this SoC the LPASS only starts driving BCLK/LRCLK when the PCM is
 * triggered, which happens after DAPM's PRE_PMU.  If the asynchronous start
 * queued there could not lock the PLL yet, start the PA here where the bit
 * clock is guaranteed to be running.  Both paths are idempotent.
 */
static int aw882xx_trigger(struct snd_pcm_substream *substream, int cmd,
			   struct snd_soc_dai *dai)
{
	struct aw882xx *aw = snd_soc_component_get_drvdata(dai->component);

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (aw->status == AW882XX_DEV_PW_ON)
			return 0;

		cancel_delayed_work_sync(&aw->start_work);
		mutex_lock(&aw->lock);
		aw882xx_start_pa(aw);
		mutex_unlock(&aw->lock);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		mutex_lock(&aw->lock);
		aw882xx_dev_stop(aw);
		mutex_unlock(&aw->lock);
		break;
	default:
		break;
	}

	return 0;
}

static const struct snd_soc_dai_ops aw882xx_dai_ops = {
	.set_fmt = aw882xx_set_fmt,
	.mute_stream = aw882xx_mute_stream,
	.trigger = aw882xx_trigger,
};

static struct snd_soc_dai_driver aw882xx_dai[] = {
	{
		.name = "aw882xx-aif",
		.id = 0,
		.playback = {
			.stream_name = "Speaker_Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = AW882XX_RATES,
			.formats = AW882XX_FORMATS,
		},
		.ops = &aw882xx_dai_ops,
	},
};

static int aw882xx_volume_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct aw882xx *aw = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = aw->volume;

	return 0;
}

static int aw882xx_volume_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct aw882xx *aw = snd_soc_component_get_drvdata(component);
	long value = ucontrol->value.integer.value[0];
	int ret;

	if (value < 0 || value > AW882XX_MUTE_VOL)
		return -EINVAL;

	if (aw->volume == (unsigned int)value)
		return 0;

	mutex_lock(&aw->lock);
	ret = aw882xx_set_volume(aw, value);
	mutex_unlock(&aw->lock);

	return ret ? ret : 1;
}

/*
 * The volume is expressed in 1/8 dB steps; 0 is 0 dB and 720 (90 dB) mutes
 * the amplifier.  This matches the vendor control, which exposes the raw
 * 10-bit volume code in the same units.
 */
static const struct snd_kcontrol_new aw882xx_controls[] = {
	SOC_SINGLE_EXT("PCM Playback Volume", AW882XX_SYSCTRL2_REG,
		       AW882XX_VOL_COARSE_SHIFT, AW882XX_MUTE_VOL, 0,
		       aw882xx_volume_get, aw882xx_volume_put),
};

static const struct snd_soc_dapm_widget aw882xx_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN_E("AIF_RX", "Speaker_Playback", 0, SND_SOC_NOPM, 0, 0,
			      aw882xx_playback_event,
			      SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("SPK_OUT"),
};

static const struct snd_soc_dapm_route aw882xx_audio_map[] = {
	{ "SPK_OUT", NULL, "AIF_RX" },
};

static const struct snd_soc_component_driver aw882xx_component_driver = {
	.controls = aw882xx_controls,
	.num_controls = ARRAY_SIZE(aw882xx_controls),
	.dapm_widgets = aw882xx_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(aw882xx_dapm_widgets),
	.dapm_routes = aw882xx_audio_map,
	.num_dapm_routes = ARRAY_SIZE(aw882xx_audio_map),
};

static irqreturn_t aw882xx_irq(int irq, void *data)
{
	struct aw882xx *aw = data;
	unsigned int int_sts, sysst;

	regmap_read(aw->regmap, AW882XX_SYSINT_REG, &int_sts);
	regmap_read(aw->regmap, AW882XX_SYSST_REG, &sysst);

	dev_dbg(aw->dev, "irq: sysint 0x%04x sysst 0x%04x\n", int_sts, sysst);

	return IRQ_HANDLED;
}

static int aw882xx_i2c_probe(struct i2c_client *i2c)
{
	struct aw882xx *aw;
	int irq, ret;

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C))
		return dev_err_probe(&i2c->dev, -ENXIO,
				     "I2C adapter lacks I2C_FUNC_I2C\n");

	aw = devm_kzalloc(&i2c->dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;

	aw->dev = &i2c->dev;
	aw->status = AW882XX_DEV_PW_OFF;
	aw->volume = 0;			/* 0 dB, the value of the vendor profile */
	mutex_init(&aw->lock);
	INIT_DELAYED_WORK(&aw->start_work, aw882xx_startup_work);
	i2c_set_clientdata(i2c, aw);

	aw->regmap = devm_regmap_init_i2c(i2c, &aw882xx_regmap_config);
	if (IS_ERR(aw->regmap))
		return dev_err_probe(&i2c->dev, PTR_ERR(aw->regmap),
				     "failed to initialise regmap\n");

	aw->reset_gpio = devm_gpiod_get_optional(&i2c->dev, "reset",
						 GPIOD_OUT_HIGH);
	if (IS_ERR(aw->reset_gpio))
		return dev_err_probe(&i2c->dev, PTR_ERR(aw->reset_gpio),
				     "failed to get reset gpio\n");

	aw882xx_hw_reset(aw);

	ret = aw882xx_read_chipid(aw);
	if (ret)
		return ret;

	aw->irq_gpio = devm_gpiod_get_optional(&i2c->dev, "irq", GPIOD_IN);
	if (IS_ERR(aw->irq_gpio)) {
		dev_warn(aw->dev, "failed to get irq gpio: %pe\n",
			 aw->irq_gpio);
		aw->irq_gpio = NULL;
	}

	if (aw->irq_gpio) {
		irq = gpiod_to_irq(aw->irq_gpio);
		if (irq < 0) {
			dev_warn(aw->dev, "failed to map irq gpio: %d\n", irq);
		} else {
			ret = devm_request_threaded_irq(aw->dev, irq, NULL,
							aw882xx_irq,
							IRQF_ONESHOT |
							IRQF_TRIGGER_FALLING,
							"aw882xx", aw);
			if (ret)
				dev_warn(aw->dev, "failed to request irq %d: %d\n",
					 irq, ret);
		}
	}

	mutex_lock(&aw->lock);
	ret = aw882xx_dev_init(aw);
	mutex_unlock(&aw->lock);
	if (ret)
		return dev_err_probe(&i2c->dev, ret, "device init failed\n");

	ret = devm_snd_soc_register_component(&i2c->dev,
					      &aw882xx_component_driver,
					      aw882xx_dai, ARRAY_SIZE(aw882xx_dai));
	if (ret)
		return dev_err_probe(&i2c->dev, ret,
				     "failed to register the component\n");

	return 0;
}

static void aw882xx_i2c_remove(struct i2c_client *i2c)
{
	struct aw882xx *aw = i2c_get_clientdata(i2c);

	cancel_delayed_work_sync(&aw->start_work);

	mutex_lock(&aw->lock);
	aw882xx_dev_stop(aw);
	mutex_unlock(&aw->lock);
}

static const struct i2c_device_id aw882xx_i2c_id[] = {
	{ "aw882xx" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw882xx_i2c_id);

static const struct of_device_id aw882xx_of_match[] = {
	{ .compatible = "awinic,aw882xx" },
	{ .compatible = "awinic,aw882xx_smartpa" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw882xx_of_match);

static struct i2c_driver aw882xx_i2c_driver = {
	.driver = {
		.name = "aw882xx",
		.of_match_table = aw882xx_of_match,
	},
	.probe = aw882xx_i2c_probe,
	.remove = aw882xx_i2c_remove,
	.id_table = aw882xx_i2c_id,
};
module_i2c_driver(aw882xx_i2c_driver);

MODULE_DESCRIPTION("ASoC AW882xx (AW88261) smart PA driver");
MODULE_LICENSE("GPL");
