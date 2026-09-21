/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Common definitions for the AI/ML plugin.
 * @ingroup mpipe_ai
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_H_
#define ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_H_

#include <stdint.h>

/**
 * @defgroup mpipe_ai AI/ML elements
 * @ingroup mpipe_plugins
 * @brief Neural network inference as pipeline elements.
 *
 * The AI/ML plugin makes a neural network a pipeline citizen: a converter
 * element turns media buffers into tensors, an inference element runs a model
 * on them through a pluggable backend, and a decode element turns the output
 * tensors into results an application can consume.
 *
 * @{
 */

/**
 * @brief Element data type of a tensor.
 *
 * Carried in the MPIPE_CAPS_TENSOR_TYPE caps field. Signedness is part of the
 * type on purpose: quantized models interpret the same 8 bits differently
 * depending on it.
 */
enum mpipe_ai_tensor_type {
	/** Unset */
	MPIPE_AI_TENSOR_TYPE_UNKNOWN = 0,
	/** Signed 8-bit integer */
	MPIPE_AI_TENSOR_TYPE_INT8,
	/** Unsigned 8-bit integer */
	MPIPE_AI_TENSOR_TYPE_UINT8,
	/** Signed 16-bit integer */
	MPIPE_AI_TENSOR_TYPE_INT16,
	/** 32-bit floating point */
	MPIPE_AI_TENSOR_TYPE_FLOAT32,
};

/**
 * @brief Size in bytes of one element of a tensor type
 *
 * @param type An @ref mpipe_ai_tensor_type value
 * @return Element size in bytes, 0 for an unknown type
 */
static inline uint32_t mpipe_ai_tensor_type_size(uint32_t type)
{
	switch (type) {
	case MPIPE_AI_TENSOR_TYPE_INT8:
	case MPIPE_AI_TENSOR_TYPE_UINT8:
		return 1;
	case MPIPE_AI_TENSOR_TYPE_INT16:
		return 2;
	case MPIPE_AI_TENSOR_TYPE_FLOAT32:
		return 4;
	default:
		return 0;
	}
}

/**
 * @brief A model blob, typically a const array in flash
 */
struct mpipe_ai_model {
	/** Model bytes */
	const uint8_t *data;
	/** Number of bytes */
	uint32_t size;
};

/**
 * @brief Working memory handed to the inference backend
 *
 * The arena is owned by the application because its size is a property of the
 * model and its placement is a property of the platform: a neural processing
 * unit that DMAs the arena may require non-cacheable, aligned memory.
 */
struct mpipe_ai_arena {
	/** Arena bytes */
	uint8_t *data;
	/** Number of bytes */
	uint32_t size;
};

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_H_ */
