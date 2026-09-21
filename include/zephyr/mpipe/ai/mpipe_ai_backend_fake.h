/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Fake inference backend.
 * @ingroup mpipe_ai
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_BACKEND_FAKE_H_
#define ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_BACKEND_FAKE_H_

/**
 * @addtogroup mpipe_ai
 * @{
 */

#include <stdint.h>

/**
 * @brief The "model" format of the fake backend
 *
 * The fake backend exists so that the element chain - properties, caps
 * negotiation, buffer flow, decoding - can run and be tested without any
 * inference runtime. Its model blob is this descriptor: it declares the
 * image-shaped input tensor and the number of classes of the classifier the
 * backend pretends to be. The fake inference is deterministic: the class at
 * index (sum of all input bytes modulo the number of classes) receives the
 * maximum score.
 */
struct mpipe_ai_fake_model {
	/** Input tensor width */
	uint16_t in_w;
	/** Input tensor height */
	uint16_t in_h;
	/** Input tensor channels */
	uint8_t channels;
	/** Input tensor data type, an @ref mpipe_ai_tensor_type value */
	uint8_t dtype;
	/** Number of classes of the fake classifier */
	uint16_t num_classes;
	/**
	 * Number of boxes of the fake detector. Zero makes the fake model a
	 * classifier; non-zero makes it a detector emitting the four-tensor
	 * post-processing convention (boxes, classes, scores, count) with
	 * one deterministic box orbiting the frame - enough to exercise a
	 * detection pipeline and an overlay with no real model.
	 */
	uint16_t num_boxes;
};

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_BACKEND_FAKE_H_ */
