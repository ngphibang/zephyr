/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/video.h>
#include <zephyr/ztest.h>
#include <zephyr/zbus/zbus.h>

#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/base/mpipe_app_sink.h>
#include <zephyr/mpipe/base/mpipe_app_src.h>
#include <zephyr/mpipe/ai/mpipe_ai_backend_fake.h>
#include <zephyr/mpipe/ai/mpipe_ai_convert.h>
#include <zephyr/mpipe/ai/mpipe_ai_decode.h>
#include <zephyr/mpipe/ai/mpipe_ai_infer.h>

ZBUS_MSG_SUBSCRIBER_DEFINE(test_ai_pipeline_sub);

/* Element IDs (values are arbitrary; only uniqueness within the pipeline matters) */
enum {
	PIPE_ID,
	SRC_ID,
	CONVERT_ID,
	INFER_ID,
	DECODE_ID,
	SINK_ID,
};

/* Number of images the application pushes before EOS */
#define TEST_BUFS_NUM 10

/* The fake model: a 16x16 single-channel int8 classifier with 4 classes */
#define TEST_DIM         16
#define TEST_NUM_CLASSES 4

static const struct mpipe_ai_fake_model fake_model_cfg = {
	.in_w = TEST_DIM,
	.in_h = TEST_DIM,
	.channels = 1,
	.dtype = MPIPE_AI_TENSOR_TYPE_INT8,
	.num_classes = TEST_NUM_CLASSES,
};

static uint8_t test_image[TEST_DIM * TEST_DIM];
static uint8_t test_arena[64];

static struct {
	atomic_t count;
	struct mpipe_ai_results last;
} cb_state;

/* Runs in the pipeline thread: copy the results snapshot out */
static void test_results_cb(const struct net_buf *buf, void *user_data)
{
	ARG_UNUSED(user_data);

	if (mpipe_buffer_get_meta(buf)->bytes_used >= sizeof(struct mpipe_ai_results)) {
		cb_state.last = *(const struct mpipe_ai_results *)(const void *)buf->data;
		atomic_inc(&cb_state.count);
	}
}

struct test_ai_pipeline_fixture {
	struct mpipe pipeline;
	struct mpipe_app_src app_src;
	struct mpipe_ai_convert convert;
	struct mpipe_ai_infer infer;
	struct mpipe_ai_decode decode;
	struct mpipe_app_sink app_sink;
};

static void *ai_pipeline_suite_setup(void)
{
	static struct test_ai_pipeline_fixture fixture;

	/* A known image: the fake inference derives the winning class from it */
	for (size_t i = 0; i < sizeof(test_image); i++) {
		test_image[i] = (uint8_t)i;
	}

	return &fixture;
}

static void ai_pipeline_before(void *f)
{
	struct test_ai_pipeline_fixture *fix = f;
	static const struct mpipe_ai_model model = {
		.data = (const uint8_t *)&fake_model_cfg,
		.size = sizeof(fake_model_cfg),
	};
	static const struct mpipe_ai_arena arena = {
		.data = test_arena,
		.size = sizeof(test_arena),
	};
	static const struct mpipe_app_sink_cb cb = {
		.fn = test_results_cb,
		.user_data = NULL,
	};
	struct mpipe_structure caps;

	memset(fix, 0, sizeof(*fix));
	atomic_set(&cb_state.count, 0);
	memset(&cb_state.last, 0, sizeof(cb_state.last));

	zassert_ok(mpipe_pipeline_init(&fix->pipeline, PIPE_ID));
	zassert_ok(mpipe_app_src_init(&fix->app_src, SRC_ID));
	zassert_ok(mpipe_ai_convert_init(&fix->convert, CONVERT_ID));
	zassert_ok(mpipe_ai_infer_init(&fix->infer, INFER_ID));
	zassert_ok(mpipe_ai_decode_init(&fix->decode, DECODE_ID));
	zassert_ok(mpipe_app_sink_init(&fix->app_sink, SINK_ID));

	/* The application pushes grey 16x16 frames */
	zassert_ok(mpipe_structure_init_fields(
			   &caps, MPIPE_MEDIA_VIDEO, MPIPE_CAPS_PIXEL_FORMAT, MPIPE_TYPE_UINT,
			   VIDEO_PIX_FMT_GREY, MPIPE_CAPS_IMAGE_WIDTH, MPIPE_TYPE_UINT, TEST_DIM,
			   MPIPE_CAPS_IMAGE_HEIGHT, MPIPE_TYPE_UINT, TEST_DIM, MPIPE_CAPS_END),
		   "Failed to init app_src caps");
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&fix->app_src,
					       MPIPE_PROP_BASE_APP_SRC_CAPS, &caps,
					       MPIPE_PROP_LIST_END),
		   "Failed to set app_src caps");

	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&fix->infer,
					       MPIPE_PROP_AI_MODEL, &model, MPIPE_PROP_AI_ARENA,
					       &arena, MPIPE_PROP_LIST_END),
		   "Failed to set infer properties");

	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&fix->app_sink,
					       MPIPE_PROP_BASE_APP_SINK_CB, &cb,
					       MPIPE_PROP_LIST_END),
		   "Failed to set app_sink callback");
}

