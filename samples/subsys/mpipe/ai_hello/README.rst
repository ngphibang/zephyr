.. zephyr:code-sample:: mpipe-ai-hello
   :name: AI hello pipeline
   :relevant-api: mpipe_ai mpipe_base

   Classify a static image with a neural network pipeline.

Overview
********

The "hello inference" of the Multimedia Pipeline framework. The application
pushes a static 96x96 grayscale image into the pipeline through the app_src
element, the AI plugin converts it into the model input tensor, runs inference
and decodes the class scores, and the app_sink element hands the results back to
the application:

.. graphviz::

   digraph pipeline {
     rankdir=LR;
     node [shape=box, style=filled, fillcolor="#e8e8e8"];
     app_src  [label="Application\nSource"];
     convert [label="AI\nConvert"];
     infer   [label="AI\nInfer"];
     decode  [label="AI\nDecode"];
     app_sink [label="Application\nSink"];
     app_src -> convert -> infer -> decode -> app_sink;
   }

Two backends are supported:

* The fake backend (default): deterministic pseudo-inference, no external
  module needed. Two synthetic patterns exercise the pipeline.
* The TensorFlow Lite Micro backend (``overlay-tflm.conf``): the stock
  person detection model and its two test images from the tflite-micro
  module. Watch the person score flip between them.

Requirements
************

The TFLM configurations need the optional tflite-micro module:

.. code-block:: console

   west config manifest.group-filter -- +optional
   west update tflite-micro

Building and Running
********************

Fake backend on :zephyr:board:`native_sim`:

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/mpipe/ai_hello
   :board: native_sim/native/64
   :goals: build run
   :compact:

Person detection on :zephyr:board:`qemu_x86`:

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/mpipe/ai_hello
   :board: qemu_x86_64
   :gen-args: -DEXTRA_CONF_FILE=overlay-tflm.conf
   :goals: build run
   :compact:

Person detection on the Arm Ethos-U55 NPU, simulated by the Corstone-300
fixed virtual platform (no hardware needed; the FVP binary
``FVP_Corstone_SSE-300_Ethos-U55`` must be in PATH). The build automatically
selects the Vela-compiled model, as only a compiled model exercises the NPU:

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/mpipe/ai_hello
   :board: mps3/corstone300/fvp
   :gen-args: -DEXTRA_CONF_FILE="overlay-tflm.conf;overlay-ethosu.conf"
   :goals: build run
   :compact:

Sample Output
*************

.. code-block:: console

   Pipeline PLAYING, classifying...
   Image 1: person (score 94%)
   Image 2: person (score 94%)
   Image 3: person (score 94%)
   Image 4: no person (score 72%)
   Image 5: no person (score 72%)
   Image 6: no person (score 72%)
   End of stream
