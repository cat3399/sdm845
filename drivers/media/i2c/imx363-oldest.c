// SPDX-License-Identifier: GPL-2.0
/*
 * FIXME copyright stuffs
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <asm/unaligned.h>

#define IMX363_REG_VALUE_08BIT		1
#define IMX363_REG_VALUE_16BIT		2

/* External clock frequency is 24.0M */
#define IMX363_XCLK_FREQ		24000000 //------------------------correct
/* Half of per-lane speed in Mbps (DDR) */
#define IMX363_DEFAULT_LINK_FREQ	504000000
/* currently only 2-lane operation is supported */
#define IMX363_NUM_LANES		4 //------------------------correct
/* no clue why this is 10, but it is */
#define IMX363_BITS_PER_SAMPLE		10
/* Pixel rate is fixed at 364.8M for all the modes */
#define IMX363_PIXEL_RATE		403200000 // (IMX363_DEFAULT_LINK_FREQ * 2 * IMX363_NUM_LANES / IMX363_BITS_PER_SAMPLE)

/* Register map */

#define IMX363_REG_MODE_SELECT				0x0100

#define IMX363_REG_ORIENTATION				0x0101

#define IMX363_REG_EXCK_FREQ_H				0x0136 // integer part
#define IMX363_REG_EXCK_FREQ_L				0x0137 // fractional part
#define IMX363_REG_CSI_DT_FMT				0x0112 //16 bits
#define IMX363_REG_CSI_LANE_MODE			0x0114

#define IMX363_REG_HDR_MODE				0x0220
#define IMX363_REG_HDR_RESO_REDU_HHDR_RESO_REDU_V	0x0221

// V-Timing controls
#define IMX363_REG_FLL							0x0340
#define IMX363_FLL_MAX							0xFFFF

#define IMX363_REG_LINE_LENGTH_PCK_H			0x0342 //present in downstream
#define IMX363_REG_LINE_LENGTH_PCK_L			0x0343 //present in downstream

#define IMX363_REG_X_EVN_INC				0x0381
#define IMX363_REG_X_ODD_INC				0x0383
#define IMX363_REG_Y_EVN_INC				0x0385
#define IMX363_REG_Y_ODD_INC				0x0387

#define IMX363_REG_BINNING_MODE				0x0900
#define IMX363_REG_BINNING_TYPE				0x0901

#define IMX363_REG_X_ADDR_START_H			0x0344 
#define IMX363_REG_X_ADDR_START_L			0x0345 
#define IMX363_REG_Y_ADDR_START_H			0x0346
#define IMX363_REG_Y_ADDR_START_L			0x0347
#define IMX363_REG_X_ADDR_END_H				0x0348
#define IMX363_REG_X_ADDR_END_L				0x0349
#define IMX363_REG_Y_ADDR_END_H				0x034A
#define IMX363_REG_Y_ADDR_END_L				0x034B
#define IMX363_REG_X_OUT_SIZE_H				0x034C
#define IMX363_REG_X_OUT_SIZE_L				0x034D
#define IMX363_REG_Y_OUT_SIZE_H				0x034E
#define IMX363_REG_Y_OUT_SIZE_L				0x034F

#define IMX363_REG_DIG_CROP_X_OFFSET_H			0x0408
#define IMX363_REG_DIG_CROP_X_OFFSET_L			0x0409
#define IMX363_REG_DIG_CROP_Y_OFFSET_H			0x040A
#define IMX363_REG_DIG_CROP_Y_OFFSET_L			0x040B
#define IMX363_REG_DIG_CROP_IMAGE_WIDTH_H			0x040C
#define IMX363_REG_DIG_CROP_IMAGE_WIDTH_L			0x040D
#define IMX363_REG_DIG_CROP_IMAGE_HEIGHT_H		0x040E
#define IMX363_REG_DIG_CROP_IMAGE_HEIGHT_L		0x040F

#define IMX363_REG_IVTSXCK_DIV				0x0301
#define IMX363_REG_IVTSYCK_DIV				0x0303
#define IMX363_REG_PREPLLCK_IVT_DIV			0x0305
#define IMX363_REG_PLL_IVT_MPY_H			0x0306
#define IMX363_REG_PLL_IVT_MPY_L			0x0307
#define IMX363_REG_IOPPXCK_DIV				0x0309
#define IMX363_REG_IOPSYCK_DIV				0x030B
#define IMX363_REG_PREPLLCK_IOP_DIV			0x030D
#define IMX363_REG_PLL_IOP_MPY_H			0x030E
#define IMX363_REG_PLL_IOP_MPY_L			0x030F
#define IMX363_REG_PLL_MULT_DRIV			0x0310

#define IMX363_REG_COARSE_INTEG_TIME			0x0202 // 16 bits
#define IMX363_REG_ST_COARSE_INTEG_TIME			0x0224 // 16 bits
#define IMX363_REG_ANA_GAIN_GLOBAL			0x0204 // 16 bits
#define IMX363_REG_ST_ANA_GAIN_GLOBAL			0x0216 // 16 bits
#define IMX363_REG_DIG_GAIN_GLOBAL			0x020E // 16 bits
#define IMX363_REG_ST_DIG_GAIN_GLOBAL			0x0226 // 16 bits
	
#define IMX363_REG_DPHY_CTRL				0x0808

#define IMX363_REG_DUAL_PD_OUT_MODE			0x31A0
#define IMX363_REG_PREFER_DIG_BIN_H			0x31A6

/* register values */

/* DPHY control */
#define IMX363_DPHY_CTRL_AUTO		0x00
#define IMX363_DPHY_CTRL_MANUAL		0x01

/* Configuration happens in standby mode */
#define IMX363_MODE_STANDBY		0x00
#define IMX363_MODE_STREAMING		0x01

/* Chip ID */
#define IMX363_REG_CHIP_ID		0x0016
#define IMX363_CHIP_ID			0x0363

/* Number of CSI lines connected */
#define IMX363_CSI_LANE_NUM_2		0x1
#define IMX363_CSI_LANE_NUM_4		0x3

#define IMX363_VBLANK_MIN		4

/* HBLANK control - read only */
#define IMX363_PPL_DEFAULT		3448

/* Analog gain control */
#define IMX363_ANA_GAIN_MIN		0
#define IMX363_ANA_GAIN_MAX		448
#define IMX363_ANA_GAIN_STEP		1
#define IMX363_ANA_GAIN_DEFAULT		0

/* Exposure control */
#define IMX363_EXPOSURE_MIN		4
#define IMX363_EXPOSURE_STEP		1
#define IMX363_EXPOSURE_DEFAULT		0x0640
#define IMX363_EXPOSURE_MAX		65535

/* Digital gain control */
#define IMX363_DGTL_GAIN_MIN		256
#define IMX363_DGTL_GAIN_MAX		4095
#define IMX363_DGTL_GAIN_DEFAULT	256
#define IMX363_DGTL_GAIN_STEP		1

