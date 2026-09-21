/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>

#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_dispatch.h>
#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/mpipe/mpipe_value.h>

#include <zephyr/mpipe/ai/mpipe_ai_infer.h>

LOG_MODULE_REGISTER(mpipe_ai_infer, CONFIG_MPIPE_LOG_LEVEL);

/* Output pool: one flat buffer per inference, holding every output tensor */
NET_BUF_POOL_FIXED_DEFINE(mpipe_ai_infer_pool, CONFIG_MPIPE_AI_INFER_POOL_NUM,
			  CONFIG_MPIPE_AI_INFER_MAX_OUT_SIZE, sizeof(struct mpipe_buffer_meta),
			  mpipe_buffer_destroy);

/*
 * Load the model and interrogate its tensor sets. Idempotent. Called from the
 * READY to PAUSED transition - the bin walks children sink-to-source going
 * up, so this runs before the source starts negotiating - and defensively
 * from the caps enumeration.
 */
static int ai_infer_ensure_loaded(struct mpipe_ai_infer *infer)
{
	int ret;

	if (infer->loaded) {
		return 0;
	}

	if (infer->backend.ops == NULL) {
		ret = mpipe_ai_backend_default_init(&infer->backend);
		if (ret != 0) {
			LOG_ERR("No inference backend available (%d)", ret);
			return ret;
		}
	}

	if (infer->model.data == NULL || infer->arena.data == NULL) {
		LOG_ERR("Model or arena not set; set MPIPE_PROP_AI_MODEL and MPIPE_PROP_AI_ARENA");
		return -ENODEV;
	}

	ret = infer->backend.ops->load(&infer->backend, infer->model.data, infer->model.size,
				       infer->arena.data, infer->arena.size);
	if (ret != 0) {
		LOG_ERR("Failed to load model (%d)", ret);
		return ret;
	}

	ret = infer->backend.ops->get_io(&infer->backend, &infer->in_set, &infer->out_set);
	if (ret != 0) {
		LOG_ERR("Failed to describe model I/O (%d)", ret);
		(void)infer->backend.ops->unload(&infer->backend);
		return ret;
	}

	if (infer->in_set.count != 1U) {
		LOG_ERR("Only single-input models are supported (%u inputs)", infer->in_set.count);
		(void)infer->backend.ops->unload(&infer->backend);
		return -ENOTSUP;
	}

	if (infer->out_set.total_bytes > CONFIG_MPIPE_AI_INFER_MAX_OUT_SIZE) {
		LOG_ERR("Model output %u exceeds CONFIG_MPIPE_AI_INFER_MAX_OUT_SIZE=%u",
			infer->out_set.total_bytes, CONFIG_MPIPE_AI_INFER_MAX_OUT_SIZE);
		(void)infer->backend.ops->unload(&infer->backend);
		return -ENOMEM;
	}

	infer->loaded = true;

	return 0;
}

/* Build the capability of the model's single input tensor */
static int ai_infer_input_candidate(struct mpipe_ai_infer *infer, struct mpipe_structure *out)
{
	const struct mpipe_ai_tensor_desc *in = &infer->in_set.desc[0];
	struct mpipe_value v;
	int ret;

	ret = mpipe_structure_init(out, MPIPE_MEDIA_TENSOR);
	if (ret != 0) {
		return ret;
	}

	v.type = MPIPE_TYPE_UINT;
	v.v_uint = in->dtype;
	ret = mpipe_structure_append_value(out, MPIPE_CAPS_TENSOR_TYPE, &v);
	if (ret != 0) {
		goto err;
	}

	/* An image-shaped input: NHWC with N == 1 */
	if (in->num_dims == 4U) {
		v.type = MPIPE_TYPE_UINT;
		v.v_uint = in->dims[3];
		ret = mpipe_structure_append_value(out, MPIPE_CAPS_NUM_OF_CHANNEL, &v);
		if (ret != 0) {
			goto err;
		}

		v.type = MPIPE_TYPE_UINT;
		v.v_uint = in->dims[2];
		ret = mpipe_structure_append_value(out, MPIPE_CAPS_IMAGE_WIDTH, &v);
		if (ret != 0) {
			goto err;
		}

		v.type = MPIPE_TYPE_UINT;
		v.v_uint = in->dims[1];
		ret = mpipe_structure_append_value(out, MPIPE_CAPS_IMAGE_HEIGHT, &v);
		if (ret != 0) {
			goto err;
		}
	}

	return 0;
err:
	mpipe_structure_clear(out);

	return ret;
}

static int ai_infer_enum_caps(struct mpipe_pad *pad, uint32_t index,
			      const struct mpipe_structure *filter, struct mpipe_structure *out)
{
	struct mpipe_ai_infer *infer = (struct mpipe_ai_infer *)pad->object.container;
	struct mpipe_structure candidate;
	int ret;

	/* A fixed model has exactly one input capability */
	if (index > 0U) {
		return -ENOENT;
	}

	ret = ai_infer_ensure_loaded(infer);
	if (ret != 0) {
		return ret;
	}

	ret = ai_infer_input_candidate(infer, &candidate);
	if (ret != 0) {
		return ret;
	}

	return mpipe_pad_enum_filter(&candidate, filter, out);
}

