/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/mpipe/ai/mpipe_ai_backend.h>
#include <zephyr/mpipe/ai/mpipe_ai_backend_fake.h>

LOG_MODULE_REGISTER(mpipe_ai_backend_fake, CONFIG_MPIPE_LOG_LEVEL);

/* One static instance is enough for tests and bring-up */
static struct mpipe_ai_fake_model fake_instance;
static bool fake_in_use;

static int fake_load(struct mpipe_ai_backend *be, const void *model, uint32_t model_size,
		     void *arena, uint32_t arena_size)
{
	ARG_UNUSED(arena);
	ARG_UNUSED(arena_size);

	if (model == NULL || model_size != sizeof(struct mpipe_ai_fake_model)) {
		LOG_ERR("A fake model is a struct mpipe_ai_fake_model (%u bytes, got %u)",
			(uint32_t)sizeof(struct mpipe_ai_fake_model), model_size);
		return -EINVAL;
	}

	if (fake_in_use) {
		return -EBUSY;
	}

	memcpy(&fake_instance, model, sizeof(fake_instance));

	if (fake_instance.in_w == 0U || fake_instance.in_h == 0U || fake_instance.channels == 0U ||
	    (fake_instance.num_classes == 0U && fake_instance.num_boxes == 0U) ||
	    mpipe_ai_tensor_type_size(fake_instance.dtype) == 0U) {
		return -EINVAL;
	}

	fake_in_use = true;
	be->priv = &fake_instance;

	return 0;
}

static int fake_get_io(struct mpipe_ai_backend *be, struct mpipe_ai_tensor_set *in,
		       struct mpipe_ai_tensor_set *out)
{
	const struct mpipe_ai_fake_model *cfg = be->priv;

	if (cfg == NULL) {
		return -EINVAL;
	}

	memset(in, 0, sizeof(*in));
	in->count = 1;
	in->desc[0].dtype = cfg->dtype;
	in->desc[0].num_dims = 4;
	in->desc[0].dims[0] = 1;
	in->desc[0].dims[1] = cfg->in_h;
	in->desc[0].dims[2] = cfg->in_w;
	in->desc[0].dims[3] = cfg->channels;
	in->desc[0].bytes = (uint32_t)cfg->in_w * cfg->in_h * cfg->channels *
			    mpipe_ai_tensor_type_size(cfg->dtype);
	in->desc[0].offset = 0;
	in->desc[0].scale = 0.0f;
	in->desc[0].zero_point = 0;
	in->total_bytes = in->desc[0].bytes;

	memset(out, 0, sizeof(*out));

	if (cfg->num_boxes > 0U) {
		/*
		 * The four-tensor detection convention, all float32:
		 * boxes [1,N,4], classes [1,N], scores [1,N], count [1]
		 */
		uint32_t offset = 0;

		out->count = 4;
		for (uint8_t i = 0; i < 4U; i++) {
			struct mpipe_ai_tensor_desc *d = &out->desc[i];

			d->dtype = MPIPE_AI_TENSOR_TYPE_FLOAT32;
			d->dims[0] = 1;
			switch (i) {
			case 0:
				d->num_dims = 3;
				d->dims[1] = cfg->num_boxes;
				d->dims[2] = 4;
				d->bytes = (uint32_t)cfg->num_boxes * 4U * sizeof(float);
				break;
			case 3:
				d->num_dims = 1;
				d->bytes = sizeof(float);
				break;
			default:
				d->num_dims = 2;
				d->dims[1] = cfg->num_boxes;
				d->bytes = (uint32_t)cfg->num_boxes * sizeof(float);
				break;
			}
			d->offset = ROUND_UP(offset, 4);
			offset = d->offset + d->bytes;
		}
		out->total_bytes = offset;

		return 0;
	}

	out->count = 1;
	out->desc[0].dtype = MPIPE_AI_TENSOR_TYPE_INT8;
	out->desc[0].num_dims = 2;
	out->desc[0].dims[0] = 1;
	out->desc[0].dims[1] = cfg->num_classes;
	out->desc[0].bytes = cfg->num_classes;
	out->desc[0].offset = 0;
	out->desc[0].scale = 1.0f / 256.0f;
	out->desc[0].zero_point = -128;
	out->total_bytes = out->desc[0].bytes;

	return 0;
}

/* Advances once per inference so the fake detection box orbits the frame */
static uint32_t fake_phase;

static int fake_invoke(struct mpipe_ai_backend *be, const uint8_t *in, uint32_t in_size,
		       uint8_t *out, uint32_t out_size)
{
	const struct mpipe_ai_fake_model *cfg = be->priv;
	uint32_t sum = 0;
	uint16_t win;

	if (cfg == NULL || in == NULL || out == NULL) {
		return -EINVAL;
	}

	if (cfg->num_boxes > 0U) {
		/* One deterministic box sweeping the frame, the rest static */
		float *boxes = (float *)(void *)out;
		float *classes = &boxes[(uint32_t)cfg->num_boxes * 4U];
		float *scores = &classes[cfg->num_boxes];
		float *count = &scores[cfg->num_boxes];
		float t = (float)(fake_phase % 32U) / 32.0f;

		if (out_size < ((uint32_t)cfg->num_boxes * 6U + 1U) * sizeof(float)) {
			return -EINVAL;
		}

		fake_phase++;

		for (uint16_t i = 0; i < cfg->num_boxes; i++) {
			float base = 0.1f + 0.5f * t;

			if (i > 0U) {
				base = 0.1f + 0.15f * (float)i;
				t = 0.0f;
			}

			boxes[4U * i] = 0.25f;            /* ymin */
			boxes[4U * i + 1U] = base;        /* xmin */
			boxes[4U * i + 2U] = 0.75f;       /* ymax */
			boxes[4U * i + 3U] = base + 0.2f; /* xmax */
			classes[i] = (float)i;
			scores[i] = 0.9f - 0.2f * (float)i;
		}
		*count = (float)cfg->num_boxes;

		return 0;
	}

	if (out_size < cfg->num_classes) {
		return -EINVAL;
	}

	/* Deterministic pseudo-inference: the input decides the winning class */
	for (uint32_t i = 0; i < in_size; i++) {
		sum += in[i];
	}

	win = (uint16_t)(sum % cfg->num_classes);

	for (uint16_t i = 0; i < cfg->num_classes; i++) {
		((int8_t *)out)[i] = (i == win) ? INT8_MAX : INT8_MIN;
	}

	return 0;
}

static int fake_unload(struct mpipe_ai_backend *be)
{
	if (be->priv != NULL) {
		be->priv = NULL;
		fake_in_use = false;
	}

	return 0;
}

static const struct mpipe_ai_backend_ops fake_ops = {
	.load = fake_load,
	.get_io = fake_get_io,
	.invoke = fake_invoke,
	.unload = fake_unload,
};

int mpipe_ai_backend_default_init(struct mpipe_ai_backend *be)
{
	if (be == NULL) {
		return -EINVAL;
	}

	be->ops = &fake_ops;
	be->priv = NULL;

	return 0;
}