/* Binning types */
#define IMX363_BINNING_NONE		0x00
#define IMX363_BINNING_V2H2		0x22
#define IMX363_BINNING_V2H4		0x42

/* Data format to use for transmission */
#define IMX363_CSI_DATA_FORMAT_RAW10	0x0A0A

/* phase data */
#define DUAL_PD_OUT_DISABLE 0x02

/* IMX363 native and active pixel array size. */
// #define IMX363_NATIVE_WIDTH		3296U
// #define IMX363_NATIVE_HEIGHT		2480U
// #define IMX363_PIXEL_ARRAY_LEFT		8U
// #define IMX363_PIXEL_ARRAY_TOP		8U
// #define IMX363_PIXEL_ARRAY_WIDTH	3280U
// #define IMX363_PIXEL_ARRAY_HEIGHT	2464U

/* Calculate start and end address for simple centered crop */

// #define IMX363_READOUT_CROP_CENTER_START(readout_len, total_len)	((total_len - readout_len)/2)
// #define IMX363_READOUT_CROP_CENTER_END(readout_len, total_len)		((total_len - 1) - (total_len - readout_len)/2)

// #define IMX363_READOUT_CROP_CENTER_LEFT(readout_width)		IMX363_READOUT_CROP_CENTER_START(readout_width, IMX363_PIXEL_ARRAY_WIDTH)
// #define IMX363_READOUT_CROP_CENTER_RIGHT(readout_width)		IMX363_READOUT_CROP_CENTER_END(readout_width, IMX363_PIXEL_ARRAY_WIDTH)
// #define IMX363_READOUT_CROP_CENTER_TOP(readout_height)		IMX363_READOUT_CROP_CENTER_START(readout_height, IMX363_PIXEL_ARRAY_HEIGHT)
// #define IMX363_READOUT_CROP_CENTER_BOTTOM(readout_height)	IMX363_READOUT_CROP_CENTER_END(readout_height, IMX363_PIXEL_ARRAY_HEIGHT)

/* A macro to generate a register list for changing the mode */

/* #define IMX363_MODE_REGS_GENERATE(readout_width, readout_height, pic_width, pic_height, binning)		\
	{IMX363_REG_X_ADDR_START, IMX363_REG_VALUE_16BIT, IMX363_READOUT_CROP_CENTER_LEFT(readout_width)},	\
	{IMX363_REG_X_ADDR_END, IMX363_REG_VALUE_16BIT, IMX363_READOUT_CROP_CENTER_RIGHT(readout_width)},	\
	{IMX363_REG_Y_ADDR_START, IMX363_REG_VALUE_16BIT, IMX363_READOUT_CROP_CENTER_TOP(readout_height)},	\
	{IMX363_REG_Y_ADDR_END, IMX363_REG_VALUE_16BIT, IMX363_READOUT_CROP_CENTER_BOTTOM(readout_height)},	\
	{IMX363_REG_X_OUT_SIZE, IMX363_REG_VALUE_16BIT, pic_width},						\
	{IMX363_REG_Y_OUT_SIZE, IMX363_REG_VALUE_16BIT, pic_height},						\
														\
	{IMX363_REG_DIG_CROP_X_OFFSET, IMX363_REG_VALUE_16BIT, 0},						\
	{IMX363_REG_DIG_CROP_Y_OFFSET, IMX363_REG_VALUE_16BIT, 0},						\
	{IMX363_REG_DIG_CROP_IMAGE_WIDTH, IMX363_REG_VALUE_16BIT, pic_width},					\
	{IMX363_REG_DIG_CROP_IMAGE_HEIGHT, IMX363_REG_VALUE_16BIT, pic_height},					\
														\
	{IMX363_REG_BINNING_TYPE, IMX363_REG_VALUE_08BIT, binning},			*/			

//	{IMX363_REG_TP_WINDOW_WIDTH_HIG, IMX363_REG_VALUE_16BIT, pic_width},
//	{IMX363_REG_TP_WINDOW_HEIGHT_HIG, IMX363_REG_VALUE_16BIT, pic_height},

struct imx363_reg {
	u16 address;
	u8 val_len;
	u16 val;
};

struct imx363_reg_list {
	unsigned int num_of_regs;
	const struct imx363_reg *regs;
};

/* Mode : resolution and related config&values */
struct imx363_mode {
	/* Frame width */
	unsigned int width;
	/* Frame height */
	unsigned int height;

	/* Analog crop rectangle. */
	// struct v4l2_rect crop;

	/* V-timing */
	u32 fll_def;
	u32 fll_min;

	/* Default register values */
	struct imx363_reg_list reg_list;
};