static int ai_infer_transform_caps(struct mpipe_transform *self, enum mpipe_pad_direction direction,
				   const struct mpipe_structure *in, uint32_t index,
				   struct mpipe_structure *out)
{
	struct mpipe_ai_infer *infer = (struct mpipe_ai_infer *)self;
	struct mpipe_value v;
	int ret;

	if (in == NULL) {
		return -EINVAL;
	}

	/* The model fixes both sides: one transformation, whatever comes in */
	if (index > 0U) {
		return -ENOENT;
	}

	ret = ai_infer_ensure_loaded(infer);
	if (ret != 0) {
		return ret;
	}

	if (direction == MPIPE_PAD_SRC) {
		ret = mpipe_structure_init(out, MPIPE_MEDIA_TENSOR);
		if (ret != 0) {
			return ret;
		}

		v.type = MPIPE_TYPE_UINT;
		v.v_uint = infer->out_set.desc[0].dtype;
		ret = mpipe_structure_append_value(out, MPIPE_CAPS_TENSOR_TYPE, &v);
		if (ret != 0) {
			mpipe_structure_clear(out);
			return ret;
		}
	} else {
		ret = ai_infer_input_candidate(infer, out);
		if (ret != 0) {
			return ret;
		}
	}

	/* Inference changes the tensors, not the rate; an input without one is fine */
	if (!mpipe_structure_is_any(in)) {
		ret = mpipe_structure_copy_field(in, out, MPIPE_CAPS_FRAME_INTERVAL);
		if (ret != 0 && ret != -ENOENT) {
			mpipe_structure_clear(out);
			return ret;
		}
	}

	return 0;
}

static int ai_infer_set_caps(struct mpipe_transform *transform, enum mpipe_pad_direction direction,
			     const struct mpipe_structure *caps)
{
	struct mpipe_ai_infer *infer = (struct mpipe_ai_infer *)transform;
	int ret;

	if (caps == NULL) {
		return -EINVAL;
	}

	ret = mpipe_transform_set_caps(transform, direction, caps);
	if (ret < 0) {
		return ret;
	}

	if (direction == MPIPE_PAD_SRC && infer->loaded) {
		infer->out_pool.config.size = infer->out_set.total_bytes;
	}

	return 0;
}

static int ai_infer_decide_buffer_pool(struct mpipe_transform *self, struct mpipe_dispatch *query)
{
	struct mpipe_ai_infer *infer = (struct mpipe_ai_infer *)self;

	ARG_UNUSED(query);

	/*
	 * Always the internal pool, ignoring downstream proposals: every
	 * produced buffer carries a pointer to this element's output tensor
	 * set in its metadata, a contract only buffers of this pool honor.
	 */
	self->out_pool = &infer->out_pool;

	return 0;
}

static int ai_infer_chain_fn(struct mpipe_pad *pad, struct net_buf *in_buf,
			     struct net_buf **out_buf)
{
	struct mpipe_transform *transform = (struct mpipe_transform *)pad->object.container;
	struct mpipe_ai_infer *infer = (struct mpipe_ai_infer *)transform;
	struct mpipe_buffer_pool *out_pool = transform->out_pool;
	struct net_buf *cur;
	struct net_buf *next;

	if (!infer->loaded) {
		LOG_ERR("No model loaded");
		net_buf_unref(in_buf);
		return -EINVAL;
	}

	*out_buf = NULL;
	cur = in_buf;
	while (cur != NULL) {
		struct net_buf *out = NULL;
		struct mpipe_buffer_meta *meta;
		int ret;

		if (mpipe_buffer_get_meta(cur)->bytes_used < infer->in_set.desc[0].bytes) {
			LOG_ERR("Input tensor too small: %u < %u",
				mpipe_buffer_get_meta(cur)->bytes_used,
				infer->in_set.desc[0].bytes);
			goto err;
		}

		if (out_pool->acquire_buffer(out_pool, &out) != 0 || out == NULL) {
			LOG_ERR("Failed to acquire output buffer");
			goto err;
		}

		ret = infer->backend.ops->invoke(&infer->backend, cur->data,
						 infer->in_set.desc[0].bytes, out->data,
						 infer->out_set.total_bytes);
		if (ret != 0) {
			LOG_ERR("Inference failed (%d)", ret);
			net_buf_unref(out);
			goto err;
		}

		meta = mpipe_buffer_get_meta(out);
		meta->bytes_used = infer->out_set.total_bytes;
		meta->pts = mpipe_buffer_get_meta(cur)->pts;
		meta->priv = &infer->out_set;
		out->len = infer->out_set.total_bytes;

		if (*out_buf == NULL) {
			*out_buf = out;
		} else {
			net_buf_frag_add(*out_buf, out);
		}

		next = cur->frags;
		cur->frags = NULL;
		net_buf_unref(cur);
		cur = next;
	}

