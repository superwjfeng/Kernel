// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 BayLibre, SAS
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 */

#include <linux/bitfield.h>

#include <drm/drm_print.h>
#include <drm/drm_fourcc.h>

#include "meson_drv.h"
#include "meson_registers.h"
#include "meson_viu.h"
#include "meson_rdma.h"
#include "meson_osd_afbcd.h"

/*
 * DOC: Driver for the ARM FrameBuffer Compression Decoders
 *
 * The Amlogic GXM and G12A SoC families embeds an AFBC Decoder,
 * to decode compressed buffers generated by the ARM Mali GPU.
 *
 * For the GXM Family, Amlogic designed their own Decoder, named in
 * the vendor source as "MESON_AFBC", and a single decoder is available
 * for the 2 OSD planes.
 * This decoder is compatible with the AFBC 1.0 specifications and the
 * Mali T820 GPU capabilities.
 * It supports :
 * - basic AFBC buffer for RGB32 only, thus YTR feature is mandatory
 * - SPARSE layout and SPLIT layout
 * - only 16x16 superblock
 *
 * The decoder reads the data from the SDRAM, decodes and sends the
 * decoded pixel stream to the OSD1 Plane pixel composer.
 *
 * For the G12A Family, Amlogic integrated an ARM AFBC Decoder, named
 * in the vendor source as "MALI_AFBC", and the decoder can decode up
 * to 4 surfaces, one for each of the 4 available OSDs.
 * This decoder is compatible with the AFBC 1.2 specifications for the
 * Mali G31 and G52 GPUs.
 * Is supports :
 * - basic AFBC buffer for multiple RGB and YUV pixel formats
 * - SPARSE layout and SPLIT layout
 * - 16x16 and 32x8 "wideblk" superblocks
 * - Tiled header
 *
 * The ARM AFBC Decoder independent from the VPU Pixel Pipeline, so
 * the ARM AFBC Decoder reads the data from the SDRAM then decodes
 * into a private internal physical address where the OSD1 Plane pixel
 * composer unpacks the decoded data.
 */

/* Amlogic AFBC Decoder for GXM Family */

#define OSD1_AFBCD_RGB32	0x15

static int meson_gxm_afbcd_pixel_fmt(u64 modifier, uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return OSD1_AFBCD_RGB32;
	/* TOFIX support mode formats */
	default:
		DRM_DEBUG("unsupported afbc format[%08x]\n", format);
		return -EINVAL;
	}
}

static bool meson_gxm_afbcd_supported_fmt(u64 modifier, uint32_t format)
{
	if (modifier & AFBC_FORMAT_MOD_BLOCK_SIZE_32x8)
		return false;

	if (!(modifier & AFBC_FORMAT_MOD_YTR))
		return false;

	return meson_gxm_afbcd_pixel_fmt(modifier, format) >= 0;
}

static int meson_gxm_afbcd_init(struct meson_drm *priv)
{
	return 0;
}

static int meson_gxm_afbcd_reset(struct meson_drm *priv)
{
	writel_relaxed(VIU_SW_RESET_OSD1_AFBCD,
		       priv->io_base + _REG(VIU_SW_RESET));
	writel_relaxed(0, priv->io_base + _REG(VIU_SW_RESET));

	return 0;
}

static int meson_gxm_afbcd_enable(struct meson_drm *priv)
{
	writel_relaxed(FIELD_PREP(OSD1_AFBCD_ID_FIFO_THRD, 0x40) |
		       OSD1_AFBCD_DEC_ENABLE,
		       priv->io_base + _REG(OSD1_AFBCD_ENABLE));

	return 0;
}

static int meson_gxm_afbcd_disable(struct meson_drm *priv)
{
	writel_bits_relaxed(OSD1_AFBCD_DEC_ENABLE, 0,
			    priv->io_base + _REG(OSD1_AFBCD_ENABLE));

	return 0;
}

