/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief eRPC backend for the client elements.
 * @ingroup mpipe_erpc
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_ERPC_MPIPE_ERPC_H_
#define ZEPHYR_INCLUDE_MPIPE_ERPC_MPIPE_ERPC_H_

/**
 * @defgroup mpipe_erpc eRPC backend
 * @ingroup mpipe
 * @brief eRPC transport for the client elements.
 *
 * A client element forwards its RPC hooks to a server on another core. This
 * backend brings up eRPC clients over the Zephyr IPC service, one per
 * endpoint, and reports the errors eRPC raises. Each plugin binds each of its
 * eRPC interfaces to a client of its own, so that the calls of one element
 * never wait for those of another.
 *
 * @{
 */

#include <stdint.h>

#include "erpc_client_setup.h"
#include "erpc_common.h"

/**
 * @brief Bring up an eRPC client on an endpoint of its own.
 *
 * Initializes an IPC service transport on the endpoint, a message buffer
 * factory and a client, and installs mpipe_erpc_error_handler() on it. The
 * call returns once the server on the other core has bound the endpoint.
 *
 * More than one client needs the dynamic allocation policy of eRPC
 * (ERPC_ALLOCATION_POLICY_DYNAMIC): the static one holds a single client.
 *
 * @param ept_name Name of the endpoint, the one the server registers.
 *
 * @return The eRPC client, or NULL when the transport could not be
 *         initialized.
 */
erpc_client_t mpipe_erpc_client_create(const char *ept_name);

/**
 * @brief Log an eRPC error.
 *
 * Installed on the clients by mpipe_erpc_client_create().
 *
 * @param err eRPC status to report; kErpcStatus_Success logs nothing.
 * @param function_id Identifier of the RPC function the error occurred in,
 *                    or 0 when the error is not bound to one.
 */
void mpipe_erpc_error_handler(erpc_status_t err, uint32_t function_id);

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_ERPC_MPIPE_ERPC_H_ */
