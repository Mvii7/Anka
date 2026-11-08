// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/nx2_reg_replica.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_front_diag_hooks.h"
#include "video_core/nx2_front_same.h"
#include "video_core/nx2_indirect_diag.h"
#include "video_core/nx2_mode_diag.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_stages.h"
#include "video_core/rasterizer_interface.h"
#include "shader_recompiler/ir_opt/nx2_root.h"
#include "video_core/textures/texture.h"
#ifdef ARCHITECTURE_x86_64
#include "video_core/macro_turing_jit.h"
#endif

namespace Tegra::Engines {

/// First register id that is actually a Macro call.
constexpr u32 MacroRegistersStart = 0xE00;

#if defined(_MSC_VER) && !defined(__clang__)
#define MAXWELL3D_FORCE_INLINE __forceinline
#define MAXWELL3D_NOINLINE __declspec(noinline)
#define MAXWELL3D_INLINE_CALLS [[msvc::forceinline_calls]]
#else
#define MAXWELL3D_FORCE_INLINE inline __attribute__((always_inline))
#define MAXWELL3D_NOINLINE __attribute__((noinline))
#define MAXWELL3D_INLINE_CALLS
#endif

namespace {

// EDEN_NX2_LIGHT_DRAW=0: every draw is set up in full (vk_rasterizer.cpp). Its changes are still
// tracked while profiling (EDEN_NX2_GPU_PROFILE), for the "draw changes" counts.
bool LightTrackingEnabled() {
    // Read at the first call, as a function-local static const is, without that static's
    // thread-safe initialization check on every call (MSVC: a thread-local read; this runs
    // for each register write).
    static std::atomic<s8> enabled{-1};
    s8 value = enabled.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        const char* env = std::getenv("EDEN_NX2_LIGHT_DRAW");
        value = static_cast<s8>(env == nullptr || env[0] != '0' ||
                                VideoCommon::Nx2Profile::Enabled());
        enabled.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

// The Maxwell3D::LightChange class of each register: what a change of it means for the next
// draw. Constant buffer data (ProcessCBMultiData) and inline data (CallMultiMethod) are noted
// where they are written.
const std::array<u16, Maxwell3D::Regs::NUM_REGS>& LightClasses() {
    static const auto table = [] {
        using LightChange = Maxwell3D::LightChange;
        std::array<u16, Maxwell3D::Regs::NUM_REGS> classes;
        classes.fill(LightChange::State);
        const auto fill = [&classes](size_t begin, size_t count, u32 kind) {
            std::fill_n(classes.begin() + begin, count, static_cast<u16>(kind));
        };
        for (size_t i = 0; i < Maxwell3D::Regs::NumVertexArrays; ++i) {
            // Address high and low; the stride/enable word and the frequency stay State.
            fill(MAXWELL3D_REG_INDEX(vertex_streams) + i * 4 + 1, 2, LightChange::VertexStreams);
            fill(MAXWELL3D_REG_INDEX(vertex_stream_limits) + i * 2, 2, LightChange::VertexStreams);
            // The native class's stream sizes (Regs::NativeVertexStreamSize, padding in the
            // legacy layout): what a stream move writes besides the address. Read only for the
            // vertex buffer ranges and vertex count estimates, as the limits.
            fill(0x600 / sizeof(u32) + i * 2, 2, LightChange::VertexStreams);
        }
        // Start and limit; the format stays State.
        fill(MAXWELL3D_REG_INDEX(index_buffer), 4, LightChange::IndexBuffer);
        for (const size_t method :
             {MAXWELL3D_REG_INDEX(index_buffer.first), MAXWELL3D_REG_INDEX(index_buffer.count),
              MAXWELL3D_REG_INDEX(vertex_buffer.first), MAXWELL3D_REG_INDEX(vertex_buffer.count),
              MAXWELL3D_REG_INDEX(vertex_id_base), MAXWELL3D_REG_INDEX(global_base_vertex_index),
              MAXWELL3D_REG_INDEX(global_base_instance_index), MAXWELL3D_REG_INDEX(draw.begin),
              MAXWELL3D_REG_INDEX(draw.end), MAXWELL3D_REG_INDEX(topology_override)}) {
            // topology_override only acts through the draw's topology (DrawManager::
            // UpdateTopology), which a light draw must share with the last draw.
            classes[method] = LightChange::DrawParams;
        }
        // Size, address and offset: read only by constant buffer binds and data writes.
        fill(MAXWELL3D_REG_INDEX(const_buffer), 4, LightChange::UniformSelect);
        fill(MAXWELL3D_REG_INDEX(const_buffer.buffer), Maxwell3D::Regs::NumCBData, 0);
        for (size_t stage = 0; stage < Maxwell3D::Regs::MaxShaderStage; ++stage) {
            classes[MAXWELL3D_REG_INDEX(bind_groups) + stage * 8 + 4] = LightChange::UniformBind;
        }
        fill(MAXWELL3D_REG_INDEX(tex_header),
             sizeof(Maxwell3D::Regs::tex_header) / sizeof(u32), LightChange::Textures);
        fill(MAXWELL3D_REG_INDEX(tex_sampler),
             sizeof(Maxwell3D::Regs::tex_sampler) / sizeof(u32), LightChange::Textures);
        classes[MAXWELL3D_REG_INDEX(sampler_binding)] = LightChange::Textures;
        for (const size_t method :
             {MAXWELL3D_REG_INDEX(inline_data), MAXWELL3D_REG_INDEX(launch_dma),
              MAXWELL3D_REG_INDEX(report_semaphore.query), MAXWELL3D_REG_INDEX(sync_info),
              MAXWELL3D_REG_INDEX(clear_report_value), MAXWELL3D_REG_INDEX(render_enable.mode),
              MAXWELL3D_REG_INDEX(wait_for_idle), MAXWELL3D_REG_INDEX(fragment_barrier),
              MAXWELL3D_REG_INDEX(invalidate_texture_data_cache),
              MAXWELL3D_REG_INDEX(tiled_cache_barrier), MAXWELL3D_REG_INDEX(clear_surface),
              MAXWELL3D_REG_INDEX(draw_texture.src_y0), MAXWELL3D_REG_INDEX(falcon[4]),
              MAXWELL3D_REG_INDEX(load_mme.instruction_ptr), MAXWELL3D_REG_INDEX(load_mme.instruction),
              MAXWELL3D_REG_INDEX(load_mme.start_address), MAXWELL3D_REG_INDEX(shadow_ram_control),
              MAXWELL3D_REG_INDEX(draw_inline_index), MAXWELL3D_REG_INDEX(inline_index_2x16.even),
              MAXWELL3D_REG_INDEX(inline_index_4x8.index0),
              MAXWELL3D_REG_INDEX(vertex_array_instance_first),
              MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent),
              MAXWELL3D_REG_INDEX(index_buffer32_first), MAXWELL3D_REG_INDEX(index_buffer16_first),
              MAXWELL3D_REG_INDEX(index_buffer8_first),
              MAXWELL3D_REG_INDEX(index_buffer32_subsequent),
              MAXWELL3D_REG_INDEX(index_buffer16_subsequent),
              MAXWELL3D_REG_INDEX(index_buffer8_subsequent)}) {
            classes[method] = LightChange::Command;
        }
        return classes;
    }();
    return table;
}

// The table, looked up once at startup: ProcessDirtyRegisters reads it for every register write
// (LightClasses' function-local static is checked for its initialization on each call).
const std::array<u16, Maxwell3D::Regs::NUM_REGS>& g_light_classes = LightClasses();

// EDEN_NX2_SAME_SKIP=0: a constant buffer write of the bytes guest memory already holds that
// IsLightSameWrite counted is still made, and its range invalidated at the next FlushCaching.
const bool g_same_skip = [] {
    const char* value = std::getenv("EDEN_NX2_SAME_SKIP");
    return value == nullptr || value[0] != '0';
}();

MAXWELL3D_FORCE_INLINE void NoteSameWriteSkipped() {
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::SameWritesSkipped, 1);
    thread_local u64 skipped{};
    if ((++skipped & ((u64{1} << 22) - 1)) == 0) {
        LOG_INFO(HW_GPU, "Constant buffer writes of the bytes memory held, skipped: {} "
                         "(EDEN_NX2_SAME_SKIP=0 makes them)",
                 skipped);
    }
}

} // Anonymous namespace

// Back stages (nx2_stage_api.h): the side state of a primary as A last sent it (StageWriteSide),
// and what StageSideWords found changed since.
struct Maxwell3D::StageSent {
    decltype(State::shader_stages) const_buffers{};
    EngineHint engine_state{};
    ::Common::unordered_map<u64, HLEReplacementAttributeType> replace_table;
    bool execute_on{};
    DrawManager::State draw_state{};
    DrawManager::IndirectParams indirect_state{};
    DrawManager::DrawTextureState draw_texture_state{};
    /// =2: the light part goes with every draw, with A's light changes since the last one.
    bool verify{};
    // StageSideWords' result, for StageWriteSide.
    u32 parts{};
    u32 words{};
    u32 cb_count{};
    std::array<u8, Regs::MaxShaderStage * Regs::MaxConstBuffers> cb_slots{};
};

namespace {

// Side state parts (nx2_reg_replica.h, SidePart), each a run of u64 words:
//   SideConstBuffers   count, then per bind: stage | slot << 8 | enabled << 16 | size << 32,
//                      address
//   SideRebound        rebound[0] | rebound[1] << 32, rebound[2] | rebound[3] << 32,
//                      rebound[4] | A's light changes since the last draw << 32 (=2 checks them)
//   SideEngineState    engine_state
//   SideReplaceTable   count, then per entry: key, value (the whole table)
//   SideExecuteOn      execute_on
//   SideDrawState      topology | draw_mode << 32, draw_indexed | base_index << 32,
//                      vertex_buffer.first | count << 32, base_instance | instance_count << 32,
//                      index_buffer's words, the inline index bytes' count, the bytes
//   SideIndirectState  is_byte_count | is_indexed << 1 | include_count << 2,
//                      count_start_address, indirect_start_address, buffer_size,
//                      max_draw_counts, stride
//   SideDrawTexture    draw_texture_state's words
//   SideDirtyFlags     dirty.flags set since the last send (ORed in)
using DrawState = Maxwell3D::DrawManager::State;
using IndirectParams = Maxwell3D::DrawManager::IndirectParams;
using DrawTextureState = Maxwell3D::DrawManager::DrawTextureState;
using DirtyFlags = Maxwell3D::DirtyState::Flags;

constexpr u32 WordsOf(size_t bytes) {
    return static_cast<u32>((bytes + sizeof(u64) - 1) / sizeof(u64));
}
constexpr u32 IndexBufferWords = WordsOf(sizeof(Maxwell3D::Regs::IndexBuffer));
constexpr u32 DrawTextureWords = WordsOf(sizeof(DrawTextureState));
constexpr u32 IndirectWords = 6;
constexpr u32 DirtyFlagWords = WordsOf(sizeof(DirtyFlags));
static_assert(std::is_trivially_copyable_v<Maxwell3D::Regs::IndexBuffer> &&
              std::is_trivially_copyable_v<DrawTextureState> &&
              std::is_trivially_copyable_v<DirtyFlags> && sizeof(DirtyFlags) == 32);

u32 DrawStateWords(const DrawState& draw_state) {
    return 5 + IndexBufferWords + WordsOf(draw_state.inline_index_draw_indexes.size());
}

bool SameConstBuffer(const ConstBufferInfo& a, const ConstBufferInfo& b) {
    return a.address == b.address && a.size == b.size && a.enabled == b.enabled;
}

bool SameDrawState(const DrawState& a, const DrawState& b) {
    return a.topology == b.topology && a.draw_mode == b.draw_mode &&
           a.draw_indexed == b.draw_indexed && a.base_index == b.base_index &&
           a.vertex_buffer.first == b.vertex_buffer.first &&
           a.vertex_buffer.count == b.vertex_buffer.count &&
           std::memcmp(&a.index_buffer, &b.index_buffer, sizeof(a.index_buffer)) == 0 &&
           a.base_instance == b.base_instance && a.instance_count == b.instance_count &&
           a.inline_index_draw_indexes == b.inline_index_draw_indexes;
}

bool SameIndirect(const IndirectParams& a, const IndirectParams& b) {
    return a.is_byte_count == b.is_byte_count && a.is_indexed == b.is_indexed &&
           a.include_count == b.include_count && a.count_start_address == b.count_start_address &&
           a.indirect_start_address == b.indirect_start_address &&
           a.buffer_size == b.buffer_size && a.max_draw_counts == b.max_draw_counts &&
           a.stride == b.stride;
}

u64 Pair(u32 low, u32 high) {
    return u64{low} | (u64{high} << 32);
}

void WriteDrawState(u64*& out, const DrawState& draw_state) {
    *out++ = Pair(static_cast<u32>(draw_state.topology), static_cast<u32>(draw_state.draw_mode));
    *out++ = Pair(draw_state.draw_indexed ? 1U : 0U, draw_state.base_index);
    *out++ = Pair(draw_state.vertex_buffer.first, draw_state.vertex_buffer.count);
    *out++ = Pair(draw_state.base_instance, draw_state.instance_count);
    out[IndexBufferWords - 1] = 0;
    std::memcpy(out, &draw_state.index_buffer, sizeof(draw_state.index_buffer));
    out += IndexBufferWords;
    const auto& bytes = draw_state.inline_index_draw_indexes;
    *out++ = bytes.size();
    if (!bytes.empty()) {
        out[WordsOf(bytes.size()) - 1] = 0;
        std::memcpy(out, bytes.data(), bytes.size());
        out += WordsOf(bytes.size());
    }
}

void ReadDrawState(const u64*& in, DrawState& draw_state) {
    draw_state.topology = static_cast<Maxwell3D::Regs::PrimitiveTopology>(static_cast<u32>(in[0]));
    draw_state.draw_mode = static_cast<Maxwell3D::DrawManager::DrawMode>(in[0] >> 32);
    draw_state.draw_indexed = (in[1] & 1) != 0;
    draw_state.base_index = static_cast<u32>(in[1] >> 32);
    draw_state.vertex_buffer.first = static_cast<u32>(in[2]);
    draw_state.vertex_buffer.count = static_cast<u32>(in[2] >> 32);
    draw_state.base_instance = static_cast<u32>(in[3]);
    draw_state.instance_count = static_cast<u32>(in[3] >> 32);
    in += 4;
    std::memcpy(&draw_state.index_buffer, in, sizeof(draw_state.index_buffer));
    in += IndexBufferWords;
    const size_t size = static_cast<size_t>(*in++);
    draw_state.inline_index_draw_indexes.resize(size);
    if (size != 0) {
        std::memcpy(draw_state.inline_index_draw_indexes.data(), in, size);
        in += WordsOf(size);
    }
}

// =2 (StageVerify, StageApplySide): the first differences are logged, all are counted.
bool LogStageDifference() {
    static std::atomic<u32> logged{};
    return logged.fetch_add(1, std::memory_order_relaxed) < 16;
}

} // Anonymous namespace

Maxwell3D::Maxwell3D(MemoryManager& memory_manager_)
    : draw_manager()
    , memory_manager{memory_manager_}
#ifdef ARCHITECTURE_x86_64
    , macro_engine(bool(Settings::values.disable_macro_jit))
#else
    , macro_engine(true)
#endif
    , upload_state{memory_manager, regs.upload}
{
    dirty.flags.flip();
    InitializeRegisterDefaults();
    execution_mask.reset();
    for (size_t i = 0; i < execution_mask.size(); i++)
        execution_mask[i] = IsMethodExecutable(u32(i));
    // EDEN_NX2_CB_RUNS=0: the split's back runs each constant buffer data word a macro writes on
    // its own (CallMethod), each with its own guest memory write (see CallMethodRun).
    static const bool cb_runs = [] {
        const char* value = std::getenv("EDEN_NX2_CB_RUNS");
        return value == nullptr || value[0] != '0';
    }();
    if (cb_runs) {
        method_run_begin = MAXWELL3D_REG_INDEX(const_buffer.buffer);
        method_run_count = 16;
    }
    // Back stages: A runs the executable methods and sends what they do (nx2_stage_api.h).
    stage_methods_on_a = true;
}

Maxwell3D::~Maxwell3D() = default;

