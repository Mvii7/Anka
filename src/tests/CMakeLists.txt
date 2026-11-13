# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/fibers.cpp
    common/host_memory.cpp
    common/nx2_rtss_osd.cpp
    common/page_table.cpp
    common/param_package.cpp
    common/range_map.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/undefined_fix.cpp
    common/unique_function.cpp
    core/core_timing.cpp
    core/internal_network/network.cpp
    core/nup_validation.cpp
    core/nup_zstd_read.cpp
    core/nx2_dk_dyn_pace.cpp
    core/nx2_frame_stats.cpp
    core/nx2_dk_dynamic_fps.cpp
    core/nx2_mkw_dynamic_fps.cpp
    core/nx2_ipc_trace.cpp
    core/nx2_osd_speed.cpp
    core/nx2_perf_settings.cpp
    core/nx2_present_rate.cpp
    core/nx2_vi2_reply.cpp
    core/nx2_vi2_events.cpp
    core/nx_archive.cpp
    core/read_traps.cpp
    core/system_test_context.h
    frontend_common/nx_archive.cpp
    video_core/memory_tracker.cpp
    video_core/native_qmd.cpp
    video_core/ampere_returns.cpp
    video_core/nx2_present.cpp
    video_core/ampere_branch_flag.cpp
    video_core/ampere_hsetp2.cpp
    video_core/ampere_hmma.cpp
    video_core/nx2_mode_gov.cpp
    video_core/nx2_mode_pace.cpp
    video_core/nx2_mode_pace21.cpp
    video_core/nx2_front_same.cpp
    video_core/nx2_macro_jit.cpp
    video_core/nx2_reactive.cpp
    video_core/nx2_record_list.cpp
    video_core/nx2_reg_replica.cpp
    video_core/nx2_root.cpp
    video_core/nx2_desc_secondary.cpp
    video_core/frame_gen_pacer.cpp
    video_core/hdr_output.cpp
    common/display_hdr.cpp
    video_core/nx2_fg_timing.cpp
    video_core/nx2_stage_bind.cpp
    video_core/nx2_stage_contract.cpp
    video_core/nx2_stage_ring.cpp
    video_core/nx2_upscale_pick.cpp
    video_core/native_macro_fifo.cpp
    video_core/nx2_write_history.cpp
    input_common/calibration_configuration_job.cpp
    input_common/rumble_mapping.cpp
    yuzu_cmd/nx2_auto_script.cpp
    ../yuzu_cmd/nx2_auto_script.cpp
)

create_target_directory_groups(tests)

target_link_libraries(tests PRIVATE common core frontend_common input_common video_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

target_compile_definitions(tests PRIVATE
    YUZU_NX_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/nx_fixtures")

add_test(NAME tests COMMAND tests)

# needed for vma
if (NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()