	return 0;
err:
	net_buf_unref(cur);
	if (*out_buf != NULL) {
		net_buf_unref(*out_buf);
		*out_buf = NULL;
	}

	return -EIO;
}

static int ai_infer_change_state(struct mpipe_element *element, enum mpipe_state_change transition)
{
	struct mpipe_ai_infer *infer = (struct mpipe_ai_infer *)element;
	int ret;

	switch (transition) {
	case MPIPE_STATE_CHANGE_READY_TO_PAUSED:
		ret = ai_infer_ensure_loaded(infer);
		if (ret != 0) {
			return ret;
		}
		break;
	default:
		/* The model stays loaded across replay; only caps reset */
		break;
	}

	return mpipe_transform_change_state(element, transition);
}

static int mpipe_ai_infer_set_property(struct mpipe_object *obj, uint32_t key, const void *val)
{
	struct mpipe_ai_infer *infer = (struct mpipe_ai_infer *)obj;

	if (val == NULL) {
		return -EINVAL;
	}

	switch (key) {
	case MPIPE_PROP_AI_MODEL:
		infer->model = *(const struct mpipe_ai_model *)val;
		break;
	case MPIPE_PROP_AI_ARENA:
		infer->arena = *(const struct mpipe_ai_arena *)val;
		break;
	default:
		return -ENOTSUP;
	}

	/* A new model or arena invalidates whatever was loaded */
	if (infer->loaded) {
		(void)infer->backend.ops->unload(&infer->backend);
		infer->loaded = false;
	}

	return 0;
}

static int mpipe_ai_infer_out_pool_acquire(struct mpipe_buffer_pool *pool, struct net_buf **buf)
{
	struct net_buf *out;
	struct mpipe_buffer_meta *m;

	__ASSERT_NO_MSG(pool != NULL);
	__ASSERT_NO_MSG(buf != NULL);

	out = net_buf_alloc_len(pool->nb_pool, pool->config.size, K_NO_WAIT);
	if (out == NULL) {
		return -ENOBUFS;
	}

	m = mpipe_buffer_get_meta(out);
	m->pool = pool;
	m->bytes_used = 0;
	m->pts = 0;
	m->driver_buf = NULL;
	m->priv = NULL;
	out->len = 0;

	*buf = out;

	return 0;
}

static int mpipe_ai_infer_out_pool_release(struct mpipe_buffer_pool *pool, struct net_buf *buf)
{
	ARG_UNUSED(pool);

	__ASSERT_NO_MSG(buf != NULL);

	struct mpipe_buffer_meta *m = mpipe_buffer_get_meta(buf);

	if (m != NULL) {
		m->bytes_used = 0;
		m->pts = 0;
		m->driver_buf = NULL;
		m->priv = NULL;
	}

	buf->len = 0;

	return 0;
}

int mpipe_ai_infer_init(struct mpipe_ai_infer *infer, uint8_t id)
{
	__ASSERT_NO_MSG(infer != NULL);

	struct mpipe_element *self = &infer->transform.element;
	struct mpipe_transform *transform = &infer->transform;
	int ret = mpipe_transform_init(transform, id);

	if (ret != 0) {
		return ret;
	}

	mpipe_element_set_name(self, "ai_infer");

	self->object.set_property = mpipe_ai_infer_set_property;
	self->change_state = ai_infer_change_state;

	transform->sink_pad.enum_caps_fn = ai_infer_enum_caps;

	infer->backend.ops = NULL;
	infer->backend.priv = NULL;
	infer->model.data = NULL;
	infer->model.size = 0;
	infer->arena.data = NULL;
	infer->arena.size = 0;
	infer->loaded = false;
	memset(&infer->in_set, 0, sizeof(infer->in_set));
	memset(&infer->out_set, 0, sizeof(infer->out_set));

	/* The net_buf pool is static: no start hook, the size follows the model */
	const struct mpipe_buffer_pool_config pool_req = {
		.size = 0,
		.align = 1,
		.min_buffers = CONFIG_MPIPE_AI_INFER_POOL_NUM,
		.max_buffers = CONFIG_MPIPE_AI_INFER_POOL_NUM,
	};

	mpipe_buffer_pool_init(&infer->out_pool);
	infer->out_pool.nb_pool = &mpipe_ai_infer_pool;
	(void)mpipe_buffer_pool_set_req_config(&infer->out_pool, &pool_req);
	infer->out_pool.acquire_buffer = mpipe_ai_infer_out_pool_acquire;
	infer->out_pool.release_buffer = mpipe_ai_infer_out_pool_release;

	transform->mode = MPIPE_MODE_NORMAL;
	transform->out_pool = &infer->out_pool;
	transform->set_caps = ai_infer_set_caps;
	transform->transform_caps = ai_infer_transform_caps;
	transform->propose_buffer_pool = NULL;
	transform->decide_buffer_pool = ai_infer_decide_buffer_pool;
	transform->sink_pad.chain_fn = ai_infer_chain_fn;

	return 0;
}
