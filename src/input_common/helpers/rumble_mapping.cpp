// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

#include "input_common/helpers/rumble_mapping.h"

namespace InputCommon {

namespace {
f32 ApplyCurve(f32 amplitude, f32 curve_factor) {
    return (amplitude + std::pow(amplitude, curve_factor)) * 0.5f;
}

f32 FrequencyScale(f32 frequency, f32 start, f32 width) {
    if (frequency <= start) {
        return 1.0f;
    }
    return (std::max)(1.0f - (frequency - start) / width, 0.3f);
}

f32 HighMotorShare(f32 frequency) {
    // Centered on the geometric mean of the default HD Rumble bands (160 and 320 Hz), so they
    // split symmetrically between the motors.
    constexpr f32 heavy_limit = 113.0f;
    constexpr f32 light_limit = 452.0f;
    if (!(frequency > heavy_limit)) {
        return 0.0f;
    }
    return std::clamp(std::log2(frequency / heavy_limit) / std::log2(light_limit / heavy_limit),
                      0.0f, 1.0f);
}

f32 LowBand(const Common::Input::VibrationStatus& vibration, f32 curve_factor) {
    return ApplyCurve(vibration.low_amplitude, curve_factor) *
           FrequencyScale(vibration.low_frequency, 140.0f, 400.0f);
}

f32 HighBand(const Common::Input::VibrationStatus& vibration, f32 curve_factor) {
    return ApplyCurve(vibration.high_amplitude, curve_factor) *
           FrequencyScale(vibration.high_frequency, 200.0f, 700.0f);
}
} // Anonymous namespace

MotorStrengths MapRumbleByBand(const Common::Input::VibrationStatus& vibration, f32 curve_factor) {
    return {
        .low = LowBand(vibration, curve_factor),
        .high = HighBand(vibration, curve_factor),
    };
}

MotorStrengths MapRumbleByFrequency(const Common::Input::VibrationStatus& vibration,
                                    f32 curve_factor) {
    const f32 low_band = LowBand(vibration, curve_factor);
    const f32 high_band = HighBand(vibration, curve_factor);
    const f32 low_share = HighMotorShare(vibration.low_frequency);
    const f32 high_share = HighMotorShare(vibration.high_frequency);
    return {
        .low = (std::min)(low_band * (1.0f - low_share) + high_band * (1.0f - high_share), 1.0f),
        .high = (std::min)(low_band * low_share + high_band * high_share, 1.0f),
    };
}

} // namespace InputCommon
