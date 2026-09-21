.. zephyr:code-sample:: mpipe-ai-detect
   :name: Live detection pipeline
   :relevant-api: mpipe_ai mpipe_vid_overlay mpipe_vid mpipe_disp mpipe_base

   Draw detection boxes over the live camera stream.

Overview
********

A tee splits the camera stream: the display branch draws the most recent
detection boxes into every frame through the overlay element, the AI branch
runs the detector. The app_sink callback publishes each result set into a
latest-results store the overlay's draw callback reads. One to two frames of
lag between video and boxes is inherent to the decoupling and accepted:

.. graphviz::

   digraph pipeline {
     rankdir=LR;
     node [shape=box, style=filled, fillcolor="#e8e8e8"];
     camera    [label="Camera\nSource"];
     caps      [label="Caps\nFilter"];
     tee       [label="Tee"];
     queue_d   [label="Queue"];
     overlay   [label="Video\nOverlay"];
     transform [label="Video\nTransform"];
     display   [label="Display\nSink"];
     queue_ai  [label="Queue\n(leaky)"];
     convert   [label="AI\nConvert"];
     infer     [label="AI\nInfer"];
     decode    [label="AI\nDecode"];
     app_sink   [label="Application\nSink"];
     camera -> caps -> tee;
     tee -> queue_d -> overlay -> transform -> display;
     tee -> queue_ai -> convert -> infer -> decode -> app_sink;
     app_sink -> overlay [style=dashed, label="results store"];
   }

With the fake backend (default) a deterministic box orbits the frame: the
whole pipeline, the detection decoding and the overlay run with no model at
all. For a real detector, build with the TFLM overlay and point ``AI_MODEL``
at a quantized detection model using the TFLite_Detection_PostProcess
output convention (boxes, classes, scores, count).

Building and Running
********************

Fake detector on :zephyr:board:`native_sim` (synthetic video, SDL display;
watch the green box sweep across the test pattern):

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/mpipe/ai_detect
   :board: native_sim/native/64
   :snippets: video-sw-generator
   :goals: build run
   :compact:

A real detector on :zephyr:board:`mimxrt1170_evk` with the OV5640 camera and
the RK055HDMIPI4MA0 display:

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/mpipe/ai_detect
   :board: mimxrt1170_evk/mimxrt1176/cm7
   :shield: nxp_btb44_ov5640,rk055hdmipi4ma0
   :gen-args: -DEXTRA_CONF_FILE=overlay-tflm.conf -DAI_MODEL=/path/to/detector_int8.tflite
   :goals: build flash
   :compact:

Sample Output
*************

.. code-block:: console

   [00:00:00.380,000] <inf> mpipe_player: Player #0 state: PLAYING
   [00:00:00.412,000] <inf> main: Frame 1: 2 box(es), best score 90% (class 0)
   [00:00:00.445,000] <inf> main: Frame 2: 2 box(es), best score 90% (class 0)