static const struct imx363_reg setup_regs[] = {
	{IMX363_REG_MODE_SELECT, IMX363_REG_VALUE_08BIT, IMX363_MODE_STANDBY}, // standby //------------------------correct
	{IMX363_REG_CSI_LANE_MODE, IMX363_REG_VALUE_08BIT, IMX363_CSI_LANE_NUM_4}, //------------------------correct
	{IMX363_REG_EXCK_FREQ_H, IMX363_REG_VALUE_08BIT, 0x18}, // 24.00 Mhz = 18.00h //------------------------correct
	{IMX363_REG_EXCK_FREQ_L, IMX363_REG_VALUE_08BIT, 0x00}, // 24.00 Mhz = 18.00h //------------------------correct

	//Magical Regs & Values - 1
	{0x31A3, IMX363_REG_VALUE_08BIT, 0x00}, //------------------------correct
	{0x64D4, IMX363_REG_VALUE_08BIT, 0x01}, //------------------------correct
	{0x64D5, IMX363_REG_VALUE_08BIT, 0xAA}, //------------------------correct
	{0x64D6, IMX363_REG_VALUE_08BIT, 0x01}, //------------------------correct
	{0x64D7, IMX363_REG_VALUE_08BIT, 0xA9}, //------------------------correct
	{0x64D8, IMX363_REG_VALUE_08BIT, 0x01}, //------------------------correct
	{0x64D9, IMX363_REG_VALUE_08BIT, 0xA5}, //------------------------correct
	{0x64DA, IMX363_REG_VALUE_08BIT, 0x01}, //------------------------correct
	{0x64DB, IMX363_REG_VALUE_08BIT, 0xA1}, //------------------------correct
	{0x720A, IMX363_REG_VALUE_08BIT, 0x24}, //------------------------correct
	{0x720B, IMX363_REG_VALUE_08BIT, 0x89}, //------------------------correct
	{0x720C, IMX363_REG_VALUE_08BIT, 0x85}, //------------------------correct
	{0x720D, IMX363_REG_VALUE_08BIT, 0xA1}, //------------------------correct
	{0x720E, IMX363_REG_VALUE_08BIT, 0x6E}, //------------------------correct
	{0x729C, IMX363_REG_VALUE_08BIT, 0x59}, //------------------------correct
	{0x817C, IMX363_REG_VALUE_08BIT, 0xFF}, //------------------------correct
	{0x817D, IMX363_REG_VALUE_08BIT, 0x80}, //------------------------correct
	{0x9348, IMX363_REG_VALUE_08BIT, 0x96}, //------------------------correct
	{0x934B, IMX363_REG_VALUE_08BIT, 0x8C}, //------------------------correct
	{0x934C, IMX363_REG_VALUE_08BIT, 0x82}, //------------------------correct
	{0x9353, IMX363_REG_VALUE_08BIT, 0xAA}, //------------------------correct
	{0x9354, IMX363_REG_VALUE_08BIT, 0xAA}, //------------------------correct

	{IMX363_REG_CSI_DT_FMT, IMX363_REG_VALUE_16BIT, IMX363_CSI_DATA_FORMAT_RAW10}, //--------------correct
	{IMX363_REG_FLL, IMX363_REG_VALUE_16BIT, 0x0C44}, //----------------correct
	{IMX363_REG_LINE_LENGTH_PCK_H, IMX363_REG_VALUE_08BIT, 0x22}, //----------------correct
	{IMX363_REG_LINE_LENGTH_PCK_L, IMX363_REG_VALUE_08BIT, 0x80}, //----------------correct

	{IMX363_REG_X_EVN_INC, IMX363_REG_VALUE_08BIT, 0x01}, //----------------correct
	{IMX363_REG_X_ODD_INC, IMX363_REG_VALUE_08BIT, 0x01}, //----------------correct
	{IMX363_REG_Y_EVN_INC, IMX363_REG_VALUE_08BIT, 0x01}, //----------------correct
	{IMX363_REG_Y_ODD_INC, IMX363_REG_VALUE_08BIT, 0x01}, //----------------correct
	{IMX363_REG_BINNING_MODE, IMX363_REG_VALUE_08BIT, 0x00}, //-------------correct
	{IMX363_REG_BINNING_TYPE, IMX363_REG_VALUE_08BIT, 0x11}, //-------------correct

	//Magical Regs & Values - 2
	{0x30F4, IMX363_REG_VALUE_08BIT, 0x02}, //------------------------correct
	{0x30F5, IMX363_REG_VALUE_08BIT, 0x80}, //------------------------correct
	{0x31A5, IMX363_REG_VALUE_08BIT, 0x00}, //------------------------correct
	{0x31A6, IMX363_REG_VALUE_08BIT, 0x00}, //------------------------correct
	{0x560F, IMX363_REG_VALUE_08BIT, 0xBE}, //------------------------correct
	{0x5856, IMX363_REG_VALUE_08BIT, 0x08}, //------------------------correct
	{0x58D0, IMX363_REG_VALUE_08BIT, 0x10}, //------------------------correct
	{0x734A, IMX363_REG_VALUE_08BIT, 0x01}, //------------------------correct
	{0x734F, IMX363_REG_VALUE_08BIT, 0x2B}, //------------------------correct
	{0x7441, IMX363_REG_VALUE_08BIT, 0x55}, //------------------------correct
	{0x7914, IMX363_REG_VALUE_08BIT, 0x03}, //------------------------correct
	{0x7928, IMX363_REG_VALUE_08BIT, 0x04}, //------------------------correct
	{0x7929, IMX363_REG_VALUE_08BIT, 0x04}, //------------------------correct
	{0x793F, IMX363_REG_VALUE_08BIT, 0x03}, //------------------------correct
	{0xBC7B, IMX363_REG_VALUE_08BIT, 0x18}, //------------------------correct, 1st run not present, 2nd run present, not sure if needed

	{IMX363_REG_X_ADDR_START_H, IMX363_REG_VALUE_08BIT, 0x00}, //-------------correct
	{IMX363_REG_X_ADDR_START_L, IMX363_REG_VALUE_08BIT, 0x00}, //-------------correct
	{IMX363_REG_Y_ADDR_START_H, IMX363_REG_VALUE_08BIT, 0x00}, //-------------correct
	{IMX363_REG_Y_ADDR_START_L, IMX363_REG_VALUE_08BIT, 0x00}, //-------------correct
	// Start = (0,0)
	{IMX363_REG_X_ADDR_END_H, IMX363_REG_VALUE_08BIT, 0x0F}, //-------------correct
	{IMX363_REG_X_ADDR_END_L, IMX363_REG_VALUE_08BIT, 0xBF}, //-------------correct
	{IMX363_REG_Y_ADDR_END_H, IMX363_REG_VALUE_08BIT, 0x0B}, //-------------correct
	{IMX363_REG_Y_ADDR_END_L, IMX363_REG_VALUE_08BIT, 0xCF}, //-------------NA, 
	// assuming 0xCF. Since X pixel ends at 4031 (0x0FBF). then its obvious Y pixel ends at 3023 (0x0BCF)
	// End = (4031,3023)
	{IMX363_REG_X_OUT_SIZE_H, IMX363_REG_VALUE_08BIT, 0x0F}, //-------------correct
	{IMX363_REG_X_OUT_SIZE_L, IMX363_REG_VALUE_08BIT, 0xC0}, //-------------NA, 
	// assuming 0xCF. Since X pixel size is 4032 (0x0FC0)
	{IMX363_REG_Y_OUT_SIZE_H, IMX363_REG_VALUE_08BIT, 0x0B}, //-------------correct
	{IMX363_REG_Y_OUT_SIZE_L, IMX363_REG_VALUE_08BIT, 0xD0}, //-------------NA, 
	// assuming 0xD0. Since Y pixel size is 3024 (0x0BD0)
	{IMX363_REG_DIG_CROP_X_OFFSET_H, IMX363_REG_VALUE_08BIT, 0x00}, //-------------correct
	{IMX363_REG_DIG_CROP_X_OFFSET_L, IMX363_REG_VALUE_08BIT, 0x00}, //-------------correct
	{IMX363_REG_DIG_CROP_Y_OFFSET_H, IMX363_REG_VALUE_08BIT, 0x00}, //-------------correct
	{IMX363_REG_DIG_CROP_Y_OFFSET_L, IMX363_REG_VALUE_08BIT, 0x00}, //-------------correct
	{IMX363_REG_DIG_CROP_IMAGE_WIDTH_H, IMX363_REG_VALUE_08BIT, 0x0F}, //-------------correct
	{IMX363_REG_DIG_CROP_IMAGE_WIDTH_L, IMX363_REG_VALUE_08BIT, 0xC0}, //-------------correct
	{IMX363_REG_DIG_CROP_IMAGE_HEIGHT_H, IMX363_REG_VALUE_08BIT, 0x0B}, //-------------correct
	{IMX363_REG_DIG_CROP_IMAGE_HEIGHT_L, IMX363_REG_VALUE_08BIT, 0xD0}, //-------------correct
	
	{IMX363_REG_IVTSXCK_DIV, IMX363_REG_VALUE_08BIT, 0x03}, //-------------correct
	{IMX363_REG_IVTSYCK_DIV, IMX363_REG_VALUE_08BIT, 0x02}, //-------------correct
	{IMX363_REG_PREPLLCK_IVT_DIV, IMX363_REG_VALUE_08BIT, 0x04}, //-----------correct
	{IMX363_REG_PLL_IVT_MPY_H, IMX363_REG_VALUE_08BIT, 0x00}, //-----------correct
	{IMX363_REG_PLL_IVT_MPY_L, IMX363_REG_VALUE_08BIT, 0xD0}, //-----------correct
	{IMX363_REG_IOPPXCK_DIV, IMX363_REG_VALUE_08BIT, 0x0A}, //-----------correct
	{IMX363_REG_IOPSYCK_DIV, IMX363_REG_VALUE_08BIT, 0x01}, //-----------correct
	{IMX363_REG_PREPLLCK_IOP_DIV, IMX363_REG_VALUE_08BIT, 0x04}, //------correct
	{IMX363_REG_PLL_IOP_MPY_H, IMX363_REG_VALUE_08BIT, 0x00}, //------------correct
	{IMX363_REG_PLL_IOP_MPY_L, IMX363_REG_VALUE_08BIT, 0xE6}, //-------------correct
	{IMX363_REG_PLL_MULT_DRIV, IMX363_REG_VALUE_08BIT, 0x01}, //-------------correct

	{IMX363_REG_COARSE_INTEG_TIME, IMX363_REG_VALUE_16BIT, IMX363_EXPOSURE_DEFAULT}, //-------------correct
	{IMX363_REG_ST_COARSE_INTEG_TIME, IMX363_REG_VALUE_16BIT, 0x01F4}, //-------------correct
	{IMX363_REG_ANA_GAIN_GLOBAL, IMX363_REG_VALUE_16BIT, 0x0000}, //-------------correct
	{IMX363_REG_ST_ANA_GAIN_GLOBAL, IMX363_REG_VALUE_16BIT, 0x0000}, //-------------correct
	{IMX363_REG_DIG_GAIN_GLOBAL, IMX363_REG_VALUE_16BIT, 0x0100}, //-------------correct
	{IMX363_REG_ST_DIG_GAIN_GLOBAL, IMX363_REG_VALUE_16BIT, 0x0100}, //-------------correct


	{IMX363_REG_HDR_MODE, IMX363_REG_VALUE_08BIT, 0x00}, //--------------------correct
	{IMX363_REG_HDR_RESO_REDU_HHDR_RESO_REDU_V, IMX363_REG_VALUE_08BIT, 0x11}, //--------------------correct
	{IMX363_REG_DPHY_CTRL, IMX363_REG_VALUE_08BIT, IMX363_DPHY_CTRL_AUTO}, //------------NA in downstream. not even sure if this is valid address. its not present in imx319 too. hopefully this is not needed and its automatically in this mode or whatever.

	// I think some of these values below are related to camera actuator or something??
	// {0x2, IMX363_REG_VALUE_08BIT, 0x00}, //------------------------dont know, present in downstream, 1 time
	// {0x3008, IMX363_REG_VALUE_08BIT, 0x00}, //------------------------dont know, present in downstream, many times but always 0x00
	// {0x0, IMX363_REG_VALUE_16BIT, 0x5c40}, //------------------------dont know, present in downstream, 1 time
	// 0x104 - dont know, present in downstream, value keeps changing btw 0 and 1 when streaming
	// whenever stream is stopped in downstream, there is this sequence below. noting it here for reference
	// CAM_DBG: CAM-CCI: cam_cci_data_queue: 741: cmd_size 1 addr 0x0 data 0x3940
	// CAM_DBG: CAM-CCI: cam_cci_data_queue: 741: cmd_size 1 addr 0x0 data 0x740
	// CAM_DBG: CAM-CCI: cam_cci_data_queue: 741: cmd_size 1 addr 0x0 data 0x240
	// CAM_DBG: CAM-CCI: cam_cci_data_queue: 741: cmd_size 1 addr 0x0 data 0x0
};