static int meson_gxm_afbcd_setup(struct meson_drm *priv)
{
	u32 conv_lbuf_len;
	u32 mode = FIELD_PREP(OSD1_AFBCD_MIF_URGENT, 3) |
		   FIELD_PREP(OSD1_AFBCD_HOLD_LINE_NUM, 4) |
		   FIELD_PREP(OSD1_AFBCD_RGBA_EXCHAN_CTRL, 0x34) |
		   meson_gxm_afbcd_pixel_fmt(priv->afbcd.modifier,
					     priv->afbcd.format);

	if (priv->afbcd.modifier & AFBC_FORMAT_MOD_SPARSE)
		mode |= OSD1_AFBCD_HREG_HALF_BLOCK;

	if (priv->afbcd.modifier & AFBC_FORMAT_MOD_SPLIT)
		mode |= OSD1_AFBCD_HREG_BLOCK_SPLIT;

	writel_relaxed(mode, priv->io_base + _REG(OSD1_AFBCD_MODE));

	writel_relaxed(FIELD_PREP(OSD1_AFBCD_HREG_VSIZE_IN,
				  priv->viu.osd1_width) |
		       FIELD_PREP(OSD1_AFBCD_HREG_HSIZE_IN,
				  priv->viu.osd1_height),
		       priv->io_base + _REG(OSD1_AFBCD_SIZE_IN));

	writel_relaxed(priv->viu.osd1_addr >> 4,
		       priv->io_base + _REG(OSD1_AFBCD_HDR_PTR));
	writel_relaxed(priv->viu.osd1_addr >> 4,
		       priv->io_base + _REG(OSD1_AFBCD_FRAME_PTR));
	/* TOFIX: bits 31:24 are not documented, nor the meaning of 0xe4 */
	writel_relaxed((0xe4 << 24) | (priv->viu.osd1_addr & 0xffffff),
		       priv->io_base + _REG(OSD1_AFBCD_CHROMA_PTR));

	if (priv->viu.osd1_width <= 128)
		conv_lbuf_len = 32;
	else if (priv->viu.osd1_width <= 256)
		conv_lbuf_len = 64;
	else if (priv->viu.osd1_width <= 512)
		conv_lbuf_len = 128;
	else if (priv->viu.osd1_width <= 1024)
		conv_lbuf_len = 256;
	else if (priv->viu.osd1_width <= 2048)
		conv_lbuf_len = 512;
	else
		conv_lbuf_len = 1024;

	writel_relaxed(conv_lbuf_len,
		       priv->io_base + _REG(OSD1_AFBCD_CONV_CTRL));

	writel_relaxed(FIELD_PREP(OSD1_AFBCD_DEC_PIXEL_BGN_H, 0) |
		       FIELD_PREP(OSD1_AFBCD_DEC_PIXEL_END_H,
				  priv->viu.osd1_width - 1),
		       priv->io_base + _REG(OSD1_AFBCD_PIXEL_HSCOPE));

	writel_relaxed(FIELD_PREP(OSD1_AFBCD_DEC_PIXEL_BGN_V, 0) |
		       FIELD_PREP(OSD1_AFBCD_DEC_PIXEL_END_V,
				  priv->viu.osd1_height - 1),
		       priv->io_base + _REG(OSD1_AFBCD_PIXEL_VSCOPE));

	return 0;
}

struct meson_afbcd_ops meson_afbcd_gxm_ops = {
	.init = meson_gxm_afbcd_init,
	.reset = meson_gxm_afbcd_reset,
	.enable = meson_gxm_afbcd_enable,
	.disable = meson_gxm_afbcd_disable,
	.setup = meson_gxm_afbcd_setup,
	.supported_fmt = meson_gxm_afbcd_supported_fmt,
};

/* ARM AFBC Decoder for G12A Family */

