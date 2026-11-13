/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The two pipelines this core runs for the pipeline of the other core, each
 * behind a server its client element talks to:
 *
 *   decoding:   app_src -> jpeg_decoder -> vid_overlay -> app_sink
 *   recording:  app_src -> caps_filter -> file_sink
 */

#include <errno.h>
#if defined(CONFIG_FAT_FILESYSTEM_ELM)
#include <ff.h>
#endif

#include <zephyr/drivers/video.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/base/mpipe_app_sink.h>
#include <zephyr/mpipe/base/mpipe_app_src.h>
#include <zephyr/mpipe/base/mpipe_caps_filter.h>
#include <zephyr/mpipe/fs/mpipe_file_sink.h>
#include <zephyr/mpipe/img/mpipe_img_jpeg_decoder.h>
#include <zephyr/mpipe/utils/mpipe_pipeline_server.h>
#include <zephyr/mpipe/vid/mpipe_vid_draw.h>
#include <zephyr/mpipe/vid/mpipe_vid_overlay.h>
#include <zephyr/mpipe/vid/mpipe_vid_pipeline_server_erpc.h>

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

/* Element IDs (values are arbitrary; only uniqueness within the pipeline matters) */
enum {
	PIPE_ID,
	APP_SRC_ID,
	JPEG_DEC_ID,
	OVERLAY_ID,
	APP_SINK_ID,
	CAPS_FILTER_ID,
	FILE_SINK_ID,
};

#define MNT_POINT "/SD:"

/* The mark of this core on every frame: a bar growing with the frames it decoded */
#define BAR_HEIGHT 6U
#define BAR_STEP   2U

static struct mpipe_pipeline_server rec_server;

static void draw_bar(uint8_t *frame, uint16_t width, uint16_t height, uint32_t pixfmt,
		     const struct net_buf *in, void *user_data)
{
	static uint32_t frames;
	struct mpipe_vid_rect bar = {
		.x0 = 0,
		.y0 = (int32_t)height - (int32_t)BAR_HEIGHT,
		.x1 = (int32_t)((frames * BAR_STEP) % width),
		.y1 = (int32_t)height,
	};

	ARG_UNUSED(in);
	ARG_UNUSED(user_data);

	frames++;
	(void)mpipe_vid_draw_fill(frame, width, height, pixfmt, &bar, 0xFFFF0000U);
}

static int mount_sd(void)
{
#if defined(CONFIG_FAT_FILESYSTEM_ELM)
	static FATFS fat_fs;
#endif
	static struct fs_mount_t mnt = {
		.type = FS_FATFS,
#if defined(CONFIG_FAT_FILESYSTEM_ELM)
		.fs_data = &fat_fs,
#endif
		.mnt_point = MNT_POINT,
	};
	int ret;

	ret = fs_mount(&mnt);
	if (ret != 0) {
		LOG_ERR("fs_mount failed (%d)", ret);
		return ret;
	}

	LOG_INF("SDCard mounted at %s", MNT_POINT);

	return 0;
}

/* app_src -> jpeg_decoder -> vid_overlay -> app_sink, served to the transform client */
static int build_decoding(struct mpipe_pipeline_server *server)
{
	static struct mpipe pipe;
	static struct mpipe_app_src app_src;
	static struct mpipe_img_jpeg_decoder jpeg_dec;
	static struct mpipe_vid_overlay overlay;
	static struct mpipe_app_sink app_sink;
	static const struct mpipe_vid_overlay_cb bar = {.fn = draw_bar};
	int ret;

	ret = mpipe_pipeline_init(&pipe, PIPE_ID);
	if (ret < 0) {
		return ret;
	}
	ret = mpipe_app_src_init(&app_src, APP_SRC_ID);
	if (ret < 0) {
		return ret;
	}
	ret = mpipe_img_jpeg_decoder_init(&jpeg_dec, JPEG_DEC_ID);
	if (ret < 0) {
		return ret;
	}
	ret = mpipe_vid_overlay_init(&overlay, OVERLAY_ID);
	if (ret < 0) {
		return ret;
	}
	ret = mpipe_app_sink_init(&app_sink, APP_SINK_ID);
	if (ret < 0) {
		return ret;
	}

	ret = mpipe_object_set_properties((struct mpipe_object *)&overlay,
					  MPIPE_PROP_VID_OVERLAY_DRAW_CB, &bar,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		return ret;
	}

	ret = mpipe_bin_add((struct mpipe_bin *)&pipe, (struct mpipe_element *)&app_src,
			    (struct mpipe_element *)&jpeg_dec, (struct mpipe_element *)&overlay,
			    (struct mpipe_element *)&app_sink, NULL);
	if (ret < 0) {
		return ret;
	}

	ret = mpipe_element_link(
		(struct mpipe_element *)&app_src, (struct mpipe_element *)&jpeg_dec,
		(struct mpipe_element *)&overlay, (struct mpipe_element *)&app_sink, NULL);
	if (ret < 0) {
		return ret;
	}

	return mpipe_pipeline_server_init(server, &pipe, &app_src, &app_sink);
}