static const struct imx363_reg_list setup_reg_list = {
	.num_of_regs = ARRAY_SIZE(setup_regs),
	.regs = setup_regs,
};

// static const struct imx363_reg mode_4032x3024_regs[] = {
// 	IMX363_MODE_REGS_GENERATE(4032, 3024, 4032, 3024, IMX363_BINNING_NONE)
// };

static const struct imx363_reg raw10_framefmt_regs[] = {
	{IMX363_REG_CSI_DT_FMT, IMX363_REG_VALUE_16BIT, IMX363_CSI_DATA_FORMAT_RAW10},
	{IMX363_REG_IOPPXCK_DIV, IMX363_REG_VALUE_08BIT, 0x0A},
};

static const s64 imx363_link_freq_menu[] = {
	IMX363_DEFAULT_LINK_FREQ,
};

/* regulator supplies */
static const char * const imx363_supply_name[] = {
	/* Supplies can be enabled in any order */
	"VANA",  /* Analog (2.8V) supply */
	"VDIG",  /* Digital Core (1.8V) supply */
	"VDDL",  /* IF (1.2V) supply */
};

#define IMX363_NUM_SUPPLIES ARRAY_SIZE(imx363_supply_name)

/*
 * The supported formats.
 * This table MUST contain 4 entries per format, to cover the various flip
 * combinations in the order
 * - no flip
 * - h flip
 * - v flip
 * - h&v flips
 */
static const u32 codes[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,

	MEDIA_BUS_FMT_SRGGB8_1X8,
	MEDIA_BUS_FMT_SGRBG8_1X8,
	MEDIA_BUS_FMT_SGBRG8_1X8,
	MEDIA_BUS_FMT_SBGGR8_1X8,
};

/*
 * Initialisation delay between XCLR low->high and the moment when the sensor
 * can start capture (i.e. can leave software stanby) must be not less than:
 *   t4 + max(t5, t6 + <time to initialize the sensor register over I2C>)
 * where
 *   t4 is fixed, and is max 200uS,
 *   t5 is fixed, and is 6000uS,
 *   t6 depends on the sensor external clock, and is max 32000 clock periods.
 * As per sensor datasheet, the external clock must be from 6MHz to 27MHz.
 * So for any acceptable external clock t6 is always within the range of
 * 1185 to 5333 uS, and is always less than t5.
 * For this reason this is always safe to wait (t4 + t5) = 6200 uS, then
 * initialize the sensor over I2C, and then exit the software standby.
 *
 * This start-up time can be optimized a bit more, if we start the writes
 * over I2C after (t4+t6), but before (t4+t5) expires. But then sensor
 * initialization over I2C may complete before (t4+t5) expires, and we must
 * ensure that capture is not started before (t4+t5).
 *
 * This delay doesn't account for the power supply startup time. If needed,
 * this should be taken care of via the regulator framework. E.g. in the
 * case of DT for regulator-fixed one should define the startup-delay-us
 * property.
 */
#define IMX363_XCLR_MIN_DELAY_US	6200
#define IMX363_XCLR_DELAY_RANGE_US	1000

