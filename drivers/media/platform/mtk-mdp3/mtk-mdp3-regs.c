// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021 MediaTek Inc.
 * Author: Ping-Hsun Wu <ping-hsun.wu@mediatek.com>
 */

#include <media/v4l2-common.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>
#include "mtk-mdp3-core.h"
#include "mtk-mdp3-regs.h"
#include "mtk-mdp3-m2m.h"

#define FHD (1920 * 1080)

static const struct mdp_format *mdp_formats;
static u32 format_len;

static const struct mdp_limit mdp_def_limit = {
	.out_limit = {
		.wmin	= 16,
		.hmin	= 16,
		.wmax	= 8176,
		.hmax	= 8176,
	},
	.cap_limit = {
		.wmin	= 2,
		.hmin	= 2,
		.wmax	= 8176,
		.hmax	= 8176,
	},
	.h_scale_up_max = 32,
	.v_scale_up_max = 32,
	.h_scale_down_max = 20,
	.v_scale_down_max = 128,
};

static const struct mdp_format *mdp_find_fmt(u32 pixelformat, u32 type)
{
	u32 i, flag;

	flag = V4L2_TYPE_IS_OUTPUT(type) ? MDP_FMT_FLAG_OUTPUT :
					MDP_FMT_FLAG_CAPTURE;
	for (i = 0; i < format_len; ++i) {
		if (!(mdp_formats[i].flags & flag))
			continue;
		if (mdp_formats[i].pixelformat == pixelformat)
			return &mdp_formats[i];
	}
	return NULL;
}

static const struct mdp_format *mdp_find_fmt_by_index(u32 index, u32 type)
{
	u32 i, flag, num = 0;

	flag = V4L2_TYPE_IS_OUTPUT(type) ? MDP_FMT_FLAG_OUTPUT :
					MDP_FMT_FLAG_CAPTURE;
	for (i = 0; i < format_len; ++i) {
		if (!(mdp_formats[i].flags & flag))
			continue;
		if (index == num)
			return &mdp_formats[i];
		num++;
	}
	return NULL;
}

enum mdp_ycbcr_profile mdp_map_ycbcr_prof_mplane(struct v4l2_format *f,
						 u32 mdp_color)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;

	if (MDP_COLOR_IS_RGB(mdp_color))
		return MDP_YCBCR_PROFILE_FULL_BT601;

	switch (pix_mp->colorspace) {
	case V4L2_COLORSPACE_JPEG:
		return MDP_YCBCR_PROFILE_JPEG;
	case V4L2_COLORSPACE_REC709:
	case V4L2_COLORSPACE_DCI_P3:
		if (pix_mp->quantization == V4L2_QUANTIZATION_FULL_RANGE)
			return MDP_YCBCR_PROFILE_FULL_BT709;
		return MDP_YCBCR_PROFILE_BT709;
	case V4L2_COLORSPACE_BT2020:
		if (pix_mp->quantization == V4L2_QUANTIZATION_FULL_RANGE)
			return MDP_YCBCR_PROFILE_FULL_BT2020;
		return MDP_YCBCR_PROFILE_BT2020;
	default:
		if (pix_mp->quantization == V4L2_QUANTIZATION_FULL_RANGE)
			return MDP_YCBCR_PROFILE_FULL_BT601;
		return MDP_YCBCR_PROFILE_BT601;
	}
}

static void mdp_bound_align_image(u32 *w, unsigned int wmin, unsigned int wmax,
				  unsigned int walign,
				  u32 *h, unsigned int hmin, unsigned int hmax,
				  unsigned int halign, unsigned int salign)
{
	unsigned int org_w, org_h, wstep, hstep;

	org_w = *w;
	org_h = *h;
	v4l_bound_align_image(w, wmin, wmax, walign, h, hmin, hmax, halign,
			      salign);

	wstep = 1 << walign;
	hstep = 1 << halign;
	if (*w < org_w && (*w + wstep) <= wmax)
		*w += wstep;
	if (*h < org_h && (*h + hstep) <= hmax)
		*h += hstep;
}

