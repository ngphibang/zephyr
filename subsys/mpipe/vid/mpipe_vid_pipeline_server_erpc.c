/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/video.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/mpipe/mpipe_element.h>
#include <zephyr/mpipe/mpipe_pad.h>
#include <zephyr/mpipe/vid/mpipe_vid_pipeline_server.h>
#include <zephyr/mpipe/vid/mpipe_vid_pipeline_server_erpc.h>

#include "erpc_mbf_setup.h"
#include "erpc_server_setup.h"
#include "erpc_transport_setup.h"
#include "c_mpipe_vid_transform_server.h"
#include "mpipe_vid_transform_common.h"

LOG_MODULE_REGISTER(mpipe_vid_pipeline_server_erpc, CONFIG_MPIPE_LOG_LEVEL);

BUILD_ASSERT((int)MPIPE_RPC_PAD_UNKNOWN == (int)MPIPE_PAD_UNKNOWN);
BUILD_ASSERT((int)MPIPE_RPC_PAD_SRC == (int)MPIPE_PAD_SRC);
BUILD_ASSERT((int)MPIPE_RPC_PAD_SINK == (int)MPIPE_PAD_SINK);

/* The generated bindings call plain functions: one server per interface */
static struct mpipe_pipeline_server *transform_server;
static struct mpipe_pipeline_server *sink_server;
/* Size of the client's output buffers, as the output format settled it */
static uint32_t transform_out_size;

static void vfc_to_rpc(const struct video_format_cap *vfc, mpipe_rpc_video_format_cap *rpc)
{
	rpc->pixelformat = vfc->pixelformat;
	rpc->width_min = vfc->width_min;
	rpc->width_max = vfc->width_max;
	rpc->height_min = vfc->height_min;
	rpc->height_max = vfc->height_max;
	rpc->width_step = vfc->width_step;
	rpc->height_step = vfc->height_step;
}

static void rpc_to_vfc(const mpipe_rpc_video_format_cap *rpc, struct video_format_cap *vfc)
{
	vfc->pixelformat = rpc->pixelformat;
	vfc->width_min = rpc->width_min;
	vfc->width_max = rpc->width_max;
	vfc->height_min = rpc->height_min;
	vfc->height_max = rpc->height_max;
	vfc->width_step = rpc->width_step;
	vfc->height_step = rpc->height_step;
}

static int32_t set_format(struct mpipe_pipeline_server *server, mpipe_rpc_video_format *rpc)
{
	struct video_format fmt = {
		.type = (enum video_buf_type)rpc->type,
		.pixelformat = rpc->pixelformat,
		.width = rpc->width,
		.height = rpc->height,
	};
	int ret;

	ret = mpipe_vid_pipeline_server_set_format(server, &fmt);
	if (ret != 0) {
		return ret;
	}

	rpc->pitch = fmt.pitch;
	rpc->size = fmt.size;

	return 0;
}

/* Video transform interface */

int32_t get_buf_caps_rpc(mpipe_rpc_pad_direction direction, uint8_t *min_buffers,
			 uint16_t *buf_align)
{
	return mpipe_vid_pipeline_server_get_buf_caps(
		transform_server, (enum mpipe_pad_direction)direction, min_buffers, buf_align);
}

int32_t get_format_caps_rpc(mpipe_rpc_pad_direction direction, uint8_t ind,
			    mpipe_rpc_video_format_cap *vfc)
{
	struct video_format_cap cap;
	int ret;

	ret = mpipe_vid_pipeline_server_get_format_caps(
		transform_server, (enum mpipe_pad_direction)direction, ind, &cap);
	if (ret != 0) {
		return ret;
	}

	vfc_to_rpc(&cap, vfc);

	return 0;
}

int32_t set_format_rpc(mpipe_rpc_video_format *fmt)
{
	int32_t ret = set_format(transform_server, fmt);

	if (ret == 0 && fmt->type == (uint8_t)VIDEO_BUF_TYPE_OUTPUT) {
		transform_out_size = fmt->size;
	}

	return ret;
}

int32_t transform_cap_rpc(mpipe_rpc_pad_direction direction, uint16_t ind,
			  const mpipe_rpc_video_format_cap *vfc,
			  mpipe_rpc_video_format_cap *other_vfc)
{
	struct video_format_cap cap;
	struct video_format_cap other;
	int ret;

	rpc_to_vfc(vfc, &cap);

	ret = mpipe_vid_pipeline_server_transform_cap(
		transform_server, (enum mpipe_pad_direction)direction, ind, &cap, &other);
	if (ret != 0) {
		return ret;
	}

	vfc_to_rpc(&other, other_vfc);

	return 0;
}

