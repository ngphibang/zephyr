/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The "hello inference" of the Multimedia Pipeline framework: the
 * application pushes a static grayscale image into the pipeline, a neural
 * network classifies it, and the results come back to the application.
 *
 *   app_src -> ai_convert -> ai_infer -> ai_decode -> app_sink
 *
 * With the TFLM backend the model is the stock person detection network from
 * the tflite-micro module and the images are its test pictures: watch the
 * person score flip between them. With the fake backend (the default, no
 * module needed) two synthetic patterns exercise the identical pipeline.
 */

#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/zbus/zbus.h>

#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/base/mpipe_app_sink.h>
#include <zephyr/mpipe/base/mpipe_app_src.h>
#include <zephyr/mpipe/ai/mpipe_ai_convert.h>
#include <zephyr/mpipe/ai/mpipe_ai_decode.h>
#include <zephyr/mpipe/ai/mpipe_ai_infer.h>

#if !defined(CONFIG_MPIPE_AI_BACKEND_TFLM)
#include <zephyr/mpipe/ai/mpipe_ai_backend_fake.h>
#endif

#define IMG_DIM  96
#define IMG_SIZE (IMG_DIM * IMG_DIM)

/* Element IDs (values are arbitrary; only uniqueness within the pipeline matters) */
enum {
	PIPE_ID,
	SRC_ID,
	CONVERT_ID,
	INFER_ID,
	DECODE_ID,
	SINK_ID,
};

ZBUS_MSG_SUBSCRIBER_DEFINE(ai_hello_sub);

static struct mpipe pipeline;
static struct mpipe_app_src app_src;
static struct mpipe_ai_convert convert;
static struct mpipe_ai_infer infer;
static struct mpipe_ai_decode decode;
static struct mpipe_app_sink app_sink;

#if defined(CONFIG_MPIPE_AI_BACKEND_TFLM)

static const uint8_t model_data[] __aligned(16) = {
#include "ai_model.inc"
};

static const uint8_t person_bmp[] = {
#include "person_bmp.inc"
};

static const uint8_t no_person_bmp[] = {
#include "no_person_bmp.inc"
};

static uint8_t arena[CONFIG_SAMPLE_AI_ARENA_SIZE] __aligned(16);

/* Class order of the person detection model */
static const char *const labels[] = {"no person", "person"};

/*
 * Unpack an uncompressed 8-bit BMP into a top-down grayscale image. BMP rows
 * are stored bottom-up; the pixel data offset is in the file header.
 */
static void bmp_unpack(const uint8_t *bmp, uint8_t *img)
{
	uint32_t offset = (uint32_t)bmp[10] | ((uint32_t)bmp[11] << 8) | ((uint32_t)bmp[12] << 16) |
			  ((uint32_t)bmp[13] << 24);

	for (uint32_t y = 0; y < IMG_DIM; y++) {
		memcpy(&img[y * IMG_DIM], &bmp[offset + (IMG_DIM - 1U - y) * IMG_DIM], IMG_DIM);
	}
}

#else /* fake backend */

static const struct mpipe_ai_fake_model fake_model_cfg = {
	.in_w = IMG_DIM,
	.in_h = IMG_DIM,
	.channels = 1,
	.dtype = MPIPE_AI_TENSOR_TYPE_INT8,
	.num_classes = 2,
};

static uint8_t arena[64];

static const char *const labels[] = {"class 0", "class 1"};

#endif /* CONFIG_MPIPE_AI_BACKEND_TFLM */

static uint8_t image_a[IMG_SIZE];
static uint8_t image_b[IMG_SIZE];

/* Runs in the pipeline thread: print the classification of one image */
static void results_cb(const struct net_buf *buf, void *user_data)
{
	const struct mpipe_ai_results *res =
		(const struct mpipe_ai_results *)(const void *)buf->data;

	ARG_UNUSED(user_data);

	if (mpipe_buffer_get_meta(buf)->bytes_used < sizeof(*res) || res->count == 0U) {
		return;
	}

	printk("Image %u: %s (score %d%%)\n", res->seq,
	       (res->cls[0].class_id < ARRAY_SIZE(labels)) ? labels[res->cls[0].class_id] : "?",
	       (int)(res->cls[0].score * 100.0f));
}

/* Push one image a number of times; a failed push is reported, not fatal */
static void push_image(const uint8_t *image, size_t size)
{
	int ret;

	/*
	 * A generous timeout: on an emulator with reference kernels one
	 * inference can take tens of seconds, and the push blocks while the
	 * small buffer pool is in use downstream.
	 */
	for (int i = 0; i < CONFIG_SAMPLE_AI_PUSH_COUNT; i++) {
		ret = mpipe_app_src_push(&app_src, image, size, K_MINUTES(2));
		if (ret != 0) {
			printk("Push failed (%d)\n", ret);
		}
	}
}

