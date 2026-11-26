/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/cache.h>
#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/mpipe/vid/mpipe_vid_buffer_pool_client.h>
#include <zephyr/mpipe/vid/mpipe_vid_object.h>

#include "vid_client.h"

LOG_MODULE_REGISTER(mpipe_vid_client, CONFIG_MPIPE_LOG_LEVEL);

/*
 * A buffer the server reads or writes shares no cache line of this core with
 * its neighbours, so it can be flushed and invalidated around a call.
 */
static inline uint32_t vid_client_cache_line(void)
{
	return MAX((uint32_t)sys_cache_data_line_size_get(), 1U);
}

int mpipe_vid_client_pool_init(struct mpipe_rpc_channel *channel,
			       const struct mpipe_vid_rpc_format_api *format,
			       enum mpipe_pad_direction direction, struct mpipe_buffer_pool *pool)
{
	struct mpipe_buffer_pool_config req = {0};

	mpipe_vid_buffer_pool_client_init(pool);

	if (format->get_buf_caps(channel, direction, &req.min_buffers, &req.align) != 0) {
		LOG_ERR("Unable to retrieve buffer pool capabilities");
		return -ENODEV;
	}

	req.align = (uint16_t)MAX((uint32_t)req.align, vid_client_cache_line());

	/* The floor every run starts from: a stop restores it */
	return mpipe_buffer_pool_set_req_config(pool, &req);
}

int mpipe_vid_client_enum_caps(struct mpipe_rpc_channel *channel,
			       const struct mpipe_vid_rpc_format_api *format,
			       enum mpipe_pad_direction direction, uint32_t index,
			       const struct mpipe_structure *filter, struct mpipe_structure *out)
{
	struct mpipe_structure candidate;
	struct video_format_cap vfc;
	int ret;

	/* The supported formats are asked for on demand: the element keeps no list */
	if (index > UINT8_MAX ||
	    format->get_format_caps(channel, direction, (uint8_t)index, &vfc) != 0) {
		return -ENOENT;
	}

	ret = mpipe_vid_vfc_to_caps(&vfc, &candidate);
	if (ret != 0) {
		return ret;
	}

	return mpipe_pad_enum_filter(&candidate, filter, out);
}

int mpipe_vid_client_set_format(struct mpipe_rpc_channel *channel,
				const struct mpipe_vid_rpc_format_api *format,
				enum mpipe_pad_direction direction,
				const struct mpipe_structure *caps, struct mpipe_buffer_pool *pool)
{
	struct video_format fmt;
	enum video_buf_type type;
	int ret;

	if (caps == NULL || !mpipe_structure_is_fixed(caps)) {
		return -EINVAL;
	}

	if (direction == MPIPE_PAD_SINK) {
		type = VIDEO_BUF_TYPE_INPUT;
	} else if (direction == MPIPE_PAD_SRC) {
		type = VIDEO_BUF_TYPE_OUTPUT;
	} else {
		return -EINVAL;
	}

	ret = mpipe_vid_caps_to_format(caps, type, &fmt);
	if (ret < 0) {
		return ret;
	}

	/* The server negotiates its side with this format and fills in the size */
	ret = format->set_format(channel, &fmt);
	if (ret < 0) {
		LOG_ERR("Unable to set format: type=%u pixfmt=%u w=%u h=%u (%d)", fmt.type,
			fmt.pixelformat, fmt.width, fmt.height, ret);
		return ret;
	}

	pool->config.size = ROUND_UP(fmt.size, vid_client_cache_line());

	return 0;
}