/* Mode configs */
static const struct imx363_mode supported_modes[] = {
	{
		/* 12.2MPix 30fps mode */
		.width = 4032,
		.height = 3024,
		// .crop = {
		// 	.left = 0,
		// 	.top = 0,
		// 	.width = 4032,
		// 	.height = 3024
		// },
		.fll_def = 3140,
		.fll_min = 3140,
		// .reg_list = {
		// 	.num_of_regs = ARRAY_SIZE(mode_4032x3024_regs),
		// 	.regs = mode_4032x3024_regs,
		// },
	},
};

struct imx363 {
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_mbus_framefmt fmt;

	struct clk *xclk; /* system clock to IMX363 */
	u32 xclk_freq;

	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[IMX363_NUM_SUPPLIES];

	struct v4l2_ctrl_handler ctrl_handler;
	/* V4L2 Controls */
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;

	/* Current mode */
	const struct imx363_mode *mode;

	/*
	 * Mutex for serialized access:
	 * Protect sensor module set pad format and start/stop streaming safely.
	 */
	struct mutex mutex;

	/* Streaming on/off */
	bool streaming;
};

static inline struct imx363 *to_imx363(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct imx363, sd);
}

/* Read registers up to 2 at a time */
static int imx363_read_reg(struct imx363 *imx363, u16 reg, u32 len, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	struct i2c_msg msgs[2];
	u8 addr_buf[2] = { reg >> 8, reg & 0xff };
	u8 data_buf[4] = { 0, };
	int ret;

	if (len > 4){
        dev_err(&client->dev, "len > 4\n");
        return -EINVAL;
    }

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_buf[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs)){
        dev_err(&client->dev, "ret != ARRAY_SIZE(msgs)\n");
        return -EIO;
    }

	*val = get_unaligned_be32(data_buf);

	return 0;
}

// /* Write registers up to 2 at a time */
static int imx363_write_reg(struct imx363 *imx363, u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	u8 buf[6];

	if (len > 4)
		return -EINVAL;

	put_unaligned_be16(reg, buf);
	put_unaligned_be32(val << (8 * (4 - len)), buf + 2);
	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

/* Write a list of registers */
static int imx363_write_regs(struct imx363 *imx363,
			     const struct imx363_reg *regs, u32 len)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = imx363_write_reg(imx363, regs[i].address, regs[i].val_len, regs[i].val);
		if (ret) {
			dev_err_ratelimited(&client->dev,
					    "Failed to write reg 0x%4.4x. error = %d\n",
					    regs[i].address, ret);

			return ret;
		}
	}

	return 0;
}

/* Get bayer order based on flip setting. */
static u32 imx363_get_format_code(struct imx363 *imx363, u32 code)
{
	unsigned int i;

	lockdep_assert_held(&imx363->mutex);

	for (i = 0; i < ARRAY_SIZE(codes); i++)
		if (codes[i] == code)
			break;

	if (i >= ARRAY_SIZE(codes))
		i = 0;

	i = (i & ~3) | (imx363->vflip->val ? 2 : 0) |
	    (imx363->hflip->val ? 1 : 0);

	return codes[i];
}

static void imx363_set_default_format(struct imx363 *imx363)
{
	struct v4l2_mbus_framefmt *fmt;

	fmt = &imx363->fmt;
	fmt->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(true,
							  fmt->colorspace,
							  fmt->ycbcr_enc);
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
	fmt->width = supported_modes[0].width;
	fmt->height = supported_modes[0].height;
	fmt->field = V4L2_FIELD_NONE;
}

static int imx363_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx363 *imx363 = to_imx363(sd);
	struct v4l2_mbus_framefmt *try_fmt =
		v4l2_subdev_state_get_format(fh->state, 0);
	// struct v4l2_rect *try_crop;

	mutex_lock(&imx363->mutex);

	/* Initialize try_fmt */
	try_fmt->width = supported_modes[0].width;
	try_fmt->height = supported_modes[0].height;
	try_fmt->code = imx363_get_format_code(imx363,
					       MEDIA_BUS_FMT_SRGGB10_1X10);
	try_fmt->field = V4L2_FIELD_NONE;

	/* Initialize try_crop rectangle. */
	// try_crop = v4l2_subdev_get_try_crop(sd, fh->pad, 0);
	// try_crop->top = IMX363_PIXEL_ARRAY_TOP;
	// try_crop->left = IMX363_PIXEL_ARRAY_LEFT;
	// try_crop->width = IMX363_PIXEL_ARRAY_WIDTH;
	// try_crop->height = IMX363_PIXEL_ARRAY_HEIGHT;

	mutex_unlock(&imx363->mutex);

	return 0;
}

static int imx363_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx363 *imx363 =
		container_of(ctrl->handler, struct imx363, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		int exposure_max, exposure_def;

		/* Update max exposure while meeting expected vblanking */
		exposure_max = imx363->mode->height + ctrl->val - 4;
		exposure_def = (exposure_max < IMX363_EXPOSURE_DEFAULT) ?
			exposure_max : IMX363_EXPOSURE_DEFAULT;
		__v4l2_ctrl_modify_range(imx363->exposure,
					 imx363->exposure->minimum,
					 exposure_max, imx363->exposure->step,
					 exposure_def);
	}

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = imx363_write_reg(imx363, IMX363_REG_ANA_GAIN_GLOBAL,
				       IMX363_REG_VALUE_08BIT, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
		ret = imx363_write_reg(imx363, IMX363_REG_COARSE_INTEG_TIME,
				       IMX363_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = imx363_write_reg(imx363, IMX363_REG_DIG_GAIN_GLOBAL,
				       IMX363_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = imx363_write_reg(imx363, IMX363_REG_ORIENTATION, 1,
				       imx363->hflip->val |
				       imx363->vflip->val << 1);
		break;
	case V4L2_CID_VBLANK:
		ret = imx363_write_reg(imx363, IMX363_REG_FLL,
				       IMX363_REG_VALUE_16BIT,
				       imx363->mode->height + ctrl->val);
		break;
	default:
		dev_info(&client->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx363_ctrl_ops = {
	.s_ctrl = imx363_set_ctrl,
};

static int imx363_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx363 *imx363 = to_imx363(sd);

	if (code->index >= (ARRAY_SIZE(codes) / 4))
		return -EINVAL;

	code->code = imx363_get_format_code(imx363, codes[code->index * 4]);

	return 0;
}

static int imx363_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx363 *imx363 = to_imx363(sd);

	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != imx363_get_format_code(imx363, fse->code))
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static void imx363_reset_colorspace(struct v4l2_mbus_framefmt *fmt)
{
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(true,
							  fmt->colorspace,
							  fmt->ycbcr_enc);
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
}

static void imx363_update_pad_format(struct imx363 *imx363,
				     const struct imx363_mode *mode,
				     struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	imx363_reset_colorspace(&fmt->format);
}

static int __imx363_get_pad_format(struct imx363 *imx363,
				  struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_format *fmt)
{
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		fmt->format = *v4l2_subdev_state_get_format(sd_state,
							    fmt->pad);
	} else {
		imx363_update_pad_format(imx363, imx363->mode, fmt);
		fmt->format.code = imx363_get_format_code(imx363,
							  imx363->fmt.code);
	}

	return 0;
}

static int imx363_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx363 *imx363 = to_imx363(sd);
	int ret;

	mutex_lock(&imx363->mutex);
	ret = __imx363_get_pad_format(imx363, sd_state, fmt);
	mutex_unlock(&imx363->mutex);

	return ret;
}