void Maxwell3D::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
    upload_state.BindRasterizer(rasterizer_);
}

void Maxwell3D::InitializeRegisterDefaults() {
    // Initializes registers to their default values - what games expect them to be at boot. This is
    // for certain registers that may not be explicitly set by games.

    // Reset all registers to zero
    std::memset(&regs, 0, sizeof(regs));

    // Depth range near/far is not always set, but is expected to be the default 0.0f, 1.0f. This is
    // needed for ARMS.
    for (auto& viewport : regs.viewports) {
        viewport.depth_range_near = 0.0f;
        viewport.depth_range_far = 1.0f;
    }
    for (auto& viewport : regs.viewport_transform) {
        viewport.swizzle.x.Assign(Regs::ViewportSwizzle::PositiveX);
        viewport.swizzle.y.Assign(Regs::ViewportSwizzle::PositiveY);
        viewport.swizzle.z.Assign(Regs::ViewportSwizzle::PositiveZ);
        viewport.swizzle.w.Assign(Regs::ViewportSwizzle::PositiveW);
    }

    // Doom and Bomberman seems to use the uninitialized registers and just enable blend
    // so initialize blend registers with sane values
    regs.blend.color_op = Regs::Blend::Equation::Add_D3D;
    regs.blend.color_source = Regs::Blend::Factor::One_D3D;
    regs.blend.color_dest = Regs::Blend::Factor::Zero_D3D;
    regs.blend.alpha_op = Regs::Blend::Equation::Add_D3D;
    regs.blend.alpha_source = Regs::Blend::Factor::One_D3D;
    regs.blend.alpha_dest = Regs::Blend::Factor::Zero_D3D;
    for (auto& blend : regs.blend_per_target) {
        blend.color_op = Regs::Blend::Equation::Add_D3D;
        blend.color_source = Regs::Blend::Factor::One_D3D;
        blend.color_dest = Regs::Blend::Factor::Zero_D3D;
        blend.alpha_op = Regs::Blend::Equation::Add_D3D;
        blend.alpha_source = Regs::Blend::Factor::One_D3D;
        blend.alpha_dest = Regs::Blend::Factor::Zero_D3D;
    }
    regs.stencil_front_op.fail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.zfail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.zpass = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.func = Regs::ComparisonOp::Always_GL;
    regs.stencil_front_func_mask = 0xFFFFFFFF;
    regs.stencil_front_mask = 0xFFFFFFFF;
    regs.stencil_two_side_enable = 1;
    regs.stencil_back_op.fail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.zfail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.zpass = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.func = Regs::ComparisonOp::Always_GL;
    regs.stencil_back_func_mask = 0xFFFFFFFF;
    regs.stencil_back_mask = 0xFFFFFFFF;

    regs.depth_test_func = Regs::ComparisonOp::Always_GL;
    regs.gl_front_face = Regs::FrontFace::CounterClockWise;
    regs.gl_cull_face = Regs::CullFace::Back;

    // TODO(Rodrigo): Most games do not set a point size. I think this is a case of a
    // register carrying a default value. Assume it's OpenGL's default (1).
    regs.point_size = 1.0f;

    // TODO(bunnei): Some games do not initialize the color masks (e.g. Sonic Mania). Assuming a
    // default of enabled fixes rendering here.
    for (auto& color_mask : regs.color_mask) {
        color_mask.R.Assign(1);
        color_mask.G.Assign(1);
        color_mask.B.Assign(1);
        color_mask.A.Assign(1);
    }

    for (auto& format : regs.vertex_attrib_format) {
        format.constant.Assign(1);
    }

    // NVN games expect these values to be enabled at boot
    regs.rasterize_enable = 1;
    regs.color_target_mrt_enable = 1;
    regs.framebuffer_srgb = 1;
    regs.line_width_aliased = 1.0f;
    regs.line_width_smooth = 1.0f;
    regs.gl_front_face = Maxwell3D::Regs::FrontFace::ClockWise;
    regs.polygon_mode_back = Maxwell3D::Regs::PolygonMode::Fill;
    regs.polygon_mode_front = Maxwell3D::Regs::PolygonMode::Fill;

    shadow_state = regs;
}

bool Maxwell3D::IsMethodExecutable(u32 method) {
    if (method >= MacroRegistersStart) {
        return true;
    }
    if (method == RootTables::SelectMethod || method == RootTables::LoadMethod) {
        // EDEN_NX2_ROOT_TABLES: each write acts (a load of the same word twice writes two).
        return Shader::Optimization::Nx2Root::TablesEnabled();
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(draw.end):
    case MAXWELL3D_REG_INDEX(draw.begin):
    case MAXWELL3D_REG_INDEX(vertex_buffer.first):
    case MAXWELL3D_REG_INDEX(vertex_buffer.count):
    case MAXWELL3D_REG_INDEX(index_buffer.first):
    case MAXWELL3D_REG_INDEX(index_buffer.count):
    case MAXWELL3D_REG_INDEX(draw_inline_index):
    case MAXWELL3D_REG_INDEX(index_buffer32_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer16_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer8_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer32_first):
    case MAXWELL3D_REG_INDEX(index_buffer16_first):
    case MAXWELL3D_REG_INDEX(index_buffer8_first):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_first):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent):
    case MAXWELL3D_REG_INDEX(draw_texture.src_y0):
    case MAXWELL3D_REG_INDEX(wait_for_idle):
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
    case MAXWELL3D_REG_INDEX(falcon[4]):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
    case MAXWELL3D_REG_INDEX(topology_override):
    case MAXWELL3D_REG_INDEX(clear_surface):
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
    case MAXWELL3D_REG_INDEX(render_enable.mode):
    case MAXWELL3D_REG_INDEX(clear_report_value):
    case MAXWELL3D_REG_INDEX(sync_info):
    case MAXWELL3D_REG_INDEX(launch_dma):
    case MAXWELL3D_REG_INDEX(inline_data):
    case MAXWELL3D_REG_INDEX(fragment_barrier):
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return true;
    default:
        return false;
    }
}

void Maxwell3D::ProcessMacro(Core::System& system, u32 method, const u32* base_start, u32 amount, bool is_last_call) {
    if (executing_macro == 0) {
        // A macro call must begin by writing the macro method's register, not its argument.
        ASSERT((method % 2) == 0 && "Can't start macro execution by writing to the ARGS register");
        executing_macro = method;
    }

    macro_segments.emplace_back(current_dma_segment, amount);
    current_macro_dirty |= current_dirty;
    current_dirty = false;

    // Nearly every call arrives whole in one method run (title 1 makes ~1,000,000 a second):
    // copy it to the stack instead of growing the parameter vector. Still a copy: the command
    // list is often guest memory read in place, which the guest may reuse while the macro runs
    // (and the macro memo stores the parameters after it ran). Only accurate GPU mode rewrites
    // the parameters in place (RefreshParameters), so it keeps the vector.
    // EDEN_NX2_MACRO_COPY=1: always use the vector (for comparisons).
    static const bool always_copy = std::getenv("EDEN_NX2_MACRO_COPY") != nullptr;
    if (is_last_call && macro_params.empty() && amount <= 64 && !always_copy &&
        !Settings::IsGPULevelHigh()) {
        std::array<u32, 64> parameters;
        std::copy_n(base_start, amount, parameters.begin());
        ConsumeSink(system);
        CallMacroMethod(system, executing_macro, std::span<const u32>{parameters.data(), amount});
        macro_segments.clear();
        current_macro_dirty = false;
        return;
    }
    macro_params.insert(macro_params.end(), base_start, base_start + amount);

    // Call the macro when there are no more parameters in the command buffer
    if (is_last_call) {
        ConsumeSink(system);
        CallMacroMethod(system, executing_macro, macro_params);
        macro_params.clear();
        macro_segments.clear();
        current_macro_dirty = false;
    }
}

void Maxwell3D::RefreshParametersImpl() {
    if (!Settings::IsGPULevelHigh()) {
        return;
    }
    size_t current_index = 0;
    for (auto& segment : macro_segments) {
        if (segment.first == 0) {
            current_index += segment.second;
            continue;
        }
        memory_manager.ReadBlock(segment.first, &macro_params[current_index],
                                 sizeof(u32) * segment.second);
        current_index += segment.second;
    }
}

u32 Maxwell3D::GetMaxCurrentVertices() {
    u32 num_vertices = 0;
    for (size_t index = 0; index < Regs::NumVertexArrays; ++index) {
        const auto& array = regs.vertex_streams[index];
        if (array.enable == 0) {
            continue;
        }
        const auto& attribute = regs.vertex_attrib_format[index];
        if (attribute.constant) {
            num_vertices = (std::max)(num_vertices, 1U);
            continue;
        }
        const auto& limit = regs.vertex_stream_limits[index];
        const GPUVAddr gpu_addr_begin = array.Address();
        const GPUVAddr gpu_addr_end = limit.Address() + 1;
        const u64 stream_size = GetNativeMacroEngine().native_turing
                                    ? regs.NativeVertexStreamSize(index)
                                    : gpu_addr_end - gpu_addr_begin;
        const u32 address_size = static_cast<u32>((std::min<u64>)(stream_size, (std::numeric_limits<u32>::max)()));
        num_vertices = (std::max)(
            num_vertices, address_size / (std::max)(attribute.SizeInBytes(), array.stride.Value()));
        break;
    }
    return num_vertices;
}

size_t Maxwell3D::EstimateIndexBufferSize() {
    GPUVAddr start_address = regs.index_buffer.StartAddress();
    GPUVAddr end_address = regs.index_buffer.EndAddress();
    auto const byte_size = regs.index_buffer.FormatSizeInBytes();
    auto const max_size = 1ull << (byte_size * CHAR_BIT);
    auto const upper_cap = GetMaxCurrentVertices() * 4 * byte_size;
    auto const lower_cap = std::min<size_t>(size_t(end_address - start_address), upper_cap);
    // EDEN_NX2_LAYOUT_MEMO=0: the layout size is looked up every time (a lock and a range map
    // walk, once per indirect draw: ~70,000 a second on Donkey Kong Bananza's wide views). It
    // depends only on the start and the GPU mappings (kind_map, which only PageTableOp and
    // BigPageTableOp change, each bumping MappingEpoch); max_size is not read. The epoch is read
    // before the lookup, so a change meanwhile leaves a memo no later call matches.
    static const bool layout_memo = [] {
        const char* value = std::getenv("EDEN_NX2_LAYOUT_MEMO");
        return value == nullptr || value[0] != '0';
    }();
    size_t layout_size;
    if (layout_memo) {
        struct LayoutMemo {
            size_t owner = ~size_t{0};
            GPUVAddr address{};
            u64 epoch{};
            size_t size{};
        };
        thread_local LayoutMemo memo;
        const u64 epoch = memory_manager.MappingEpoch();
        if (memo.owner != memory_manager.GetID() || memo.address != start_address ||
            memo.epoch != epoch) {
            memo = LayoutMemo{
                .owner = memory_manager.GetID(),
                .address = start_address,
                .epoch = epoch,
                .size = memory_manager.GetMemoryLayoutSize(start_address, byte_size * max_size),
            };
        }
        layout_size = memo.size;
    } else {
        layout_size = memory_manager.GetMemoryLayoutSize(start_address, byte_size * max_size);
    }
    return std::min<size_t>(layout_size / byte_size, lower_cap);
}

u32 Maxwell3D::ProcessShadowRam(u32 method, u32 argument) {
    // Keep track of the register value in shadow_state when requested.
    auto const c = shadow_state.shadow_ram_control;
    if (c == Regs::ShadowRamControl::Track || c == Regs::ShadowRamControl::TrackWithFilter)
        return shadow_state.reg_array[method] = argument;
    else if (c == Regs::ShadowRamControl::Replay)
        return shadow_state.reg_array[method];
    return argument;
}

void Maxwell3D::ConsumeSinkImpl(Core::System& system) {
    const auto control = shadow_state.shadow_ram_control;
    // Sunk methods are state writes only (not in execution_mask): skip unchanged values, as
    // CallMethod does.
    const bool skip_same = SkipSameValueWrites();
    const auto process = [&](u32 method, u32 value) {
        if (!skip_same || regs.reg_array[method] != value) {
            ProcessDirtyRegisters(method, value);
        }
    };
    if (control == Regs::ShadowRamControl::Track || control == Regs::ShadowRamControl::TrackWithFilter) {
        for (auto [method, value] : method_sink) {
            shadow_state.reg_array[method] = value;
            process(method, value);
        }
    } else if (control == Regs::ShadowRamControl::Replay) {
        for (auto [method, value] : method_sink)
            process(method, shadow_state.reg_array[method]);
    } else {
        for (auto [method, value] : method_sink)
            process(method, value);
    }
    method_sink.clear();
}

void Maxwell3D::ProcessDirtyRegisters(u32 method, u32 argument) {
    // The instances after the first are drawn together once their loop ends, and they must see
    // the state they were begun with: NVN's draw macros reset the base instance (and its copy in
    // c[0][0x644]) right after the loop, before the macro returns.
    if (draw_manager.HasDeferredDraw() && !DrawManager::IsInstanceLoopMethod(method)) [[unlikely]] {
        draw_manager.DrawDeferred(*this);
    }
    if (stage_capture) [[unlikely]] StageNoteWrite(method, argument);
    // A state register written with the value it holds changes nothing a draw reads (its dirty
    // flags only make the next full setup redo the same); executable methods act whatever their
    // value. With EDEN_NX2_SKIP_SAME such writes do not get here (CallMethod, ConsumeSinkImpl).
    if (method - RootTables::SelectMethod <= RootTables::LoadMethod - RootTables::SelectMethod)
        [[unlikely]] {
        if (Shader::Optimization::Nx2Root::DiagEnabled()) {
            NoteRootWrite(method, argument);
        }
    }
    const bool changed = regs.reg_array[method] != argument;
    regs.reg_array[method] = argument;
    for (auto const& table : dirty.tables)
        dirty.flags[table[method]] = true;
    if (LightTrackingEnabled() && (changed || execution_mask[method])) {
        NoteLightChange(g_light_classes[method], method);
    }
}

// EDEN_NX2_ROOT_DIAG: the writes to 0x504 / 0x508 that reach the back (without
// EDEN_NX2_ROOT_TABLES, a word written again with the value it holds may be skipped as a same-value
// state write): the first 32 with the table and offset they go to, then a count at powers of two.
void Maxwell3D::NoteRootWrite(u32 method, u32 argument) {
    static u64 selects{};
    static u64 loads{};
    static u32 table{};
    static u32 offset{};
    const bool select = method == RootTables::SelectMethod;
    ++(select ? selects : loads);
    const u64 total = selects + loads;
    if (select) {
        table = argument & 7;
        offset = (argument >> 8) & 0xff;
    }
    if (total <= 32) {
        if (select) {
            LOG_INFO(HW_GPU, "nx2 root diag: 3D 0x504 = {:#x} (table {}, byte offset {:#x})",
                     argument, table, offset);
        } else {
            LOG_INFO(HW_GPU, "nx2 root diag: 3D 0x508 = {:#x} (table {}, byte offset {:#x})",
                     argument, table, offset);
        }
    } else if ((total & (total - 1)) == 0) {
        LOG_INFO(HW_GPU, "nx2 root diag: {} writes to 3D 0x504 and {} to 0x508 so far", selects,
                 loads);
    }
    if (!select) {
        offset += 4;
    }
}

