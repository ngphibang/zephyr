/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/mpipe/mpipe_value.h>
#include <zephyr/mpipe/vid/mpipe_vid_buffer_pool_client.h>
#include <zephyr/mpipe/vid/mpipe_vid_object.h>
#include <zephyr/mpipe/vid/mpipe_vid_transform_client.h>

#include "vid_client.h"

LOG_MODULE_REGISTER(mpipe_vid_transform_client, CONFIG_MPIPE_LOG_LEVEL);

static int mpipe_vid_transform_client_enum_caps(struct mpipe_pad *pad, uint32_t index,
						const struct mpipe_structure *filter,
						struct mpipe_structure *out)
{
	struct mpipe_vid_transform_client *vtc =
		(struct mpipe_vid_transform_client *)pad->object.container;

	return mpipe_vid_client_enum_caps(vtc->transform.channel, vtc->format, pad->direction,
					  index, filter, out);
}

static int mpipe_vid_transform_client_set_caps(struct mpipe_transform *transform,
					       enum mpipe_pad_direction direction,
					       const struct mpipe_structure *caps)
{
	struct mpipe_vid_transform_client *vtc = (struct mpipe_vid_transform_client *)transform;
	bool is_src = (direction == MPIPE_PAD_SRC);
	int ret;

	ret = mpipe_vid_client_set_format(vtc->transform.channel, vtc->format, direction, caps,
					  is_src ? transform->out_pool : transform->in_pool);
	if (ret != 0) {
		return ret;
	}

	/* Set pad's caps only when everything is OK */
	return mpipe_pad_set_caps(is_src ? &transform->src_pad : &transform->sink_pad, caps);
}

static int mpipe_vid_transform_client_transform_caps(struct mpipe_transform *self,
						     enum mpipe_pad_direction direction,
						     const struct mpipe_structure *in,
						     uint32_t index, struct mpipe_structure *out)
{
	struct mpipe_vid_transform_client *vtc = (struct mpipe_vid_transform_client *)self;
	struct video_format_cap vfc, other_vfc;

	if (direction != MPIPE_PAD_SINK && direction != MPIPE_PAD_SRC) {
		return -EINVAL;
	}

	if (in == NULL || mpipe_vid_caps_to_vfc(in, &vfc) < 0) {
		return -ENOENT;
	}

	if (index > UINT16_MAX ||
	    vtc->format->transform_cap(vtc->transform.channel, direction, (uint16_t)index, &vfc,
				       &other_vfc) != 0) {
		return -ENOENT;
	}

	return mpipe_vid_vfc_to_caps(&other_vfc, out);
}

int mpipe_vid_transform_client_init(struct mpipe_vid_transform_client *vtc, uint8_t id,
				    const struct mpipe_vid_rpc_channel *vid_channel)
{
	__ASSERT_NO_MSG(vtc != NULL);
	__ASSERT_NO_MSG(vid_channel != NULL && vid_channel->channel != NULL &&
			vid_channel->format != NULL);

	struct mpipe_element *self = &vtc->transform.transform.element;
	struct mpipe_transform *transform = &vtc->transform.transform;
	int ret;

	vtc->transform.channel = vid_channel->channel;
	vtc->format = vid_channel->format;

	ret = mpipe_transform_client_init(&vtc->transform, id);
	if (ret != 0) {
		return ret;
	}

	mpipe_element_set_name(self, "vid_transform_client");

	transform->in_pool = &vtc->in_pool.pool;
	transform->out_pool = &vtc->out_pool.pool;

	transform->sink_pad.enum_caps_fn = mpipe_vid_transform_client_enum_caps;
	transform->src_pad.enum_caps_fn = mpipe_vid_transform_client_enum_caps;

	transform->set_caps = mpipe_vid_transform_client_set_caps;
	transform->transform_caps = mpipe_vid_transform_client_transform_caps;

	/* The pad caps stay ANY until a format of the server is negotiated */
	ret = mpipe_vid_client_pool_init(vtc->transform.channel, vtc->format, MPIPE_PAD_SINK,
					 transform->in_pool);
	if (ret != 0) {
		return ret;
	}

	return mpipe_vid_client_pool_init(vtc->transform.channel, vtc->format, MPIPE_PAD_SRC,
					  transform->out_pool);
}
