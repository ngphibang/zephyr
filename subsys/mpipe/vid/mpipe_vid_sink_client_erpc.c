/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/mpipe/erpc/mpipe_erpc.h>
#include <zephyr/mpipe/vid/mpipe_vid_sink_client.h>
#include <zephyr/mpipe/vid/mpipe_vid_sink_client_erpc.h>

#include "c_mpipe_vid_transform_client.h"
#include "mpipe_vid_transform_common.h"

LOG_MODULE_REGISTER(mpipe_vid_sink_client_erpc, CONFIG_MPIPE_LOG_LEVEL);

static int32_t mpipe_vid_sink_client_erpc_get_buf_caps(uint8_t *min_buffers, uint16_t *buf_align)
{
	return sink_get_buf_caps_rpc(min_buffers, buf_align);
}

static int32_t mpipe_vid_sink_client_erpc_get_format_caps(uint8_t ind, struct video_format_cap *vfc)
{
	mpipe_rpc_video_format_cap rpc;
	int32_t ret;

	ret = sink_get_format_caps_rpc(ind, &rpc);
	if (ret != 0) {
		return ret;
	}

	vfc->pixelformat = rpc.pixelformat;
	vfc->width_min = rpc.width_min;
	vfc->width_max = rpc.width_max;
	vfc->height_min = rpc.height_min;
	vfc->height_max = rpc.height_max;
	vfc->width_step = rpc.width_step;
	vfc->height_step = rpc.height_step;

	return 0;
}

static int32_t mpipe_vid_sink_client_erpc_set_format(struct video_format *fmt)
{
	mpipe_rpc_video_format rpc = {
		.type = (uint8_t)fmt->type,
		.pixelformat = fmt->pixelformat,
		.width = fmt->width,
		.height = fmt->height,
		.pitch = fmt->pitch,
		.size = fmt->size,
	};
	int32_t ret;

	ret = sink_set_format_rpc(&rpc);
	if (ret != 0) {
		return ret;
	}

	/* The server fills in what the format implies, the pitch and size above all */
	fmt->pixelformat = rpc.pixelformat;
	fmt->width = rpc.width;
	fmt->height = rpc.height;
	fmt->pitch = rpc.pitch;
	fmt->size = rpc.size;

	return 0;
}

static int mpipe_vid_sink_client_erpc_process(uintptr_t in_buf, uint32_t in_sz)
{
	/* The interface carries 32-bit addresses */
	if (in_buf > UINT32_MAX) {
		return -EFAULT;
	}

	return sink_process_fn_rpc((uint32_t)in_buf, in_sz);
}

static int mpipe_vid_sink_client_erpc_set_state(enum mpipe_state_change transition)
{
	return sink_set_state_rpc((uint8_t)transition);
}

static int mpipe_vid_sink_client_erpc_eos(void)
{
	return sink_eos_rpc();
}

static int mpipe_vid_sink_client_erpc_init_rpc(void)
{
	/* The generated binding holds one client of the interface */
	static bool bound;
	erpc_client_t client;

	if (bound) {
		return -EBUSY;
	}

	client = mpipe_erpc_client_create(CONFIG_MPIPE_VID_ERPC_SINK_ENDPOINT);
	if (client == NULL) {
		return -EIO;
	}

	initMpipeVidSink_client(client);
	bound = true;

	return 0;
}

int mpipe_vid_sink_client_erpc_init(struct mpipe_vid_sink_client *vsc, uint8_t id)
{
	__ASSERT_NO_MSG(vsc != NULL);

	vsc->sink.init_rpc = mpipe_vid_sink_client_erpc_init_rpc;
	vsc->sink.process_fn_rpc = mpipe_vid_sink_client_erpc_process;
	vsc->sink.set_state_rpc = mpipe_vid_sink_client_erpc_set_state;
	vsc->sink.eos_rpc = mpipe_vid_sink_client_erpc_eos;

	vsc->get_buf_caps_rpc = mpipe_vid_sink_client_erpc_get_buf_caps;
	vsc->get_format_caps_rpc = mpipe_vid_sink_client_erpc_get_format_caps;
	vsc->set_format_rpc = mpipe_vid_sink_client_erpc_set_format;

	return mpipe_vid_sink_client_init(vsc, id);
}
