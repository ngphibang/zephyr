/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief eRPC binding of the video pipeline server.
 * @ingroup mpipe_vid_pipeline_servers
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_VID_MPIPE_VID_PIPELINE_SERVER_ERPC_H_
#define ZEPHYR_INCLUDE_MPIPE_VID_MPIPE_VID_PIPELINE_SERVER_ERPC_H_

/**
 * @addtogroup mpipe_vid_pipeline_servers
 * @{
 */

#include <zephyr/mpipe/utils/mpipe_pipeline_server.h>

/**
 * @brief Serve a pipeline to the video transform client of another core
 *
 * Registers the endpoint @kconfig{CONFIG_MPIPE_VID_ERPC_TRANSFORM_ENDPOINT},
 * waits for the client to bind it and answers its calls with @p server until
 * the transport fails. The calling thread becomes the streaming thread of the
 * served pipeline: give it the stack and the priority that work needs.
 *
 * The generated binding holds one server of the interface, so an image
 * serves one pipeline this way.
 *
 * @param server The pipeline server, initialized on a pipeline that has an
 *               application sink.
 *
 * @retval -EBUSY The interface is being served already
 * @retval -EIO The transport could not be brought up, or failed
 */
int mpipe_vid_transform_server_erpc_run(struct mpipe_pipeline_server *server);

/**
 * @brief Serve a pipeline to the video sink client of another core
 *
 * As @ref mpipe_vid_transform_server_erpc_run, on the endpoint
 * @kconfig{CONFIG_MPIPE_VID_ERPC_SINK_ENDPOINT}, for a pipeline ending in a
 * sink of its own.
 *
 * @param server The pipeline server, initialized on a pipeline that has no
 *               application sink.
 *
 * @retval -EBUSY The interface is being served already
 * @retval -EIO The transport could not be brought up, or failed
 */
int mpipe_vid_sink_server_erpc_run(struct mpipe_pipeline_server *server);

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_VID_MPIPE_VID_PIPELINE_SERVER_ERPC_H_ */