/* Amlogic G12A Mali AFBC Decoder supported formats */
enum {
	MAFBC_FMT_RGB565 = 0,
	MAFBC_FMT_RGBA5551,
	MAFBC_FMT_RGBA1010102,
	MAFBC_FMT_YUV420_10B,
	MAFBC_FMT_RGB888,
	MAFBC_FMT_RGBA8888,
	MAFBC_FMT_RGBA4444,
	MAFBC_FMT_R8,
	MAFBC_FMT_RG88,
	MAFBC_FMT_YUV420_8B,
	MAFBC_FMT_YUV422_8B = 11,
	MAFBC_FMT_YUV422_10B = 14,
};

static int meson_g12a_afbcd_pixel_fmt(u64 modifier, uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		/* YTR is forbidden for non XBGR formats */
		if (modifier & AFBC_FORMAT_MOD_YTR)
			return -EINVAL;
	/* fall through */
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return MAFBC_FMT_RGBA8888;
	case DRM_FORMAT_RGB888:
		/* YTR is forbidden for non XBGR formats */
		if (modifier & AFBC_FORMAT_MOD_YTR)
			return -EINVAL;
		return MAFBC_FMT_RGB888;
	case DRM_FORMAT_RGB565:
		/* YTR is forbidden for non XBGR formats */
		if (modifier & AFBC_FORMAT_MOD_YTR)
			return -EINVAL;
		return MAFBC_FMT_RGB565;
	/* TOFIX support mode formats */
	default:
		DRM_DEBUG("unsupported afbc format[%08x]\n", format);
		return -EINVAL;
	}
}

static int meson_g12a_afbcd_bpp(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return 32;
	case DRM_FORMAT_RGB888:
		return 24;
	case DRM_FORMAT_RGB565:
		return 16;
	/* TOFIX support mode formats */
	default:
		DRM_ERROR("unsupported afbc format[%08x]\n", format);
		return 0;
	}
}

static int meson_g12a_afbcd_fmt_to_blk_mode(u64 modifier, uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return OSD_MALI_COLOR_MODE_RGBA8888;
	case DRM_FORMAT_RGB888:
		return OSD_MALI_COLOR_MODE_RGB888;
	case DRM_FORMAT_RGB565:
		return OSD_MALI_COLOR_MODE_RGB565;
	/* TOFIX support mode formats */
	default:
		DRM_DEBUG("unsupported afbc format[%08x]\n", format);
		return -EINVAL;
	}
}

static bool meson_g12a_afbcd_supported_fmt(u64 modifier, uint32_t format)
{
	return meson_g12a_afbcd_pixel_fmt(modifier, format) >= 0;
}

static int meson_g12a_afbcd_init(struct meson_drm *priv)
{
	int ret;

	ret = meson_rdma_init(priv);
	if (ret)
		return ret;

	meson_rdma_setup(priv);

	/* Handle AFBC Decoder reset manually */
	writel_bits_relaxed(MALI_AFBCD_MANUAL_RESET, MALI_AFBCD_MANUAL_RESET,
			    priv->io_base + _REG(MALI_AFBCD_TOP_CTRL));

	return 0;
}

static int meson_g12a_afbcd_reset(struct meson_drm *priv)
{
	meson_rdma_reset(priv);

	meson_rdma_writel_sync(priv, VIU_SW_RESET_G12A_AFBC_ARB |
			       VIU_SW_RESET_G12A_OSD1_AFBCD,
			       VIU_SW_RESET);
	meson_rdma_writel_sync(priv, 0, VIU_SW_RESET);

	return 0;
}

static int meson_g12a_afbcd_enable(struct meson_drm *priv)
{
	meson_rdma_writel_sync(priv, VPU_MAFBC_IRQ_SURFACES_COMPLETED |
			       VPU_MAFBC_IRQ_CONFIGURATION_SWAPPED |
			       VPU_MAFBC_IRQ_DECODE_ERROR |
			       VPU_MAFBC_IRQ_DETILING_ERROR,
			       VPU_MAFBC_IRQ_MASK);

	meson_rdma_writel_sync(priv, VPU_MAFBC_S0_ENABLE,
			       VPU_MAFBC_SURFACE_CFG);

	meson_rdma_writel_sync(priv, VPU_MAFBC_DIRECT_SWAP,
			       VPU_MAFBC_COMMAND);

	/* This will enable the RDMA replaying the register writes on vsync */
	meson_rdma_flush(priv);

	return 0;
}

