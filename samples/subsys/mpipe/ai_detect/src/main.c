/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Live detection with overlay: the camera stream is shown on the display
 * with the detected bounding boxes drawn on top, while the detector runs on
 * a second branch of the pipeline.
 *
 *   vid_src -> caps_filter -> tee -> queue_d -> vid_overlay -> [vid_trans] -> disp_sink
 *                                -> queue_ai -> ai_convert -> ai_infer
 *                                               -> ai_decode -> app_sink
 *
 * The overlay element is generic video plumbing: it hands each frame to an
 * application draw callback. The AI coupling lives entirely in this file:
 * the app_sink callback publishes each result set into a latest-results
 * store, and the draw callback renders it, accepting one to two frames of
 * lag. The AI branch queue holds one frame and drops the oldest, so
 * detection always works on the freshest frame.
 *
 * The fake backend (default) emits a deterministic box orbiting the frame:
 * the whole pipeline, decode and overlay run with no model at all. For a
 * real detector, build with the TFLM overlay and point AI_MODEL at a
 * quantized detection model using the TFLite_Detection_PostProcess output
 * convention.
 */

#include <zephyr/drivers/video.h>
#include <zephyr/video/controls.h>
#include <zephyr/logging/log.h>
#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/base/mpipe_app_sink.h>
#include <zephyr/mpipe/base/mpipe_caps_filter.h>
#include <zephyr/mpipe/base/mpipe_queue.h>
#include <zephyr/mpipe/base/mpipe_tee.h>
#include <zephyr/mpipe/disp/mpipe_disp_sink.h>
#include <zephyr/mpipe/vid/mpipe_vid_src.h>
#include <zephyr/mpipe/utils/mpipe_player.h>
#include <zephyr/mpipe/ai/mpipe_ai_convert.h>
#include <zephyr/mpipe/ai/mpipe_ai_decode.h>
#include <zephyr/mpipe/ai/mpipe_ai_infer.h>
#include <zephyr/mpipe/vid/mpipe_vid_draw.h>
#include <zephyr/mpipe/vid/mpipe_vid_overlay.h>
#if DT_HAS_CHOSEN(zephyr_videotrans)
#include <zephyr/mpipe/vid/mpipe_vid_transform.h>
#endif
#if !defined(CONFIG_MPIPE_AI_BACKEND_TFLM)
#include <zephyr/mpipe/ai/mpipe_ai_backend_fake.h>
#endif
#include <zephyr/sys/util_macro.h>

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

/* Element IDs (values are arbitrary; only uniqueness within the pipeline matters) */
enum {
	PIPE_ID,
	VID_SRC_ID,
	CAPS_FILTER_ID,
	TEE_ID,
	QUEUE_D_ID,
	OVERLAY_ID,
	VID_TRANS_ID,
	DISP_SINK_ID,
	QUEUE_AI_ID,
	AI_CONVERT_ID,
	AI_INFER_ID,
	AI_DECODE_ID,
	APP_SINK_ID,
};

static struct mpipe pipe;
static struct mpipe_vid_src vid_src;
static struct mpipe_caps_filter caps_filter;
static struct mpipe_tee tee;
static struct mpipe_queue queue_d;
static struct mpipe_queue queue_ai;
static struct mpipe_vid_overlay overlay;
#if (DT_HAS_CHOSEN(zephyr_videotrans))
static struct mpipe_vid_transform vid_trans;
#endif
static struct mpipe_disp_sink disp_sink;
static struct mpipe_ai_convert ai_convert;
static struct mpipe_ai_infer ai_infer;
static struct mpipe_ai_decode ai_decode;
static struct mpipe_app_sink app_sink;
static struct mpipe_player player;

/* The overlay in the display branch reads what the app_sink publishes here */
static struct mpipe_ai_results_store results_store;

#if defined(CONFIG_MPIPE_AI_BACKEND_TFLM)

static const uint8_t model_data[] __aligned(16) = {
#include "ai_model.inc"
};

static uint8_t arena[CONFIG_SAMPLE_AI_ARENA_SIZE] __aligned(16);

#else /* fake backend */

static const struct mpipe_ai_fake_model fake_model_cfg = {
	.in_w = 96,
	.in_h = 96,
	.channels = 1,
	.dtype = MPIPE_AI_TENSOR_TYPE_INT8,
	.num_classes = 0,
	.num_boxes = 2,
};

static uint8_t arena[64];

#endif /* CONFIG_MPIPE_AI_BACKEND_TFLM */