static int mdp_clamp_align(s32 *x, int min, int max, unsigned int align)
{
	unsigned int mask;

	if (min < 0 || max < 0)
		return -ERANGE;

	/* Bits that must be zero to be aligned */
	mask = ~((1 << align) - 1);

	min = 0 ? 0 : ((min + ~mask) & mask);
	max = max & mask;
	if ((unsigned int)min > (unsigned int)max)
		return -ERANGE;

	/* Clamp to aligned min and max */
	*x = clamp(*x, min, max);

	/* Round to nearest aligned value */
	if (align)
		*x = (*x + (1 << (align - 1))) & mask;
	return 0;
}

int mdp_enum_fmt_mplane(struct v4l2_fmtdesc *f)
{
	const struct mdp_format *fmt;

	fmt = mdp_find_fmt_by_index(f->index, f->type);
	if (!fmt)
		return -EINVAL;

	f->pixelformat = fmt->pixelformat;
	return 0;
}

static u32 mdp_fmt_get_hyfbc_plane_size(u32 width,
					u32 height, u32 color, unsigned int plane)
{
	u32 y_data_size = 0;
	u32 c_data_size = 0;
	u32 y_header_size = 0;
	u32 c_header_size = 0;
	u32 y_data_ofst = 0;
	u32 y_header_ofst = 0;
	u32 c_data_ofst = 0;
	u32 c_header_ofst = 0;

	y_data_size = (((width + 63) >> 6) << 6) * (((height + 63) >> 6) << 6);
	y_header_size = y_data_size >> 6;
	if (MDP_COLOR_IS_10BIT_PACKED(color))
		y_data_size = (y_data_size * 6) >> 2;

	c_data_size = y_data_size >> 1;
	c_header_size = (((y_header_size >> 1) + 63) >> 6) << 6;

	// Setup source buffer base
	y_data_ofst = ((y_header_size + 4095) >> 12) << 12; // align 4k
	y_header_ofst = y_data_ofst - y_header_size;
	c_data_ofst = ((y_data_ofst + y_data_size + c_header_size + 4095) >> 12) << 12; // align 4k
	c_header_ofst = c_data_ofst - c_header_size;

	if (plane == 0)
		return c_header_ofst;
	else
		return (c_data_ofst + c_data_size);
}

static u32 mdp_fmt_get_afbc_plane_size(u32 width, u32 height, u32 color)
{
	u32 align_w = ((width + 31) >> 5) << 5;
	u32 align_h = ((height + 31) >> 5) << 5;

	if (MDP_COLOR_IS_10BIT_PACKED(color))
		return ((align_w >> 4) * (align_h >> 4) * (16 + 512));
	else
		return ((align_w >> 4) * (align_h >> 4) * (16 + 384));
}