void Maxwell3D::ProcessMethodCall(u32 method, u32 argument, u32 nonshadow_argument, bool is_last_call) {
    if (GetNativeMacroEngine().native_turing && method == MAXWELL3D_REG_INDEX(clear_surface)) {
        LOG_DEBUG(HW_GPU, "Native GPU surface clear: value={:#x}", argument);
    }
    switch (method) {
    // EDEN_NX2_ROOT_TABLES (executable only then). A write is noted as a State change
    // (ProcessDirtyRegisters): the next draw takes the full setup, which streams table 0 as an
    // unbound bank 17 again (BindHostGraphicsUniformBuffer), so no light draw keeps an old copy.
    case RootTables::SelectMethod:
        root_tables.selected = argument & 7;
        root_tables.offset = (argument >> 8) & 0xff;
        return;
    case RootTables::LoadMethod:
        if (root_tables.offset < 256 && root_tables.offset % 4 == 0) {
            root_tables.tables[root_tables.selected][root_tables.offset / 4] = argument;
            if (root_tables.selected == 0 && !root_tables.loaded) {
                root_tables.loaded = true;
                LOG_INFO(HW_GPU, "nx2 root tables (EDEN_NX2_ROOT_TABLES): the game loads root "
                                 "table 0 (3D 0x504 / 0x508), bank 17 reads it where unbound");
            }
        }
        root_tables.offset += 4;
        return;
    case MAXWELL3D_REG_INDEX(wait_for_idle):
        return rasterizer->WaitForIdle();
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
        shadow_state.shadow_ram_control = static_cast<Regs::ShadowRamControl>(nonshadow_argument);
        return;
    // With macros on the split GPU thread's front (FrontWrite), the front loads and binds them.
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
        if (front_macros) return;
        return GetNativeMacroEngine().ClearCode(regs.load_mme.instruction_ptr);
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
        if (front_macros) return;
        return GetNativeMacroEngine().AddCode(regs.load_mme.instruction_ptr, argument);
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
        if (front_macros) {
            ++regs.load_mme.start_address_ptr;
            return;
        }
        return ProcessMacroBind(argument);
    case MAXWELL3D_REG_INDEX(falcon[4]):
        return ProcessFirmwareCall4();
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
        return ProcessCBData(argument);
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
        return ProcessCBBind(0);
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
        return ProcessCBBind(1);
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
        return ProcessCBBind(2);
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
        return ProcessCBBind(3);
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
        return ProcessCBBind(4);
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
        return ProcessQueryGet();
    case MAXWELL3D_REG_INDEX(render_enable.mode):
        return ProcessQueryCondition();
    case MAXWELL3D_REG_INDEX(clear_report_value):
        return ProcessCounterReset();
    case MAXWELL3D_REG_INDEX(sync_info):
        return ProcessSyncPoint();
    case MAXWELL3D_REG_INDEX(launch_dma):
        return upload_state.ProcessExec(regs.launch_dma.memory_layout.Value() == Regs::LaunchDMA::Layout::Pitch);
    case MAXWELL3D_REG_INDEX(inline_data):
        upload_state.ProcessData(argument, is_last_call);
        return;
    case MAXWELL3D_REG_INDEX(fragment_barrier):
        return rasterizer->FragmentBarrier();
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
        rasterizer->InvalidateGPUCache();
        return rasterizer->WaitForIdle();
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return rasterizer->TiledCacheBarrier();
    default:
        draw_manager.ProcessMethodCall(*this, method, argument);
        break;
    }
}

void Maxwell3D::CallMacroMethod(Core::System& system, u32 method, std::span<const u32> parameters) {
    // Reset the current macro.
    executing_macro = 0;
    // HLE macros change registers and dirty flags directly: no light draw after them.
    NoteLightChange(LightChange::State, method);
    if (stage_capture) [[unlikely]] StageNoteDirect(0, false);

    // Lookup the macro offset
    const u32 entry =
        ((method - MacroRegistersStart) >> 1) % static_cast<u32>(GetNativeMacroPositions().size());

    // Execute the current macro.
    GetNativeMacroEngine().Execute(system, *this, GetNativeMacroPositions()[entry], parameters);
    draw_manager.DrawDeferred(*this);
}

// EDEN_NX2_3D_METHODS: log the first write to each 3D register (finds methods Eden doesn't model).
static const bool g_trace_first_method_writes = std::getenv("EDEN_NX2_3D_METHODS") != nullptr;

void Maxwell3D::TraceFirstMethodWrite(u32 method, u32 argument) {
    static std::array<bool, Regs::NUM_REGS> seen{};
    if (method < seen.size() && !seen[method]) {
        seen[method] = true;
        LOG_INFO(HW_GPU, "3D method first write: {:#x} (byte {:#x}) = {:#x}", method, method * 4, argument);
    }
}

MAXWELL3D_FORCE_INLINE void Maxwell3D::CallMethodImpl(Core::System& system, u32 method,
                                                      u32 method_argument, bool is_last_call) {
    // It is an error to write to a register other than the current macro's ARG register before
    // it has finished execution.
    if (executing_macro != 0 && method != executing_macro + 1) {
        DumpRecentWrites(method);
    }
    if (executing_macro != 0) {
        ASSERT(method == executing_macro + 1);
    }
    RecordWrite(method, method_argument, 1, is_last_call ? 1 : 2);

    // Methods after 0xE00 are special, they're actually triggers for some microcode that was
    // uploaded to the GPU during initialization.
    if (method >= MacroRegistersStart) {
        ProcessMacro(system, method, &method_argument, 1, is_last_call);
        return;
    }
    if (g_trace_first_method_writes) [[unlikely]] {
        TraceFirstMethodWrite(method, method_argument);
    }

    ASSERT(method < Regs::NUM_REGS && "Invalid Maxwell3D register, increase the size of the Regs structure");
    if (VideoCommon::Nx2Profile::Enabled()) [[unlikely]] {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::MethodWrites, 1);
        if (regs.reg_array[method] == method_argument) {
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::SameValueWrites, 1);
        }
    }
    if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && front_macros) [[unlikely]] {
        back_control_writes[back_control_count++ % back_control_writes.size()] = {
            method_argument, static_cast<u32>(shadow_state.shadow_ram_control), is_last_call ? 1U : 0U};
    }
    const u32 argument = ProcessShadowRam(method, method_argument);
    // A state register written with the value it holds changes nothing: no dirty flags (they
    // would make the next draw re-check that state), no method processing. Title 1 makes ~6
    // million 3D register writes a second, ~60% of them same-value, most from macro replays.
    // Methods with side effects (execution_mask: draws, clears, constant buffer data and
    // binds, semaphores, ...) always run. EDEN_NX2_SKIP_SAME=0 turns it off.
    if (!execution_mask[method]) {
        // ProcessMethodCall (and the draw manager behind it) acts only on executable methods.
        if (!SkipSameValueWrites() || regs.reg_array[method] != argument) {
            ProcessDirtyRegisters(method, argument);
        }
        return;
    }
    ProcessDirtyRegisters(method, argument);
    ProcessMethodCall(method, argument, method_argument, is_last_call);
}

void Maxwell3D::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    CallMethodImpl(system, method, method_argument, is_last_call);
}

void Maxwell3D::CallMethodBatch(Core::System& system, const u64* writes, u32 count) {
    // The split's back takes ~3 million macro writes a second this way (EDEN_NX2_WRITE_BATCH):
    // the same calls as EngineInterface::CallMethodBatch, CallMethod inlined into the loop.
    ForEachBatchWrite(
        writes, count,
        [&](u32 method, u32 argument) { CallMethodImpl(system, method, argument, true); },
        [&](const u32* methods, const u32* arguments, u32 run) {
            CallMethodRun(system, methods, arguments, run);
        });
}

void Maxwell3D::CallMethodRun(Core::System& system, const u32* methods, const u32* arguments,
                              u32 count) {
    // NVN's draw macros write two words at a time into c[0][0x640] (base vertex and instance,
    // set and reset around each draw): the split's back made one guest memory write (page walk,
    // invalidation range) per word, ~2% of it. Each word still gets what CallMethod gives it
    // (shadow RAM, its register, dirty flags); the words then go to memory together. Same result:
    // what the words set does not move the write (CB_ADDRESS, CB_OFFSET and CB_SIZE are other
    // registers), and nothing in between reads guest memory: only the first word can end a
    // deferred instanced draw (ProcessDirtyRegisters clears it), before any word is written, as
    // before. The invalidation ranges cover the bytes the single writes did (contiguous).
    // Runs that would trip ProcessCBMultiData's per-write checks take CallMethod word by word.
    const auto& cb = regs.const_buffer;
    if (count > MaxMethodRun || executing_macro != 0 || cb.Address() == 0 ||
        u64{cb.offset} + u64{count - 1} * sizeof(u32) > u64{cb.size}) {
        for (u32 i = 0; i < count; ++i) {
            CallMethod(system, methods[i], arguments[i], true);
        }
        return;
    }
    std::array<u32, MaxMethodRun> values;
    for (u32 i = 0; i < count; ++i) {
        const u32 method = methods[i];
        const u32 method_argument = arguments[i];
        RecordWrite(method, method_argument, 1, 1);
        if (g_trace_first_method_writes) [[unlikely]] {
            TraceFirstMethodWrite(method, method_argument);
        }
        if (VideoCommon::Nx2Profile::Enabled()) [[unlikely]] {
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::MethodWrites, 1);
            if (regs.reg_array[method] == method_argument) {
                VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::SameValueWrites, 1);
            }
        }
        const u32 argument = ProcessShadowRam(method, method_argument);
        ProcessDirtyRegisters(method, argument);
        values[i] = argument;
    }
    ProcessCBMultiData(values.data(), count);
}

void Maxwell3D::DumpRecentWrites(u32 method) {
    static u32 dumps{};
    if (dumps++ >= 4) {
        return;
    }
    LOG_ERROR(HW_GPU, "Write to {:#x} while macro {:#x} collects parameters ({} so far); last writes:",
              method, executing_macro, macro_params.size());
    if (!record_write_history) {
        LOG_ERROR(HW_GPU, "Register write history disabled; EDEN_NX2_3D_HISTORY=1 enables it");
        return;
    }
    for (size_t i = 0; i < recent_writes.size(); ++i) {
        const auto& w = recent_writes[(recent_write_index + i) % recent_writes.size()];
        if (w.amount != 0) {
            LOG_ERROR(HW_GPU, "  method {:#x} arg {:#x} amount {} pending {}", w.method, w.argument,
                      w.amount, w.pending);
        }
    }
}

void Maxwell3D::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    RecordWrite(method, amount ? base_start[0] : 0, amount, methods_pending);
    // Methods after 0xE00 are special, they're actually triggers for some microcode that was
    // uploaded to the GPU during initialization.
    if (method >= MacroRegistersStart) {
        ProcessMacro(system, method, base_start, amount, amount == methods_pending);
        return;
    }
    // As in ProcessDirtyRegisters: the paths below write without it.
    if (draw_manager.HasDeferredDraw() && !DrawManager::IsInstanceLoopMethod(method)) [[unlikely]] {
        draw_manager.DrawDeferred(*this);
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
        ProcessCBMultiData(base_start, amount);
        break;
    case MAXWELL3D_REG_INDEX(inline_data): {
        ASSERT(methods_pending == amount);
        NoteLightChange(LightChange::Command, method);
        if (stage_capture) [[unlikely]] StageNoteDirect(method, false);
        upload_state.ProcessData(base_start, amount);
        return;
    }
    case MAXWELL3D_REG_INDEX(draw_inline_index):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
        if (shadow_state.shadow_ram_control != Regs::ShadowRamControl::Replay) {
            ProcessInlineIndexMultiData(method, base_start, amount);
            break;
        }
        [[fallthrough]];
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

void Maxwell3D::ProcessMacroUpload(u32 data) {
    GetNativeMacroEngine().AddCode(regs.load_mme.instruction_ptr++, data);
}

void Maxwell3D::ProcessMacroBind(u32 data) {
    const auto positions = GetNativeMacroPositions();
    const u32 index = regs.load_mme.start_address_ptr++;
    if (index < positions.size()) {
        positions[index] = data;
    }
}

void Maxwell3D::ProcessFirmwareCall4() {
    LOG_DEBUG(HW_GPU, "(STUBBED) called");

    // Firmware call 4 is a blob that changes some registers depending on its parameters.
    // These registers don't affect emulation and so are stubbed by setting 0xd00 to 1.
    regs.shadow_scratch[0] = 1;
    if (stage_capture) [[unlikely]] StageNoteDirect(MAXWELL3D_REG_INDEX(shadow_scratch), true);
}

void Maxwell3D::StampQueryResult(Core::System& system, u64 payload, bool long_query) {
    const GPUVAddr sequence_address{regs.report_semaphore.Address()};
    if (long_query) {
        memory_manager.Write<u64>(sequence_address + sizeof(u64), system.GPU().GetTicks());
        memory_manager.Write<u64>(sequence_address, payload);
    } else {
        memory_manager.Write<u32>(sequence_address, static_cast<u32>(payload));
    }
}

void Maxwell3D::ProcessQueryGet() {
    VideoCommon::Nx2ModeDiag::SourceScope diag_source{VideoCommon::Nx2ModeDiag::Source::Report3D};
    if (VideoCommon::Nx2ModeDiag::Enabled()) [[unlikely]] {
        VideoCommon::Nx2ModeDiag::Report3D(regs.report_semaphore.query.raw);
    }
    VideoCommon::QueryPropertiesFlags flags{};
    if (regs.report_semaphore.query.short_query == 0) {
        flags |= VideoCommon::QueryPropertiesFlags::HasTimeout;
    }
    const GPUVAddr sequence_address{regs.report_semaphore.Address()};
    const VideoCommon::QueryType query_type =
        static_cast<VideoCommon::QueryType>(regs.report_semaphore.query.report.Value());
    const u32 payload = regs.report_semaphore.payload;
    const u32 subreport = regs.report_semaphore.query.sub_report;
    switch (regs.report_semaphore.query.operation) {
    case Regs::ReportSemaphore::Operation::Release:
        if (regs.report_semaphore.query.short_query != 0) {
            flags |= VideoCommon::QueryPropertiesFlags::IsAFence;
        }
        rasterizer->Query(sequence_address, query_type, flags, payload, subreport);
        break;
    case Regs::ReportSemaphore::Operation::Acquire:
        // TODO(Blinkhawk): Under this operation, the GPU waits for the CPU to write a value that
        // matches the current payload.
        UNIMPLEMENTED_MSG("Unimplemented query operation ACQUIRE");
        break;
    case Regs::ReportSemaphore::Operation::ReportOnly:
        rasterizer->Query(sequence_address, query_type, flags, payload, subreport);
        break;
    case Regs::ReportSemaphore::Operation::Trap:
        UNIMPLEMENTED_MSG("Unimplemented query operation TRAP");
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown query operation");
        break;
    }
}

void Maxwell3D::ProcessQueryCondition() {
    if (stage_capture && VideoCommon::Nx2Stages::Forwarding()) [[unlikely]] return StageQueryCondition();
    if (rasterizer->AccelerateConditionalRendering()) {
        execute_on = true;
        return;
    }
    const GPUVAddr condition_address{regs.render_enable.Address()};
    switch (regs.render_enable_override) {
    case Regs::RenderEnable::Override::AlwaysRender:
        execute_on = true;
        break;
    case Regs::RenderEnable::Override::NeverRender:
        execute_on = false;
        break;
    case Regs::RenderEnable::Override::UseRenderEnable: {
        switch (regs.render_enable.mode) {
        case Regs::RenderEnable::Mode::True: {
            execute_on = true;
            break;
        }
        case Regs::RenderEnable::Mode::False: {
            execute_on = false;
            break;
        }
        case Regs::RenderEnable::Mode::Conditional: {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence != 0U && cmp.initial_mode != 0U;
            break;
        }
        case Regs::RenderEnable::Mode::IfEqual: {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence == cmp.current_sequence &&
                         cmp.initial_mode == cmp.current_mode;
            break;
        }
        case Regs::RenderEnable::Mode::IfNotEqual: {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence != cmp.current_sequence ||
                         cmp.initial_mode != cmp.current_mode;
            break;
        }
        default: {
            UNIMPLEMENTED_MSG("Uninplemented Condition Mode!");
            execute_on = true;
            break;
        }
        }
        break;
    }
    }
}

void Maxwell3D::ProcessCounterReset() {
    const auto query_type = [clear_report = regs.clear_report_value]() {
        switch (clear_report) {
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::ZPassPixelCount:
            return VideoCommon::QueryType::ZPassPixelCount64;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::StreamingPrimitivesSucceeded:
            return VideoCommon::QueryType::StreamingPrimitivesSucceeded;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::PrimitivesGenerated:
            return VideoCommon::QueryType::PrimitivesGenerated;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::VtgPrimitivesOut:
            return VideoCommon::QueryType::VtgPrimitivesOut;
        default:
            LOG_DEBUG(HW_GPU, "Unimplemented counter reset={}", clear_report);
            return VideoCommon::QueryType::Payload;
        }
    }();
    rasterizer->ResetCounter(query_type);
}

void Maxwell3D::ProcessSyncPoint() {
    if (GetNativeMacroEngine().native_turing) {
        LOG_DEBUG(HW_GPU, "Native 3D syncpoint increment: id={}, raw={:#x}", regs.sync_info.sync_point.Value(), regs.reg_array[MAXWELL3D_REG_INDEX(sync_info)]);
    }
    const u32 sync_point = regs.sync_info.sync_point.Value();
    [[maybe_unused]] const u32 cache_flush = regs.sync_info.clean_l2.Value();
    rasterizer->SignalSyncPoint(sync_point);
}

void Maxwell3D::ProcessCBBind(size_t stage_index) {
    // Bind the buffer currently in CB_ADDRESS to the specified index in the desired shader
    // stage.
    const auto& bind_data = regs.bind_groups[stage_index];
    static std::array<std::array<bool, 32>, 5> seen{};
    if (!seen[stage_index][bind_data.shader_slot]) {
        seen[stage_index][bind_data.shader_slot] = true;
        LOG_INFO(HW_GPU, "Constant buffer bind: stage {} slot {} address {:#x} size {:#x}", stage_index,
                 bind_data.shader_slot.Value(), regs.const_buffer.Address(), regs.const_buffer.size);
    }
    if (bind_data.shader_slot >= Regs::MaxConstBuffers) {
        return; // slot field is 5 bits; the state array holds 18
    }
    auto& buffer = state.shader_stages[stage_index].const_buffers[bind_data.shader_slot];
    if (buffer.enabled != (bind_data.valid.Value() != 0) ||
        buffer.address != regs.const_buffer.Address() || buffer.size != regs.const_buffer.size) {
        light.rebound[stage_index] |= 1u << bind_data.shader_slot;
    }
    buffer.enabled = bind_data.valid.Value() != 0;
    buffer.address = regs.const_buffer.Address();
    buffer.size = regs.const_buffer.size;

    const bool is_enabled = bind_data.valid.Value() != 0;
    if (!is_enabled) {
        rasterizer->DisableGraphicsUniformBuffer(stage_index, bind_data.shader_slot);
        return;
    }
    const GPUVAddr gpu_addr = regs.const_buffer.Address();
    const u32 size = regs.const_buffer.size;
    rasterizer->BindGraphicsUniformBuffer(stage_index, bind_data.shader_slot, gpu_addr, size);
}

// ProcessCBMultiData's write (CbWriteImpl) and the light helpers it calls are forced inline:
// ProcessCBMultiData inlines all of them as it did before StageCbWrite made the same write (with
// two callers MSVC calls the helpers; T2, R1 sifir-1).
MAXWELL3D_FORCE_INLINE void Maxwell3D::NoteLightWrite(GPUVAddr address, u32 size) {
    ++light.cb_writes;
    auto& written = light.written;
    if (light.written_count != 0 && light.written_count <= written.size()) {
        auto& [last_address, last_size] = written[light.written_count - 1];
        if (last_address + last_size == address) {
            last_size += size;
            return;
        }
    }
    if (light.written_count < written.size()) {
        written[light.written_count] = {address, size};
    }
    light.written_count = (std::min)(light.written_count + 1, static_cast<u32>(written.size()) + 1);
}

MAXWELL3D_FORCE_INLINE void Maxwell3D::CbWriteImpl(GPUVAddr address, const u32* start_base,
                                                   u32 amount) {
    const size_t copy_size = amount * sizeof(u32);
    bool skip_write = false;
    if (LightTrackingEnabled()) {
        const u64 same_writes = light.same_writes;
        NoteLightChange(IsLightSameWrite(address, start_base, copy_size) ? LightChange::UniformSame
                                                                         : LightChange::UniformData,
                        MAXWELL3D_REG_INDEX(const_buffer.buffer));
        // EDEN_NX2_SAME_SKIP: a write IsLightSameWrite counted changes no byte of guest memory
        // (outside sparse regions), and no cache holds anything on its pages that its
        // invalidation would change: no GPU-written buffer data or image guest memory lacks, no
        // query result whose write it would cancel (HoldsGpuWrittenData), no older bytes left by
        // a write around the caches (Tegra::Nx2UntrackedWritten). What the caches hold there
        // came from guest memory as it is; the invalidation at the next FlushCaching only made
        // them upload, compare or rebuild from the same bytes (NVN's base vertex and instance
        // words at c[0][0x640], set and reset around each draw). It is not made: neither a cached
        // write nor a same write, so the light path's count (as many same writes as cached writes
        // since the last draw) reads as before, and it is no constant buffer write either
        // (NoteLightWrite): the uniform way counts and checks only writes that were made, and
        // this one changed no handle, storage buffer descriptor or pool entry it would check.
        // That holds when it is skipped; the memory manager keeps it until the next FlushCaching
        // and invalidates it then if anything may have changed it (MemoryManager::
        // ReviewSkippedWrites). Only writes within one 4 KiB page (one device range) are skipped.
        bool skip = g_same_skip && light.same_writes != same_writes &&
                    (address >> 12) == ((address + copy_size - 1) >> 12);
        if (skip && !memory_manager.NoteSkippedWrite(address, light.same_device,
                                                     static_cast<u32>(copy_size))) {
            skip = false;
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::SameWritesMade, 1);
        }
        if (skip) {
            light.same_writes = same_writes;
            skip_write = true;
        } else {
            NoteLightWrite(address, static_cast<u32>(copy_size));
        }
    }
    if (skip_write) {
        NoteSameWriteSkipped();
    } else {
        memory_manager.WriteBlockCached(address, start_base, copy_size);
    }

    // Increment the current buffer position.
    regs.const_buffer.offset += static_cast<u32>(copy_size);
}

