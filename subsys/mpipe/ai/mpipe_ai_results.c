/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/mpipe/ai/mpipe_ai_results.h>

void mpipe_ai_results_store_init(struct mpipe_ai_results_store *store)
{
	memset(store, 0, sizeof(*store));
}

void mpipe_ai_results_store_put(struct mpipe_ai_results_store *store,
				const struct mpipe_ai_results *res)
{
	k_spinlock_key_t key = k_spin_lock(&store->lock);

	store->res = *res;
	store->valid = true;

	k_spin_unlock(&store->lock, key);
}

int mpipe_ai_results_store_get(struct mpipe_ai_results_store *store, struct mpipe_ai_results *out)
{
	k_spinlock_key_t key = k_spin_lock(&store->lock);

	if (!store->valid) {
		k_spin_unlock(&store->lock, key);
		return -ENODATA;
	}

	*out = store->res;

	k_spin_unlock(&store->lock, key);

	return 0;
}