/* app_src -> caps_filter -> file_sink, served to the sink client */
static int build_recording(struct mpipe_pipeline_server *server)
{
	static struct mpipe pipe;
	static struct mpipe_app_src app_src;
	static struct mpipe_caps_filter caps_filter;
	static struct mpipe_file_sink file_sink;
	struct mpipe_structure caps;
	int ret;

	ret = mpipe_pipeline_init(&pipe, PIPE_ID);
	if (ret < 0) {
		return ret;
	}
	ret = mpipe_app_src_init(&app_src, APP_SRC_ID);
	if (ret < 0) {
		return ret;
	}
	ret = mpipe_caps_filter_init(&caps_filter, CAPS_FILTER_ID);
	if (ret < 0) {
		return ret;
	}
	ret = mpipe_file_sink_init(&file_sink, FILE_SINK_ID);
	if (ret < 0) {
		return ret;
	}

	/* A file takes anything: the filter is what tells the client what is recorded */
	ret = mpipe_structure_init_fields(&caps, MPIPE_MEDIA_VIDEO, MPIPE_CAPS_PIXEL_FORMAT,
					  MPIPE_TYPE_UINT, VIDEO_PIX_FMT_JPEG, MPIPE_CAPS_END);
	if (ret != 0) {
		return ret;
	}
	ret = mpipe_object_set_properties((struct mpipe_object *)&caps_filter,
					  MPIPE_PROP_BASE_CAPS_FILTER_CAPS, &caps,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		return ret;
	}

	ret = mpipe_object_set_properties((struct mpipe_object *)&file_sink,
					  MPIPE_PROP_FS_SINK_PATH, CONFIG_FILE_OUTPUT_PATH,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		return ret;
	}

	ret = mpipe_bin_add((struct mpipe_bin *)&pipe, (struct mpipe_element *)&app_src,
			    (struct mpipe_element *)&caps_filter,
			    (struct mpipe_element *)&file_sink, NULL);
	if (ret < 0) {
		return ret;
	}

	ret = mpipe_element_link((struct mpipe_element *)&app_src,
				 (struct mpipe_element *)&caps_filter,
				 (struct mpipe_element *)&file_sink, NULL);
	if (ret < 0) {
		return ret;
	}

	/* The pipeline ends in a sink of its own: no application sink */
	return mpipe_pipeline_server_init(server, &pipe, &app_src, NULL);
}

/* The recording pipeline streams on this thread, the decoding one on main() */
static void record_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	(void)mpipe_vid_sink_server_erpc_run(&rec_server);
}

K_THREAD_STACK_DEFINE(record_stack, CONFIG_RECORD_STACK_SIZE);

int main(void)
{
	static struct mpipe_pipeline_server dec_server;
	static struct k_thread record;
	int ret;

	LOG_INF("Remote started");

	/* Without a card the recording pipeline fails to start, and says so to its client */
	(void)mount_sd();

	ret = build_decoding(&dec_server);
	if (ret != 0) {
		LOG_ERR("Failed to build the decoding pipeline (%d)", ret);
		goto err;
	}

	ret = build_recording(&rec_server);
	if (ret != 0) {
		LOG_ERR("Failed to build the recording pipeline (%d)", ret);
		goto err;
	}

	/* Each server has its endpoint: the two clients never wait for each other */
	(void)k_thread_create(&record, record_stack, K_THREAD_STACK_SIZEOF(record_stack),
			      record_thread, NULL, NULL, NULL, CONFIG_MAIN_THREAD_PRIORITY, 0,
			      K_NO_WAIT);

	(void)mpipe_vid_transform_server_erpc_run(&dec_server);

err:
	LOG_ERR("Aborting sample");
	return 0;
}