void Maxwell3D::ProcessCBMultiData(const u32* start_base, u32 amount) {
    if (stage_capture) [[unlikely]] return StageCbForward(start_base, amount);
    // Write the input value to the current const buffer at the current position.
    const GPUVAddr buffer_address = regs.const_buffer.Address();
    ASSERT(buffer_address != 0);

    // Don't allow writing past the end of the buffer.
    ASSERT(regs.const_buffer.offset <= regs.const_buffer.size);

    CbWriteImpl(buffer_address + regs.const_buffer.offset, start_base, amount);
}

void Maxwell3D::ProcessCBData(u32 value) {
    ProcessCBMultiData(&value, 1);
}

MAXWELL3D_FORCE_INLINE bool Maxwell3D::IsLightSameWrite(GPUVAddr address, const u32* data,
                                                         size_t size) {
    // NVN's draw macros write the base vertex and instance into c[0][0x640] and reset them after
    // the draw; FrontIndirectDraw sends the resets. Mostly the bytes are already there. Such a
    // write still goes to memory and its range is invalidated at the next FlushCaching, but a
    // draw after it sees what it would without the write, as long as no cache holds data for
    // the range that guest memory lacks (GPU-written buffer data or images), no query result
    // waits to be written there and nothing wrote its pages around the caches (semaphores,
    // queries, downloads, DMA: a cache may hold older bytes there, which the invalidation brings
    // up to date): then nothing uploads anything else over it. Those writes are
    // counted (light.same_writes); the rasterizer takes the next draw the light way only if every
    // cached write since the last one is such a write. EDEN_NX2_SAME_SKIP (ProcessCBMultiData)
    // leaves the counted ones unmade.
    const bool may_be_light = light.armed && (light.changes & ~LightChange::Light) == 0;
    if (!may_be_light && !VideoCommon::Nx2Profile::Enabled()) {
        return false;
    }
    const u8* const memory = memory_manager.GetSpan(address, size);
    if (memory == nullptr || std::memcmp(memory, data, size) != 0) {
        return false;
    }
    // A sparse region's first write gives it memory (Nx2BackSparseWrite).
    const bool sparse =
        address + size > Tegra::g_nx2_sparse_low.load(std::memory_order_relaxed) &&
        address < Tegra::g_nx2_sparse_high.load(std::memory_order_relaxed);
    if (may_be_light && !sparse) {
        const std::optional<DAddr> device_address = memory_manager.GpuToCpuAddress(address);
        bool untracked{};
        // Inlined into ProcessCBMultiData as when this function was not forced inline.
        MAXWELL3D_INLINE_CALLS untracked =
            device_address && Tegra::Nx2UntrackedWritten(*device_address, size);
        if (untracked) {
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::SameWritesUntracked, 1);
        } else if (device_address && !rasterizer->HoldsGpuWrittenData(*device_address, size)) {
            ++light.same_writes;
            light.same_device = *device_address;
        }
    }
    return true;
}

void Maxwell3D::ProcessInlineIndexMultiData(u32 method, const u32* start_base, u32 amount) {
    if (amount == 0) {
        return;
    }
    const u32 argument = ProcessShadowRam(method, start_base[amount - 1]);
    ProcessDirtyRegisters(method, argument);
    draw_manager.SetInlineIndexBuffer(*this, method, start_base, amount);
}

Texture::TICEntry Maxwell3D::GetTICEntry(u32 tic_index) const {
    const GPUVAddr tic_address_gpu{regs.tex_header.Address() +
                                   tic_index * sizeof(Texture::TICEntry)};
    Texture::TICEntry tic_entry;
    memory_manager.ReadBlockUnsafe(tic_address_gpu, &tic_entry, sizeof(Texture::TICEntry));
    return tic_entry;
}