const struct mdp_format *mdp_try_fmt_mplane(struct v4l2_format *f,
					    struct mdp_frameparam *param,
					    u32 ctx_id)
{
	struct device *dev = &param->ctx->mdp_dev->pdev->dev;
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct mdp_format *fmt;
	const struct mdp_pix_limit *pix_limit;
	u32 wmin, wmax, hmin, hmax, org_w, org_h;
	unsigned int i;

	fmt = mdp_find_fmt(pix_mp->pixelformat, f->type);
	if (!fmt) {
		fmt = mdp_find_fmt_by_index(0, f->type);
		if (!fmt) {
			dev_dbg(dev, "%d: pixelformat %c%c%c%c invalid", ctx_id,
				(pix_mp->pixelformat & 0xff),
				(pix_mp->pixelformat >>  8) & 0xff,
				(pix_mp->pixelformat >> 16) & 0xff,
				(pix_mp->pixelformat >> 24) & 0xff);
			return NULL;
		}
	}

	pix_mp->field = V4L2_FIELD_NONE;
	pix_mp->flags = 0;
	pix_mp->pixelformat = fmt->pixelformat;
	if (!V4L2_TYPE_IS_OUTPUT(f->type)) {
		pix_mp->colorspace = param->colorspace;
		pix_mp->xfer_func = param->xfer_func;
		pix_mp->ycbcr_enc = param->ycbcr_enc;
		pix_mp->quantization = param->quant;
	}

	pix_limit = V4L2_TYPE_IS_OUTPUT(f->type) ? &param->limit->out_limit :
						&param->limit->cap_limit;
	wmin = pix_limit->wmin;
	wmax = pix_limit->wmax;
	hmin = pix_limit->hmin;
	hmax = pix_limit->hmax;
	org_w = pix_mp->width;
	org_h = pix_mp->height;

	mdp_bound_align_image(&pix_mp->width, wmin, wmax, fmt->walign,
			      &pix_mp->height, hmin, hmax, fmt->halign,
			      fmt->salign);
	if (org_w != pix_mp->width || org_h != pix_mp->height)
		dev_dbg(dev, "%d: size change: %ux%u to %ux%u", ctx_id,
			org_w, org_h, pix_mp->width, pix_mp->height);

	if (pix_mp->num_planes && pix_mp->num_planes != fmt->num_planes)
		dev_dbg(dev, "%d num of planes change: %u to %u", ctx_id,
			pix_mp->num_planes, fmt->num_planes);
	pix_mp->num_planes = fmt->num_planes;

	for (i = 0; i < pix_mp->num_planes; ++i) {
		u32 min_bpl = (pix_mp->width * fmt->row_depth[i]) / 8;
		u32 bpl = pix_mp->plane_fmt[i].bytesperline;
		u32 si;

		if (bpl < min_bpl)
			bpl = min_bpl;
		si = (bpl * pix_mp->height * fmt->depth[i]) / fmt->row_depth[i];

		if (MDP_COLOR_IS_HYFBC_COMPRESS(fmt->mdp_color)) {
			si = mdp_fmt_get_hyfbc_plane_size(pix_mp->width,
							  pix_mp->height, fmt->mdp_color, i);
		} else if (MDP_COLOR_IS_COMPRESS(fmt->mdp_color)) {
			si = mdp_fmt_get_afbc_plane_size(pix_mp->width,
							 pix_mp->height, fmt->mdp_color);
		}

		pix_mp->plane_fmt[i].bytesperline = bpl;
		if (pix_mp->plane_fmt[i].sizeimage < si)
			pix_mp->plane_fmt[i].sizeimage = si;
	}

	return fmt;
}

static int mdp_clamp_start(s32 *x, int min, int max, unsigned int align,
			   u32 flags)
{
	if (flags & V4L2_SEL_FLAG_GE)
		max = *x;
	if (flags & V4L2_SEL_FLAG_LE)
		min = *x;
	return mdp_clamp_align(x, min, max, align);
}

static int mdp_clamp_end(s32 *x, int min, int max, unsigned int align,
			 u32 flags)
{
	if (flags & V4L2_SEL_FLAG_GE)
		min = *x;
	if (flags & V4L2_SEL_FLAG_LE)
		max = *x;
	return mdp_clamp_align(x, min, max, align);
}

int mdp_try_crop(struct mdp_m2m_ctx *ctx, struct v4l2_rect *r,
		 const struct v4l2_selection *s, struct mdp_frame *frame)
{
	struct device *dev = &ctx->mdp_dev->pdev->dev;
	s32 left, top, right, bottom;
	u32 framew, frameh, walign, halign;
	int ret;

	dev_dbg(dev, "%d target:%d, set:(%d,%d) %ux%u", ctx->id,
		s->target, s->r.left, s->r.top, s->r.width, s->r.height);

	left = s->r.left;
	top = s->r.top;
	right = s->r.left + s->r.width;
	bottom = s->r.top + s->r.height;
	framew = frame->format.fmt.pix_mp.width;
	frameh = frame->format.fmt.pix_mp.height;

	if (mdp_target_is_crop(s->target)) {
		walign = 1;
		halign = 1;
	} else {
		walign = frame->mdp_fmt->walign;
		halign = frame->mdp_fmt->halign;
	}

	dev_dbg(dev, "%d align:%u,%u, bound:%ux%u", ctx->id,
		walign, halign, framew, frameh);

	ret = mdp_clamp_start(&left, 0, right, walign, s->flags);
	if (ret)
		return ret;
	ret = mdp_clamp_start(&top, 0, bottom, halign, s->flags);
	if (ret)
		return ret;
	ret = mdp_clamp_end(&right, left, framew, walign, s->flags);
	if (ret)
		return ret;
	ret = mdp_clamp_end(&bottom, top, frameh, halign, s->flags);
	if (ret)
		return ret;

	r->left = left;
	r->top = top;
	r->width = right - left;
	r->height = bottom - top;

	dev_dbg(dev, "%d crop:(%d,%d) %ux%u", ctx->id,
		r->left, r->top, r->width, r->height);
	return 0;
}

