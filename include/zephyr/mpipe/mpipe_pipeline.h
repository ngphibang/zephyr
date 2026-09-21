/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Pipeline: the top-level bin that runs a graph.
 * @ingroup mpipe_pipeline
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_MPIPE_PIPELINE_H_
#define ZEPHYR_INCLUDE_MPIPE_MPIPE_PIPELINE_H_

/**
 * @defgroup mpipe_pipeline Pipelines
 * @ingroup mpipe_framework
 * @brief The top-level bin, and what actually runs a graph.
 *
 * A pipeline is the outermost @ref mpipe_bin. Being the outermost is what gives
 * it three jobs no inner bin has.
 *
 * It **owns the thread**. One thread sits at the head of the graph acquiring
 * buffers from the source and pushing each one downstream through the chain
 * functions until a sink consumes it. An element that needs its own thread -
 * to decouple two halves of a graph - gets one by putting a queue between them.
 *
 * It **orders the teardown**. Going down from PAUSED to READY, the pipeline
 * raises a flushing gate on every pad before the children dismantle their pools,
 * so a buffer still in flight is dropped rather than pushed into an element that
 * has already been torn down; and it joins the thread only after the children
 * have drained, because a child still holding the thread in a full queue would
 * otherwise deadlock the join. Going from PLAYING to PAUSED it does neither -
 * a pause is not a teardown, so whatever is queued survives and a resume
 * continues without loss.
 *
 * It **folds the end of the stream**. A graph with several sinks produces one
 * end-of-stream message per sink; the pipeline counts them and passes on only
 * the last, so the application is told once and never tears a graph down while
 * a branch is still running.
 *
 * @{
 */

#include <stdint.h>

#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

#include <zephyr/mpipe/mpipe_bin.h>
#include <zephyr/mpipe/mpipe_clock.h>
#include <zephyr/mpipe/mpipe_message.h>
#include <zephyr/mpipe/mpipe_thread.h>

/**
 * @brief Properties for a pipeline
 *
 * Enumeration of properties that can be configured for a pipeline
 */
enum mpipe_prop_pipeline {
	/** Thread scheduling priority used when the pipeline thread is created.
	 *  Defaults to CONFIG_MPIPE_THREAD_DEFAULT_PRIORITY.
	 */
	MPIPE_PROP_PIPELINE_THREAD_PRIORITY,
};

/**
 * @brief A pipeline: the top-level bin that runs a graph.
 *
 * Adds to @ref mpipe_bin the thread that drives the source, the clock the
 * running time is derived from and the end-of-stream accounting that lets a
 * graph with several sinks report completion exactly once.
 */
struct mpipe {
	/** Base bin container */
	struct mpipe_bin bin;
	/** Thread associated with the pipeline */
	struct mpipe_thread thread;
	/** The pipeline clock; the monotonic system clock unless replaced */
	struct mpipe_clock *clock;
	/** Clock time at which the running time was zero */
	uint64_t base_time;
	/**
	 * The running time frozen while not PLAYING: total time spent in
	 * PLAYING since READY, excluding pauses
	 */
	uint64_t stream_time;
	/** Number of sink elements in the pipeline (computed on READY->PAUSED) */
	uint32_t num_sinks;
	/** Number of EOS messages seen so far during the current run */
	atomic_t eos_count;
};

/**
 * @brief Initialize a pipeline
 *
 * Initializes the pipeline structure, including the base bin and its message channel.
 *
 * @param pipe Pointer to the @ref mpipe to initialize.
 * @param id   Unique element identifier.
 *
 * @return 0 on success, negative errno otherwise.
 */
int mpipe_pipeline_init(struct mpipe *pipe, uint8_t id);

/**
 * @brief Push a buffer downstream starting from a given source pad
 *
 * Walks downstream from an element's @p src_pad, calling each next element's chain_fn
 * until a sink is reached, a chain_fn fails, or the output buffer is NULL.
 *
 * The chain function owns the buffer it is given and releases it whether it
 * succeeds or fails, so the walk does not release it on a chain error. The walk
 * does release the buffer itself in the two cases where no chain function is
 * reached: the source pad has no peer, and the peer pad is flushing.
 *
 * @param src_pad Source pad to start pushing from (its peer's chain_fn is first called)
 * @param buffer Buffer to push (ownership transferred)
 *
 * @return 0 on success, negative errno on failure
 */
int mpipe_push_buffer(struct mpipe_pad *src_pad, struct net_buf *buffer);

/**
 * @brief Replace the pipeline clock
 *
 * Call while the pipeline is in READY; the running time restarts from zero
 * with the new clock. The default is the monotonic system clock.
 *
 * @param pipe The pipeline
 * @param clock The clock to use
 *
 * @retval 0 Success
 * @retval -EINVAL @p pipe or @p clock is NULL
 */
int mpipe_pipeline_set_clock(struct mpipe *pipe, struct mpipe_clock *clock);

/**
 * @brief The pipeline running time
 *
 * The time spent in PLAYING since READY, in microseconds, excluding pauses:
 * it advances while PLAYING and freezes while PAUSED. Buffer timestamps are
 * expressed in it.
 *
 * @param pipe The pipeline
 * @return Running time in microseconds, 0 when @p pipe is NULL
 */
uint64_t mpipe_pipeline_running_time(struct mpipe *pipe);

/**
 * @brief The running time an uptime instant corresponds to
 *
 * Converts a time a driver stamped from the kernel uptime, such as the
 * capture time of a video frame, into the pipeline running time: the
 * running time now, minus the age of the instant on the uptime clock.
 * That holds whatever clock the pipeline runs on.
 *
 * @param pipe The pipeline
 * @param uptime_us Kernel uptime of the instant, in microseconds
 * @return Running time in microseconds, 0 when @p pipe is NULL, when the
 *         instant lies in the future, or when it predates the run
 */
uint64_t mpipe_pipeline_running_time_at(struct mpipe *pipe, uint64_t uptime_us);

/**
 * @brief The pipeline an element is rooted in
 *
 * Walks the container chain to the root of the graph.
 *
 * @param element The element
 * @return The pipeline, or NULL when the element is not in a container yet
 */
struct mpipe *mpipe_pipeline_from_element(struct mpipe_element *element);

/**
 * @brief The running time of the pipeline an element belongs to
 *
 * Convenience for elements stamping or interpreting buffer timestamps.
 *
 * @param element The element
 * @return Running time in microseconds, 0 when the element is not in a pipeline
 */
static inline uint64_t mpipe_element_running_time(struct mpipe_element *element)
{
	return mpipe_pipeline_running_time(mpipe_pipeline_from_element(element));
}

/**
 * @brief The running time an uptime instant corresponds to, for an element
 *
 * @param element The element
 * @param uptime_us Kernel uptime of the instant, in microseconds
 * @return Running time in microseconds, 0 when the element is not in a
 *         pipeline or the instant is outside the run
 */
static inline uint64_t mpipe_element_running_time_at(struct mpipe_element *element,
						     uint64_t uptime_us)
{
	return mpipe_pipeline_running_time_at(mpipe_pipeline_from_element(element), uptime_us);
}

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_MPIPE_PIPELINE_H_ */