int main(void)
{
	static const struct mpipe_app_sink_cb cb = {.fn = results_cb, .user_data = NULL};
#if defined(CONFIG_MPIPE_AI_BACKEND_TFLM)
	static const struct mpipe_ai_model model = {.data = model_data, .size = sizeof(model_data)};
#else
	static const struct mpipe_ai_model model = {.data = (const uint8_t *)&fake_model_cfg,
						    .size = sizeof(fake_model_cfg)};
#endif
	static const struct mpipe_ai_arena arena_desc = {.data = arena, .size = sizeof(arena)};
	struct mpipe_structure caps;
	const struct zbus_channel *chan;
	struct zbus_channel *bus;
	struct mpipe_message msg;
	int ret;

#if defined(CONFIG_MPIPE_AI_BACKEND_TFLM)
	bmp_unpack(person_bmp, image_a);
	bmp_unpack(no_person_bmp, image_b);
#else
	/* Two patterns the fake backend classifies differently */
	memset(image_a, 0, sizeof(image_a));
	memset(image_b, 0, sizeof(image_b));
	image_b[0] = 1;
#endif

	ret = mpipe_pipeline_init(&pipeline, PIPE_ID);
	if (ret != 0) {
		goto err;
	}
	ret = mpipe_app_src_init(&app_src, SRC_ID);
	if (ret != 0) {
		goto err;
	}
	ret = mpipe_ai_convert_init(&convert, CONVERT_ID);
	if (ret != 0) {
		goto err;
	}
	ret = mpipe_ai_infer_init(&infer, INFER_ID);
	if (ret != 0) {
		goto err;
	}
	ret = mpipe_ai_decode_init(&decode, DECODE_ID);
	if (ret != 0) {
		goto err;
	}
	ret = mpipe_app_sink_init(&app_sink, SINK_ID);
	if (ret != 0) {
		goto err;
	}

	ret = mpipe_structure_init_fields(
		&caps, MPIPE_MEDIA_VIDEO, MPIPE_CAPS_PIXEL_FORMAT, MPIPE_TYPE_UINT,
		VIDEO_PIX_FMT_GREY, MPIPE_CAPS_IMAGE_WIDTH, MPIPE_TYPE_UINT, IMG_DIM,
		MPIPE_CAPS_IMAGE_HEIGHT, MPIPE_TYPE_UINT, IMG_DIM, MPIPE_CAPS_END);
	if (ret != 0) {
		goto err;
	}

	ret = mpipe_object_set_properties((struct mpipe_object *)&app_src,
					  MPIPE_PROP_BASE_APP_SRC_CAPS, &caps, MPIPE_PROP_LIST_END);
	if (ret != 0) {
		goto err;
	}

	ret = mpipe_object_set_properties((struct mpipe_object *)&infer, MPIPE_PROP_AI_MODEL,
					  &model, MPIPE_PROP_AI_ARENA, &arena_desc,
					  MPIPE_PROP_LIST_END);
	if (ret != 0) {
		goto err;
	}

	ret = mpipe_object_set_properties((struct mpipe_object *)&app_sink,
					  MPIPE_PROP_BASE_APP_SINK_CB, &cb, MPIPE_PROP_LIST_END);
	if (ret != 0) {
		goto err;
	}

	ret = mpipe_bin_add((struct mpipe_bin *)&pipeline, (struct mpipe_element *)&app_src,
			    (struct mpipe_element *)&convert, (struct mpipe_element *)&infer,
			    (struct mpipe_element *)&decode, (struct mpipe_element *)&app_sink,
			    NULL);
	if (ret != 0) {
		goto err;
	}

	ret = mpipe_element_link((struct mpipe_element *)&app_src, (struct mpipe_element *)&convert,
				 (struct mpipe_element *)&infer, (struct mpipe_element *)&decode,
				 (struct mpipe_element *)&app_sink, NULL);
	if (ret != 0) {
		goto err;
	}

	/* Listen for end of stream before anything can produce it */
	bus = mpipe_element_get_bus_chan((struct mpipe_element *)&pipeline);
	ret = zbus_chan_add_obs(bus, &ai_hello_sub, K_FOREVER);
	if (ret != 0) {
		goto err;
	}

	ret = mpipe_element_set_state((struct mpipe_element *)&pipeline, MPIPE_STATE_PLAYING);
	if (ret != 0) {
		goto err;
	}

	printk("Pipeline PLAYING, classifying...\n");

	push_image(image_a, sizeof(image_a));
	push_image(image_b, sizeof(image_b));
	(void)mpipe_app_src_eos(&app_src, K_MINUTES(2));

	/* Wait for end of stream or an error, whichever comes first */
	do {
		ret = zbus_sub_wait_msg(&ai_hello_sub, &chan, &msg, K_FOREVER);
	} while (ret == 0 && msg.type != MPIPE_MESSAGE_EOS && msg.type != MPIPE_MESSAGE_ERROR);

	(void)mpipe_element_set_state((struct mpipe_element *)&pipeline, MPIPE_STATE_READY);
	(void)zbus_chan_rm_obs(bus, &ai_hello_sub, K_FOREVER);

	if (msg.type == MPIPE_MESSAGE_ERROR) {
		printk("Pipeline error %d\n", msg.code);
		return -EIO;
	}

	printk("End of stream\n");

	return 0;

err:
	printk("Aborting sample (%d)\n", ret);
	return ret;
}
