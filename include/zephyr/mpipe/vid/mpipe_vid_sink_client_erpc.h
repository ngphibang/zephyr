/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Client-side video sink element over eRPC.
 * @ingroup mpipe_vid_sink_clients
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_VID_MPIPE_VID_SINK_CLIENT_ERPC_H_
#define ZEPHYR_INCLUDE_MPIPE_VID_MPIPE_VID_SINK_CLIENT_ERPC_H_

/**
 * @addtogroup mpipe_vid_sink_clients
 * @{
 */

#include <stdint.h>

#include <zephyr/mpipe/vid/mpipe_vid_sink_client.h>

/**
 * @brief Initialize a client-side video sink element bound to eRPC.
 *
 * Installs the eRPC hooks on the element, binds the video sink interface to
 * an eRPC client on the endpoint
 * @kconfig{CONFIG_MPIPE_VID_ERPC_SINK_ENDPOINT}, then initializes the element
 * as mpipe_vid_sink_client_init() does. The call waits for the server on the
 * other core to register the endpoint.
 *
 * The generated binding holds one client of the interface, so an image has
 * one such element.
 *
 * @param vsc Pointer to the element to initialize.
 * @param id Unique element identifier.
 *
 * @retval 0 Success.
 * @retval -EIO The eRPC transport could not be brought up.
 * @retval -EBUSY The interface is bound to another element already.
 * @return Other negative errno from mpipe_vid_sink_client_init().
 */
int mpipe_vid_sink_client_erpc_init(struct mpipe_vid_sink_client *vsc, uint8_t id);

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_VID_MPIPE_VID_SINK_CLIENT_ERPC_H_ */
