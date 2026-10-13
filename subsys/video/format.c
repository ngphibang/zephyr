/*
 * SPDX-FileCopyrightText: The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>
#include <zephyr/video/video.h>

LOG_MODULE_REGISTER(video_format, CONFIG_VIDEO_LOG_LEVEL);

int video_set_selection(const struct device *dev, struct video_selection *sel)
{
	if (dev == NULL || sel == NULL) {
		return -EINVAL;
	}

	return video_driver_set_selection(dev, sel);
}

int video_get_selection(const struct device *dev, struct video_selection *sel)
{
	if (dev == NULL || sel == NULL) {
		return -EINVAL;
	}

	return video_driver_get_selection(dev, sel);
}

/*
 * A driver that exchanges buffers with the application must tell it how large they are:
 * pitch and size are its to fill, since only it knows the padding its hardware needs.
 * Compressed formats have no pitch.
 */
static int check_fmt_size(const struct device *dev, const struct video_format *fmt)
{
	const struct video_driver_api *api = DEVICE_API_GET(video, dev);

	if (api->enqueue == NULL) {
		return 0;
	}

	if (fmt->size == 0) {
		LOG_ERR("%s: driver did not set the format size", dev->name);
		return -EIO;
	}

	if (fmt->pitch == 0 && video_bits_per_pixel(fmt->pixelformat) != 0) {
		LOG_ERR("%s: driver did not set the format pitch", dev->name);
		return -EIO;
	}

	return 0;
}

int video_set_format(const struct device *dev, struct video_format *fmt)
{
	int ret;

	if (dev == NULL || fmt == NULL) {
		return -EINVAL;
	}

	ret = video_driver_set_format(dev, fmt);
	if (ret != 0) {
		return ret;
	}

	return check_fmt_size(dev, fmt);
}

int video_get_format(const struct device *dev, struct video_format *fmt)
{
	int ret;

	if (dev == NULL || fmt == NULL) {
		return -EINVAL;
	}

	ret = video_driver_get_format(dev, fmt);
	if (ret != 0) {
		return ret;
	}

	return check_fmt_size(dev, fmt);
}

int video_estimate_fmt_size(struct video_format *fmt)
{
	if (fmt == NULL) {
		return -EINVAL;
	}

	switch (fmt->pixelformat) {
	case VIDEO_PIX_FMT_JPEG:
	case VIDEO_PIX_FMT_PNG:
	case VIDEO_PIX_FMT_H264:
		/* Rough estimate for the worst case (quality = 100) */
		fmt->pitch = 0;
		fmt->size = fmt->width * fmt->height * 2;
		break;
	case VIDEO_PIX_FMT_NV12:
	case VIDEO_PIX_FMT_NV21:
		fmt->pitch = fmt->width;
		fmt->size = fmt->pitch * fmt->height * 2U;
		break;
	default:
		/* Uncompressed format */
		fmt->pitch = fmt->width * video_bits_per_pixel(fmt->pixelformat) / BITS_PER_BYTE;
		if (fmt->pitch == 0) {
			return -ENOTSUP;
		}
		fmt->size = fmt->pitch * fmt->height;
		break;
	}

	return 0;
}

int video_set_compose_format(const struct device *dev, struct video_format *fmt)
{
	struct video_selection sel = {
		.type = fmt->type,
		.target = VIDEO_SEL_TGT_COMPOSE,
		.rect.left = 0,
		.rect.top = 0,
		.rect.width = fmt->width,
		.rect.height = fmt->height,
	};
	int ret;

	ret = video_set_selection(dev, &sel);
	if (ret < 0 && ret != -ENOSYS) {
		LOG_ERR("Unable to set selection compose");
		return ret;
	}

	return video_set_format(dev, fmt);
}
