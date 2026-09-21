/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Inference results of the AI/ML plugin.
 * @ingroup mpipe_ai
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_RESULTS_H_
#define ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_RESULTS_H_

/**
 * @addtogroup mpipe_ai
 * @{
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/spinlock.h>

/**
 * @brief Kind of results a decode element produces
 */
enum mpipe_ai_results_type {
	/** Class scores, e.g. person / no person */
	MPIPE_AI_RESULTS_CLASSIFICATION = 1,
	/** Bounding boxes with class and score */
	MPIPE_AI_RESULTS_DETECTION,
};

/**
 * @brief One classification result
 */
struct mpipe_ai_classification {
	/** Class index in the model's label order */
	uint16_t class_id;
	/** Dequantized score */
	float score;
};

/**
 * @brief One detected bounding box
 *
 * Coordinates are normalized to [0, 1] in model space: the decode element
 * knows nothing about any display geometry, and different branches of a
 * pipeline may run different resolutions. The consumer scales at use.
 */
struct mpipe_ai_box {
	/** Left edge */
	float xmin;
	/** Top edge */
	float ymin;
	/** Right edge */
	float xmax;
	/** Bottom edge */
	float ymax;
	/** Detection score */
	float score;
	/** Class index in the model's label order */
	uint16_t class_id;
};

/**
 * @brief Results of one inference
 */
struct mpipe_ai_results {
	/** Incremented once per inference */
	uint32_t seq;
	/** Presentation timestamp of the source buffer the inference ran on */
	uint64_t pts;
	/** One of @ref mpipe_ai_results_type */
	uint8_t type;
	/** Number of valid entries */
	uint8_t count;
	/** The results */
	union {
		/** Classification entries, best first */
		struct mpipe_ai_classification cls[CONFIG_MPIPE_AI_RESULTS_MAX];
		/** Detection entries */
		struct mpipe_ai_box boxes[CONFIG_MPIPE_AI_RESULTS_MAX];
	};
};

/**
 * @brief Latest-results store
 *
 * A helper decoupling a results producer from consumers on other threads:
 * the producer overwrites, a consumer reads the most recent snapshot.
 * Typical wiring: an app_sink callback on the AI branch puts each results
 * buffer here, and an overlay element in the display branch reads it,
 * accepting the one to two frames of lag this implies.
 */
struct mpipe_ai_results_store {
	/** Protects @ref res */
	struct k_spinlock lock;
	/** The most recent results */
	struct mpipe_ai_results res;
	/** True once the first results were put */
	bool valid;
};

/**
 * @brief Initialize a results store
 *
 * @param store Store to initialize
 */
void mpipe_ai_results_store_init(struct mpipe_ai_results_store *store);

/**
 * @brief Publish results, overwriting the previous snapshot
 *
 * @param store The store
 * @param res Results to publish
 */
void mpipe_ai_results_store_put(struct mpipe_ai_results_store *store,
				const struct mpipe_ai_results *res);

/**
 * @brief Read the most recent results
 *
 * @param store The store
 * @param out Filled with a snapshot of the most recent results
 * @return 0 on success, -ENODATA before the first put
 */
int mpipe_ai_results_store_get(struct mpipe_ai_results_store *store, struct mpipe_ai_results *out);

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_RESULTS_H_ */
