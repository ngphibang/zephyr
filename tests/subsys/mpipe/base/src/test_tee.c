/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/video.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>
#include <zephyr/zbus/zbus.h>

#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/base/mpipe_app_sink.h>
#include <zephyr/mpipe/base/mpipe_app_src.h>
#include <zephyr/mpipe/base/mpipe_tee.h>

ZBUS_MSG_SUBSCRIBER_DEFINE(test_tee_sub);

enum {
	TEE_PIPE_ID,
	TEE_SRC_ID,
	TEE_ID,
	TEE_SINK_A_ID,
	TEE_SINK_B_ID,
};

#define TEST_BUFS_NUM 4
#define TEST_PAYLOAD  16

struct test_tee_fixture {
	struct mpipe pipeline;
	struct mpipe_app_src app_src;
	struct mpipe_tee tee;
	struct mpipe_app_sink sink_a;
	struct mpipe_app_sink sink_b;
	struct zbus_channel *bus;
	atomic_t seen_a;
	atomic_t seen_b;
};

static void count_cb(const struct net_buf *buf, void *user_data)
{
	ARG_UNUSED(buf);

	atomic_inc(user_data);
}

static void *tee_suite_setup(void)
{
	static struct test_tee_fixture fixture;

	return &fixture;
}

static void tee_set_caps(struct mpipe_object *obj, uint32_t key)
{
	struct mpipe_structure caps;

	zassert_ok(mpipe_structure_init_fields(
		&caps, MPIPE_MEDIA_VIDEO, MPIPE_CAPS_PIXEL_FORMAT, MPIPE_TYPE_UINT,
		VIDEO_PIX_FMT_GREY, MPIPE_CAPS_IMAGE_WIDTH, MPIPE_TYPE_UINT, 8,
		MPIPE_CAPS_IMAGE_HEIGHT, MPIPE_TYPE_UINT, 8, MPIPE_CAPS_END));
	zassert_ok(mpipe_object_set_properties(obj, key, &caps, MPIPE_PROP_LIST_END));
}

static void tee_set_cb(struct mpipe_app_sink *sink, atomic_t *counter)
{
	struct mpipe_app_sink_cb cb = {.fn = count_cb, .user_data = counter};

	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)sink,
					       MPIPE_PROP_BASE_APP_SINK_CB, &cb,
					       MPIPE_PROP_LIST_END));
}

static void tee_before(void *f)
{
	struct test_tee_fixture *fix = f;

	memset(fix, 0, sizeof(*fix));

	zassert_ok(mpipe_pipeline_init(&fix->pipeline, TEE_PIPE_ID));
	zassert_ok(mpipe_app_src_init(&fix->app_src, TEE_SRC_ID));
	zassert_ok(mpipe_tee_init(&fix->tee, TEE_ID));
	zassert_ok(mpipe_app_sink_init(&fix->sink_a, TEE_SINK_A_ID));
	zassert_ok(mpipe_app_sink_init(&fix->sink_b, TEE_SINK_B_ID));

	zassert_ok(mpipe_bin_add(
		(struct mpipe_bin *)&fix->pipeline, (struct mpipe_element *)&fix->app_src,
		(struct mpipe_element *)&fix->tee, (struct mpipe_element *)&fix->sink_a,
		(struct mpipe_element *)&fix->sink_b, NULL));

	/* Each link to the tee takes its next unlinked source pad */
	zassert_ok(mpipe_element_link((struct mpipe_element *)&fix->app_src,
				      (struct mpipe_element *)&fix->tee, NULL));
	zassert_ok(mpipe_element_link((struct mpipe_element *)&fix->tee,
				      (struct mpipe_element *)&fix->sink_a, NULL));
	zassert_ok(mpipe_element_link((struct mpipe_element *)&fix->tee,
				      (struct mpipe_element *)&fix->sink_b, NULL));

	tee_set_caps((struct mpipe_object *)&fix->app_src, MPIPE_PROP_BASE_APP_SRC_CAPS);
	tee_set_caps((struct mpipe_object *)&fix->sink_a, MPIPE_PROP_BASE_APP_SINK_CAPS);
	tee_set_caps((struct mpipe_object *)&fix->sink_b, MPIPE_PROP_BASE_APP_SINK_CAPS);
	tee_set_cb(&fix->sink_a, &fix->seen_a);
	tee_set_cb(&fix->sink_b, &fix->seen_b);

	fix->bus = mpipe_element_get_bus_chan((struct mpipe_element *)&fix->pipeline);
	zassert_ok(zbus_chan_add_obs(fix->bus, &test_tee_sub, K_FOREVER));
}

