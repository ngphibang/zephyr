# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ai_detect_pipeline)

file(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

if(CONFIG_MPIPE_AI_BACKEND_TFLM)
  # A detection model is not shipped: supply a quantized model using the
  # TFLite_Detection_PostProcess output convention.
  if(NOT DEFINED AI_MODEL)
    message(FATAL_ERROR
      "ai_detect with the TFLM backend needs a model: build with -DAI_MODEL=<path to .tflite>")
  endif()

  generate_inc_file_for_target(app ${AI_MODEL}
    ${ZEPHYR_BINARY_DIR}/include/generated/ai_model.inc)
endif()
