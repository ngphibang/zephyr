/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zephyr/mpipe/mpipe_thread.h>

static struct k_sem thread_ran_sem;
static atomic_t thread_run_count;
static atomic_t thread_exited;

static void simple_thread_func(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	atomic_inc(&thread_run_count);
	k_sem_give((struct k_sem *)p1);
}

/* Runs once per resume, then parks in wait() until the next resume or the join */
static void loop_thread_func(void *p1, void *p2, void *p3)
{
	struct mpipe_thread *thread = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (mpipe_thread_wait(thread) == 0) {
		atomic_inc(&thread_run_count);
		/* Pause before signaling, so the test's next resume finds the thread paused */
		mpipe_thread_pause(thread);
		k_sem_give(&thread_ran_sem);
	}

	atomic_set(&thread_exited, 1);
}

struct mpipe_thread_api_fixture {
	struct mpipe_thread thread;
	struct mpipe_thread other;
};

static void *thread_suite_setup(void)
{
	static struct mpipe_thread_api_fixture fixture;

	return &fixture;
}

static void thread_before(void *f)
{
	struct mpipe_thread_api_fixture *fix = f;

	memset(fix, 0, sizeof(*fix));

	k_sem_init(&thread_ran_sem, 0, 1);
	atomic_set(&thread_run_count, 0);
	atomic_set(&thread_exited, 0);
}

ZTEST_SUITE(mpipe_thread_api, NULL, thread_suite_setup, thread_before, NULL, NULL);

ZTEST_F(mpipe_thread_api, test_create_and_join)
{
	k_tid_t tid =
		mpipe_thread_create(&fixture->thread, simple_thread_func, &thread_ran_sem, NULL,
				    NULL, CONFIG_MPIPE_THREAD_DEFAULT_PRIORITY, K_NO_WAIT);

	zassert_not_null(tid, "mpipe_thread_create returned NULL");
	zassert_true(fixture->thread.stack_id < CONFIG_MPIPE_THREADS_NUM,
		     "stack_id %u out of range [0, %d)", fixture->thread.stack_id,
		     CONFIG_MPIPE_THREADS_NUM);

	zassert_ok(k_sem_take(&thread_ran_sem, K_MSEC(1000)),
		   "entry function not called within 1s");
	zassert_equal(atomic_get(&thread_run_count), 1, "entry function call count != 1");

	zassert_ok(mpipe_thread_join(&fixture->thread, K_SECONDS(1)), "join failed");
}

/* A joined thread gives its stack back; a pool with none left refuses a thread */
ZTEST_F(mpipe_thread_api, test_join_releases_the_stack)
{
	BUILD_ASSERT(CONFIG_MPIPE_THREADS_NUM == 1, "the exhaustion test needs a pool of one");

	zassert_not_null(mpipe_thread_create(&fixture->thread, simple_thread_func, &thread_ran_sem,
					     NULL, NULL, CONFIG_MPIPE_THREAD_DEFAULT_PRIORITY,
					     K_NO_WAIT));
	zassert_is_null(mpipe_thread_create(&fixture->other, simple_thread_func, &thread_ran_sem,
					    NULL, NULL, CONFIG_MPIPE_THREAD_DEFAULT_PRIORITY,
					    K_NO_WAIT),
			"a second thread got a stack from a pool of one");

	zassert_ok(mpipe_thread_join(&fixture->thread, K_SECONDS(1)));

	zassert_not_null(mpipe_thread_create(&fixture->other, simple_thread_func, &thread_ran_sem,
					     NULL, NULL, CONFIG_MPIPE_THREAD_DEFAULT_PRIORITY,
					     K_NO_WAIT),
			 "the joined thread's stack was not released");
	zassert_ok(mpipe_thread_join(&fixture->other, K_SECONDS(1)));
}

/* A thread created sleeping runs on resume, parks on pause, and exits on join */
ZTEST_F(mpipe_thread_api, test_resume_pause_and_join)
{
	zassert_not_null(mpipe_thread_create(&fixture->thread, loop_thread_func, &fixture->thread,
					     NULL, NULL, CONFIG_MPIPE_THREAD_DEFAULT_PRIORITY,
					     K_FOREVER));
	zassert_equal(k_sem_take(&thread_ran_sem, K_MSEC(50)), -EAGAIN,
		      "a thread created sleeping ran before its resume");

	mpipe_thread_resume(&fixture->thread);
	zassert_ok(k_sem_take(&thread_ran_sem, K_SECONDS(1)), "the thread did not run on resume");
	zassert_equal(atomic_get(&thread_run_count), 1);

	mpipe_thread_resume(&fixture->thread);
	zassert_ok(k_sem_take(&thread_ran_sem, K_SECONDS(1)), "the thread did not run again");
	zassert_equal(atomic_get(&thread_run_count), 2);

	zassert_ok(mpipe_thread_join(&fixture->thread, K_SECONDS(1)), "join failed");
	zassert_equal(atomic_get(&thread_exited), 1, "wait() did not report the join");
}