int mdp_check_scaling_ratio(const struct v4l2_rect *crop,
			    const struct v4l2_rect *compose, s32 rotation,
	const struct mdp_limit *limit)
{
	u32 crop_w, crop_h, comp_w, comp_h;

	crop_w = crop->width;
	crop_h = crop->height;
	if (90 == rotation || 270 == rotation) {
		comp_w = compose->height;
		comp_h = compose->width;
	} else {
		comp_w = compose->width;
		comp_h = compose->height;
	}

	if ((crop_w / comp_w) > limit->h_scale_down_max ||
	    (crop_h / comp_h) > limit->v_scale_down_max ||
	    (comp_w / crop_w) > limit->h_scale_up_max ||
	    (comp_h / crop_h) > limit->v_scale_up_max)
		return -ERANGE;
	return 0;
}

/* Stride that is accepted by MDP HW */
static u32 mdp_fmt_get_stride(const struct mdp_format *fmt,
			      u32 bytesperline, unsigned int plane)
{
	enum mdp_color c = fmt->mdp_color;
	u32 stride;

	if (MDP_COLOR_IS_COMPRESS(c)) {
		bytesperline = ((bytesperline + 31) >> 5) << 5;
		stride = (bytesperline * MDP_COLOR_BITS_PER_PIXEL(c))
			/ fmt->row_depth[0];
	} else {
		stride = (bytesperline * MDP_COLOR_BITS_PER_PIXEL(c))
			/ fmt->row_depth[0];
	}
	if (plane == 0)
		return stride;
	if (plane < MDP_COLOR_GET_PLANE_COUNT(c)) {
		if (MDP_COLOR_IS_BLOCK_MODE(c))
			stride = stride / 2;
		return stride;
	}
	return 0;
}

/* Stride that is accepted by MDP HW of format with contiguous planes */
static u32 mdp_fmt_get_stride_contig(const struct mdp_format *fmt,
				     u32 pix_stride, unsigned int plane)
{
	enum mdp_color c = fmt->mdp_color;
	u32 stride = pix_stride;

	if (plane == 0)
		return stride;
	if (plane < MDP_COLOR_GET_PLANE_COUNT(c)) {
		stride = stride >> MDP_COLOR_GET_H_SUBSAMPLE(c);
		if (MDP_COLOR_IS_UV_COPLANE(c) && !MDP_COLOR_IS_BLOCK_MODE(c))
			stride = stride * 2;
		return stride;
	}
	return 0;
}

/* Plane size that is accepted by MDP HW */
static u32 mdp_fmt_get_plane_size(const struct mdp_format *fmt,
				  u32 stride, u32 height, unsigned int plane)
{
	enum mdp_color c = fmt->mdp_color;
	u32 bytesperline;

	bytesperline = (stride * fmt->row_depth[0])
		/ MDP_COLOR_BITS_PER_PIXEL(c);
	if (plane == 0)
		return bytesperline * height;
	if (plane < MDP_COLOR_GET_PLANE_COUNT(c)) {
		height = height >> MDP_COLOR_GET_V_SUBSAMPLE(c);
		if (MDP_COLOR_IS_BLOCK_MODE(c))
			bytesperline = bytesperline * 2;
		return bytesperline * height;
	}
	return 0;
}

