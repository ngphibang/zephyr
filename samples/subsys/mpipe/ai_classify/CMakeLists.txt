# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ai_classify_pipeline)

file(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

if(CONFIG_MPIPE_AI_BACKEND_TFLM)
  # The model: stock person detection from the tflite-micro module.
  # Override with -DAI_MODEL=<path>.
  if(NOT DEFINED AI_MODEL)
    set(AI_MODEL
      ${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/tensorflow/lite/micro/models/person_detect.tflite)
  endif()

  generate_inc_file_for_target(app ${AI_MODEL}
    ${ZEPHYR_BINARY_DIR}/include/generated/ai_model.inc)
endif()
