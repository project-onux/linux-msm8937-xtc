/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * aw882xx.h -- Awinic AW882xx (AW88261 family) smart PA register map
 *
 * The register map below was recovered from the vendor module shipped on the
 * msm8937 watch (/vendor/lib/modules/audio_awinic882xx.ko, driver "v1.11.0"),
 * by disassembling the register accessors and cross-checking every field
 * against a live register dump of the running chip.
 *
 * I2C protocol: one 8-bit register address byte followed by a 16-bit
 * big-endian data word (see aw882xx_i2c_reads()/aw882xx_i2c_write() in the
 * vendor module, which issue a two-message i2c_transfer and reassemble the
 * value as (buf[0] << 8) | buf[1]).
 */

#ifndef __AW882XX_H__
#define __AW882XX_H__

/* Register map */
#define AW882XX_ID_REG			0x00
#define AW882XX_SYSST_REG		0x01
#define AW882XX_SYSINT_REG		0x02
#define AW882XX_SYSINTM_REG		0x03
#define AW882XX_SYSCTRL_REG		0x04
#define AW882XX_SYSCTRL2_REG		0x05
#define AW882XX_I2SCTRL1_REG		0x06
#define AW882XX_I2SCTRL2_REG		0x07
#define AW882XX_I2SCTRL3_REG		0x08
#define AW882XX_DACCFG1_REG		0x09
#define AW882XX_DACCFG2_REG		0x0a
#define AW882XX_DACCFG3_REG		0x0b
#define AW882XX_DACCFG4_REG		0x0c
#define AW882XX_DACCFG5_REG		0x0d
#define AW882XX_DACCFG6_REG		0x0e
#define AW882XX_DACCFG7_REG		0x0f
#define AW882XX_DACCFG8_REG		0x10
#define AW882XX_PWMCTRL1_REG		0x11
#define AW882XX_PWMCTRL2_REG		0x12
#define AW882XX_I2SCFG1_REG		0x13
#define AW882XX_DBGCTRL_REG		0x14
#define AW882XX_DACCFG9_REG		0x15
#define AW882XX_DACCFG10_REG		0x16
#define AW882XX_DACST_REG		0x20
#define AW882XX_VBAT_REG		0x21
#define AW882XX_TEMP_REG		0x22
#define AW882XX_PVDD_REG		0x23
#define AW882XX_ISNDAT_REG		0x24
#define AW882XX_VSNDAT_REG		0x25
#define AW882XX_I2SINT_REG		0x26
#define AW882XX_I2SCAPCNT_REG		0x27
#define AW882XX_ANASTA1_REG		0x28
#define AW882XX_ANASTA2_REG		0x29
#define AW882XX_ANASTA3_REG		0x2a
#define AW882XX_TESTDET_REG		0x2b
#define AW882XX_DSMCFG1_REG		0x30
#define AW882XX_DSMCFG2_REG		0x31
#define AW882XX_DSMCFG3_REG		0x32
#define AW882XX_DSMCFG4_REG		0x33
#define AW882XX_DSMCFG5_REG		0x34
#define AW882XX_DSMCFG6_REG		0x35
#define AW882XX_DSMCFG7_REG		0x36
#define AW882XX_DSMCFG8_REG		0x37
#define AW882XX_TESTIN_REG		0x38
#define AW882XX_TESTOUT_REG		0x39
#define AW882XX_SADCCTRL1_REG		0x3a
#define AW882XX_SADCCTRL2_REG		0x3b
#define AW882XX_SADCCTRL3_REG		0x3c
#define AW882XX_SADCCTRL4_REG		0x3d
#define AW882XX_SADCCTRL5_REG		0x3e
#define AW882XX_SADCCTRL6_REG		0x3f
#define AW882XX_SADCCTRL7_REG		0x40
#define AW882XX_VSNTM1_REG		0x50
#define AW882XX_VSNTM2_REG		0x51
#define AW882XX_ISNCTRL1_REG		0x52
#define AW882XX_ISNCTRL2_REG		0x53
#define AW882XX_PLLCTRL1_REG		0x54
#define AW882XX_PLLCTRL2_REG		0x55
#define AW882XX_PLLCTRL3_REG		0x56
#define AW882XX_CDACTRL1_REG		0x57
#define AW882XX_CDACTRL2_REG		0x58
#define AW882XX_DITHERCFG1_REG		0x59
#define AW882XX_DITHERCFG2_REG		0x5a
#define AW882XX_DITHERCFG3_REG		0x5b
#define AW882XX_CPCTRL_REG		0x5c
#define AW882XX_BSTCTRL1_REG		0x60
#define AW882XX_BSTCTRL2_REG		0x61
#define AW882XX_BSTCTRL3_REG		0x62
#define AW882XX_BSTCTRL4_REG		0x63
#define AW882XX_BSTCTRL5_REG		0x64
#define AW882XX_BSTCTRL6_REG		0x65
#define AW882XX_BSTCTRL7_REG		0x66
#define AW882XX_BSTCTRL8_REG		0x67
#define AW882XX_BSTCTRL9_REG		0x68
#define AW882XX_TM_REG			0x6f
#define AW882XX_TESTCTRL1_REG		0x70
#define AW882XX_TESTCTRL2_REG		0x71
#define AW882XX_EFCTRL1_REG		0x72
#define AW882XX_EFCTRL2_REG		0x73
#define AW882XX_EFWH_REG		0x74
#define AW882XX_EFWM2_REG		0x75
#define AW882XX_EFWM1_REG		0x76
#define AW882XX_EFWL_REG		0x77
#define AW882XX_EFRH4_REG		0x78
#define AW882XX_EFRH3_REG		0x79
#define AW882XX_EFRH2_REG		0x7a
#define AW882XX_EFRH1_REG		0x7b
#define AW882XX_EFRL4_REG		0x7c
#define AW882XX_EFRL3_REG		0x7d
#define AW882XX_EFRL2_REG		0x7e
#define AW882XX_EFRL1_REG		0x7f