Texture::TSCEntry Maxwell3D::GetTSCEntry(u32 tsc_index) const {
    const GPUVAddr tsc_address_gpu{regs.tex_sampler.Address() +
                                   tsc_index * sizeof(Texture::TSCEntry)};
    Texture::TSCEntry tsc_entry;
    memory_manager.ReadBlockUnsafe(tsc_address_gpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

u32 Maxwell3D::GetRegisterValue(u32 method) const {
    ASSERT(method < Regs::NUM_REGS && "Invalid Maxwell3D register");
    return regs.reg_array[method];
}

void Maxwell3D::SetHLEReplacementAttributeType(u32 bank, u32 offset,
                                               HLEReplacementAttributeType name) {
    const u64 key = (static_cast<u64>(bank) << 32) | offset;
    replace_table.emplace(key, name);
}

// Split GPU thread with macros on its front half (gpu_split.h, EDEN_NX2_SPLIT=2). The front keeps
// what macros read: the registers as the back thread will hold them when it reaches the same
// point of the method stream. That is every write the front sends, with the shadow RAM applied,
// plus the few changes the back makes on its own (constant buffer offset, shadow_scratch[0] on
// falcon[4], the macro bind pointer); the front loads and binds the macro code itself.
struct Maxwell3D::FrontState {
    std::array<ControlWrite, 16> control_writes{};
    u32 control_count{};
    std::array<u32, Regs::NUM_REGS> regs{};
    std::array<u32, Regs::NUM_REGS> shadow{};
    Regs::ShadowRamControl control{};
    u32 executing_macro{};
    std::vector<u32> parameters;
    u64 calls{};
    /// A method the draw manager handles was sent since the back last ran DrawDeferred: only
    /// then can it have a deferred instanced draw to issue at the end of a macro.
    bool draw_pending{true};
    bool skip_same{}; ///< SkipSameValueWrites() && FrontSkipSame(), read once
    /// FrontTakeIndirectRecord: the GPU address of the current call's record (0: none).
    GPUVAddr indirect_record{};
    /// Per macro start: how many calls made exactly NVN's indexed indirect draw writes
    /// (IndirectDrawWrites), or whether one did not. Cleared when the macro code changes.
    struct IndirectCheck {
        static constexpr u32 Needed = 16;
        u32 matches{};
        bool failed{};
    };
    std::unordered_map<u32, IndirectCheck> indirect_checks;
    u64 indirect_generation{~0ULL};
    std::vector<std::pair<u32, u32>> checked_writes; ///< The writes of a call being checked
    /// The last FrontIndirectDraw's entry while the back cannot see it yet (split ring
    /// position `start`, followed by the macro's resets up to `end`).
    struct HeldIndirectDraw {
        u64* payload{};
        u64 start{};
        u64 end{};
        u64 session{};
    } held_indirect;
    /// EDEN_NX2_GPU_PROFILE: the first entry written after held_indirect (kind << 16 | method,
    /// see Nx2Profile::Report; 0: none or not a 3D method).
    u32 indirect_interposer{};
    /// EDEN_NX2_GPU_PROFILE: methods noted since the last indirect draw (~0: none yet).
    u32 gap_methods{~0U};

    /// Profiling: remembers what was written first after the held indirect draw. Lean: from
    /// the write position and the macro writes held, without sending those (WritePosition does).
    void NoteInterposer(VideoCommon::GpuSplit& split, u32 kind, u32 method) {
        if (held_indirect.payload != nullptr && indirect_interposer == 0 &&
            (lean ? split.DiagHeldWrites() == 0 && split.DiagWritePosition() == held_indirect.end
                  : split.WritePosition() == held_indirect.end)) {
            indirect_interposer = (kind << 16) | (method & 0xffff);
        }
    }
    /// Profiling: the first 16 methods written (or macros called, kind 4) after each indirect
    /// draw, whether or not they change a register.
    void NoteGapMethod(u32 kind, u32 method) {
        if (gap_methods < 16) {
            ++gap_methods;
            VideoCommon::Nx2Profile::Note(VideoCommon::Nx2Profile::Histogram::IndirectGapMethods,
                                          (kind << 16) | (method & 0xffff));
        }
    }

    /// EDEN_NX2_PROFILE_LEAN=1 (with the profile, read once): FrontWrite's profile counts kept
    /// here and added at each macro's end (FlushLean), the macro zone's time left to push, the
    /// interposer read without side effects.
    bool lean{};
    VideoCommon::Nx2FrontSame::LeanCounts lean_counts;
    void FlushLean() {
        namespace Profile = VideoCommon::Nx2Profile;
        const auto [writes, same_writes] = lean_counts.Take();
        Profile::Add(Profile::Counter::MethodWrites, writes);
        Profile::Add(Profile::Counter::SameValueWrites, same_writes);
    }
    /// EDEN_NX2_FRONT_DIAG_REGS=0: front diag without its register table (which counts every
    /// write, so the same value filter stays armed).
    bool diag_regs{true};
    /// EDEN_NX2_JIT_SAME (nx2_front_same.h): the methods whose same value writes the filter
    /// drops, each macro call's filter, and with =1 / =2 the JIT compiled with it (the engine's
    /// own JIT is shared with the back and the compute front).
    VideoCommon::Nx2FrontSame::Table same_table{};
    VideoCommon::Nx2FrontSame::Filter same;
#ifdef ARCHITECTURE_x86_64
    std::unique_ptr<TuringMacroJit> front_jit;
#endif
    /// EDEN_NX2_FRONT_MACRO_LEAN=1: HleIndirectEnabled, FrontVerifyEnabled and
    /// IndirectBatchEnabled read once (no guarded statics per call), indirect_checks' decided
    /// entries found without a lookup (decided_checks, direct mapped by start), front_jit's emits
    /// reloading the FIFO only after MME_DMA_READ_FIFOED.
    bool macro_lean{};
    bool hle_indirect{};
    bool front_verify{};
    bool indirect_batch{};
    struct DecidedCheck {
        u32 key{}; ///< The macro's start + 1 (0: none; a start of ~0 is never kept)
        bool indirect{}; ///< NVN's indexed indirect draw (else not)
    };
    std::array<DecidedCheck, 256> decided_checks{};
    DecidedCheck& Decided(u32 start) {
        return decided_checks[start % decided_checks.size()];
    }
    /// Whether `slot` holds `start`'s verdict.
    static bool Holds(const DecidedCheck& slot, u32 start) {
        return slot.key != 0 && slot.key == start + 1;
    }
    /// Keeps `entry`'s verdict for `start` when it is decided.
    void NoteDecided(u32 start, const IndirectCheck& entry) {
        if (start != ~0U && (entry.failed || entry.matches >= IndirectCheck::Needed)) {
            Decided(start) = {start + 1, !entry.failed};
        }
    }
};

// What nx2_front_same.h (and the JIT's filter) assume of the registers.
static_assert(VideoCommon::Nx2FrontSame::NumRegs == Maxwell3D::Regs::NUM_REGS);
static_assert(VideoCommon::Nx2FrontSame::NumRegs == MacroRegistersStart);
static_assert(static_cast<u32>(Maxwell3D::Regs::ShadowRamControl::Track) ==
                  VideoCommon::Nx2FrontSame::Track &&
              static_cast<u32>(Maxwell3D::Regs::ShadowRamControl::TrackWithFilter) ==
                  VideoCommon::Nx2FrontSame::TrackWithFilter &&
              static_cast<u32>(Maxwell3D::Regs::ShadowRamControl::Passthrough) ==
                  VideoCommon::Nx2FrontSame::Passthrough &&
              static_cast<u32>(Maxwell3D::Regs::ShadowRamControl::Replay) ==
                  VideoCommon::Nx2FrontSame::Replay);

// What the back runs for FrontIndirectDraw: `count` records `stride` bytes apart.
struct Maxwell3D::IndirectDrawCall {
    Maxwell3D* engine;
    Core::System* system;
    GPUVAddr record;
    u32 topology;
    u32 count;
    u32 stride;

    void operator()() const {
        engine->BackDrawIndexedIndirect(*system, topology, record, count, stride);
    }
};

namespace {

// EDEN_NX2_SPLIT_VERIFY=1: every 4096 front macro calls, wait for the back and compare the
// front's registers with the back's (logs and resynchronizes differences).
bool FrontVerifyEnabled() {
    static const bool verify = std::getenv("EDEN_NX2_SPLIT_VERIFY") != nullptr;
    return verify;
}

// EDEN_NX2_HLE_INDIRECT=0: run NVN's indexed indirect draw macro as it is, with its record read
// back from the GPU (DmaPusher drains the back and downloads it).
bool HleIndirectEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_HLE_INDIRECT");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// EDEN_NX2_INDIRECT_BATCH=1: calls of NVN's indexed indirect draw macro whose records follow each
// other in memory share one host draw (and one draw setup on the back). Off by default: one host
// draw per call. Merged, Bananza's terrain drew garbage polygons and lost chunks after minutes
// of play, and the merge saved little on a Ryzen 7 5700X3D with an RTX 5070 Ti.
bool IndirectBatchEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_INDIRECT_BATCH");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// Topologies an indirect draw takes as they are (HLE_DrawIndexedIndirect's IsTopologySafe).
bool IndirectTopologySafe(u32 topology) {
    return topology <= 6 || (topology >= 0xa && topology <= 0xe);
}

// The writes NVN's indexed indirect draw macro makes for its parameters {topology, index count,
// instance count 1, first index, base vertex, base instance}: index_buffer.first, vertex_id_base,
// global_base_vertex_index and global_base_instance_index, the base vertex and instance into
// c[0][0x640] (CB_OFFSET and two CB_DATA), draw begin, index_buffer.count, draw end; then it
// resets the base registers and c[0][0x640].
[[maybe_unused]] std::array<std::pair<u32, u32>, 16> IndirectDrawWrites(std::span<const u32> p) {
    return {{{0x5f7, p[3]}, {0x446, p[4]}, {0x50d, p[4]}, {0x50e, p[5]}, {0x8e3, 0x640},
             {0x8e4, p[4]}, {0x8e5, p[5]}, {0x586, p[0]}, {0x5f8, p[1]}, {0x585, 0},
             {0x446, 0}, {0x50d, 0}, {0x50e, 0}, {0x8e3, 0x640}, {0x8e4, 0}, {0x8e5, 0}}};
}

// Executable methods ProcessMethodCall handles itself; the rest go to the draw manager.
bool FrontHandledOutsideDrawManager(u32 method) {
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    if (method >= cb_data && method < cb_data + 16) {
        return true;
    }
    switch (method) {
    case Maxwell3D::RootTables::SelectMethod: // executable with EDEN_NX2_ROOT_TABLES only
    case Maxwell3D::RootTables::LoadMethod:
    case MAXWELL3D_REG_INDEX(wait_for_idle):
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
    case MAXWELL3D_REG_INDEX(falcon[4]):
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
    case MAXWELL3D_REG_INDEX(render_enable.mode):
    case MAXWELL3D_REG_INDEX(clear_report_value):
    case MAXWELL3D_REG_INDEX(sync_info):
    case MAXWELL3D_REG_INDEX(launch_dma):
    case MAXWELL3D_REG_INDEX(inline_data):
    case MAXWELL3D_REG_INDEX(fragment_barrier):
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return true;
    default:
        return false;
    }
}

// EDEN_NX2_SPLIT_SKIP=0: send state writes that keep the register's value too.
bool FrontSkipSame() {
    static const bool skip = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT_SKIP");
        return value == nullptr || value[0] != '0';
    }();
    return skip;
}

// Front diag (EDEN_NX2_GPU_PROFILE, nx2_front_diag_hooks.h): the guest memory a method the front
// just sent writes on the back, from the front's registers, or the work it starts. `words`: the
// constant buffer words written; `executable`: in the execution mask; `instance_loop`: a method
// of an instance loop. Out of line: the front's path keeps a call.
YUZU_NO_INLINE void FrontDiagNote(const Tegra::MemoryManager& memory,
                                  const std::array<u32, Maxwell3D::Regs::NUM_REGS>& regs,
                                  u32 method, u32 words, bool executable, bool instance_loop) {
    namespace Diag = VideoCommon::Nx2FrontDiag;
    Diag::NoteDeferredDraw(instance_loop, executable, false);
    const bool draw_work = executable && !FrontHandledOutsideDrawManager(method);
    const auto address = [&regs](u32 high, u32 low) {
        return (GPUVAddr{regs[high]} << 32) | GPUVAddr{regs[low]};
    };
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    if (method >= cb_data && method < cb_data + 16) {
        // Written at the offset before the words (the front advanced it already).
        const GPUVAddr buffer = address(MAXWELL3D_REG_INDEX(const_buffer.address_high),
                                        MAXWELL3D_REG_INDEX(const_buffer.address_low));
        const u32 offset = regs[MAXWELL3D_REG_INDEX(const_buffer.offset)] - words * sizeof(u32);
        Diag::NoteWrite(memory, Diag::WriteKind::CbData, buffer + offset, u64{words} * sizeof(u32));
        return;
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(report_semaphore.query): {
        // Release and report only write (ProcessQueryGet): a short query's word, or 16 bytes.
        const u32 query = regs[MAXWELL3D_REG_INDEX(report_semaphore.query)];
        if ((query & 3) == 0 || (query & 3) == 2) {
            Diag::NoteWrite(memory, Diag::WriteKind::Report,
                            address(MAXWELL3D_REG_INDEX(report_semaphore.address_high),
                                    MAXWELL3D_REG_INDEX(report_semaphore.address_low)),
                            ((query >> 28) & 1) != 0 ? 4 : 16);
        }
        return;
    }
    case MAXWELL3D_REG_INDEX(sync_info):
        // The syncpoint shim's word of it changes once the back increments it.
        Diag::NoteWork(Diag::WorkKind::Syncpoint);
        return;
    case MAXWELL3D_REG_INDEX(inline_data): {
        constexpr u32 dest = MAXWELL3D_REG_INDEX(upload.dest.address_high);
        const u32 blocks = regs[MAXWELL3D_REG_INDEX(upload.dest.pitch) + 1];
        const auto [begin, end] = Diag::InlineRange(
            address(dest, dest + 1), regs[MAXWELL3D_REG_INDEX(upload.line_length_in)],
            regs[MAXWELL3D_REG_INDEX(upload.line_count)],
            regs[MAXWELL3D_REG_INDEX(upload.dest.pitch)],
            (regs[MAXWELL3D_REG_INDEX(launch_dma)] & 1) != 0,
            regs[MAXWELL3D_REG_INDEX(upload.dest.width)],
            regs[MAXWELL3D_REG_INDEX(upload.dest.height)],
            regs[MAXWELL3D_REG_INDEX(upload.dest.depth)], (blocks >> 4) & 0xf, (blocks >> 8) & 0xf);
        Diag::NoteWrite(memory, Diag::WriteKind::Inline, begin, end - begin);
        return;
    }
    default:
        break;
    }
    if (draw_work) {
        Diag::NoteWork(Diag::WorkKind::Draw);
        if (method == MAXWELL3D_REG_INDEX(draw.end)) {
            // The vertex program's address, as RESETFIX reads it: pipelines[1].reserved[0-1].
            using Pipeline = Maxwell3D::Regs::Pipeline;
            constexpr u32 program = MAXWELL3D_REG_INDEX(pipelines) +
                                    sizeof(Pipeline) / sizeof(u32) +
                                    offsetof(Pipeline, reserved) / sizeof(u32);
            Diag::NoteDrawEnd(memory, (GPUVAddr{regs[program] & 0xff} << 32) | regs[program + 1]);
        }
    }
}

} // Anonymous namespace

Maxwell3D::FrontState& Maxwell3D::Front(Core::System& system, VideoCommon::GpuSplit& split) {
    if (!front) [[unlikely]] {
        // Copy the back's state and take the macros over while it is idle (state writes it
        // still holds in its method sink first).
        VideoCommon::Nx2FrontDiag::CountDrain(split,
                                              VideoCommon::Nx2Profile::DrainSite::Front3DOther);
        const bool staged = stage_capture != nullptr;
        if (!staged) {
            ConsumeSink(system);
        }
        front = std::make_unique<FrontState>();
        std::memcpy(front->regs.data(), regs.reg_array.data(), sizeof(front->regs));
        std::memcpy(front->shadow.data(), shadow_state.reg_array.data(), sizeof(front->shadow));
        front->control = shadow_state.shadow_ram_control;
        if (staged) [[unlikely]] {
            // Staged (T2, R1 sahiplik-2): the sink's writes go to the capture and ring 2, and a
            // deferred instanced draw they issue to the proxy, all A's: A consumes it, before
            // anything sent after this. The copy holds them already.
            using Control = Regs::ShadowRamControl;
            VideoCommon::Nx2RegReplica::SinkInto(
                front->regs.data(), front->shadow.data(),
                front->control == Control::Track || front->control == Control::TrackWithFilter,
                front->control == Control::Replay, method_sink);
        }
        front->skip_same = SkipSameValueWrites() && FrontSkipSame();
        {
            // EDEN_NX2_PROFILE_LEAN, EDEN_NX2_FRONT_DIAG_REGS, EDEN_NX2_JIT_SAME.
            namespace Same = VideoCommon::Nx2FrontSame;
            const char* lean = std::getenv("EDEN_NX2_PROFILE_LEAN");
            front->lean = VideoCommon::Nx2Profile::Enabled() && lean != nullptr && lean[0] == '1';
            front->diag_regs = VideoCommon::Nx2FrontDiag::RegistersOn();
            front->same_table = Same::BuildTable(execution_mask);
#ifdef ARCHITECTURE_x86_64
            front->same.mode = Same::ParseMode(std::getenv("EDEN_NX2_JIT_SAME"));
            if (front->same.mode == 1 || front->same.mode == 2) {
                front->front_jit = std::make_unique<TuringMacroJit>(
                    front->same.mode == 1 ? TuringMacroJit::SameFilter::Skip
                                          : TuringMacroJit::SameFilter::Verify);
            }
#endif
            const char* macro_lean = std::getenv("EDEN_NX2_FRONT_MACRO_LEAN");
            front->macro_lean = macro_lean != nullptr && macro_lean[0] == '1';
            front->hle_indirect = HleIndirectEnabled();
            front->front_verify = FrontVerifyEnabled();
            front->indirect_batch = IndirectBatchEnabled();
            bool fifo_narrowed = false;
#ifdef ARCHITECTURE_x86_64
            if (front->macro_lean && front->front_jit) {
                front->front_jit->ReloadFifoOnlyAfter(Same::FifoAppend);
                fifo_narrowed = true;
            }
#endif
            if (front->lean || !front->diag_regs || front->same.mode != 0 || front->macro_lean) {
                LOG_INFO(HW_GPU,
                         "Split front: EDEN_NX2_JIT_SAME={} (1: the macro JIT drops state writes "
                         "the front would not send, 2: checks that against FrontWrite, 3: drops "
                         "them in C++), EDEN_NX2_PROFILE_LEAN={} (profile counts batched, macro "
                         "time within push, filter armed after indirect draws), "
                         "EDEN_NX2_FRONT_DIAG_REGS={}, EDEN_NX2_FRONT_MACRO_LEAN={} (switches "
                         "read once, decided indirect checks without a lookup; the front JIT's "
                         "FIFO reloaded only after MME_DMA_READ_FIFOED: {})",
                         front->same.mode, front->lean ? 1 : 0, front->diag_regs ? 1 : 0,
                         front->macro_lean ? 1 : 0,
                         fifo_narrowed ? "yes" : "no, EDEN_NX2_JIT_SAME=1/2 only");
            }
        }
        front->executing_macro = executing_macro;
        front->parameters = std::move(macro_params);
        macro_params.clear();
        executing_macro = 0;
        front_macros = true;
        if (staged) [[unlikely]] {
            // Last: A may run it at once, and the front has read the primary.
            split.DeferTo<VideoCommon::DeferTarget::OnA>(
                [this, system_ptr = &system] { ConsumeSink(*system_ptr); });
        }
    }
    return *front;
}

void Maxwell3D::FrontWrite(VideoCommon::GpuSplit& split, FrontState& fs, u32 method,
                           u32 argument, bool is_last_call, bool from_macro) {
    namespace Profile = VideoCommon::Nx2Profile;
    // As ProcessShadowRam does on the back.
    u32 value = argument;
    bool shadow_kept = true;
    if (fs.control == Regs::ShadowRamControl::Track ||
        fs.control == Regs::ShadowRamControl::TrackWithFilter) {
        shadow_kept = fs.shadow[method] == argument;
        fs.shadow[method] = argument;
    } else if (fs.control == Regs::ShadowRamControl::Replay) {
        value = fs.shadow[method];
    }
    if (Profile::Enabled()) [[unlikely]] {
        if (fs.lean) {
            if (fs.lean_counts.Write()) {
                fs.FlushLean();
            }
        } else {
            Profile::Add(Profile::Counter::MethodWrites, 1);
        }
        fs.NoteGapMethod(from_macro ? 3 : 1, method);
        if (VideoCommon::Nx2FrontDiag::On() && fs.diag_regs) [[unlikely]] {
            const bool same = fs.regs[method] == value;
            VideoCommon::Nx2FrontDiag::NoteRegisterWrite(
                method, same, !execution_mask[method] && same && shadow_kept && fs.skip_same,
                from_macro);
        }
    }
    if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && FrontVerifyEnabled()) [[unlikely]] {
        fs.control_writes[fs.control_count++ % fs.control_writes.size()] = {
            argument, static_cast<u32>(fs.control), from_macro ? 2U : 1U};
    }
    if (!execution_mask[method]) {
        // Nothing would change on the back: CallMethod and ConsumeSinkImpl skip same-value state
        // writes, and the shadow RAM (tracked by default) holds the value already.
        if (fs.regs[method] == value && shadow_kept && fs.skip_same) {
            if (Profile::Enabled()) [[unlikely]] {
                if (fs.lean) {
                    ++fs.lean_counts.same;
                } else {
                    Profile::Add(Profile::Counter::SameValueWrites, 1);
                }
            }
            return;
        }
        fs.regs[method] = value;
    } else {
        fs.regs[method] = value;
        constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
        switch (method) {
        case MAXWELL3D_REG_INDEX(shadow_ram_control):
            // The back's mode is shadow_state.shadow_ram_control: the shadow copy of this very
            // register, which a later Replay write reads back.
            fs.control = static_cast<Regs::ShadowRamControl>(argument);
            fs.shadow[method] = argument;
            if (fs.same.mode != 0) {
                // A macro may write it: its same value filter follows the mode.
                fs.same.Control(argument, fs.regs.data(), fs.shadow.data());
            }
            break;
        case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
            GetNativeMacroEngine().ClearCode(fs.regs[MAXWELL3D_REG_INDEX(load_mme.instruction_ptr)]);
            break;
        case MAXWELL3D_REG_INDEX(load_mme.instruction):
            GetNativeMacroEngine().AddCode(fs.regs[MAXWELL3D_REG_INDEX(load_mme.instruction_ptr)], value);
            break;
        case MAXWELL3D_REG_INDEX(load_mme.start_address): {
            const auto positions = GetNativeMacroPositions();
            u32& pointer = fs.regs[MAXWELL3D_REG_INDEX(load_mme.start_address_ptr)];
            if (pointer < positions.size()) {
                positions[pointer] = value;
            }
            ++pointer;
            break;
        }
        case MAXWELL3D_REG_INDEX(falcon[4]):
            fs.regs[MAXWELL3D_REG_INDEX(shadow_scratch)] = 1;
            break;
        default:
            if (method >= cb_data && method < cb_data + 16) {
                fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)] += sizeof(u32);
            } else if (!FrontHandledOutsideDrawManager(method)) {
                fs.draw_pending = true;
            }
            break;
        }
    }
    if (Profile::Enabled()) [[unlikely]] {
        fs.NoteInterposer(split, from_macro ? 3 : 1, method);
    }
    if (fs.same.mode == 2) [[unlikely]] {
        fs.same.sent = true;
    }
    if (from_macro) {
        split.MacroWrite(this, method, argument);
    } else {
        split.Method(this, method, argument, is_last_call, nullptr);
    }
    if (VideoCommon::Nx2FrontDiag::On()) [[unlikely]] {
        FrontDiagNote(memory_manager, fs.regs, method, 1, execution_mask[method],
                      DrawManager::IsInstanceLoopMethod(method));
    }
}