/* Runs in the display branch thread: render the latest boxes into the frame */
static void draw_cb(uint8_t *frame, uint16_t width, uint16_t height, uint32_t pixfmt,
		    void *user_data)
{
	struct mpipe_ai_results res;

	ARG_UNUSED(user_data);

	if (mpipe_ai_results_store_get(&results_store, &res) != 0 ||
	    res.type != MPIPE_AI_RESULTS_DETECTION) {
		return;
	}

	for (uint8_t i = 0; i < res.count; i++) {
		const struct mpipe_ai_box *box = &res.boxes[i];
		struct mpipe_vid_rect rect = {
			.x0 = (int32_t)(box->xmin * (float)width),
			.y0 = (int32_t)(box->ymin * (float)height),
			.x1 = (int32_t)(box->xmax * (float)width),
			.y1 = (int32_t)(box->ymax * (float)height),
		};

		(void)mpipe_vid_draw_rect(frame, width, height, pixfmt, &rect, 0x0000FF00, 2);
	}
}

/* Runs in the AI branch thread: publish the boxes for the overlay */
static void results_cb(const struct net_buf *buf, void *user_data)
{
	const struct mpipe_ai_results *res =
		(const struct mpipe_ai_results *)(const void *)buf->data;

	ARG_UNUSED(user_data);

	if (mpipe_buffer_get_meta(buf)->bytes_used < sizeof(*res)) {
		return;
	}

	mpipe_ai_results_store_put(&results_store, res);

	if (res->count > 0U) {
		LOG_INF("Frame %u: %u box(es), best score %d%% (class %u)", res->seq, res->count,
			(int)(res->boxes[0].score * 100.0f), res->boxes[0].class_id);
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
	static const uint8_t ai_queue_size = 1;
	static const enum mpipe_base_queue_leak ai_queue_leak = MPIPE_BASE_QUEUE_LEAK_OLDEST;
	static const uint8_t decode_type = MPIPE_AI_RESULTS_DETECTION;
	static const struct mpipe_vid_overlay_cb draw = {.fn = draw_cb, .user_data = NULL};
	int ret;

	mpipe_ai_results_store_init(&results_store);

	ret = mpipe_pipeline_init(&pipe, PIPE_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_vid_src_init(&vid_src, VID_SRC_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_caps_filter_init(&caps_filter, CAPS_FILTER_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_tee_init(&tee, TEE_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_queue_init(&queue_d, QUEUE_D_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_queue_init(&queue_ai, QUEUE_AI_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_vid_overlay_init(&overlay, OVERLAY_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_disp_sink_init(&disp_sink, DISP_SINK_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_ai_convert_init(&ai_convert, AI_CONVERT_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_ai_infer_init(&ai_infer, AI_INFER_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_ai_decode_init(&ai_decode, AI_DECODE_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_app_sink_init(&app_sink, APP_SINK_ID);
	if (ret < 0) {
		goto err;
	}

	struct video_rect __maybe_unused crop = {
		CONFIG_VIDEO_SOURCE_CROP_LEFT, CONFIG_VIDEO_SOURCE_CROP_TOP,
		CONFIG_VIDEO_SOURCE_CROP_WIDTH, CONFIG_VIDEO_SOURCE_CROP_HEIGHT};

	/* clang-format off */
	ret = mpipe_object_set_properties((struct mpipe_object *)&vid_src,
		COND_CODE_0(CONFIG_PROP_NUM_BUFS, (),
			    (MPIPE_PROP_SRC_NUM_BUFS, CONFIG_PROP_NUM_BUFS,))
		COND_CODE_0(CONFIG_VIDEO_SOURCE_CROP_WIDTH, (), (MPIPE_PROP_VID_CROP, &crop,))
		IF_ENABLED(CONFIG_VIDEO_CTRL_HFLIP, (VIDEO_CID_HFLIP, CONFIG_VIDEO_CTRL_HFLIP,))
		IF_ENABLED(CONFIG_VIDEO_CTRL_VFLIP, (VIDEO_CID_VFLIP, CONFIG_VIDEO_CTRL_VFLIP,))
		MPIPE_PROP_LIST_END);
	/* clang-format on */
	if (ret < 0) {
		goto err;
	}

	/* clang-format off */
	struct mpipe_structure caps;
	struct mpipe_value pixfmt;

	ret = mpipe_structure_init_fields(&caps, MPIPE_MEDIA_VIDEO,
		COND_CODE_0(CONFIG_VIDEO_FRAME_WIDTH,
			(), (MPIPE_CAPS_IMAGE_WIDTH, MPIPE_TYPE_UINT, CONFIG_VIDEO_FRAME_WIDTH,))
		COND_CODE_0(CONFIG_VIDEO_FRAME_HEIGHT,
			(), (MPIPE_CAPS_IMAGE_HEIGHT, MPIPE_TYPE_UINT, CONFIG_VIDEO_FRAME_HEIGHT,))
		MPIPE_CAPS_END);
	/* clang-format on */
	if (ret != 0) {
		goto err;
	}

	if (strcmp(CONFIG_VIDEO_PIXEL_FORMAT, "") != 0) {
		mpipe_value_set(&pixfmt, MPIPE_TYPE_UINT,
				VIDEO_FOURCC_FROM_STR(CONFIG_VIDEO_PIXEL_FORMAT));
		mpipe_structure_append_value(&caps, MPIPE_CAPS_PIXEL_FORMAT, &pixfmt);
	}

	ret = mpipe_object_set_properties((struct mpipe_object *)&caps_filter,
					  MPIPE_PROP_BASE_CAPS_FILTER_CAPS, &caps,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}

	/*
	 * The AI branch queue: one slot, dropping the oldest. Inference gets
	 * the freshest frame; capture and display never wait for it.
	 */
	struct mpipe_object *queue_ai_obj = (struct mpipe_object *)&queue_ai;

	ret = mpipe_object_set_properties(queue_ai_obj, MPIPE_PROP_BASE_QUEUE_SIZE, &ai_queue_size,
					  MPIPE_PROP_BASE_QUEUE_LEAK, &ai_queue_leak,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_object_set_properties((struct mpipe_object *)&ai_infer, MPIPE_PROP_AI_MODEL,
					  &model, MPIPE_PROP_AI_ARENA, &arena_desc,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_object_set_properties((struct mpipe_object *)&ai_decode,
					  MPIPE_PROP_AI_DECODE_TYPE, &decode_type,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_object_set_properties((struct mpipe_object *)&overlay,
					  MPIPE_PROP_VID_OVERLAY_DRAW_CB, &draw,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_object_set_properties((struct mpipe_object *)&app_sink,
					  MPIPE_PROP_BASE_APP_SINK_CB, &cb, MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}

#if (DT_HAS_CHOSEN(zephyr_videotrans))
	ret = mpipe_vid_transform_init(&vid_trans, VID_TRANS_ID);
	if (ret < 0) {
		goto err;
	}

	/* clang-format off */
	ret = mpipe_object_set_properties((struct mpipe_object *)&vid_trans,
		COND_CODE_0(CONFIG_VIDEO_ROTATION_ANGLE,
			(), (VIDEO_CID_ROTATE, CONFIG_VIDEO_ROTATION_ANGLE,)) MPIPE_PROP_LIST_END);
	/* clang-format on */
	if (ret < 0) {
		goto err;
	}
#endif

	/* clang-format off */
	/* Add elements to the pipeline - order does not matter */
	ret = mpipe_bin_add((struct mpipe_bin *)&pipe,
			(struct mpipe_element *)&vid_src,
			(struct mpipe_element *)&caps_filter,
			(struct mpipe_element *)&tee,
			(struct mpipe_element *)&queue_d,
			(struct mpipe_element *)&overlay,
			IF_ENABLED(DT_HAS_CHOSEN(zephyr_videotrans),
				   ((struct mpipe_element *)&vid_trans,))
			(struct mpipe_element *)&disp_sink,
			(struct mpipe_element *)&queue_ai,
			(struct mpipe_element *)&ai_convert,
			(struct mpipe_element *)&ai_infer,
			(struct mpipe_element *)&ai_decode,
			(struct mpipe_element *)&app_sink, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add elements (%d)", ret);
		goto err;
	}

	/* Branch 1: camera to display, boxes drawn before the transform */
	ret = mpipe_element_link((struct mpipe_element *)&vid_src,
			(struct mpipe_element *)&caps_filter,
			(struct mpipe_element *)&tee,
			(struct mpipe_element *)&queue_d,
			(struct mpipe_element *)&overlay,
			IF_ENABLED(DT_HAS_CHOSEN(zephyr_videotrans),
				   ((struct mpipe_element *)&vid_trans,))
			(struct mpipe_element *)&disp_sink, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to link branch 1 (%d)", ret);
		goto err;
	}

	/* Branch 2: tee (2nd src pad) to the detector */
	ret = mpipe_element_link((struct mpipe_element *)&tee,
			(struct mpipe_element *)&queue_ai,
			(struct mpipe_element *)&ai_convert,
			(struct mpipe_element *)&ai_infer,
			(struct mpipe_element *)&ai_decode,
			(struct mpipe_element *)&app_sink, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to link branch 2 (%d)", ret);
		goto err;
	}
	/* clang-format on */

	LOG_INF("Pipeline linked.");

	ret = mpipe_player_init(&player, &pipe);
	if (ret != 0) {
		LOG_ERR("Failed to init player (%d)", ret);
		goto err;
	}

	(void)mpipe_player_play(&player);
	(void)mpipe_player_wait_quit(&player);
	(void)mpipe_player_deinit(&player);

	LOG_INF("Done.");

	return 0;

err:
	LOG_ERR("Aborting sample");
	return 0;
}