bool mdp_is_framechange(struct mdp_framechange_param *prev,
			struct mdp_framechange_param *cur)
{
	if (cur->frame_count == 0 ||
	    prev->scenario != cur->scenario ||
	    prev->in.buffer.format.colorformat != cur->in.buffer.format.colorformat ||
	    prev->in.buffer.format.width != cur->in.buffer.format.width ||
	    prev->in.buffer.format.height != cur->in.buffer.format.height ||
	    prev->out.buffer.format.width != cur->out.buffer.format.width ||
	    prev->out.buffer.format.height != cur->out.buffer.format.height ||
	    prev->out.crop.left != cur->out.crop.left ||
	    prev->out.crop.top != cur->out.crop.top ||
	    prev->out.crop.width != cur->out.crop.width ||
	    prev->out.crop.height != cur->out.crop.height)
		return true;

	return false;
}

void mdp_set_scenario(struct mdp_dev *mdp,
		      struct img_ipi_frameparam *param,
		      struct mdp_frame *frame)
{
	u32 width = frame->format.fmt.pix_mp.width;
	u32 height = frame->format.fmt.pix_mp.height;

	if (!mdp)
		return;

	if (mdp->mdp_data->mdp_cfg->support_dual_pipe) {
		if ((width * height) >= FHD)
			param->type = MDP_STREAM_TYPE_DUAL_BITBLT;
	}
}

static void mdp_prepare_buffer(struct img_image_buffer *b,
			       struct mdp_frame *frame, struct vb2_buffer *vb)
{
	struct v4l2_pix_format_mplane *pix_mp = &frame->format.fmt.pix_mp;
	unsigned int i;

	b->format.colorformat = frame->mdp_fmt->mdp_color;
	b->format.ycbcr_prof = frame->ycbcr_prof;
	for (i = 0; i < pix_mp->num_planes; ++i) {
		u32 stride = mdp_fmt_get_stride(frame->mdp_fmt,
			pix_mp->plane_fmt[i].bytesperline, i);

		b->format.plane_fmt[i].stride = stride;
		/*
		 * TODO : The way to pass an offset within a DMA-buf
		 * is not defined in V4L2 specification, so we abuse
		 * data_offset for now. Fix it when we have the right interface,
		 * including any necessary validation and potential alignment
		 * issues.
		 */
		b->format.plane_fmt[i].size =
			mdp_fmt_get_plane_size(frame->mdp_fmt, stride,
					       pix_mp->height, i) -
					       vb->planes[i].data_offset;

		if (MDP_COLOR_IS_HYFBC_COMPRESS(b->format.colorformat)) {
			b->format.plane_fmt[i].size =
				mdp_fmt_get_hyfbc_plane_size(pix_mp->width,
							     pix_mp->height,
							     b->format.colorformat, i);
		} else if (MDP_COLOR_IS_COMPRESS(b->format.colorformat)) {
			b->format.plane_fmt[i].size =
				mdp_fmt_get_afbc_plane_size(pix_mp->width,
							    pix_mp->height,
							    b->format.colorformat);
		}

		b->iova[i] = vb2_dma_contig_plane_dma_addr(vb, i) +
			     vb->planes[i].data_offset;
	}
	for (; i < MDP_COLOR_GET_PLANE_COUNT(b->format.colorformat); ++i) {
		u32 stride = mdp_fmt_get_stride_contig(frame->mdp_fmt,
			b->format.plane_fmt[0].stride, i);

		b->format.plane_fmt[i].stride = stride;
		b->format.plane_fmt[i].size =
			mdp_fmt_get_plane_size(frame->mdp_fmt, stride,
					       pix_mp->height, i);

		if (MDP_COLOR_IS_HYFBC_COMPRESS(b->format.colorformat)) {
			b->format.plane_fmt[i].size =
				mdp_fmt_get_hyfbc_plane_size(pix_mp->width,
							     pix_mp->height,
							     b->format.colorformat, i);
		} else if (MDP_COLOR_IS_COMPRESS(b->format.colorformat)) {
			b->format.plane_fmt[i].size =
				mdp_fmt_get_afbc_plane_size(pix_mp->width,
							    pix_mp->height,
							    b->format.colorformat);
		}
		b->iova[i] = b->iova[i - 1] + b->format.plane_fmt[i - 1].size;
	}
	b->usage = frame->usage;
}