void Maxwell3D::FrontCallMethod(Core::System& system, VideoCommon::GpuSplit& split, u32 method,
                                u32 argument, bool is_last_call) {
    auto& fs = Front(system, split);
    if (method >= MacroRegistersStart) {
        if (fs.executing_macro == 0) {
            fs.executing_macro = method;
            if (VideoCommon::Nx2Profile::Enabled()) [[unlikely]] {
                fs.NoteGapMethod(4, method);
            }
        }
        fs.parameters.push_back(argument);
        if (is_last_call) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    FrontWrite(split, fs, method, argument, is_last_call, false);
}

void Maxwell3D::FrontCallMultiMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                     u32 method, const u32* base_start, u32 amount,
                                     u32 methods_pending) {
    auto& fs = Front(system, split);
    if (method >= MacroRegistersStart) {
        if (fs.executing_macro == 0) {
            fs.executing_macro = method;
            if (VideoCommon::Nx2Profile::Enabled()) [[unlikely]] {
                fs.NoteGapMethod(4, method);
            }
        }
        fs.parameters.insert(fs.parameters.end(), base_start, base_start + amount);
        if (amount == methods_pending) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    // What CallMultiMethod does to the registers on the back, then one entry for all the words.
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    const auto tracked = [&fs] {
        return fs.control == Regs::ShadowRamControl::Track ||
               fs.control == Regs::ShadowRamControl::TrackWithFilter;
    };
    if (method >= cb_data && method < cb_data + 16) {
        fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)] += amount * sizeof(u32);
    } else if (method == MAXWELL3D_REG_INDEX(inline_data)) {
        // Upload data only.
    } else if ((method == MAXWELL3D_REG_INDEX(draw_inline_index) ||
                method == MAXWELL3D_REG_INDEX(inline_index_2x16.even) ||
                method == MAXWELL3D_REG_INDEX(inline_index_4x8.index0)) &&
               fs.control != Regs::ShadowRamControl::Replay) {
        if (amount != 0) {
            const u32 last = base_start[amount - 1];
            if (tracked()) {
                fs.shadow[method] = last;
            }
            fs.regs[method] = last;
        }
        fs.draw_pending = true;
    } else {
        // CallMethod for each word, macro uploads among them (load_mme.instruction). A word may
        // change the shadow RAM mode for the next (a run of SET_SHADOW_RAM_CONTROL writes).
        for (u32 i = 0; i < amount; ++i) {
            const u32 argument = base_start[i];
            u32 value = argument;
            if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && FrontVerifyEnabled()) [[unlikely]] {
                fs.control_writes[fs.control_count++ % fs.control_writes.size()] = {
                    argument, static_cast<u32>(fs.control), 3U};
            }
            if (tracked()) {
                fs.shadow[method] = argument;
            } else if (fs.control == Regs::ShadowRamControl::Replay) {
                value = fs.shadow[method];
            }
            fs.regs[method] = value;
            if (!execution_mask[method]) {
                continue;
            }
            switch (method) {
            case MAXWELL3D_REG_INDEX(shadow_ram_control):
                fs.control = static_cast<Regs::ShadowRamControl>(argument);
                fs.shadow[method] = argument; // The same word on the back (see FrontWrite)
                break;
            case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
                GetNativeMacroEngine().ClearCode(value);
                break;
            case MAXWELL3D_REG_INDEX(load_mme.instruction):
                GetNativeMacroEngine().AddCode(fs.regs[MAXWELL3D_REG_INDEX(load_mme.instruction_ptr)], value);
                break;
            case MAXWELL3D_REG_INDEX(load_mme.start_address): {
                const auto positions = GetNativeMacroPositions();
                u32& pointer = fs.regs[MAXWELL3D_REG_INDEX(load_mme.start_address_ptr)];
                if (pointer < positions.size()) {
                    positions[pointer] = value;
                }
                ++pointer;
                break;
            }
            case MAXWELL3D_REG_INDEX(falcon[4]):
                fs.regs[MAXWELL3D_REG_INDEX(shadow_scratch)] = 1;
                break;
            default:
                if (!FrontHandledOutsideDrawManager(method)) {
                    fs.draw_pending = true;
                }
                break;
            }
        }
    }
    if (VideoCommon::Nx2Profile::Enabled()) [[unlikely]] {
        fs.NoteInterposer(split, 2, method);
        fs.NoteGapMethod(2, method);
    }
    split.MultiMethod(this, method, base_start, amount, methods_pending, nullptr);
    if (VideoCommon::Nx2FrontDiag::On() && amount != 0) [[unlikely]] {
        FrontDiagNote(memory_manager, fs.regs, method, amount, execution_mask[method],
                      DrawManager::IsInstanceLoopMethod(method));
    }
}

void Maxwell3D::FrontRunMacro(Core::System& system, VideoCommon::GpuSplit& split,
                              FrontState& fs) {
#ifdef ARCHITECTURE_x86_64
    const u32 method = fs.executing_macro;
    fs.executing_macro = 0;
    auto& engine = GetNativeMacroEngine();
    const auto positions = GetNativeMacroPositions();
    const u32 start = positions[((method - MacroRegistersStart) >> 1) % positions.size()];
    namespace Diag = VideoCommon::Nx2FrontDiag;
    const bool diag = Diag::On();
    if (fs.indirect_record != 0) {
        const GPUVAddr record = std::exchange(fs.indirect_record, GPUVAddr{0});
        if (fs.parameters.size() == 6) {
            const Diag::Timer diag_timer = diag ? Diag::MacroBegin(start) : Diag::Timer{};
            FrontIndirectDraw(system, split, fs, record);
            if (diag) [[unlikely]] {
                Diag::MacroEnd(diag_timer, start, true, 0, 0);
            }
            fs.parameters.clear();
            if (fs.lean) [[unlikely]] {
                fs.FlushLean();
            }
            return;
        }
        LOG_ERROR(HW_GPU, "Indirect draw macro {:#x} got {} parameters, not 6", method,
                  fs.parameters.size());
    }
    // Whether the macro is NVN's indexed indirect draw (FrontTakeIndirectRecord): compare its
    // writes on its first calls with one instance.
    if (fs.indirect_generation != engine.native_code_generation) {
        fs.indirect_checks.clear();
        if (fs.macro_lean) {
            fs.decided_checks.fill({});
        }
        fs.indirect_generation = engine.native_code_generation;
    }
    FrontState::IndirectCheck* check = nullptr;
    if ((fs.macro_lean ? fs.hle_indirect : HleIndirectEnabled()) && fs.parameters.size() == 6 &&
        fs.parameters[2] == 1) {
        if (!fs.macro_lean || !FrontState::Holds(fs.Decided(start), start)) {
            auto& entry = fs.indirect_checks[start];
            if (!entry.failed && entry.matches < FrontState::IndirectCheck::Needed) {
                check = &entry;
                fs.checked_writes.clear();
            } else if (fs.macro_lean) {
                // Decided for this code generation: no lookup from now on.
                fs.NoteDecided(start, entry);
            }
        }
        if (diag) [[unlikely]] {
            Diag::NoteIndirectLookup(check == nullptr);
        }
    }
    {
        namespace Same = VideoCommon::Nx2FrontSame;
        // Lean: the macro's time stays in the zone around it (push), the front's busy time too.
        VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Macro,
                                                     !fs.lean && VideoCommon::Nx2Profile::Enabled()};
        engine.native_interpreter.active_dram = &native_data_ram;
        // EDEN_NX2_JIT_SAME=1/2: the front's JIT with the filter (same code and generation).
        TuringMacroJit* jit = fs.front_jit.get();
        if (jit == nullptr) {
            if (!engine.native_jit) {
                engine.native_jit = std::make_shared<TuringMacroJit>();
            }
            jit = engine.native_jit.get();
        }
        jit->Prepare(engine.native_code, engine.native_code_generation);
        // The macro reads its parameters in place; MME_DMA_READ_FIFOED appends to them
        // (FrontDmaRead), the JIT reloading them after such a write.
        const size_t parameter_count = fs.parameters.size();
        if (fs.same.mode != 0) [[unlikely]] {
            // Lean: the gap histogram notes the methods sent, the filter is not disarmed for it.
            fs.same.Begin(fs.same_table.data(), fs.skip_same, static_cast<u32>(fs.control),
                          check != nullptr, diag && fs.diag_regs,
                          VideoCommon::Nx2Profile::Enabled() && !fs.lean, fs.gap_methods,
                          fs.regs.data(), fs.shadow.data());
        }
        const Diag::Timer diag_timer = diag ? Diag::MacroBegin(start) : Diag::Timer{};
        u32 diag_emits = 0;
        u32 diag_reads = 0;
        const auto read = [&fs, diag, &diag_reads](u32 address) {
            if (diag) [[unlikely]] {
                ++diag_reads;
            }
            return address < Regs::NUM_REGS ? fs.regs[address] : 0U;
        };
        const auto write = [&, diag](u32 address, u32 value) {
            if (address >= MacroRegistersStart) {
                return false;
            }
            if (check != nullptr && fs.checked_writes.size() < 32) [[unlikely]] {
                fs.checked_writes.emplace_back(address, value);
            }
            if (diag) [[unlikely]] {
                ++diag_emits;
                Diag::TimedMacroWrite([&] { FrontWrite(split, fs, address, value, true, true); });
            } else {
                FrontWrite(split, fs, address, value, true, true);
            }
            // MME_DMA_READ (into data RAM) and MME_DMA_READ_FIFOED (into the load FIFO).
            if (address == 0x157 || address == 0x158) {
                FrontDmaRead(split, fs, address, value);
            }
            return true;
        };
        const std::span<u32> data_ram{native_data_ram};
        const auto result = [&] {
            if (fs.same.mode == 2) [[unlikely]] {
                // The JIT's choice against FrontWrite's: a write it would drop comes through
                // verify, the rest through checked; all are sent as without the filter.
                const auto checked = [&](u32 address, u32 value) {
                    const bool in_scope = Same::InScope(fs.same.ctx, address);
                    fs.same.sent = false;
                    const bool ok = write(address, value);
                    if (in_scope && !fs.same.sent) {
                        ++fs.same.missed;
                    }
                    return ok;
                };
                const auto verify = [&](u32 address, u32 value) {
                    fs.same.sent = false;
                    const bool ok = write(address, value);
                    if (fs.same.sent && fs.same.NoteMismatch()) {
                        LOG_ERROR(HW_GPU,
                                  "Front same filter (EDEN_NX2_JIT_SAME=2): the JIT would drop "
                                  "macro {:#x}'s write {:#x} = {:#x} (shadow RAM mode {}), which "
                                  "the front sends ({} so far)",
                                  start, address, value, static_cast<u32>(fs.control),
                                  fs.same.mismatches);
                    }
                    return ok;
                };
                return jit->Execute(start, fs.parameters, data_ram, read, checked,
                                    &fs.same.ctx, verify);
            }
            if (fs.same.mode == 3) [[unlikely]] {
                // The filter in C++, on the JIT without it.
                const auto filtered = [&](u32 address, u32 value) {
                    if (Same::Hit(fs.same.ctx, address, value)) {
                        ++fs.same.ctx.hits;
                        return true;
                    }
                    return write(address, value);
                };
                return jit->Execute(start, fs.parameters, data_ram, read, filtered);
            }
            return jit->Execute(start, fs.parameters, data_ram, read, write,
                                fs.same.mode == 1 ? &fs.same.ctx : nullptr);
        }();
        if (fs.same.mode != 0) [[unlikely]] {
            // The writes dropped, as FrontWrite would have counted them.
            namespace Profile = VideoCommon::Nx2Profile;
            const u64 dropped = fs.same.Dropped();
            if (Profile::Enabled()) {
                if (fs.lean) {
                    fs.lean_counts.writes += dropped;
                    fs.lean_counts.same += dropped;
                } else if (dropped != 0) {
                    Profile::Add(Profile::Counter::MethodWrites, dropped);
                    Profile::Add(Profile::Counter::SameValueWrites, dropped);
                }
                Diag::NoteSameCall(fs.same.mode, fs.same.why, fs.same.ctx.hits,
                                   fs.same.CallMismatches(), fs.same.CallMissed());
            }
            diag_emits += static_cast<u32>(dropped);
        }
        if (diag) [[unlikely]] {
            Diag::MacroEnd(diag_timer, start, false, diag_emits, diag_reads);
        }
        if (!result.success) {
            engine.native_execution_failed = true;
            LOG_ERROR(HW_GPU, "Native MME (JIT, split front) failed: start={:#x}, consumed={}/{}, error={}",
                      start, result.consumed, parameter_count, result.error);
        }
        if (check != nullptr) {
            const auto expected = IndirectDrawWrites(fs.parameters);
            if (result.success && std::ranges::equal(fs.checked_writes, expected)) {
                if (++check->matches == FrontState::IndirectCheck::Needed) {
                    LOG_INFO(HW_GPU,
                             "Macro at {:#x} is NVN's indexed indirect draw: the GPU reads the "
                             "records it gets from GPU memory (EDEN_NX2_HLE_INDIRECT=0 turns "
                             "this off)",
                             start);
                }
            } else {
                check->failed = true;
                LOG_INFO(HW_GPU,
                         "Macro at {:#x} takes 6 parameters but is not NVN's indexed indirect "
                         "draw ({} writes)",
                         start, fs.checked_writes.size());
            }
        }
    }
    fs.parameters.clear();
    if (fs.draw_pending) {
        fs.draw_pending = false;
        split.DeferTo<VideoCommon::DeferTarget::OnA>(
            [this, system_ptr = &system] { FinishFrontMacro(*system_ptr); });
        if (diag) [[unlikely]] {
            Diag::NoteDeferredDraw(false, false, true);
        }
    }
    if ((fs.macro_lean ? fs.front_verify : FrontVerifyEnabled()) && (++fs.calls & 4095) == 0)
        [[unlikely]] {
        FrontVerify(split, fs);
    }
    if (fs.lean) [[unlikely]] {
        fs.FlushLean();
    }
#else
    fs.executing_macro = 0;
    fs.parameters.clear();
#endif
}

void Maxwell3D::FrontDmaRead(VideoCommon::GpuSplit& split, FrontState& fs, u32 method,
                             u32 count) {
    namespace Diag = VideoCommon::Nx2FrontDiag;
    const GPUVAddr source = (GPUVAddr{fs.regs[0x154] & 0x1ffffff} << 32) | fs.regs[0x155];
    const u32 ram = fs.regs[0x156];
    const u32 words = std::min<u32>(count, 0x1000);
    const size_t read_bytes = size_t{words} * sizeof(u32);
    // Front diag: what was pending of the words, and the words, before the drain.
    const bool diag = Diag::On();
    const Diag::ReadProbe probe =
        diag ? Diag::DmaReadBefore(memory_manager, source, words) : Diag::ReadProbe{};
    // The words may be written by methods still queued for the back (indirect draw arguments
    // from a culling dispatch), and the caches that may hold newer ones belong to it.
    const bool waited = Diag::CountDrain(split, VideoCommon::Nx2Profile::DrainSite::FrontDmaRead);
    if (VideoCommon::Nx2Profile::Enabled()) {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::DmaReadBytes, read_bytes);
    }
    const bool gpu_dirty =
        memory_manager.IsMemoryDirty(source, read_bytes, VideoCommon::CacheType::BufferCache);
    if (gpu_dirty) {
        memory_manager.FlushRegion(source, read_bytes, VideoCommon::CacheType::BufferCache);
    }
    for (u32 i = 0; i < words; ++i) {
        const u32 word = memory_manager.Read<u32>(source + u64{i} * sizeof(u32));
        if (method == 0x158) {
            fs.parameters.push_back(word);
        } else if (ram + i < native_data_ram.size()) {
            native_data_ram[ram + i] = word;
        }
    }
    if (diag) [[unlikely]] {
        Diag::DmaReadAfter(probe, memory_manager, source, words, method == 0x158, waited,
                           gpu_dirty);
    }
}