static int imx363_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx363 *imx363 = to_imx363(sd);
	const struct imx363_mode *mode;
	struct v4l2_mbus_framefmt *framefmt;
	int exposure_max, exposure_def, hblank;
	unsigned int i;
	s32 vblank_def;
	s32 vblank_min;
	u32 height;

	mutex_lock(&imx363->mutex);

	for (i = 0; i < ARRAY_SIZE(codes); i++)
		if (codes[i] == fmt->format.code)
			break;
	if (i >= ARRAY_SIZE(codes))
		i = 0;

	/* Bayer order varies with flips */
	fmt->format.code = imx363_get_format_code(imx363, codes[i]);

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);
	imx363_update_pad_format(imx363, mode, fmt);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		framefmt = v4l2_subdev_state_get_format(sd_state, fmt->pad);
		*framefmt = fmt->format;
	} else if (imx363->mode != mode ||
		   imx363->fmt.code != fmt->format.code) {
		imx363->fmt = fmt->format;
		imx363->mode = mode;
		/* Update limits and set FPS to default */
		height = mode->height;
		vblank_def = mode->fll_def - mode->height;
		vblank_min = mode->fll_min - mode->height;
		height = IMX363_FLL_MAX - height;
		__v4l2_ctrl_modify_range(imx363->vblank, vblank_min, mode->height, 1,
					 vblank_def);
		__v4l2_ctrl_s_ctrl(imx363->vblank, vblank_def);
		/* Update max exposure while meeting expected vblanking */
		exposure_max = mode->fll_def - 4;
		exposure_def = (exposure_max < IMX363_EXPOSURE_DEFAULT) ?
			exposure_max : IMX363_EXPOSURE_DEFAULT;
		__v4l2_ctrl_modify_range(imx363->exposure,
					 imx363->exposure->minimum,
					 exposure_max, imx363->exposure->step,
					 exposure_def);
		/*
		 * Currently PPL is fixed to IMX363_PPL_DEFAULT, so hblank
		 * depends on mode->width only, and is not changeble in any
		 * way other than changing the mode.
		 */
		hblank = IMX363_PPL_DEFAULT - mode->width;
		__v4l2_ctrl_modify_range(imx363->hblank, hblank, hblank, 1,
					 hblank);
	}

	mutex_unlock(&imx363->mutex);

	return 0;
}

static int imx363_set_framefmt(struct imx363 *imx363)
{
	switch (imx363->fmt.code) {
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
	case MEDIA_BUS_FMT_SBGGR10_1X10:
		return imx363_write_regs(imx363, raw10_framefmt_regs,
					ARRAY_SIZE(raw10_framefmt_regs));
	}

	return -EINVAL;
}

// static const struct v4l2_rect *
// __imx363_get_pad_crop(struct imx363 *imx363, struct v4l2_subdev_pad_config *cfg,
// 		      unsigned int pad, enum v4l2_subdev_format_whence which)
// {
// 	switch (which) {
// 	case V4L2_SUBDEV_FORMAT_TRY:
// 		return v4l2_subdev_get_try_crop(&imx363->sd, cfg, pad);
// 	case V4L2_SUBDEV_FORMAT_ACTIVE:
// 		return &imx363->mode->crop;
// 	}

// 	return NULL;
// }

// static int imx363_get_selection(struct v4l2_subdev *sd,
// 				struct v4l2_subdev_pad_config *cfg,
// 				struct v4l2_subdev_selection *sel)
// {
// 	switch (sel->target) {
// 	case V4L2_SEL_TGT_CROP: {
// 		struct imx363 *imx363 = to_imx363(sd);

// 		mutex_lock(&imx363->mutex);
// 		sel->r = *__imx363_get_pad_crop(imx363, cfg, sel->pad,
// 						sel->which);
// 		mutex_unlock(&imx363->mutex);

// 		return 0;
// 	}

// 	case V4L2_SEL_TGT_NATIVE_SIZE:
// 		sel->r.top = 0;
// 		sel->r.left = 0;
// 		sel->r.width = IMX363_NATIVE_WIDTH;
// 		sel->r.height = IMX363_NATIVE_HEIGHT;

// 		return 0;

// 	case V4L2_SEL_TGT_CROP_DEFAULT:
// 		sel->r.top = IMX363_PIXEL_ARRAY_TOP;
// 		sel->r.left = IMX363_PIXEL_ARRAY_LEFT;
// 		sel->r.width = IMX363_PIXEL_ARRAY_WIDTH;
// 		sel->r.height = IMX363_PIXEL_ARRAY_HEIGHT;

// 		return 0;
// 	}

// 	return -EINVAL;
// }

static int imx363_start_streaming(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	const struct imx363_reg_list *reg_list;
	int ret;

	/* Apply default configuration */
	reg_list = &setup_reg_list;
	ret = imx363_write_regs(imx363, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		dev_err(&client->dev, "%s failed to set mode\n", __func__);
		return ret;
	}

	/* Set up registers according to current mode */
	// reg_list = &imx363->mode->reg_list;
	// ret = imx363_write_regs(imx363, reg_list->regs, reg_list->num_of_regs);
	// if (ret) {
	// 	dev_err(&client->dev, "%s failed to set mode\n", __func__);
	// 	return ret;
	// }

	ret = imx363_set_framefmt(imx363);
	if (ret) {
		dev_err(&client->dev, "%s failed to set frame format: %d\n",
			__func__, ret);
		return ret;
	}

	/* Apply customized values from user */
	ret =  __v4l2_ctrl_handler_setup(imx363->sd.ctrl_handler);
	if (ret)
		return ret;

	/* set stream on register */
	return imx363_write_reg(imx363, IMX363_REG_MODE_SELECT,
				IMX363_REG_VALUE_08BIT, IMX363_MODE_STREAMING);
}

static void imx363_stop_streaming(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	int ret;

	/* set stream off register */
	ret = imx363_write_reg(imx363, IMX363_REG_MODE_SELECT,
			       IMX363_REG_VALUE_08BIT, IMX363_MODE_STANDBY);
	if (ret)
		dev_err(&client->dev, "%s failed to set stream\n", __func__);
}

