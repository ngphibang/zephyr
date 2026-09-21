/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Inference backend interface of the AI/ML plugin.
 * @ingroup mpipe_ai
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_BACKEND_H_
#define ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_BACKEND_H_

/**
 * @addtogroup mpipe_ai
 * @{
 */

#include <stdint.h>

#include <zephyr/mpipe/ai/mpipe_ai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of dimensions a tensor descriptor carries */
#define MPIPE_AI_MAX_DIMS 4

/**
 * @brief Description of one model input or output tensor
 */
struct mpipe_ai_tensor_desc {
	/** Element data type, an @ref mpipe_ai_tensor_type value */
	uint8_t dtype;
	/** Number of dimensions */
	uint8_t num_dims;
	/** Dimensions as the model declares them, e.g. NHWC for images */
	uint16_t dims[MPIPE_AI_MAX_DIMS];
	/** Payload size of this tensor in bytes */
	uint32_t bytes;
	/** Offset of this tensor in the flat buffer, 4-byte aligned */
	uint32_t offset;
	/** Quantization scale, 0.0f when the tensor is not quantized */
	float scale;
	/** Quantization zero point */
	int32_t zero_point;
};

/**
 * @brief The set of input or output tensors of a loaded model
 *
 * All tensors of one inference travel in a single flat buffer; each
 * descriptor holds its offset. The inference element owns one set per
 * direction and references the output set from the buffer metadata of every
 * buffer it produces, so downstream elements can interpret the payload
 * without the caps system having to describe every tensor.
 */
struct mpipe_ai_tensor_set {
	/** Number of tensors */
	uint8_t count;
	/** Total bytes of the flat buffer, including alignment padding */
	uint32_t total_bytes;
	/** Per-tensor descriptions */
	struct mpipe_ai_tensor_desc desc[CONFIG_MPIPE_AI_MAX_IO_TENSORS];
};

struct mpipe_ai_backend;

/**
 * @brief Operations an inference backend implements
 *
 * The interface is plain C so that the elements never depend on the types of
 * any particular runtime. A backend wraps one runtime: the fake backend for
 * tests, TensorFlow Lite Micro for real models, with vendor NPU support as
 * build-time variants of the latter.
 */
struct mpipe_ai_backend_ops {
	/**
	 * @brief Parse the model and prepare it for inference
	 *
	 * @param be Backend instance
	 * @param model Model bytes
	 * @param model_size Number of model bytes
	 * @param arena Working memory for the runtime
	 * @param arena_size Number of arena bytes
	 * @return 0 on success, -ENOMEM when the arena is too small, -EINVAL
	 *         on a malformed model, -ENOTSUP when the model needs an
	 *         operator the backend does not provide
	 */
	int (*load)(struct mpipe_ai_backend *be, const void *model, uint32_t model_size,
		    void *arena, uint32_t arena_size);
	/**
	 * @brief Describe the loaded model's input and output tensors
	 *
	 * @param be Backend instance
	 * @param in Filled with the input tensor set
	 * @param out Filled with the output tensor set, including flat-buffer
	 *            offsets and quantization parameters
	 * @return 0 on success, negative errno on failure
	 */
	int (*get_io)(struct mpipe_ai_backend *be, struct mpipe_ai_tensor_set *in,
		      struct mpipe_ai_tensor_set *out);
	/**
	 * @brief Run one inference
	 *
	 * Copies the input into the runtime, invokes the model, and copies
	 * every output tensor to its offset in @p out. Blocks the calling
	 * thread for the duration of the inference.
	 *
	 * @param be Backend instance
	 * @param in Input tensor payload
	 * @param in_size Number of input bytes
	 * @param out Flat output buffer
	 * @param out_size Number of output bytes available
	 * @return 0 on success, negative errno on failure
	 */
	int (*invoke)(struct mpipe_ai_backend *be, const uint8_t *in, uint32_t in_size,
		      uint8_t *out, uint32_t out_size);
	/**
	 * @brief Release the loaded model
	 *
	 * @param be Backend instance
	 * @return 0 on success, negative errno on failure
	 */
	int (*unload)(struct mpipe_ai_backend *be);
};

/**
 * @brief An inference backend instance
 */
struct mpipe_ai_backend {
	/** Operations */
	const struct mpipe_ai_backend_ops *ops;
	/** Backend private state */
	void *priv;
};

/**
 * @brief Bind the backend selected at build time
 *
 * Provided by whichever backend implementation is compiled in.
 *
 * @param be Backend instance to bind
 * @return 0 on success, negative errno on failure
 */
int mpipe_ai_backend_default_init(struct mpipe_ai_backend *be);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_BACKEND_H_ */