static void ai_pipeline_after(void *f)
{
	struct test_ai_pipeline_fixture *fix = f;
	struct zbus_channel *bus =
		mpipe_element_get_bus_chan((struct mpipe_element *)&fix->pipeline);

	/* Also runs when the test failed before detaching its own observer. */
	(void)zbus_chan_rm_obs(bus, &test_ai_pipeline_sub, K_FOREVER);
}

ZTEST_SUITE(test_ai_pipeline, NULL, ai_pipeline_suite_setup, ai_pipeline_before, ai_pipeline_after,
	    NULL);

/*
 * app_src(grey 16x16 image) -> ai_convert -> ai_infer(fake backend) ->
 * ai_decode -> app_sink. Negotiation must settle on the model's input tensor,
 * every pushed image must produce one deterministic classification result at
 * the app_sink, and EOS must reach the bus, on every run of a replay.
 */
ZTEST_F(test_ai_pipeline, test_pipeline_ai_classification)
{
	const struct zbus_channel *chan;
	struct zbus_channel *bus;
	struct mpipe_message msg;
	uint32_t sum = 0;
	uint16_t expected_class;

	/*
	 * The converter stores each pixel as its raw byte (the int8 tensor
	 * reinterprets it). The fake backend crowns class (sum of tensor
	 * bytes) mod num_classes.
	 */
	for (size_t i = 0; i < sizeof(test_image); i++) {
		sum += test_image[i];
	}
	expected_class = (uint16_t)(sum % TEST_NUM_CLASSES);

	zassert_ok(mpipe_bin_add((struct mpipe_bin *)&fixture->pipeline,
				 (struct mpipe_element *)&fixture->app_src,
				 (struct mpipe_element *)&fixture->convert,
				 (struct mpipe_element *)&fixture->infer,
				 (struct mpipe_element *)&fixture->decode,
				 (struct mpipe_element *)&fixture->app_sink, NULL),
		   "Failed to add elements");

	zassert_ok(mpipe_element_link((struct mpipe_element *)&fixture->app_src,
				      (struct mpipe_element *)&fixture->convert,
				      (struct mpipe_element *)&fixture->infer,
				      (struct mpipe_element *)&fixture->decode,
				      (struct mpipe_element *)&fixture->app_sink, NULL),
		   "Failed to link elements");

	bus = mpipe_element_get_bus_chan((struct mpipe_element *)&fixture->pipeline);
	zassert_ok(zbus_chan_add_obs(bus, &test_ai_pipeline_sub, K_FOREVER),
		   "Failed to add observer to pipeline channel");

	/* A replay is where state left behind by the previous run shows up */
	for (int run = 0; run < 3; run++) {
		atomic_set(&cb_state.count, 0);

		zassert_ok(mpipe_element_set_state((struct mpipe_element *)&fixture->pipeline,
						   MPIPE_STATE_PLAYING),
			   "run %d failed to start PLAYING", run);

		/* Stream the image from the application, then declare end of stream */
		for (int i = 0; i < TEST_BUFS_NUM; i++) {
			zassert_ok(mpipe_app_src_push(&fixture->app_src, test_image,
						      sizeof(test_image), K_SECONDS(1)),
				   "run %d failed to push image %d", run, i);
		}
		zassert_ok(mpipe_app_src_eos(&fixture->app_src, K_SECONDS(1)),
			   "run %d failed to push EOS", run);

		/* Wait for EOS posted by the app_sink */
		zassert_ok(zbus_sub_wait_msg(&test_ai_pipeline_sub, &chan, &msg, K_FOREVER),
			   "run %d timed out waiting for a pipeline message", run);
		zassert_equal(msg.type, MPIPE_MESSAGE_EOS, "run %d: expected EOS, got %d", run,
			      msg.type);

		/* Exactly one EOS per run: a second would mean the aggregation leaked */
		zassert_equal(zbus_sub_wait_msg(&test_ai_pipeline_sub, &chan, &msg, K_MSEC(50)),
			      -ENOMSG, "run %d produced more than one message", run);

		/* Every image produced one result, in order, numbered from one */
		zassert_equal(atomic_get(&cb_state.count), TEST_BUFS_NUM,
			      "run %d: expected %d results, got %ld", run, TEST_BUFS_NUM,
			      atomic_get(&cb_state.count));
		zassert_equal(cb_state.last.seq, TEST_BUFS_NUM, "run %d: expected seq %d, got %u",
			      run, TEST_BUFS_NUM, cb_state.last.seq);
		zassert_equal(cb_state.last.type, MPIPE_AI_RESULTS_CLASSIFICATION,
			      "run %d: wrong results type", run);
		zassert_equal(cb_state.last.count, TEST_NUM_CLASSES,
			      "run %d: expected %d ranked classes, got %u", run, TEST_NUM_CLASSES,
			      cb_state.last.count);
		zassert_equal(cb_state.last.cls[0].class_id, expected_class,
			      "run %d: expected winning class %u, got %u", run, expected_class,
			      cb_state.last.cls[0].class_id);
		zassert_true(cb_state.last.cls[0].score > 0.9f,
			     "run %d: winning score not dominant", run);

		zassert_ok(mpipe_element_set_state((struct mpipe_element *)&fixture->pipeline,
						   MPIPE_STATE_READY),
			   "run %d failed to return to READY", run);
	}

	zassert_ok(zbus_chan_rm_obs(bus, &test_ai_pipeline_sub, K_FOREVER),
		   "Failed to remove observer from pipeline channel");
}