static int imx363_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx363 *imx363 = to_imx363(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&imx363->mutex);
	if (imx363->streaming == enable) {
		mutex_unlock(&imx363->mutex);
		return 0;
	}

	if (enable) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto err_unlock;
		}

		/*
		 * Apply default & customized values
		 * and then start streaming.
		 */
		ret = imx363_start_streaming(imx363);
		if (ret)
			goto err_rpm_put;
	} else {
		imx363_stop_streaming(imx363);
		pm_runtime_put(&client->dev);
	}

	imx363->streaming = enable;

	/* vflip and hflip cannot change during streaming */
	__v4l2_ctrl_grab(imx363->vflip, enable);
	__v4l2_ctrl_grab(imx363->hflip, enable);

	mutex_unlock(&imx363->mutex);

	return ret;

err_rpm_put:
	pm_runtime_put(&client->dev);
err_unlock:
	mutex_unlock(&imx363->mutex);

	return ret;
}

/* Power/clock management functions */
static int imx363_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx363 *imx363 = to_imx363(sd);
	int ret;

	// ret = regulator_bulk_enable(IMX363_NUM_SUPPLIES,
	// 			    imx363->supplies);
	// if (ret) {
	// 	dev_err(&client->dev, "%s: failed to enable regulators\n",
	// 		__func__);
	// 	return ret;
	// }

	ret = clk_prepare_enable(imx363->xclk);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable clock\n",
			__func__);
		goto reg_off;
	}

	gpiod_set_value_cansleep(imx363->reset_gpio, 1);
	usleep_range(IMX363_XCLR_MIN_DELAY_US,
		     IMX363_XCLR_MIN_DELAY_US + IMX363_XCLR_DELAY_RANGE_US);

	return 0;

reg_off:
	regulator_bulk_disable(IMX363_NUM_SUPPLIES, imx363->supplies);

	return ret;
}

static int imx363_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx363 *imx363 = to_imx363(sd);

	gpiod_set_value_cansleep(imx363->reset_gpio, 0);
	regulator_bulk_disable(IMX363_NUM_SUPPLIES, imx363->supplies);
	clk_disable_unprepare(imx363->xclk);

	return 0;
}

static int __maybe_unused imx363_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx363 *imx363 = to_imx363(sd);

	if (imx363->streaming)
		imx363_stop_streaming(imx363);

	return 0;
}

static int __maybe_unused imx363_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx363 *imx363 = to_imx363(sd);
	int ret;

	if (imx363->streaming) {
		ret = imx363_start_streaming(imx363);
		if (ret)
			goto error;
	}

	return 0;

error:
	imx363_stop_streaming(imx363);
	imx363->streaming = 0;

	return ret;
}

// static int imx363_get_regulators(struct imx363 *imx363)
// {
// 	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
// 	unsigned int i;

// 	for (i = 0; i < IMX363_NUM_SUPPLIES; i++)
// 		imx363->supplies[i].supply = imx363_supply_name[i];

// 	return devm_regulator_bulk_get(&client->dev,
// 				       IMX363_NUM_SUPPLIES,
// 				       imx363->supplies);
// }

// /* Verify chip ID */
static int imx363_identify_module(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	int ret;
	u32 val;

	ret = imx363_read_reg(imx363, IMX363_REG_CHIP_ID,
			      IMX363_REG_VALUE_16BIT, &val);
	if (ret) {
		dev_err(&client->dev, "failed to read chip id %x\n",
			IMX363_CHIP_ID);
		return ret;
	}

	if (val != IMX363_CHIP_ID) {
		dev_err(&client->dev, "chip id mismatch: %x!=%x\n",
			IMX363_CHIP_ID, val);
		return -EIO;
	}

    dev_err(&client->dev, "chip id freaking matched! : %x\n", val);

	return 0;
}

static const struct v4l2_subdev_core_ops imx363_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops imx363_video_ops = {
	.s_stream = imx363_set_stream,
};

static const struct v4l2_subdev_pad_ops imx363_pad_ops = {
	.enum_mbus_code = imx363_enum_mbus_code,
	.get_fmt = imx363_get_pad_format,
	.set_fmt = imx363_set_pad_format,
	// .get_selection = imx363_get_selection,
	.enum_frame_size = imx363_enum_frame_size,
};

static const struct v4l2_subdev_ops imx363_subdev_ops = {
	.core = &imx363_core_ops,
	.video = &imx363_video_ops,
	.pad = &imx363_pad_ops,
};

static const struct media_entity_operations imx363_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_internal_ops imx363_internal_ops = {
	.open = imx363_open,
};

/* Initialize control handlers */
static int imx363_init_controls(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct v4l2_fwnode_device_properties props;
	int exposure_max, exposure_def, hblank;
	const struct imx363_mode *mode;
	s64 vblank_def;
	s64 vblank_min;
	int ret;

	ctrl_hdlr = &imx363->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 11);
	if (ret)
		return ret;

	mutex_init(&imx363->mutex);
	ctrl_hdlr->lock = &imx363->mutex;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s v4l2_ctrl_handler_init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	/* By default, PIXEL_RATE is read only */
	imx363->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       IMX363_PIXEL_RATE,
					       IMX363_PIXEL_RATE, 1,
					       IMX363_PIXEL_RATE);
	
	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s imx363->pixel_rate failed (%d)\n",
			__func__, ret);
		goto error;
	}

	imx363->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &imx363_ctrl_ops,
				       V4L2_CID_LINK_FREQ,
				       ARRAY_SIZE(imx363_link_freq_menu) - 1, 0,
				       imx363_link_freq_menu);
	if (imx363->link_freq)
		imx363->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s imx363->link_freq failed (%d)\n",
			__func__, ret);
		goto error;
	}

	/* Initial vblank/hblank/exposure parameters based on current mode */
	mode = imx363->mode;
	vblank_def = mode->fll_def - mode->height;
	vblank_min = mode->fll_min - mode->height;
	imx363->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					   V4L2_CID_VBLANK, vblank_min,
					   IMX363_FLL_MAX - mode->height,
					   1, vblank_def);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s imx363->vblank failed (%d)\n",
			__func__, ret);
		goto error;
	}

	hblank = IMX363_PPL_DEFAULT - imx363->mode->width;
	imx363->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank,
					   1, hblank);
	if (imx363->hblank)
		imx363->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s imx363->hblank failed (%d)\n",
			__func__, ret);
		goto error;
	}

	exposure_max = mode->fll_def - 4;
	exposure_def = (exposure_max < IMX363_EXPOSURE_DEFAULT) ?
		exposure_max : IMX363_EXPOSURE_DEFAULT;
	imx363->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX363_EXPOSURE_MIN, exposure_max,
					     IMX363_EXPOSURE_STEP,
					     exposure_def);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s imx363->exposure failed (%d)\n",
			__func__, ret);
		goto error;
	}

	v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX363_ANA_GAIN_MIN, IMX363_ANA_GAIN_MAX,
			  IMX363_ANA_GAIN_STEP, IMX363_ANA_GAIN_DEFAULT);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s IMX363_ANA_GAIN failed (%d)\n",
			__func__, ret);
		goto error;
	}

	v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  IMX363_DGTL_GAIN_MIN, IMX363_DGTL_GAIN_MAX,
			  IMX363_DGTL_GAIN_STEP, IMX363_DGTL_GAIN_DEFAULT);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s IMX363_DGTL_GAIN failed (%d)\n",
			__func__, ret);
		goto error;
	}

	imx363->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (imx363->hflip)
		imx363->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s imx363->hflip failed (%d)\n",
			__func__, ret);
		goto error;
	}

	imx363->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (imx363->vflip)
		imx363->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s imx363->vflip failed (%d)\n",
			__func__, ret);
		goto error;
	}

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx363_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	imx363->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	mutex_destroy(&imx363->mutex);

	return ret;
}

