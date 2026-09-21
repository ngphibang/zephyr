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
endif()
