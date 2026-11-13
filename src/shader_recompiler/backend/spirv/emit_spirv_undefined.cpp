// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
// Registers read before any write are zero rather than OpUndef, as NVIDIA gives them; AMD's
// OpUndef is whatever the register held (green patches near glowing objects). On by default for
// AMD drivers only; EDEN_NX2_UNDEF_ZERO=1 forces it on everywhere, =0 off.
bool UndefIsZero(const EmitContext& ctx) {
    static const char* const value = std::getenv("EDEN_NX2_UNDEF_ZERO");
    if (value != nullptr && (value[0] == '0' || value[0] == '1')) {
        return value[0] == '1';
    }
    return ctx.profile.zero_undefined_registers;
}
} // Anonymous namespace

Id EmitUndefU1(EmitContext& ctx) {
    return UndefIsZero(ctx) ? ctx.false_value : ctx.OpUndef(ctx.U1);
}

Id EmitUndefU8(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitUndefU16(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitUndefU32(EmitContext& ctx) {
    return UndefIsZero(ctx) ? ctx.u32_zero_value : ctx.OpUndef(ctx.U32[1]);
}

Id EmitUndefU64(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

} // namespace Shader::Backend::SPIRV
