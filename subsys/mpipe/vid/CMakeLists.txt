#
# Copyright 2025 NXP
#
# SPDX-License-Identifier: Apache-2.0
#

if(CONFIG_MPIPE_VID)
  zephyr_library_sources(
    mpipe_vid_buffer_pool.c
    mpipe_vid_object.c
    mpipe_vid_src.c
    mpipe_vid_transform.c
    )

  zephyr_library_sources_ifdef(CONFIG_MPIPE_VID_CONVERT
    mpipe_vid_convert.c
    mpipe_vid_convert_table.c
  )

  zephyr_library_sources_ifdef(CONFIG_MPIPE_VID_OVERLAY
    mpipe_vid_draw.c
    mpipe_vid_overlay.c
  )

  zephyr_library_sources_ifdef(CONFIG_MPIPE_RPC
    mpipe_vid_buffer_pool_client.c
    mpipe_vid_sink_client.c
    mpipe_vid_transform_client.c
  )

  zephyr_library_sources_ifdef(CONFIG_MPIPE_VID_PIPELINE_SERVER
    mpipe_vid_pipeline_server.c
  )

  if(CONFIG_MPIPE_ERPC OR CONFIG_MPIPE_VID_PIPELINE_SERVER_ERPC)
    # The video interfaces, compiled by the erpcgen host tool: the client side
    # for the client elements, the server side for the pipeline server. A
    # server that is not built with this library generates its side from the
    # same file.
    find_program(ERPCGEN erpcgen REQUIRED)
    set(mpipe_vid_erpc_idl ${CMAKE_CURRENT_LIST_DIR}/erpc/mpipe_vid_transform.erpc)
    set(mpipe_vid_erpc_dir ${ZEPHYR_BINARY_DIR}/generated/mpipe/erpc)
    set(mpipe_vid_erpc_client_sources
      ${mpipe_vid_erpc_dir}/c_mpipe_vid_transform_client.cpp
      ${mpipe_vid_erpc_dir}/mpipe_vid_transform_client.cpp
    )
    set(mpipe_vid_erpc_server_sources
      ${mpipe_vid_erpc_dir}/c_mpipe_vid_transform_server.cpp
      ${mpipe_vid_erpc_dir}/mpipe_vid_transform_server.cpp
    )
    set(mpipe_vid_erpc_common_sources
      ${mpipe_vid_erpc_dir}/mpipe_vid_transform_interface.cpp
    )
    add_custom_command(
      OUTPUT
        ${mpipe_vid_erpc_client_sources}
        ${mpipe_vid_erpc_server_sources}
        ${mpipe_vid_erpc_common_sources}
        ${mpipe_vid_erpc_dir}/c_mpipe_vid_transform_client.h
        ${mpipe_vid_erpc_dir}/c_mpipe_vid_transform_server.h
        ${mpipe_vid_erpc_dir}/mpipe_vid_transform_common.h
      COMMAND ${CMAKE_COMMAND} -E make_directory ${mpipe_vid_erpc_dir}
      COMMAND ${ERPCGEN} -g c -o ${mpipe_vid_erpc_dir} ${mpipe_vid_erpc_idl}
      DEPENDS ${mpipe_vid_erpc_idl}
      COMMENT "Generating the eRPC code of the video interfaces"
    )
    add_custom_target(mpipe_vid_erpc_gen
      DEPENDS
        ${mpipe_vid_erpc_client_sources}
        ${mpipe_vid_erpc_server_sources}
        ${mpipe_vid_erpc_common_sources}
    )
    add_dependencies(${ZEPHYR_CURRENT_LIBRARY} mpipe_vid_erpc_gen)
    zephyr_library_include_directories(${mpipe_vid_erpc_dir})
    zephyr_library_sources(${mpipe_vid_erpc_common_sources})
    zephyr_library_sources_ifdef(CONFIG_MPIPE_ERPC
      mpipe_vid_sink_client_erpc.c
      mpipe_vid_transform_client_erpc.c
      ${mpipe_vid_erpc_client_sources}
    )
    zephyr_library_sources_ifdef(CONFIG_MPIPE_VID_PIPELINE_SERVER_ERPC
      mpipe_vid_pipeline_server_erpc.c
      ${mpipe_vid_erpc_server_sources}
    )
  endif()
endif()
