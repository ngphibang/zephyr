/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/mpipe/vid/mpipe_vid_buffer_pool_client.h>
#include <zephyr/mpipe/vid/mpipe_vid_src_client.h>

#include "vid_client.h"

LOG_MODULE_REGISTER(mpipe_vid_src_client, CONFIG_MPIPE_LOG_LEVEL);

static int mpipe_vid_src_client_enum_caps(struct mpipe_pad *pad, uint32_t index,
					  const struct mpipe_structure *filter,
					  struct mpipe_structure *out)
{
	struct mpipe_vid_src_client *vsrc = (struct mpipe_vid_src_client *)pad->object.container;

	return mpipe_vid_client_enum_caps(vsrc->src.channel, vsrc->format, MPIPE_PAD_SRC, index,
					  filter, out);
}

static int mpipe_vid_src_client_set_caps(struct mpipe_src *src, const struct mpipe_structure *caps)
{
	struct mpipe_vid_src_client *vsrc = (struct mpipe_vid_src_client *)src;
	int ret;

	ret = mpipe_vid_client_set_format(vsrc->src.channel, vsrc->format, MPIPE_PAD_SRC, caps,
					  &vsrc->pool.pool);
	if (ret != 0) {
		return ret;
	}

	return mpipe_pad_set_caps(&src->src_pad, caps);
}

/* A free buffer of the pool, filled by the server */
static int mpipe_vid_src_client_acquire_buffer(struct mpipe_buffer_pool *pool, struct net_buf **buf)
{
	struct mpipe_vid_src_client *vsrc =
		CONTAINER_OF(pool, struct mpipe_vid_src_client, pool.pool);
	int ret;

	ret = vsrc->pool_acquire(pool, buf);
	if (ret != 0) {
		return ret;
	}

	ret = mpipe_src_client_fill(&vsrc->src, *buf);
	if (ret != 0) {
		net_buf_unref(*buf);
		*buf = NULL;
	}

	return ret;
}

int mpipe_vid_src_client_init(struct mpipe_vid_src_client *vsrc, uint8_t id,
			      const struct mpipe_vid_rpc_channel *vid_channel)
{
	__ASSERT_NO_MSG(vsrc != NULL);
	__ASSERT_NO_MSG(vid_channel != NULL && vid_channel->channel != NULL &&
			vid_channel->format != NULL);

	struct mpipe_element *self = &vsrc->src.src.element;
	struct mpipe_src *src = &vsrc->src.src;
	int ret;

	vsrc->src.channel = vid_channel->channel;
	vsrc->format = vid_channel->format;

	ret = mpipe_src_client_init(&vsrc->src, id);
	if (ret != 0) {
		return ret;
	}

	mpipe_element_set_name(self, "vid_src_client");

	src->src_pad.enum_caps_fn = mpipe_vid_src_client_enum_caps;
	src->set_caps = mpipe_vid_src_client_set_caps;

	src->pool = &vsrc->pool.pool;

	ret = mpipe_vid_client_pool_init(vsrc->src.channel, vsrc->format, MPIPE_PAD_SRC, src->pool);
	if (ret != 0) {
		return ret;
	}

	/* The server fills each buffer the pool gives */
	vsrc->pool_acquire = src->pool->acquire_buffer;
	src->pool->acquire_buffer = mpipe_vid_src_client_acquire_buffer;

	return 0;
}