void mdp_set_src_config(struct img_input *in,
			struct mdp_frame *frame, struct vb2_buffer *vb)
{
	in->buffer.format.width = frame->format.fmt.pix_mp.width;
	in->buffer.format.height = frame->format.fmt.pix_mp.height;
	mdp_prepare_buffer(&in->buffer, frame, vb);
}

static u32 mdp_to_fixed(u32 *r, struct v4l2_fract *f)
{
	u32 q;

	if (f->denominator == 0) {
		*r = 0;
		return 0;
	}

	q = f->numerator / f->denominator;
	*r = div_u64(((u64)f->numerator - q * f->denominator) <<
		     IMG_SUBPIXEL_SHIFT, f->denominator);
	return q;
}

static void mdp_set_src_crop(struct img_crop *c, struct mdp_crop *crop)
{
	c->left = crop->c.left
		+ mdp_to_fixed(&c->left_subpix, &crop->left_subpix);
	c->top = crop->c.top
		+ mdp_to_fixed(&c->top_subpix, &crop->top_subpix);
	c->width = crop->c.width
		+ mdp_to_fixed(&c->width_subpix, &crop->width_subpix);
	c->height = crop->c.height
		+ mdp_to_fixed(&c->height_subpix, &crop->height_subpix);
}

static void mdp_set_orientation(struct img_output *out,
				s32 rotation, bool hflip, bool vflip)
{
	u8 flip = 0;

	if (hflip)
		flip ^= 1;
	if (vflip) {
		/*
		 * A vertical flip is equivalent to
		 * a 180-degree rotation with a horizontal flip
		 */
		rotation += 180;
		flip ^= 1;
	}

	out->rotation = rotation % 360;
	if (flip != 0)
		out->flags |= IMG_CTRL_FLAG_HFLIP;
	else
		out->flags &= ~IMG_CTRL_FLAG_HFLIP;
}

void mdp_set_dst_config(struct img_output *out,
			struct mdp_frame *frame, struct vb2_buffer *vb)
{
	out->buffer.format.width = frame->compose.width;
	out->buffer.format.height = frame->compose.height;
	mdp_prepare_buffer(&out->buffer, frame, vb);
	mdp_set_src_crop(&out->crop, &frame->crop);
	mdp_set_orientation(out, frame->rotation, frame->hflip, frame->vflip);
}

int mdp_frameparam_init(struct mdp_frameparam *param)
{
	struct mdp_frame *frame;

	if (!param)
		return -EINVAL;

	INIT_LIST_HEAD(&param->list);
	mutex_init(&param->state_lock);
	param->limit = &mdp_def_limit;
	param->type = MDP_STREAM_TYPE_BITBLT;

	frame = &param->output;
	frame->format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	frame->mdp_fmt = mdp_try_fmt_mplane(&frame->format, param, 0);
	frame->ycbcr_prof =
		mdp_map_ycbcr_prof_mplane(&frame->format,
					  frame->mdp_fmt->mdp_color);
	frame->usage = MDP_BUFFER_USAGE_HW_READ;

	param->num_captures = 1;
	frame = &param->captures[0];
	frame->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	frame->mdp_fmt = mdp_try_fmt_mplane(&frame->format, param, 0);
	frame->ycbcr_prof =
		mdp_map_ycbcr_prof_mplane(&frame->format,
					  frame->mdp_fmt->mdp_color);
	frame->usage = MDP_BUFFER_USAGE_MDP;
	frame->crop.c.width = param->output.format.fmt.pix_mp.width;
	frame->crop.c.height = param->output.format.fmt.pix_mp.height;
	frame->compose.width = frame->format.fmt.pix_mp.width;
	frame->compose.height = frame->format.fmt.pix_mp.height;

	return 0;
}

void mdp_format_init(const struct mdp_format *format, u32 length)
{
	mdp_formats = format;
	format_len = length;
}

