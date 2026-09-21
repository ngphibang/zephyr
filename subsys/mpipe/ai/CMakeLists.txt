#
# Copyright 2026 NXP
#
# SPDX-License-Identifier: Apache-2.0
#

if(CONFIG_MPIPE_AI)
  zephyr_library_sources_ifdef(CONFIG_MPIPE_AI_CONVERT mpipe_ai_convert.c)
  zephyr_library_sources_ifdef(CONFIG_MPIPE_AI_INFER mpipe_ai_infer.c)
  zephyr_library_sources_ifdef(CONFIG_MPIPE_AI_DECODE mpipe_ai_decode.c mpipe_ai_results.c)
  zephyr_library_sources_ifdef(CONFIG_MPIPE_AI_BACKEND_FAKE mpipe_ai_backend_fake.c)

  if(CONFIG_MPIPE_AI_BACKEND_TFLM)
    # The TFLM backend is C++; a separate library keeps the C++ compile
    # options away from the C-only "mpipe" library. This block must stay last:
    # zephyr_library_named() retargets subsequent zephyr_library_* calls.
    zephyr_library_named(mpipe_ai_tflm)
    zephyr_library_sources(mpipe_ai_backend_tflm.cpp)
    set(NO_THREADSAFE_STATICS $<TARGET_PROPERTY:compiler-cpp,no_threadsafe_statics>)
    zephyr_library_compile_options($<$<COMPILE_LANGUAGE:CXX>:${NO_THREADSAFE_STATICS}>)
  endif()
endif()