static int meson_g12a_afbcd_disable(struct meson_drm *priv)
{
	writel_bits_relaxed(VPU_MAFBC_S0_ENABLE, 0,
			    priv->io_base + _REG(VPU_MAFBC_SURFACE_CFG));

	return 0;
}

static int meson_g12a_afbcd_setup(struct meson_drm *priv)
{
	u32 format = meson_g12a_afbcd_pixel_fmt(priv->afbcd.modifier,
						priv->afbcd.format);

	if (priv->afbcd.modifier & AFBC_FORMAT_MOD_YTR)
		format |= VPU_MAFBC_YUV_TRANSFORM;

	if (priv->afbcd.modifier & AFBC_FORMAT_MOD_SPLIT)
		format |= VPU_MAFBC_BLOCK_SPLIT;

	if (priv->afbcd.modifier & AFBC_FORMAT_MOD_TILED)
		format |= VPU_MAFBC_TILED_HEADER_EN;

	if ((priv->afbcd.modifier & AFBC_FORMAT_MOD_BLOCK_SIZE_MASK) ==
		AFBC_FORMAT_MOD_BLOCK_SIZE_32x8)
		format |= FIELD_PREP(VPU_MAFBC_SUPER_BLOCK_ASPECT, 1);

	meson_rdma_writel_sync(priv, format,
			       VPU_MAFBC_FORMAT_SPECIFIER_S0);

	meson_rdma_writel_sync(priv, priv->viu.osd1_addr,
			       VPU_MAFBC_HEADER_BUF_ADDR_LOW_S0);
	meson_rdma_writel_sync(priv, 0,
			       VPU_MAFBC_HEADER_BUF_ADDR_HIGH_S0);

	meson_rdma_writel_sync(priv, priv->viu.osd1_width,
			       VPU_MAFBC_BUFFER_WIDTH_S0);
	meson_rdma_writel_sync(priv, ALIGN(priv->viu.osd1_height, 32),
			       VPU_MAFBC_BUFFER_HEIGHT_S0);

	meson_rdma_writel_sync(priv, 0,
			       VPU_MAFBC_BOUNDING_BOX_X_START_S0);
	meson_rdma_writel_sync(priv, priv->viu.osd1_width - 1,
			       VPU_MAFBC_BOUNDING_BOX_X_END_S0);
	meson_rdma_writel_sync(priv, 0,
			       VPU_MAFBC_BOUNDING_BOX_Y_START_S0);
	meson_rdma_writel_sync(priv, priv->viu.osd1_height - 1,
			       VPU_MAFBC_BOUNDING_BOX_Y_END_S0);

	meson_rdma_writel_sync(priv, MESON_G12A_AFBCD_OUT_ADDR,
			       VPU_MAFBC_OUTPUT_BUF_ADDR_LOW_S0);
	meson_rdma_writel_sync(priv, 0,
			       VPU_MAFBC_OUTPUT_BUF_ADDR_HIGH_S0);

	meson_rdma_writel_sync(priv, priv->viu.osd1_width *
			       (meson_g12a_afbcd_bpp(priv->afbcd.format) / 8),
			       VPU_MAFBC_OUTPUT_BUF_STRIDE_S0);

	return 0;
}

struct meson_afbcd_ops meson_afbcd_g12a_ops = {
	.init = meson_g12a_afbcd_init,
	.reset = meson_g12a_afbcd_reset,
	.enable = meson_g12a_afbcd_enable,
	.disable = meson_g12a_afbcd_disable,
	.setup = meson_g12a_afbcd_setup,
	.fmt_to_blk_mode = meson_g12a_afbcd_fmt_to_blk_mode,
	.supported_fmt = meson_g12a_afbcd_supported_fmt,
};