static void tee_after(void *f)
{
	struct test_tee_fixture *fix = f;

	(void)mpipe_element_set_state((struct mpipe_element *)&fix->pipeline, MPIPE_STATE_READY);
	(void)zbus_chan_rm_obs(fix->bus, &test_tee_sub, K_FOREVER);
}

ZTEST_SUITE(test_tee, NULL, tee_suite_setup, tee_before, tee_after, NULL);

/* Every buffer reaches every branch, and the two end-of-streams fold into one message */
ZTEST_F(test_tee, test_every_branch_gets_every_buffer)
{
	struct mpipe_element *pipe = (struct mpipe_element *)&fixture->pipeline;
	const struct zbus_channel *chan;
	struct mpipe_message msg;
	uint8_t payload[TEST_PAYLOAD] = {0};

	for (int run = 0; run < 2; run++) {
		atomic_set(&fixture->seen_a, 0);
		atomic_set(&fixture->seen_b, 0);

		zassert_ok(mpipe_element_set_state(pipe, MPIPE_STATE_PLAYING),
			   "run %d failed to start PLAYING", run);

		for (uint8_t i = 0; i < TEST_BUFS_NUM; i++) {
			zassert_ok(mpipe_app_src_push(&fixture->app_src, payload, sizeof(payload),
						      K_SECONDS(1)),
				   "run %d: push %u failed", run, i);
		}
		zassert_ok(mpipe_app_src_eos(&fixture->app_src, K_SECONDS(1)));

		zassert_ok(zbus_sub_wait_msg(&test_tee_sub, &chan, &msg, K_SECONDS(2)),
			   "run %d timed out waiting for a message", run);
		zassert_equal(msg.type, MPIPE_MESSAGE_EOS, "run %d: expected EOS, got %u", run,
			      msg.type);
		zassert_equal(zbus_sub_wait_msg(&test_tee_sub, &chan, &msg, K_MSEC(50)), -ENOMSG,
			      "run %d: the two sinks' EOS were not folded into one", run);

		zassert_equal(atomic_get(&fixture->seen_a), TEST_BUFS_NUM,
			      "run %d: branch A saw %ld buffers", run,
			      atomic_get(&fixture->seen_a));
		zassert_equal(atomic_get(&fixture->seen_b), TEST_BUFS_NUM,
			      "run %d: branch B saw %ld buffers", run,
			      atomic_get(&fixture->seen_b));

		zassert_ok(mpipe_element_set_state(pipe, MPIPE_STATE_READY),
			   "run %d failed to return to READY", run);
	}
}

static int propose_a(struct mpipe_sink *self, struct mpipe_dispatch *query)
{
	ARG_UNUSED(self);

	query->pool_cfg = (struct mpipe_buffer_pool_config){
		.size = 64, .align = 16, .min_buffers = 3, .max_buffers = 8};

	return 0;
}

static int propose_b(struct mpipe_sink *self, struct mpipe_dispatch *query)
{
	ARG_UNUSED(self);

	query->pool_cfg = (struct mpipe_buffer_pool_config){
		.size = 128, .align = 0, .min_buffers = 2, .max_buffers = 6};

	return 0;
}

/*
 * The tee answers a pool query with the merged demand of its branches: the
 * largest size and alignment, the counts added up, the smallest maximum, and
 * no pool of any branch.
 */
ZTEST(test_tee, test_pool_query_merges_the_branches)
{
	static struct mpipe_tee tee;
	static struct mpipe_sink sink_a;
	static struct mpipe_sink sink_b;
	struct mpipe_structure caps;
	struct mpipe_dispatch query = {.type = MPIPE_DISPATCH_BUFFER_POOL, .caps = &caps};

	zassert_ok(mpipe_tee_init(&tee, TEE_ID));
	zassert_ok(mpipe_sink_init(&sink_a, TEE_SINK_A_ID));
	zassert_ok(mpipe_sink_init(&sink_b, TEE_SINK_B_ID));
	sink_a.propose_buffer_pool = propose_a;
	sink_b.propose_buffer_pool = propose_b;

	zassert_ok(mpipe_element_link(&tee.element, &sink_a.element, NULL));
	zassert_ok(mpipe_element_link(&tee.element, &sink_b.element, NULL));
	zassert_ok(mpipe_structure_init_any(&caps));

	zassert_ok(tee.sink_pad.query_fn(&tee.sink_pad, &query));

	zassert_is_null(query.pool, "a branch's pool travelled up through the tee");
	zassert_equal(query.pool_cfg.size, 128, "size is not the largest");
	zassert_equal(query.pool_cfg.align, 16, "a branch without alignment reset it");
	zassert_equal(query.pool_cfg.min_buffers, 5, "the minimum counts were not added up");
	zassert_equal(query.pool_cfg.max_buffers, 6, "the maximum is not the smallest");
}
