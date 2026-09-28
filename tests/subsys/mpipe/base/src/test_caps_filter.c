/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/video.h>
#include <zephyr/ztest.h>

#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/base/mpipe_app_sink.h>
#include <zephyr/mpipe/base/mpipe_app_src.h>
#include <zephyr/mpipe/base/mpipe_caps_filter.h>

enum {
	FILTER_PIPE_ID,
	FILTER_SRC_ID,
	FILTER_ID,
	FILTER_SINK_ID,
};

struct test_caps_filter_fixture {
	struct mpipe pipeline;
	struct mpipe_app_src app_src;
	struct mpipe_caps_filter filter;
	struct mpipe_app_sink app_sink;
};

static void *caps_filter_suite_setup(void)
{
	static struct test_caps_filter_fixture fixture;

	return &fixture;
}

static void caps_filter_before(void *f)
{
	struct test_caps_filter_fixture *fix = f;
	struct mpipe_structure caps;

	memset(fix, 0, sizeof(*fix));

	zassert_ok(mpipe_pipeline_init(&fix->pipeline, FILTER_PIPE_ID));
	zassert_ok(mpipe_app_src_init(&fix->app_src, FILTER_SRC_ID));
	zassert_ok(mpipe_caps_filter_init(&fix->filter, FILTER_ID));
	zassert_ok(mpipe_app_sink_init(&fix->app_sink, FILTER_SINK_ID));

	zassert_ok(mpipe_bin_add((struct mpipe_bin *)&fix->pipeline,
				 (struct mpipe_element *)&fix->app_src,
				 (struct mpipe_element *)&fix->filter,
				 (struct mpipe_element *)&fix->app_sink, NULL));
	zassert_ok(mpipe_element_link((struct mpipe_element *)&fix->app_src,
				      (struct mpipe_element *)&fix->filter,
				      (struct mpipe_element *)&fix->app_sink, NULL));

	/* The source offers a span of widths; the filter pins one of them */
	zassert_ok(mpipe_structure_init_fields(
		&caps, MPIPE_MEDIA_VIDEO, MPIPE_CAPS_PIXEL_FORMAT, MPIPE_TYPE_UINT,
		VIDEO_PIX_FMT_GREY, MPIPE_CAPS_IMAGE_WIDTH, MPIPE_TYPE_UINT_RANGE, 8, 64, 8,
		MPIPE_CAPS_IMAGE_HEIGHT, MPIPE_TYPE_UINT, 8, MPIPE_CAPS_END));
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&fix->app_src,
					       MPIPE_PROP_BASE_APP_SRC_CAPS, &caps,
					       MPIPE_PROP_LIST_END));

	zassert_ok(mpipe_structure_init_fields(&caps, MPIPE_MEDIA_VIDEO, MPIPE_CAPS_IMAGE_WIDTH,
					       MPIPE_TYPE_UINT, 16, MPIPE_CAPS_END));
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&fix->filter,
					       MPIPE_PROP_BASE_CAPS_FILTER_CAPS, &caps,
					       MPIPE_PROP_LIST_END));
}

static void caps_filter_after(void *f)
{
	struct test_caps_filter_fixture *fix = f;

	(void)mpipe_element_set_state((struct mpipe_element *)&fix->pipeline, MPIPE_STATE_READY);
}

ZTEST_SUITE(test_caps_filter, NULL, caps_filter_suite_setup, caps_filter_before, caps_filter_after,
	    NULL);

/*
 * The filter pins the width, then steps out of the graph so its neighbors link
 * to each other, and steps back in on teardown so the next run negotiates
 * through it again.
 */
ZTEST_F(test_caps_filter, test_filter_pins_the_format_and_steps_aside)
{
	struct mpipe_element *pipe = (struct mpipe_element *)&fixture->pipeline;
	struct mpipe_pad *src_pad = &fixture->app_src.src.src_pad;
	struct mpipe_pad *sink_pad = &fixture->app_sink.sink.sink_pad;
	struct mpipe_transform *transform = &fixture->filter.transform;
	const struct mpipe_value *width;

	for (int run = 0; run < 2; run++) {
		zassert_ok(mpipe_element_set_state(pipe, MPIPE_STATE_PAUSED),
			   "run %d failed to reach PAUSED", run);

		width = mpipe_structure_get_value(&sink_pad->caps, MPIPE_CAPS_IMAGE_WIDTH);
		zassert_not_null(width, "run %d: no width negotiated on the sink", run);
		zassert_equal(mpipe_value_get_uint(width), 16, "run %d: the filter did not pin 16",
			      run);

		zassert_equal_ptr(src_pad->peer, sink_pad,
				  "run %d: the filter did not relink its neighbors", run);
		zassert_is_null(transform->sink_pad.peer, "run %d: the filter is still linked",
				run);
		zassert_is_null(transform->src_pad.peer, "run %d: the filter is still linked", run);

		zassert_ok(mpipe_element_set_state(pipe, MPIPE_STATE_READY),
			   "run %d failed to return to READY", run);

		zassert_equal_ptr(src_pad->peer, &transform->sink_pad,
				  "run %d: the filter is not back upstream", run);
		zassert_equal_ptr(transform->src_pad.peer, sink_pad,
				  "run %d: the filter is not back downstream", run);
	}
}

/* A filter the source cannot satisfy fails the negotiation instead of passing anything */
ZTEST_F(test_caps_filter, test_filter_refuses_what_the_source_cannot_offer)
{
	struct mpipe_element *pipe = (struct mpipe_element *)&fixture->pipeline;
	struct mpipe_structure caps;

	zassert_ok(mpipe_structure_init_fields(&caps, MPIPE_MEDIA_VIDEO, MPIPE_CAPS_IMAGE_WIDTH,
					       MPIPE_TYPE_UINT, 12, MPIPE_CAPS_END));
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&fixture->filter,
					       MPIPE_PROP_BASE_CAPS_FILTER_CAPS, &caps,
					       MPIPE_PROP_LIST_END));

	zassert_not_equal(mpipe_element_set_state(pipe, MPIPE_STATE_PAUSED), 0,
			  "a width off the source's grid was negotiated");
}
