# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ai_hello_pipeline)

file(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

if(CONFIG_MPIPE_AI_BACKEND_TFLM)
  set(TFLM_DIR ${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/tensorflow/lite/micro)

  # The model: stock person detection from the tflite-micro module, or its
  # Vela-compiled variant when the Ethos-U NPU is enabled (only a compiled
  # model exercises the NPU). Override with -DAI_MODEL=<path>.
  if(NOT DEFINED AI_MODEL)
    if(CONFIG_ETHOS_U)
      set(AI_MODEL ${TFLM_DIR}/models/person_detect_vela.tflite)
    else()
      set(AI_MODEL ${TFLM_DIR}/models/person_detect.tflite)
    endif()
  endif()

  generate_inc_file_for_target(app ${AI_MODEL}
    ${ZEPHYR_BINARY_DIR}/include/generated/ai_model.inc)

  # The module's 96x96 grayscale test images, embedded as BMP and unpacked
  # at boot
  generate_inc_file_for_target(app
    ${TFLM_DIR}/examples/person_detection/testdata/person.bmp
    ${ZEPHYR_BINARY_DIR}/include/generated/person_bmp.inc)
  generate_inc_file_for_target(app
    ${TFLM_DIR}/examples/person_detection/testdata/no_person.bmp
    ${ZEPHYR_BINARY_DIR}/include/generated/no_person_bmp.inc)
endif()