void Maxwell3D::FrontVerify(VideoCommon::GpuSplit& split, FrontState& fs) {
    VideoCommon::Nx2FrontDiag::CountDrain(split, VideoCommon::Nx2Profile::DrainSite::Front3DOther);
    static u64 checks{};
    static u64 differences{};
    ++checks;
    // State writes the back still holds in its method sink count as done.
    std::array<u32, Regs::NUM_REGS> expected;
    std::memcpy(expected.data(), regs.reg_array.data(), sizeof(expected));
    for (const auto& [method, value] : method_sink) {
        expected[method] = shadow_state.shadow_ram_control == Regs::ShadowRamControl::Replay
                               ? shadow_state.reg_array[method]
                               : value;
    }
    static u64 control_differences{};
    if ((fs.control != shadow_state.shadow_ram_control ||
         fs.shadow[MAXWELL3D_REG_INDEX(shadow_ram_control)] != shadow_state.reg_array[MAXWELL3D_REG_INDEX(shadow_ram_control)] ||
         fs.regs[MAXWELL3D_REG_INDEX(shadow_ram_control)] != expected[MAXWELL3D_REG_INDEX(shadow_ram_control)]) &&
        control_differences < 4) {
        LOG_WARNING(HW_GPU, "Split shadow RAM control writes, front ({} total) / back ({} total):",
                    fs.control_count, back_control_count);
        for (u32 i = 0; i < 16; ++i) {
            const auto& f = fs.control_writes[(fs.control_count + i) % 16];
            const auto& b = back_control_writes[(back_control_count + i) % 16];
            LOG_WARNING(HW_GPU, "  front {:#x} (mode {}, path {}) | back {:#x} (mode {}, last {})",
                        f.argument, f.before, f.path, b.argument, b.before, b.path);
        }
    }
    for (u32 i = 0; i < Regs::NUM_REGS; ++i) {
        if (fs.regs[i] != expected[i]) {
            if (differences++ < 64) {
                LOG_WARNING(HW_GPU, "Split front register {:#x}: front {:#x}, back {:#x}", i,
                            fs.regs[i], expected[i]);
            }
            fs.regs[i] = expected[i];
        }
    }
    if (fs.control != shadow_state.shadow_ram_control) {
        if (control_differences++ < 16) {
            LOG_WARNING(HW_GPU, "Split front shadow RAM control {} vs back {}",
                        static_cast<u32>(fs.control),
                        static_cast<u32>(shadow_state.shadow_ram_control));
        }
        fs.control = shadow_state.shadow_ram_control;
    }
    for (u32 i = 0; i < Regs::NUM_REGS; ++i) {
        if (fs.shadow[i] != shadow_state.reg_array[i]) {
            if (control_differences++ < 16) {
                LOG_WARNING(HW_GPU, "Split front shadow register {:#x}: front {:#x}, back {:#x}", i,
                            fs.shadow[i], shadow_state.reg_array[i]);
            }
            fs.shadow[i] = shadow_state.reg_array[i];
        }
    }
    if ((checks & 63) == 1) {
        LOG_INFO(HW_GPU, "Split front register checks: {} done, {} differences", checks, differences);
    }
}

bool Maxwell3D::FrontTakeIndirectRecord(Core::System& system, VideoCommon::GpuSplit& split,
                                        u32 method, GPUVAddr record) {
    // Donkey Kong Bananza draws its terrain with NVN's indexed indirect draw macro: a culling
    // dispatch fills in a 5-word record per piece and the macro's parameters continue with the
    // record itself (a GP entry pointing at it). Reading the record on the CPU meant draining
    // the split's back and downloading it from the host GPU (~20,000 times a second, the main
    // cost in its open areas). The host GPU reads it instead, as HLE_DrawIndexedIndirect does
    // for the first console's macro.
    if (!HleIndirectEnabled() || !split.FrontMacros() || !GetNativeMacroEngine().native_turing) {
        return false;
    }
    auto& fs = Front(system, split);
    if (fs.executing_macro == 0 || (method & ~1U) != fs.executing_macro ||
        fs.parameters.size() != 1 || !IndirectTopologySafe(fs.parameters[0]) ||
        fs.control == Regs::ShadowRamControl::Replay ||
        fs.indirect_generation != GetNativeMacroEngine().native_code_generation) {
        return false;
    }
    const auto positions = GetNativeMacroPositions();
    const u32 start =
        positions[((fs.executing_macro - MacroRegistersStart) >> 1) % positions.size()];
    if (fs.macro_lean) {
        if (const auto& decided = fs.Decided(start); FrontState::Holds(decided, start)) {
            if (!decided.indirect) {
                return false;
            }
            fs.indirect_record = record;
            return true;
        }
    }
    const auto it = fs.indirect_checks.find(start);
    if (fs.macro_lean && it != fs.indirect_checks.end()) {
        // The decided indirect draw's calls are taken here, not in FrontRunMacro's check.
        fs.NoteDecided(start, it->second);
    }
    if (it == fs.indirect_checks.end() || it->second.failed ||
        it->second.matches < FrontState::IndirectCheck::Needed) {
        return false;
    }
    fs.indirect_record = record;
    return true;
}

void Maxwell3D::FrontIndirectDraw(Core::System& system, VideoCommon::GpuSplit& split,
                                  FrontState& fs, GPUVAddr record) {
    namespace Profile = VideoCommon::Nx2Profile;
    Profile::Add(Profile::Counter::HleIndirectDraws, 1);
    const u32 topology = fs.parameters[0];
    fs.draw_pending = false;
    fs.gap_methods = ~0U;
    // NVN's multi-draw calls the macro once per record, with nothing in between. While the last
    // call's entry is still invisible to the back and nothing followed it but its resets, a
    // call whose record comes next in memory joins it: one draw setup on the back and one
    // vkCmdDrawIndexedIndirect for all. The resets need not be repeated: they set the same
    // registers and c[0][0x640..0x647] to the same values, and the draws in between read their
    // base vertex and instance from the record instead (BackDrawIndexedIndirect).
    auto& held = fs.held_indirect;
    const bool batch = fs.macro_lean ? fs.indirect_batch : IndirectBatchEnabled();
    if (held.payload != nullptr && batch && held.session == split.Session() &&
        held.end == split.WritePosition() && !split.Published(held.start)) {
        IndirectDrawCall call;
        std::memcpy(&call, held.payload, sizeof(call));
        constexpr u32 RecordSize = 5 * sizeof(u32);
        constexpr u32 MaxStride = 256;
        constexpr u32 MaxCount = 256;
        const u64 distance = record - call.record;
        const bool follows =
            record > call.record &&
            (call.count == 1 ? distance >= RecordSize && distance <= MaxStride && distance % 4 == 0
                             : distance == u64{call.count} * call.stride);
        if (call.topology == topology && call.count < MaxCount && follows) {
            if (call.count == 1) {
                call.stride = static_cast<u32>(distance);
            }
            ++call.count;
            std::memcpy(held.payload, &call, sizeof(call));
            Profile::Add(Profile::Counter::HleIndirectMerged, 1);
            return;
        }
        if (Profile::Enabled()) [[unlikely]] {
            const GPUVAddr last = call.record + u64{call.count - 1} * call.stride;
            const s64 step = static_cast<s64>(record - last);
            if (call.topology != topology) {
                Profile::Add(Profile::Counter::IndirectTopology, 1);
            } else if (call.count >= MaxCount) {
                Profile::Add(Profile::Counter::IndirectGroupFull, 1);
            } else if (record <= call.record) {
                Profile::Add(Profile::Counter::IndirectNotAfter, 1);
                Profile::Note(Profile::Histogram::IndirectStep, step);
            } else if (call.count == 1) {
                Profile::Add(Profile::Counter::IndirectStepRange, 1);
                Profile::Note(Profile::Histogram::IndirectStep, step);
            } else {
                Profile::Add(Profile::Counter::IndirectStepMismatch, 1);
                Profile::Note(Profile::Histogram::IndirectStep, step);
                Profile::Note(Profile::Histogram::IndirectGroupStride, call.stride);
            }
        }
    } else if (Profile::Enabled() && batch) [[unlikely]] {
        if (held.payload == nullptr) {
            Profile::Add(Profile::Counter::IndirectNoPrevious, 1);
        } else if (held.session != split.Session()) {
            Profile::Add(Profile::Counter::IndirectRestarted, 1);
        } else if (held.end != split.WritePosition()) {
            Profile::Add(Profile::Counter::IndirectInterposed, 1);
            Profile::Note(Profile::Histogram::IndirectInterposer, fs.indirect_interposer);
        } else {
            Profile::Add(Profile::Counter::IndirectPublished, 1);
        }
    }
    held = {};
    u64 start{};
    u64* const payload = split.DeferEditable(
        IndirectDrawCall{this, &system, record, topology, 1, 0}, start);
    // The state the macro leaves behind (the end of IndirectDrawWrites). index_buffer.first and
    // index_buffer.count keep their values: the macro would leave the record's in them.
    static constexpr std::array<std::pair<u32, u32>, 6> resets{
        {{0x446, 0}, {0x50d, 0}, {0x50e, 0}, {0x8e3, 0x640}, {0x8e4, 0}, {0x8e5, 0}}};
    for (const auto& [address, value] : resets) {
        FrontWrite(split, fs, address, value, true, true);
    }
    held = {payload, start, split.WritePosition(), split.Session()};
    fs.indirect_interposer = 0;
    fs.gap_methods = 0;
    if (VideoCommon::Nx2FrontDiag::On()) [[unlikely]] {
        VideoCommon::Nx2FrontDiag::NoteWork(VideoCommon::Nx2FrontDiag::WorkKind::Draw);
    }
}

void Maxwell3D::BackDrawIndexedIndirect(Core::System& system, u32 topology, GPUVAddr record,
                                        u32 count, u32 stride) {
    // What CallMacroMethod does around a macro.
    ConsumeSink(system);
    draw_manager.DrawDeferred(*this);
    // As HLE_DrawIndexedIndirect does: the GPU takes the index count, instance count, first
    // index, base vertex and base instance from the record. The shaders read the base vertex
    // and instance from c[0][0x640] and c[0][0x644], which the macro fills in from the record:
    // they read the draw's own instead.
    const u32 estimate = static_cast<u32>(EstimateIndexBufferSize());
    dirty.flags[VideoCommon::Dirty::IndexBuffer] = true;
    engine_state = EngineHint::OnHLEMacro;
    SetHLEReplacementAttributeType(0, 0x640, HLEReplacementAttributeType::BaseVertex);
    SetHLEReplacementAttributeType(0, 0x644, HLEReplacementAttributeType::BaseInstance);
    auto& params = draw_manager.indirect_state;
    params.is_byte_count = false;
    params.is_indexed = true;
    params.include_count = false;
    params.count_start_address = 0;
    params.indirect_start_address = record;
    params.buffer_size = size_t{count - 1} * stride + 5 * sizeof(u32);
    params.max_draw_counts = count;
    params.stride = count > 1 ? stride : 0;
    draw_manager.DrawIndexedIndirect(*this, static_cast<Regs::PrimitiveTopology>(topology), 0,
                                     estimate);
    VideoCommon::Nx2IndirectDiag::Note(*this, topology, record, count);
    engine_state = EngineHint::None;
    replace_table.clear();
}

void Maxwell3D::FinishFrontMacro(Core::System& system) {
    // What ProcessMacro and CallMacroMethod do around the macro.
    ConsumeSink(system);
    draw_manager.DrawDeferred(*this);
}

// Back stages (nx2_stage_api.h; T2) ------------------------------------------------------------

static_assert(VideoCommon::Nx2RegReplica::NumRegs == Maxwell3D::Regs::NUM_REGS);
static_assert(VideoCommon::Nx2RegReplica::CbDataMethod == MAXWELL3D_REG_INDEX(const_buffer.buffer));
static_assert(VideoCommon::Nx2RegReplica::CbDataCount == Maxwell3D::Regs::NumCBData);
static_assert(std::is_same_v<VideoCommon::Nx2RegReplica::DirtyFlags, Maxwell3D::DirtyState::Flags>);
static_assert(std::is_same_v<VideoCommon::Nx2RegReplica::DirtyTables, Maxwell3D::DirtyState::Tables>);

MAXWELL3D_NOINLINE void Maxwell3D::StageNoteWrite(u32 method, u32 argument) {
    // ProcessDirtyRegisters' write: its dirty flags always, a light change as it notes one.
    stage_capture->Note(*this, method, argument, true,
                        regs.reg_array[method] != argument || execution_mask[method]);
}

MAXWELL3D_NOINLINE void Maxwell3D::StageNoteDirect(u32 method, bool note_primary_light) {
    // A write ProcessDirtyRegisters does not make (the register holds it already), or a light
    // change noted outside it (the register's light class standing for it): the replica notes
    // the class. The primary's light changes are A's shadow of the replica's (=2, K2): where the
    // old code noted none, note it here too.
    stage_capture->Note(*this, method, regs.reg_array[method], false, true);
    if (note_primary_light) {
        NoteLightChange(g_light_classes[method], method);
    }
}

MAXWELL3D_NOINLINE void Maxwell3D::StageCbForward(const u32* start_base, u32 amount) {
    // ProcessCBMultiData while staged: the replica makes the write (StageCbWrite) with its own
    // light state and moves its offset on as A does here, before the record (=2 compares them
    // after it). No delta word for the offset: a CbData record may then join the one before
    // (Nx2Stages::PostCbData) when A wrote no register in between.
    const GPUVAddr buffer_address = regs.const_buffer.Address();
    ASSERT(buffer_address != 0);
    ASSERT(regs.const_buffer.offset <= regs.const_buffer.size);
    const GPUVAddr address{buffer_address + regs.const_buffer.offset};
    regs.const_buffer.offset += amount * static_cast<u32>(sizeof(u32));
    VideoCommon::Nx2Stages::PostCbData(VideoCommon::Nx2Stages::Ring::ToB, *this, address,
                                       start_base, amount);
}

MAXWELL3D_NOINLINE void Maxwell3D::StageQueryCondition() {
    // The condition reads guest memory, and the accelerated way flushes the caches and asks the
    // query cache (S1): all of it with the stages parked, at this point of the stream.
    VideoCommon::Nx2Stages::JoinFromA(VideoCommon::Nx2Stages::JoinReason::Condition,
                                      [this] { ProcessQueryCondition(); });
}

void Maxwell3D::StageCbWrite(GPUVAddr address, const u32* start_base, u32 amount) {
    // ProcessCBMultiData's write as the primary would have made it here (at the address it had).
    CbWriteImpl(address, start_base, amount);
}

void Maxwell3D::StageApplyDelta(const u64* words, u32 count) {
    const bool light_tracking = LightTrackingEnabled();
    VideoCommon::Nx2RegReplica::ApplyDelta(regs.reg_array.data(), dirty.flags, dirty.tables, words,
                                           count, [this, light_tracking](u32 method) {
                                               if (light_tracking) {
                                                   NoteLightChange(g_light_classes[method],
                                                                   method);
                                               }
                                           });
}

