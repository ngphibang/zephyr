/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>

#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/mpipe/vid/mpipe_vid_buffer_pool_client.h>
#include <zephyr/mpipe/vid/mpipe_vid_sink_client.h>

#include "vid_client.h"

LOG_MODULE_REGISTER(mpipe_vid_sink_client, CONFIG_MPIPE_LOG_LEVEL);

static int mpipe_vid_sink_client_enum_caps(struct mpipe_pad *pad, uint32_t index,
					   const struct mpipe_structure *filter,
					   struct mpipe_structure *out)
{
	struct mpipe_vid_sink_client *vsc = (struct mpipe_vid_sink_client *)pad->object.container;

	return mpipe_vid_client_enum_caps(vsc->sink.channel, vsc->format, MPIPE_PAD_SINK, index,
					  filter, out);
}

static int mpipe_vid_sink_client_set_caps(struct mpipe_sink *sink,
					  const struct mpipe_structure *caps)
{
	struct mpipe_vid_sink_client *vsc = (struct mpipe_vid_sink_client *)sink;
	int ret;

	ret = mpipe_vid_client_set_format(vsc->sink.channel, vsc->format, MPIPE_PAD_SINK, caps,
					  &vsc->in_pool.pool);
	if (ret != 0) {
		return ret;
	}

	return mpipe_pad_set_caps(&sink->sink_pad, caps);
}

int mpipe_vid_sink_client_init(struct mpipe_vid_sink_client *vsc, uint8_t id,
			       const struct mpipe_vid_rpc_channel *vid_channel)
{
	__ASSERT_NO_MSG(vsc != NULL);
	__ASSERT_NO_MSG(vid_channel != NULL && vid_channel->channel != NULL &&
			vid_channel->format != NULL);

	struct mpipe_element *self = &vsc->sink.sink.element;
	struct mpipe_sink *sink = &vsc->sink.sink;
	int ret;

	vsc->sink.channel = vid_channel->channel;
	vsc->format = vid_channel->format;

	ret = mpipe_sink_client_init(&vsc->sink, id);
	if (ret != 0) {
		return ret;
	}

	mpipe_element_set_name(self, "vid_sink_client");

	sink->sink_pad.enum_caps_fn = mpipe_vid_sink_client_enum_caps;
	sink->set_caps = mpipe_vid_sink_client_set_caps;

	vsc->sink.in_pool = &vsc->in_pool.pool;

	return mpipe_vid_client_pool_init(vsc->sink.channel, vsc->format, MPIPE_PAD_SINK,
					  vsc->sink.in_pool);
}
