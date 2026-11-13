.. zephyr:code-sample:: mpipe-multicore-pipelines
   :name: Pipelines on two cores
   :relevant-api: mpipe_pipeline_server mpipe_vid_pipeline_servers mpipe_vid_transform_clients mpipe_vid_sink_clients mpipe_erpc mpipe_base

   Play Motion-JPEG with a pipeline on each core: decode and record on the second core.

Overview
********

This sample runs Multimedia Pipelines on the two cores of a SoC, working as one. The
primary core plays an MJPEG stream: it splits it into JPEG frames, completes the decoded
frames and displays them. The secondary core does two jobs for it, at two places of the
pipeline, each with a pipeline of its own:

- it decodes every frame and draws on it, two elements in a row that the primary core
  sees as one;
- it records the compressed stream on the SD card, which it owns.

Each of the two pipelines of the secondary core is reached through a client element of
the primary core's pipeline, a transform for the first and a sink for the second, over
eRPC and the Zephyr IPC service. A frame crosses between the cores once for each job,
however many elements work on it there, and the primary core is idle meanwhile.

Compare with :zephyr:code-sample:`mpipe-multicore`, where the secondary core runs no
pipeline but a hand-written server around a decoder: what a core that runs neither Zephyr
nor this framework would do.

Pipeline topology
=================

.. graphviz::

   digraph pipelines {
     rankdir=LR;
     node [shape=box, style=filled, fillcolor="#e8e8e8"];

     subgraph cluster_primary {
       label="primary core";
       app_src     [label="app_src\n(base)"];
       jpeg_parser [label="jpeg_parser\n(img)"];
       caps_filter [label="caps_filter\n(base)"];
       tee         [label="tee\n(base)"];
       dec_queue   [label="queue\n(base)"];
       dec_client  [label="vid_transform_client\n(vid, eRPC)", fillcolor="#c8d8f0"];
       overlay     [label="vid_overlay\n(vid)"];
       display     [label="display\n(disp)"];
       rec_queue   [label="queue\n(base)"];
       rec_client  [label="vid_sink_client\n(vid, eRPC)", fillcolor="#c8d8f0"];
       app_src -> jpeg_parser -> caps_filter -> tee;
       tee -> dec_queue -> dec_client -> overlay -> display;
       tee -> rec_queue -> rec_client;
     }

     subgraph cluster_decoding {
       label="secondary core: decoding pipeline";
       dec_src   [label="app_src\n(base)"];
       jpeg_dec  [label="jpeg_decoder\n(img)"];
       dec_draw  [label="vid_overlay\n(vid)"];
       dec_sink  [label="app_sink\n(base)"];
       dec_src -> jpeg_dec -> dec_draw -> dec_sink;
     }

     subgraph cluster_recording {
       label="secondary core: recording pipeline";
       rec_src   [label="app_src\n(base)"];
       rec_caps  [label="caps_filter\n(base)"];
       file_sink [label="file_sink\n(fs)"];
       rec_src -> rec_caps -> file_sink;
     }

     dec_client -> dec_src [style=dashed, constraint=false, label="pipeline server"];
     rec_client -> rec_src [style=dashed, constraint=false, label="pipeline server"];
   }

Primary core
------------

- ``mpipe_app_src`` takes the MJPEG stream from the application, which holds it in its
  image, and pushes it again at every replay.
- ``mpipe_img_jpeg_parser`` splits the stream into JPEG frames and ``mpipe_caps_filter``
  states their size (:kconfig:option:`CONFIG_JPEG_IMAGE_WIDTH`,
  :kconfig:option:`CONFIG_JPEG_IMAGE_HEIGHT`).
- ``mpipe_tee`` gives every frame to both branches, and each ``mpipe_queue`` runs its
  branch on a thread of its own, so that the two wait for the other core independently.
- ``mpipe_vid_transform_client`` stands for the decoding pipeline of the other core: it
  hands each JPEG frame over by address and waits for the decoded one. The formats it
  negotiates with its neighbors are those of that pipeline.
- ``mpipe_vid_overlay`` draws a frame counter, the work of this core on the frames, and
  ``mpipe_disp_sink`` shows them.
- ``mpipe_vid_sink_client`` stands for the recording pipeline of the other core: the
  stream ends there as far as this pipeline is concerned.

Secondary core
--------------

The ``remote`` application builds two ordinary pipelines and serves each to its client
with a pipeline server:

- decoding: ``mpipe_img_jpeg_decoder`` decodes the frame and ``mpipe_vid_overlay`` draws a
  bar that grows with the frames, between an ``mpipe_app_src`` and an ``mpipe_app_sink``.
  The decoder writes into the memory of the primary core: the frame is never copied.
- recording: ``mpipe_file_sink`` writes the frames to
  :kconfig:option:`CONFIG_FILE_OUTPUT_PATH`, behind an ``mpipe_caps_filter`` that tells the
  client what is recorded.

Each server has its IPC endpoint and its thread, which is the streaming thread of the
pipeline it serves, so a slow card write never delays the decoding. The interfaces
between the clients and the servers are defined in
:zephyr_file:`subsys/mpipe/vid/erpc/mpipe_vid_transform.erpc`.

Requirements
************

The sample supports :zephyr:board:`mimxrt1170_evk` with the ``rk055hdmipi4ma0`` display
shield and a FAT-formatted, writable SD card. The M7 is the primary core, the M4 the
secondary one. The M4 owns the SD card: its image holds the file system and the card
stack and fills the 128 KB of code RAM of the core almost entirely.

The build host needs:

- GStreamer (the ``gst-launch-1.0`` tool and the ``good`` plugins which include
  ``videotestsrc`` and ``jpegenc``) to generate the test stream. On Debian/Ubuntu::

    sudo apt install gstreamer1.0-tools gstreamer1.0-plugins-good

- ``erpcgen``, the eRPC interface compiler, on the ``PATH``. It is built from the
  ``erpcgen`` directory of the ``erpc`` module (``modules/lib/erpc``).

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/mpipe/multicore_pipelines
   :board: mimxrt1170_evk/mimxrt1176/cm7
   :west-args: --sysbuild
   :gen-args: -Dmulticore_pipelines_SHIELD=rk055hdmipi4ma0
   :goals: build flash
   :compact:

Sysbuild builds the remote application for the M4 and the primary application loads it at
boot. The M4 console is on ``LPUART2``:

.. code-block:: console

   I: Remote started
   I: SDCard mounted at /SD:
   I: Serving mpipe_vid_transform
   I: Serving mpipe_vid_sink

The display shows the test stream with the counter of the M7 at the top and the bar of
the M4 at the bottom, and the M7 console shows the pipeline reaching end of stream:

.. code-block:: console

   [00:00:00.000,000] <inf> main: Pipeline linked.
   [00:00:xx.xxx,xxx] <inf> mpipe_player: Player #0: end of stream

The recording is then on the card. The player's shell commands on the M7 console drive
both cores: ``player replay`` plays and records the stream again, and ``player pause``
pauses the three pipelines.