void Maxwell3D::StageCopyFrom(Maxwell3D& primary, VideoCore::RasterizerInterface* rasterizer_) {
    regs = primary.regs;
    shadow_state = primary.shadow_state;
    dirty.flags = primary.dirty.flags;
    dirty.tables = primary.dirty.tables;
    state = primary.state;
    draw_manager.draw_state = primary.draw_manager.draw_state;
    draw_manager.draw_texture_state = primary.draw_manager.draw_texture_state;
    draw_manager.indirect_state = primary.draw_manager.indirect_state;
    light = primary.light;
    // No light draw follows one made with another engine (the light anchor compares pointers).
    light.armed = false;
    // =2: what the replica holds before the first draw (the primary's light changes start over).
    stage_light_base = light.changes;
    stage_side_applied = false;
    engine_state = primary.engine_state;
    replace_table = primary.replace_table;
    execute_on = primary.execute_on;
    // The caches read native_turing (GetNativeMacroEngine); no macro runs here, no JIT is made.
    native_macro_context = primary.native_macro_context;
    macro_engine.native_turing = primary.macro_engine.native_turing;
    BindRasterizer(rasterizer_);
}

void Maxwell3D::StageBegin(VideoCommon::Nx2RegReplica::RegCapture* capture,
                           std::array<Maxwell3D*, 2> replicas) {
    auto sent = std::make_unique<StageSent>();
    std::memcpy(&sent->const_buffers, &state.shader_stages, sizeof(sent->const_buffers));
    sent->engine_state = engine_state;
    sent->replace_table = replace_table;
    sent->execute_on = execute_on;
    sent->draw_state = draw_manager.draw_state;
    sent->indirect_state = draw_manager.indirect_state;
    sent->draw_texture_state = draw_manager.draw_texture_state;
    sent->verify = VideoCommon::Nx2Stages::Get().mode == 2;
    stage_sent = std::move(sent);
    // The capture leaves the constant buffer data registers out (but at =2): no dirty flag may
    // hang on them but NullEntry, as dirty_flags.cpp and the StateTracker leave them.
    for (u32 i = 0; i < Regs::NumCBData; ++i) {
        const u32 method = MAXWELL3D_REG_INDEX(const_buffer.buffer) + i;
        if (dirty.tables[0][method] != VideoCommon::Dirty::NullEntry ||
            dirty.tables[1][method] != VideoCommon::Dirty::NullEntry) {
            LOG_WARNING(HW_GPU, "Back stages: constant buffer data register {:#x} sets dirty "
                                "flags the replicas will not get (only =2 sends them)",
                        method);
        }
    }
    // From here on the primary's dirty flags, light.rebound and light changes gather what A
    // sets until the next side state (the replicas hold what came before).
    dirty.flags.reset();
    light.rebound.fill(0);
    light.changes = 0;
    light.first_blocking = 0;
    light.first_blocking_uniform = 0;
    light.armed = false;
    stage_replicas = replicas;
    stage_capture = capture;
}

void Maxwell3D::StageEnd() {
    stage_capture = nullptr;
    stage_replicas = {};
    stage_sent.reset();
    // The caches read the primary again: everything set up anew, no light draw (S18).
    dirty.flags.set();
    light.armed = false;
}

u32 Maxwell3D::StageSideWords() {
    using namespace VideoCommon::Nx2RegReplica;
    auto& sent = *stage_sent;
    // Called for every draw record (T5): A's light changes, its shadow of the replica's, count
    // from here on; the draw clears the replica's (K2). The words pending now go with the draw.
    light.changes = 0;
    light.first_blocking = 0;
    light.first_blocking_uniform = 0;
    u32 parts = 0;
    u32 words = 0;
    sent.cb_count = 0;
    if (std::memcmp(&sent.const_buffers, &state.shader_stages, sizeof(sent.const_buffers)) != 0) {
        for (size_t stage = 0; stage < Regs::MaxShaderStage; ++stage) {
            for (size_t slot = 0; slot < Regs::MaxConstBuffers; ++slot) {
                if (!SameConstBuffer(state.shader_stages[stage].const_buffers[slot],
                                     sent.const_buffers[stage].const_buffers[slot])) {
                    sent.cb_slots[sent.cb_count++] =
                        static_cast<u8>(stage * Regs::MaxConstBuffers + slot);
                }
            }
        }
        if (sent.cb_count != 0) {
            parts |= SideConstBuffers;
            words += 1 + 2 * sent.cb_count;
        } else {
            // Only padding bytes differed.
            std::memcpy(&sent.const_buffers, &state.shader_stages, sizeof(sent.const_buffers));
        }
    }
    if (std::ranges::any_of(light.rebound, [](u32 bits) { return bits != 0; })) {
        parts |= SideRebound;
        words += 3;
    }
    if (engine_state != sent.engine_state) {
        parts |= SideEngineState;
        words += 1;
    }
    if (replace_table != sent.replace_table) {
        parts |= SideReplaceTable;
        words += 1 + 2 * static_cast<u32>(replace_table.size());
    }
    if (execute_on != sent.execute_on) {
        parts |= SideExecuteOn;
        words += 1;
    }
    if (!SameDrawState(draw_manager.draw_state, sent.draw_state)) {
        parts |= SideDrawState;
        words += DrawStateWords(draw_manager.draw_state);
    }
    if (!SameIndirect(draw_manager.indirect_state, sent.indirect_state)) {
        parts |= SideIndirectState;
        words += IndirectWords;
    }
    if (std::memcmp(&draw_manager.draw_texture_state, &sent.draw_texture_state,
                    sizeof(DrawTextureState)) != 0) {
        parts |= SideDrawTexture;
        words += DrawTextureWords;
    }
    if (dirty.flags.any()) {
        parts |= SideDirtyFlags;
        words += DirtyFlagWords;
    }
    // =2: every draw carries one, a header alone if nothing changed (VerifyReplica compares
    // the side state after it, K2).
    if (parts != 0 || sent.verify) {
        words += 1;
    }
    sent.parts = parts;
    sent.words = words;
    return words;
}

void Maxwell3D::StageWriteSide(u64* out) {
    using namespace VideoCommon::Nx2RegReplica;
    auto& sent = *stage_sent;
    const u32 parts = sent.parts;
    u64* const begin = out;
    *out++ = MakeSideHeader(parts, sent.words - 1);
    if ((parts & SideConstBuffers) != 0) {
        *out++ = sent.cb_count;
        for (u32 i = 0; i < sent.cb_count; ++i) {
            const u32 stage = sent.cb_slots[i] / Regs::MaxConstBuffers;
            const u32 slot = sent.cb_slots[i] % Regs::MaxConstBuffers;
            const auto& buffer = state.shader_stages[stage].const_buffers[slot];
            *out++ = stage | (slot << 8) | (buffer.enabled ? 1U << 16 : 0U) |
                     (u64{buffer.size} << 32);
            *out++ = buffer.address;
        }
        std::memcpy(&sent.const_buffers, &state.shader_stages, sizeof(sent.const_buffers));
    }
    if ((parts & SideRebound) != 0) {
        *out++ = Pair(light.rebound[0], light.rebound[1]);
        *out++ = Pair(light.rebound[2], light.rebound[3]);
        *out++ = Pair(light.rebound[4], 0);
        light.rebound.fill(0);
    }
    if ((parts & SideEngineState) != 0) {
        *out++ = static_cast<u64>(engine_state);
        sent.engine_state = engine_state;
    }
    if ((parts & SideReplaceTable) != 0) {
        *out++ = replace_table.size();
        for (const auto& [key, value] : replace_table) {
            *out++ = key;
            *out++ = static_cast<u64>(value);
        }
        sent.replace_table = replace_table;
    }
    if ((parts & SideExecuteOn) != 0) {
        *out++ = execute_on ? 1 : 0;
        sent.execute_on = execute_on;
    }
    if ((parts & SideDrawState) != 0) {
        WriteDrawState(out, draw_manager.draw_state);
        sent.draw_state = draw_manager.draw_state;
    }
    if ((parts & SideIndirectState) != 0) {
        const auto& indirect = draw_manager.indirect_state;
        *out++ = (indirect.is_byte_count ? 1 : 0) | (indirect.is_indexed ? 2 : 0) |
                 (indirect.include_count ? 4 : 0);
        *out++ = indirect.count_start_address;
        *out++ = indirect.indirect_start_address;
        *out++ = indirect.buffer_size;
        *out++ = indirect.max_draw_counts;
        *out++ = indirect.stride;
        sent.indirect_state = indirect;
    }
    if ((parts & SideDrawTexture) != 0) {
        out[DrawTextureWords - 1] = 0;
        std::memcpy(out, &draw_manager.draw_texture_state, sizeof(DrawTextureState));
        out += DrawTextureWords;
        sent.draw_texture_state = draw_manager.draw_texture_state;
    }
    if ((parts & SideDirtyFlags) != 0) {
        std::memcpy(out, &dirty.flags, sizeof(DirtyFlags));
        out += DirtyFlagWords;
        dirty.flags.reset();
    }
    ASSERT(static_cast<u32>(out - begin) == sent.words);
    sent.parts = 0;
    sent.words = 0;
}

void Maxwell3D::StageApplySide(const u64* words, u32 count) {
    using namespace VideoCommon::Nx2RegReplica;
    const u64 header = words[0];
    const u32 parts = SideParts(header);
    ASSERT(SideWords(header) + 1 == count);
    const u64* in = words + 1;
    stage_side_applied = true;
    if ((parts & SideConstBuffers) != 0) {
        const u64 binds = *in++;
        for (u64 i = 0; i < binds; ++i) {
            const u64 word = *in++;
            auto& buffer = state.shader_stages[word & 0xff].const_buffers[(word >> 8) & 0xff];
            buffer.enabled = ((word >> 16) & 1) != 0;
            buffer.size = static_cast<u32>(word >> 32);
            buffer.address = *in++;
        }
    }
    if ((parts & SideRebound) != 0) {
        light.rebound[0] |= static_cast<u32>(in[0]);
        light.rebound[1] |= static_cast<u32>(in[0] >> 32);
        light.rebound[2] |= static_cast<u32>(in[1]);
        light.rebound[3] |= static_cast<u32>(in[1] >> 32);
        light.rebound[4] |= static_cast<u32>(in[2]);
        in += 3;
    }
    if ((parts & SideEngineState) != 0) {
        engine_state = static_cast<EngineHint>(*in++);
    }
    if ((parts & SideReplaceTable) != 0) {
        replace_table.clear();
        const u64 entries = *in++;
        for (u64 i = 0; i < entries; ++i) {
            const u64 key = *in++;
            replace_table.emplace(key, static_cast<HLEReplacementAttributeType>(*in++));
        }
    }
    if ((parts & SideExecuteOn) != 0) {
        execute_on = *in++ != 0;
    }
    if ((parts & SideDrawState) != 0) {
        ReadDrawState(in, draw_manager.draw_state);
    }
    if ((parts & SideIndirectState) != 0) {
        auto& indirect = draw_manager.indirect_state;
        indirect.is_byte_count = (in[0] & 1) != 0;
        indirect.is_indexed = (in[0] & 2) != 0;
        indirect.include_count = (in[0] & 4) != 0;
        indirect.count_start_address = in[1];
        indirect.indirect_start_address = in[2];
        indirect.buffer_size = static_cast<size_t>(in[3]);
        indirect.max_draw_counts = static_cast<size_t>(in[4]);
        indirect.stride = static_cast<size_t>(in[5]);
        in += IndirectWords;
    }
    if ((parts & SideDrawTexture) != 0) {
        std::memcpy(&draw_manager.draw_texture_state, in, sizeof(DrawTextureState));
        in += DrawTextureWords;
    }
    if ((parts & SideDirtyFlags) != 0) {
        DirtyFlags flags;
        std::memcpy(static_cast<void*>(&flags), in, sizeof(DirtyFlags));
        in += DirtyFlagWords;
        dirty.flags |= flags;
    }
    ASSERT(in == words + count);
}

bool Maxwell3D::StageVerify(Maxwell3D& replica) const {
    bool same = true;
    const auto differs = [&same](const char* what, u64 index, u64 primary_value,
                                 u64 replica_value) {
        same = false;
        if (LogStageDifference()) {
            LOG_ERROR(HW_GPU, "Back stages (=2): replica {} {:#x} holds {:#x}, primary {:#x}",
                      what, index, replica_value, primary_value);
        }
    };
    // After every compared record: the registers. Not compared: the macro upload and bind
    // pointers (ProcessMacroUpload, ProcessMacroBind and ProcessMethodCall step them outside
    // ProcessDirtyRegisters; only macro loading reads them, on A and the front).
    for (u32 method = 0; method < Regs::NUM_REGS; ++method) {
        if (method == MAXWELL3D_REG_INDEX(load_mme.instruction_ptr) ||
            method == MAXWELL3D_REG_INDEX(load_mme.start_address_ptr)) {
            continue;
        }
        if (regs.reg_array[method] != replica.regs.reg_array[method]) {
            differs("register", method, regs.reg_array[method], replica.regs.reg_array[method]);
        }
    }
    const auto& sent = *stage_sent;
    if (replica.stage_side_applied) {
        // After a record that carried side state: the parts as A sent them (the primary's own
        // may have moved on since when not every record is compared).
        replica.stage_side_applied = false;
        for (size_t stage = 0; stage < Regs::MaxShaderStage; ++stage) {
            for (size_t slot = 0; slot < Regs::MaxConstBuffers; ++slot) {
                const auto& a = sent.const_buffers[stage].const_buffers[slot];
                const auto& b = replica.state.shader_stages[stage].const_buffers[slot];
                if (!SameConstBuffer(a, b)) {
                    differs("constant buffer bind (stage << 8 | slot)", (stage << 8) | slot,
                            a.address, b.address);
                }
            }
        }
        if (sent.engine_state != replica.engine_state) {
            differs("engine_state", 0, static_cast<u64>(sent.engine_state),
                    static_cast<u64>(replica.engine_state));
        }
        if (sent.replace_table != replica.replace_table) {
            differs("replace_table (entries)", 0, sent.replace_table.size(),
                    replica.replace_table.size());
        }
        if (sent.execute_on != replica.execute_on) {
            differs("execute_on", 0, sent.execute_on ? 1 : 0, replica.execute_on ? 1 : 0);
        }
        if (!SameDrawState(sent.draw_state, replica.draw_manager.draw_state)) {
            differs("draw_state (instance count)", 0, sent.draw_state.instance_count,
                    replica.draw_manager.draw_state.instance_count);
        }
        if (!SameIndirect(sent.indirect_state, replica.draw_manager.indirect_state)) {
            differs("indirect_state (address)", 0, sent.indirect_state.indirect_start_address,
                    replica.draw_manager.indirect_state.indirect_start_address);
        }
        if (std::memcmp(&sent.draw_texture_state, &replica.draw_manager.draw_texture_state,
                        sizeof(DrawTextureState)) != 0) {
            differs("draw_texture_state (texture)", 0, sent.draw_texture_state.src_texture,
                    replica.draw_manager.draw_texture_state.src_texture);
        }
        // No light.rebound bit of A left unsent (S2). Only when this very record is compared:
        // else A may have bound since.
        if (VideoCommon::Nx2Stages::Get().verify_every <= 1) {
            for (size_t stage = 0; stage < Regs::MaxShaderStage; ++stage) {
                if (light.rebound[stage] != 0) {
                    differs("unsent light.rebound (stage)", stage, light.rebound[stage], 0);
                }
            }
        }
        // A draw through PrepareDraw cleared the replica's light changes; DrawTexture leaves
        // them: those stay until the next draw that clears them. (Comparing every n-th record,
        // what came after the draw joins them: the check is laxer there, never wrong.)
        replica.stage_light_base = replica.light.changes;
    }
    // After every compared record: the light changes, A's (since the draw record before) and the
    // replica's, but UniformSame and UniformData: cb data writes note those on the replica alone
    // (StageCbWrite).
    constexpr u32 replica_only = LightChange::UniformSame | LightChange::UniformData;
    const u32 expected = (light.changes | replica.stage_light_base) & ~replica_only;
    const u32 actual = replica.light.changes & ~replica_only;
    // (EDEN_NX2_LIGHT_DRAW=0: the replica notes none, the old code some: nothing reads them.)
    if (expected != actual && LightTrackingEnabled()) {
        differs("light changes", 0, expected, actual);
    }
    // Never compared: light.armed, same_writes, same_device, cb_writes, written and
    // first_blocking*: the stage's rasterizer and StageCbWrite make them on the replica alone.
    return same;
}

} // namespace Tegra::Engines