int32_t process_fn_rpc(uint32_t in_buf, uint32_t in_sz, uint32_t out_buf, uint32_t *out_sz)
{
	return mpipe_pipeline_server_process(transform_server, (void *)(uintptr_t)in_buf, in_sz,
					     (void *)(uintptr_t)out_buf, transform_out_size,
					     out_sz);
}

int32_t set_state_rpc(uint8_t transition)
{
	return mpipe_pipeline_server_set_state(transform_server,
					       MPIPE_STATE_TRANSITION_NEXT(transition));
}

int32_t eos_rpc(void)
{
	return mpipe_pipeline_server_eos(transform_server);
}

/* Video sink interface */

int32_t sink_get_buf_caps_rpc(uint8_t *min_buffers, uint16_t *buf_align)
{
	return mpipe_vid_pipeline_server_get_buf_caps(sink_server, MPIPE_PAD_SINK, min_buffers,
						      buf_align);
}

int32_t sink_get_format_caps_rpc(uint8_t ind, mpipe_rpc_video_format_cap *vfc)
{
	struct video_format_cap cap;
	int ret;

	ret = mpipe_vid_pipeline_server_get_format_caps(sink_server, MPIPE_PAD_SINK, ind, &cap);
	if (ret != 0) {
		return ret;
	}

	vfc_to_rpc(&cap, vfc);

	return 0;
}

int32_t sink_set_format_rpc(mpipe_rpc_video_format *fmt)
{
	return set_format(sink_server, fmt);
}

int32_t sink_process_fn_rpc(uint32_t in_buf, uint32_t in_sz)
{
	return mpipe_pipeline_server_process(sink_server, (void *)(uintptr_t)in_buf, in_sz, NULL, 0,
					     NULL);
}

int32_t sink_set_state_rpc(uint8_t transition)
{
	return mpipe_pipeline_server_set_state(sink_server,
					       MPIPE_STATE_TRANSITION_NEXT(transition));
}

int32_t sink_eos_rpc(void)
{
	return mpipe_pipeline_server_eos(sink_server);
}

/* Register the endpoint, wait for the client and answer its calls */
static int serve(const char *ept_name, erpc_service_t (*create_service)(void))
{
	static K_MUTEX_DEFINE(open_lock);
	erpc_transport_t transport;
	erpc_service_t service;
	erpc_server_t server;
	erpc_status_t status;
	erpc_mbf_t mbf;

	/*
	 * The servers start from threads of their own and share the instance the
	 * transport opens: have it open before either registers its endpoint, as
	 * an instance being opened by another thread refuses the registration.
	 */
	(void)k_mutex_lock(&open_lock, K_FOREVER);
	(void)ipc_service_open_instance(DEVICE_DT_GET(DT_ALIAS(erpc_ipc)));
	(void)k_mutex_unlock(&open_lock);

	transport = erpc_transport_zephyr_ipc_remote_init(ept_name, 0);
	if (transport == NULL) {
		LOG_ERR("Failed to initialize the eRPC transport on %s", ept_name);
		return -EIO;
	}

	mbf = erpc_mbf_ipc_service_init(transport);
	server = erpc_server_init(transport, mbf);
	service = create_service();
	if (mbf == NULL || server == NULL || service == NULL) {
		LOG_ERR("Failed to initialize the eRPC server on %s", ept_name);
		return -EIO;
	}

	erpc_add_service_to_server(server, service);

	LOG_INF("Serving %s", ept_name);

	do {
		status = erpc_server_poll(server);
	} while (status == kErpcStatus_Success);

	LOG_ERR("eRPC error %d on %s, no longer serving", (int)status, ept_name);

	return -EIO;
}

int mpipe_vid_transform_server_erpc_run(struct mpipe_pipeline_server *server)
{
	__ASSERT_NO_MSG(server != NULL);

	if (transform_server != NULL) {
		return -EBUSY;
	}

	transform_server = server;

	return serve(CONFIG_MPIPE_VID_ERPC_TRANSFORM_ENDPOINT, create_MpipeVidTransform_service);
}

int mpipe_vid_sink_server_erpc_run(struct mpipe_pipeline_server *server)
{
	__ASSERT_NO_MSG(server != NULL);

	if (sink_server != NULL) {
		return -EBUSY;
	}

	sink_server = server;

	return serve(CONFIG_MPIPE_VID_ERPC_SINK_ENDPOINT, create_MpipeVidSink_service);
}
