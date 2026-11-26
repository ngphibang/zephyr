/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_MPIPE_VID_VID_CLIENT_H_
#define ZEPHYR_SUBSYS_MPIPE_VID_VID_CLIENT_H_

#include <stdint.h>

#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_pad.h>
#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/mpipe/vid/mpipe_vid_rpc.h>

/*
 * What the video client elements have in common: they differ by their base
 * element only. Each function takes the channel of the element and the format
 * calls that go with it.
 */

/*
 * Initialize a pool of a client element and ask the server, once, for what it
 * needs of the buffers of that direction. Returns 0 or -ENODEV when the server
 * does not answer.
 */
int mpipe_vid_client_pool_init(struct mpipe_rpc_channel *channel,
			       const struct mpipe_vid_rpc_format_api *format,
			       enum mpipe_pad_direction direction, struct mpipe_buffer_pool *pool);

/*
 * The format the server gives at an index for the pad of that direction,
 * narrowed by the filter. Returns 0, -EAGAIN when the filter excludes the
 * format, or -ENOENT past the last format.
 */
int mpipe_vid_client_enum_caps(struct mpipe_rpc_channel *channel,
			       const struct mpipe_vid_rpc_format_api *format,
			       enum mpipe_pad_direction direction, uint32_t index,
			       const struct mpipe_structure *filter, struct mpipe_structure *out);

/*
 * Set a fixed format on the server for the pad of that direction, and give the
 * buffers of the pool the size the server states for it. Returns 0 or a
 * negative errno.
 */
int mpipe_vid_client_set_format(struct mpipe_rpc_channel *channel,
				const struct mpipe_vid_rpc_format_api *format,
				enum mpipe_pad_direction direction,
				const struct mpipe_structure *caps, struct mpipe_buffer_pool *pool);

#endif /* ZEPHYR_SUBSYS_MPIPE_VID_VID_CLIENT_H_ */