#define AW882XX_REG_MAX			0x80

/* Chip ID (register 0x00), read back from the live chip */
#define AW882XX_CHIP_ID_88261		0x2113

/* Writing this to register 0x00 triggers the chip's software reset */
#define AW882XX_SOFT_RESET_VALUE	0x55aa

/* AW882XX_SYSST_REG (0x01) status bits */
#define AW882XX_PLLS_LOCKED		BIT(0)
#define AW882XX_OTHS_OVERTEMP		BIT(1)
#define AW882XX_OCDS_OVERCRRENT		BIT(3)
#define AW882XX_CLKS_STABLE		BIT(4)
#define AW882XX_NOCLKS_NO_CLOCK		BIT(5)
#define AW882XX_SWS_SWITCHING		BIT(8)
#define AW882XX_BSTS_FINISHED		BIT(9)
#define AW882XX_BSTOCS_OVERCURRENT	BIT(11)
#define AW882XX_UVLS_UVLO		BIT(14)

/* Bits that must be set before the PA may be started / unmuted */
#define AW882XX_SYSST_PLL_OK		(AW882XX_CLKS_STABLE | AW882XX_PLLS_LOCKED)
#define AW882XX_SYSST_RUN_OK		(AW882XX_BSTS_FINISHED | AW882XX_SWS_SWITCHING | \
					 AW882XX_CLKS_STABLE | AW882XX_PLLS_LOCKED)

/* AW882XX_SYSCTRL_REG (0x04) */
#define AW882XX_PWDN			BIT(0)
#define AW882XX_AMPPD			BIT(1)
#define AW882XX_HMUTE			BIT(8)
#define AW882XX_ULS_HMUTE		BIT(14)

/* AW882XX_SYSCTRL2_REG (0x05): 10-bit volume code, 6 bit coarse (6 dB/step)
 * and 4 bit fine (0.5 dB/step). 0x3c0 (code 0x0f << 6) is the vendor's mute
 * setting, i.e. 15 * 6 dB = 90 dB of attenuation.
 */
#define AW882XX_VOL_MASK		GENMASK(9, 0)
#define AW882XX_VOL_COARSE_SHIFT	6
#define AW882XX_VOLUME_STEP		48	/* 6 dB in 1/8 dB units */
#define AW882XX_MUTE_VOL		720	/* 90 dB in 1/8 dB units */

/* AW882XX_I2SCTRL3_REG (0x08): I2S TX (feedback) enable */
#define AW882XX_I2STXEN			BIT(6)

/* efuse calibration words: 10-bit signed fields */
#define AW882XX_EF_ICALK_MASK		GENMASK(9, 0)
#define AW882XX_EF_ICALK_SIGN		BIT(9)
#define AW882XX_EF_FIELD_SIGN_EXT	0xfc00
#define AW882XX_CABL_BASE_VALUE		1000
#define AW882XX_VCAL_FACTOR		(1 << 13)

#define AW882XX_SYSST_CHECK_MAX		10
#define AW882XX_CHIPID_RETRIES		5
#define AW882XX_START_RETRIES		5

#define AW882XX_RATES	(SNDRV_PCM_RATE_8000_48000)
#define AW882XX_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | \
			 SNDRV_PCM_FMTBIT_S24_LE | \
			 SNDRV_PCM_FMTBIT_S32_LE)

enum aw882xx_status {
	AW882XX_DEV_PW_OFF = 0,
	AW882XX_DEV_PW_ON,
};

enum aw882xx_start_mode {
	AW882XX_ASYNC_START = 0,
	AW882XX_SYNC_START,
};

struct aw882xx {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *irq_gpio;
	struct mutex lock;
	struct delayed_work start_work;

	unsigned int chip_id;
	unsigned int volume;		/* 1/8 dB units, 0..AW882XX_MUTE_VOL */
	unsigned int status;
};

#endif /* __AW882XX_H__ */