static void imx363_free_controls(struct imx363 *imx363)
{
	v4l2_ctrl_handler_free(imx363->sd.ctrl_handler);
	mutex_destroy(&imx363->mutex);
}

static int imx363_check_hwcfg(struct device *dev)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret = -EINVAL;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	if (v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg)) {
		dev_err(dev, "could not parse endpoint\n");
		goto error_out;
	}

	/* Check the number of MIPI CSI2 data lanes */
	if (ep_cfg.bus.mipi_csi2.num_data_lanes != IMX363_NUM_LANES) {
		dev_err(dev, "only 4 data lanes are supported\n");
		goto error_out;
	}

	/* Check the link frequency set in device tree */
	if (!ep_cfg.nr_of_link_frequencies) {
		dev_err(dev, "link-frequency property not found in DT\n");
		goto error_out;
	}

	if (ep_cfg.nr_of_link_frequencies != 1 ||
	    ep_cfg.link_frequencies[0] != IMX363_DEFAULT_LINK_FREQ) {
		dev_err(dev, "Link frequency not supported: %lld\n",
			ep_cfg.link_frequencies[0]);
		goto error_out;
	}

	ret = 0;

error_out:
	v4l2_fwnode_endpoint_free(&ep_cfg);
	fwnode_handle_put(endpoint);

	return ret;
}

static int imx363_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx363 *imx363;
	int ret;

	imx363 = devm_kzalloc(&client->dev, sizeof(*imx363), GFP_KERNEL);
	if (!imx363)
    {
        dev_err(dev, "no mem\n");
        return -ENOMEM;
    }

	dev_err(dev, "IMX TEST NUMBER - 3");
	
	v4l2_i2c_subdev_init(&imx363->sd, client, &imx363_subdev_ops);

	/* Check the hardware configuration in device tree */
	ret = imx363_check_hwcfg(dev);
	if (ret) {
		dev_err(dev, "failed to get hwcfg");
		return ret;
	}

	/* Get system clock (xclk) */
	imx363->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(imx363->xclk)) {
		dev_err(dev, "failed to get xclk\n");
		ret = PTR_ERR(imx363->xclk);
		goto error_power_off;
	}

	ret = fwnode_property_read_u32(dev_fwnode(dev), "clock-frequency", &imx363->xclk_freq);
	if (ret) {
		dev_err(dev, "could not get xclk frequency\n");
		goto error_power_off;
	}

	/* this driver currently expects 24MHz; allow 1% tolerance */
	if (imx363->xclk_freq < 23760000 || imx363->xclk_freq > 24240000) {
		dev_err(dev, "xclk frequency not supported: %d Hz\n",
			imx363->xclk_freq);
		ret = -EINVAL;
		goto error_power_off;
	}

	ret = clk_set_rate(imx363->xclk, imx363->xclk_freq);
	if (ret) {
		dev_err(dev, "could not set xclk frequency\n");
		goto error_power_off;
	}


	// ret = imx363_get_regulators(imx363);
	// if (ret) {
	// 	dev_err(dev, "failed to get regulators\n");
	// 	// return ret;
	// }

	/* Request optional enable pin */
	imx363->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);

	/*
	 * The sensor must be powered for imx363_identify_module()
	 * to be able to read the CHIP_ID register
	 */
	ret = imx363_power_on(dev);
	if (ret){
        dev_err(dev, "failed to power on\n");
        goto error_power_off;
    }

	ret = imx363_identify_module(imx363);
	if (ret)
		goto error_power_off;

    dev_info(dev, "probed successfully\n");
	/* Set default mode to max resolution */
	imx363->mode = &supported_modes[0];

	/* sensor doesn't enter LP-11 state upon power up until and unless
	 * streaming is started, so upon power up switch the modes to:
	 * streaming -> standby
	 */
	ret = imx363_write_reg(imx363, IMX363_REG_MODE_SELECT,
			       IMX363_REG_VALUE_08BIT, IMX363_MODE_STREAMING);
	if (ret < 0)
		goto error_power_off;
	usleep_range(100, 110);

	/* put sensor back to standby mode */
	ret = imx363_write_reg(imx363, IMX363_REG_MODE_SELECT,
			       IMX363_REG_VALUE_08BIT, IMX363_MODE_STANDBY);
	if (ret < 0)
		goto error_power_off;
	usleep_range(100, 110);

	ret = imx363_init_controls(imx363);
	if (ret)
		goto error_power_off;

	/* Initialize subdev */
	imx363->sd.internal_ops = &imx363_internal_ops;
	imx363->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
		V4L2_SUBDEV_FL_HAS_EVENTS;
	imx363->sd.entity.ops = &imx363_subdev_entity_ops;
	imx363->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	imx363->pad.flags = MEDIA_PAD_FL_SOURCE;

	/* Initialize default format */
	imx363_set_default_format(imx363);

	ret = media_entity_pads_init(&imx363->sd.entity, 1, &imx363->pad);
	if (ret) {
		dev_err(dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	ret = v4l2_async_register_subdev_sensor(&imx363->sd);
	if (ret < 0) {
		dev_err(dev, "failed to register sensor sub-device: %d\n", ret);
		goto error_media_entity;
	}

	/* Enable runtime PM and turn off the device */
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	return 0;

error_media_entity:
	media_entity_cleanup(&imx363->sd.entity);

error_handler_free:
	imx363_free_controls(imx363);

error_power_off:
	imx363_power_off(dev);

	return ret;
}

static void imx363_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx363 *imx363 = to_imx363(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	imx363_free_controls(imx363);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx363_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static const struct of_device_id imx363_dt_ids[] = {
	{ .compatible = "sony,imx363" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx363_dt_ids);

static const struct dev_pm_ops imx363_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(imx363_suspend, imx363_resume)
	SET_RUNTIME_PM_OPS(imx363_power_off, imx363_power_on, NULL)
};

static struct i2c_driver imx363_i2c_driver = {
	.driver = {
		.name = "imx363",
		.of_match_table	= imx363_dt_ids,
		// .pm = &imx363_pm_ops,
	},
	.probe = imx363_probe,
	.remove = imx363_remove,
};

module_i2c_driver(imx363_i2c_driver);

MODULE_AUTHOR("Mis012");
MODULE_AUTHOR("Joel Selvaraj <jo@jsfamily.in>");
MODULE_DESCRIPTION("Sony IMX363 sensor driver");
MODULE_LICENSE("GPL v2");
