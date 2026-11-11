// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
// A register read before any write is zero on the hardware. OpUndef is whatever the driver
// left there. Bananza's glow pass returns before it writes the colour on some pixels of a
// debris piece: that value blooms into the halo around the black mesh. Zero matches the
// hardware. EDEN_NX2_UNDEF_ZERO=0 emits OpUndef again.
bool UndefIsZero() {
    static const bool zero = [] {
        const char* const value = std::getenv("EDEN_NX2_UNDEF_ZERO");
        return value == nullptr || value[0] != '0';
    }();
    return zero;
}
} // Anonymous namespace

Id EmitUndefU1(EmitContext& ctx) {
    return UndefIsZero() ? ctx.false_value : ctx.OpUndef(ctx.U1);
}

Id EmitUndefU8(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitUndefU16(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitUndefU32(EmitContext& ctx) {
    return UndefIsZero() ? ctx.u32_zero_value : ctx.OpUndef(ctx.U32[1]);
}

Id EmitUndefU64(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

} // namespace Shader::Backend::SPIRV
