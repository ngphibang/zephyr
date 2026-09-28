/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_dispatch.h>
#include <zephyr/mpipe/mpipe_transform_client.h>

LOG_MODULE_REGISTER(mpipe_transform_client, CONFIG_MPIPE_LOG_LEVEL);

static int mpipe_transform_client_process_fn(struct mpipe_pad *pad, struct net_buf *in_buf,
					     struct net_buf **out_buf)
{
	struct mpipe_transform *transform = (struct mpipe_transform *)pad->object.container;
	struct mpipe_transform_client *transform_client =
		(struct mpipe_transform_client *)transform;
	struct mpipe_buffer_meta *in_meta;
	struct mpipe_buffer_meta *out_meta;
	uint32_t in_used;
	uint32_t out_used;
	int ret;

	__ASSERT_NO_MSG(in_buf != NULL);
	__ASSERT_NO_MSG(out_buf != NULL);

	/* The processing function owns in_buf on every path */
	if (transform->out_pool == NULL || transform->out_pool->acquire_buffer == NULL) {
		net_buf_unref(in_buf);
		return -EINVAL;
	}

	in_meta = mpipe_buffer_get_meta(in_buf);
	in_used = in_meta->bytes_used;

	ret = transform->out_pool->acquire_buffer(transform->out_pool, out_buf);
	if (ret != 0 || *out_buf == NULL) {
		LOG_ERR("Element %u: failed to acquire an output buffer (%d)",
			transform->element.object.id, ret);
		net_buf_unref(in_buf);
		return (ret != 0) ? ret : -ENOMEM;
	}

	out_meta = mpipe_buffer_get_meta(*out_buf);
	out_used = out_meta->bytes_used;

	/* The RPC interface carries 32-bit addresses */
	ret = transform_client->process_fn_rpc((uint32_t)(uintptr_t)in_buf->data, in_used,
					       (uint32_t)(uintptr_t)(*out_buf)->data, &out_used);
	if (ret != 0) {
		LOG_ERR("Element %u: remote processing failed (%d)", transform->element.object.id,
			ret);
	} else if (out_used > (*out_buf)->size) {
		LOG_ERR("Element %u: remote wrote %u bytes into a %u byte buffer",
			transform->element.object.id, out_used, (*out_buf)->size);
		ret = -EOVERFLOW;
	}

	if (ret != 0) {
		net_buf_unref(*out_buf);
		*out_buf = NULL;
		net_buf_unref(in_buf);

		return ret;
	}

	out_meta->bytes_used = out_used;
	out_meta->timestamp = k_uptime_get_32();
	(*out_buf)->len = out_used;

	net_buf_unref(in_buf);

	return 0;
}

static int mpipe_transform_client_propose_buffer_pool(struct mpipe_transform *self,
						      struct mpipe_dispatch *query)
{
	query->pool = self->in_pool;

	return 0;
}

static int mpipe_transform_client_decide_buffer_pool(struct mpipe_transform *self,
						     struct mpipe_dispatch *query)
{
	struct mpipe_buffer_pool_config *pool_config;
	const struct mpipe_buffer_pool_config *qpc;

	__ASSERT_NO_MSG(self->out_pool != NULL);

	pool_config = &self->out_pool->config;
	qpc = (query->pool != NULL) ? &query->pool->config : &query->pool_cfg;

	/* Always use its own pool, just merge the downstream demands into its config */
	if (qpc->min_buffers > pool_config->min_buffers) {
		pool_config->min_buffers = qpc->min_buffers;
	}

	if (qpc->align != 0U) {
		uint64_t align = (pool_config->align == 0U)
					 ? qpc->align
					 : sys_lcm(qpc->align, pool_config->align);

		if (align > UINT16_MAX) {
			return -EINVAL;
		}

		pool_config->align = (uint16_t)align;
	}

	return 0;
}

int mpipe_transform_client_init(struct mpipe_transform_client *transform_client, uint8_t id)
{
	__ASSERT_NO_MSG(transform_client != NULL);
	__ASSERT_NO_MSG(transform_client->init_rpc != NULL);
	__ASSERT_NO_MSG(transform_client->process_fn_rpc != NULL);

	struct mpipe_element *self = &transform_client->transform.element;
	struct mpipe_transform *transform = &transform_client->transform;
	int ret;

	ret = transform_client->init_rpc();
	if (ret != 0) {
		LOG_ERR("Failed to set up RPC to the remote transform (%d)", ret);
		return ret;
	}

	ret = mpipe_transform_init(transform, id);
	if (ret != 0) {
		return ret;
	}

	mpipe_element_set_name(self, "transform_client");

	/* Only NORMAL mode is supported */
	transform->mode = MPIPE_TRANSFORM_MODE_NORMAL;

	transform->sink_pad.process_fn = mpipe_transform_client_process_fn;
	transform->decide_buffer_pool = mpipe_transform_client_decide_buffer_pool;
	transform->propose_buffer_pool = mpipe_transform_client_propose_buffer_pool;

	return 0;
}
