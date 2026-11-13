// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/present/frame_gen.h"
#include "video_core/renderer_vulkan/present/nx2_fg_refresh.h"
#include "video_core/renderer_vulkan/present/nx2_fg_timing.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

constexpr size_t COLOR_CHANNELS = 4;
constexpr u64 LSFG_REQUIRED_FRAMES = 2;
constexpr u32 LSFG_RECURRENCE_FRAMES = 2;
constexpr u32 TIMING_DIAG_FRAMES = 600;
constexpr auto DISPLAY_REFRESH_RECHECK = std::chrono::seconds(2);

[[nodiscard]] f32 ManualFlowScale() {
    return static_cast<f32>(Settings::values.frame_gen_flow_scale.GetValue()) / 100.0f;
}

[[nodiscard]] f32 ConfiguredFlowScale(VkExtent2D guest_extent, VkExtent2D presented_extent) {
    if (!Settings::values.frame_gen_flow_scale_auto.GetValue()) {
        return ManualFlowScale();
    }
    if (guest_extent.width == 0 || presented_extent.width == 0) {
        return 1.0f;
    }

    const f32 rendered_width = static_cast<f32>(guest_extent.width) *
                               Settings::values.resolution_info.up_factor;
    const f32 ratio = rendered_width / static_cast<f32>(presented_extent.width);

    constexpr f32 FLOW_SCALE_STEPS = 20.0f;
    const f32 stepped = std::ceil(ratio * FLOW_SCALE_STEPS) / FLOW_SCALE_STEPS;
    return std::clamp(stepped, 0.25f, 1.0f);
}

bool IsBlueFirst(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

VkDeviceSize BytesPerTexel(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM:
        return 1;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    default:
        return COLOR_CHANNELS;
    }
}

void WritePortablePixmap(const std::filesystem::path& path, const std::string& magic,
                         VkExtent2D extent, std::span<const u8> pixels) {
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Write,
                            Common::FS::FileType::BinaryFile};
    if (!file.IsOpen()) {
        return;
    }

    const std::string header = magic + "\n" + std::to_string(extent.width) + " " +
                               std::to_string(extent.height) + "\n255\n";
    if (file.Write(header) != header.size()) {
        return;
    }

    void(file.Write(pixels));
    void(file.Flush());
}

void WriteGrayscalePgm(const std::filesystem::path& path, VkExtent2D extent,
                       std::span<const u8> pixels) {
    const size_t expected = static_cast<size_t>(extent.width) * extent.height;
    WritePortablePixmap(path, "P5", extent, pixels.subspan(0, std::min(expected, pixels.size())));
}

void WriteRaw(const std::filesystem::path& path, std::span<const u8> pixels) {
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Write,
                            Common::FS::FileType::BinaryFile};
    if (!file.IsOpen()) {
        return;
    }
    void(file.Write(pixels));
    void(file.Flush());
}

void WriteColorPpm(const std::filesystem::path& path, VkExtent2D extent,
                   std::span<const u8> pixels, bool blue_first) {
    const size_t pixel_count = static_cast<size_t>(extent.width) * extent.height;
    if (pixels.size() < pixel_count * COLOR_CHANNELS) {
        return;
    }

    std::vector<u8> rgb(pixel_count * 3);
    for (size_t i = 0; i < pixel_count; ++i) {
        const u8 first = pixels[i * COLOR_CHANNELS];
        const u8 green = pixels[i * COLOR_CHANNELS + 1];
        const u8 third = pixels[i * COLOR_CHANNELS + 2];
        rgb[i * 3] = blue_first ? third : first;
        rgb[i * 3 + 1] = green;
        rgb[i * 3 + 2] = blue_first ? first : third;
    }

    WritePortablePixmap(path, "P6", extent, rgb);
}

VkImageMemoryBarrier MakeTransitionBarrier(VkImage image, VkAccessFlags src_access,
                                           VkAccessFlags dst_access, VkImageLayout old_layout,
                                           VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
}

VkImageCopy MakeCopyRegion(VkExtent2D extent) {
    return VkImageCopy{
        .srcSubresource{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcOffset = {},
        .dstSubresource{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .dstOffset = {},
        .extent = {.width = extent.width, .height = extent.height, .depth = 1},
    };
}

void CopyPresentedFrame(vk::CommandBuffer cmdbuf, VkImage source, LsfgImage& destination,
                        VkExtent2D extent) {
    const auto make_barrier = MakeTransitionBarrier;

    const std::array before{
        make_barrier(source, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        make_barrier(destination.Handle(), VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     destination.Layout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);

    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.Handle(),
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, MakeCopyRegion(extent));

    const std::array after{
        make_barrier(source, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        make_barrier(destination.Handle(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, {}, {}, after);

    destination.SetLayout(VK_IMAGE_LAYOUT_GENERAL);
}

} // Anonymous namespace

FrameGen::FrameGen(MemoryAllocator& memory_allocator_, Scheduler& scheduler_,
                   void* native_window_)
    : memory_allocator{memory_allocator_}, scheduler{scheduler_}, native_window{native_window_} {
    timing.timed = Nx2FgTiming::SwitchOn(std::getenv("EDEN_NX2_FG_TIMED"));
    const char* present_diag = std::getenv("EDEN_NX2_PRESENT_DIAG");
    timing.diag = Nx2FgTiming::SwitchOn(std::getenv("EDEN_NX2_FG_DIAG")) ||
                  (timing.timed && present_diag != nullptr && present_diag[0] != '0');
    if (!timing.timed && !timing.diag) {
        return;
    }
    bool invalid = false;
    timing.override_hz =
        Nx2FgTiming::ParseRefreshHz(std::getenv("EDEN_NX2_FG_REFRESH_HZ"), &invalid);
    if (invalid) {
        LOG_WARNING(Render_Vulkan, "nx2:fg EDEN_NX2_FG_REFRESH_HZ is not a refresh of 20-1000 Hz, "
                                   "the display's refresh is used");
    }
    LOG_INFO(Render_Vulkan,
             "nx2:fg timed generated frames {} (EDEN_NX2_FG_TIMED, FSR3 only), refresh {}",
             timing.timed ? "on" : "off",
             timing.override_hz
                 ? fmt::format("{:.2f} Hz (EDEN_NX2_FG_REFRESH_HZ)", *timing.override_hz)
                 : std::string{"from the display"});
}

FrameGen::~FrameGen() = default;

void FrameGen::Process(const Device& device, Frame* frame, VkFormat format,
                       VkExtent2D guest_extent) {
    generated = false;
    if (timing.timed || timing.diag) {
        ++timing.composites;
    }

    if (unavailable || !Settings::values.frame_gen.GetValue()) {
        if (chain || fsr3_chain) {
            scheduler.Finish();
            chain.reset();
            fsr3_chain.reset();
        }
        warm_streak = 0;
        return;
    }

    if (!shaders) {
        shaders.emplace(device);
    }

    if (!shaders->IsValid() && Settings::values.frame_gen_type.GetValue() != Settings::FrameGenType::Fsr3) {
        LOG_WARNING(Render_Vulkan, "Frame gen off: LSFG shaders could not be loaded");
        unavailable = true;
        return;
    }

    if (!frame->storage_view) {
        warm_streak = 0;
        return;
    }

    peak_guest_extent.width = std::max(peak_guest_extent.width, guest_extent.width);
    peak_guest_extent.height = std::max(peak_guest_extent.height, guest_extent.height);

    const VkExtent2D extent{.width = frame->width, .height = frame->height};
    const f32 flow_scale = ConfiguredFlowScale(peak_guest_extent, extent);
    if ((!chain && !fsr3_chain) || built_extent.width != extent.width || built_extent.height != extent.height ||
        built_format != format || built_flow_scale != flow_scale) {
        Rebuild(device, extent, format, flow_scale);
    }

    const u64 count = frame_count++;
    last_count = count;
    last_generations = plan.generations;

    const bool warm = plan.warm && count + 1 >= LSFG_REQUIRED_FRAMES;
    warm_streak = warm ? warm_streak + 1 : 0;
    generated = warm && warm_streak >= LSFG_RECURRENCE_FRAMES && plan.generations > 0;

    const bool use_fsr3 = Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::Fsr3;

    if (timing.timed || timing.diag) {
        UpdateTiming(use_fsr3 && fsr3_chain);
    }

    if (use_fsr3 && fsr3_chain) {
        scheduler.RequestOutsideRenderPassOperationContext();
        fsr3_chain->Process(device, scheduler, *frame->image, *frame->storage_view, frame->index,
                            count);
    } else if (chain) {
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([this, source = *frame->image, extent, count,
                          dispatch = warm](vk::CommandBuffer cmdbuf) {
            CopyPresentedFrame(cmdbuf, source, chain->Input(count), extent);
            if (dispatch) {
                chain->DispatchShared(cmdbuf, count);
            }
        });

        const bool dump_requested = generated && Settings::values.frame_gen_dump_flow.GetValue();
        if (!dump_requested) {
            dumped = false;
        } else if (!dumped) {
            DumpDebugImages(count);
            dumped = true;
        }
    }
}

void FrameGen::Pause() {
    pacer.Reset();
    plan = {};
    frame_count = 0;
    last_count = 0;
    last_generations = 0;
    warm_streak = 0;
    generated = false;
}

size_t FrameGen::WantedGenerations(size_t capacity) {
    if (unavailable) {
        plan = {};
        return 0;
    }
    // The display refresh caps Dynamic (a target rate); not asked from Windows otherwise
    const bool needs_refresh = Settings::values.frame_gen.GetValue() &&
                               (Settings::values.frame_gen_target_rate.GetValue() > 0 ||
                                timing.timed || timing.diag);
    plan = pacer.Plan(capacity, needs_refresh ? RefreshPeriod() : 0.0f);
    return plan.generations;
}

size_t FrameGen::GeneratedFrameCount() const {
    return generated ? last_generations : 0;
}

void FrameGen::GenerateInto(const Device& device, Frame* destination, size_t generation) {
    const bool use_fsr3 = Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::Fsr3;

    if (++generated_total % 3000 == 1) {
        LOG_INFO(Render_Vulkan, "Frame gen: {} frames generated so far ({})", generated_total,
                 use_fsr3 ? "FSR3" : "LSFG");
    }

    if (use_fsr3 && fsr3_chain) {
        const f32 blend =
            timing.applies
                ? Nx2FgTiming::TimedBlend(generation, last_generations, timing.interval,
                                          timing.period)
                : static_cast<f32>(generation + 1) / static_cast<f32>(last_generations + 1);

        scheduler.RequestOutsideRenderPassOperationContext();
        fsr3_chain->Generate(device, scheduler, *destination->storage_view, destination->index,
                             blend);
    } else if (chain) {
        chain->SetTarget(device, last_generations, generation, destination->index,
                         *destination->storage_view);

        const VkExtent2D extent{.width = destination->width, .height = destination->height};

        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([this, count = last_count, generation_count = last_generations, generation,
                          target = destination->index, image = *destination->image,
                          extent](vk::CommandBuffer cmdbuf) {
            chain->DispatchGeneration(cmdbuf, count, generation_count, generation, target, image,
                                      extent);
        });
    }
}

f32 FrameGen::RefreshPeriod() {
    if (timing.override_hz) {
        return 1.0f / *timing.override_hz;
    }
    // Read again now and then: the window can move to another display, the mode can change
    const auto now = std::chrono::steady_clock::now();
    if (!timing.display_read || now - *timing.display_read >= DISPLAY_REFRESH_RECHECK) {
        timing.display_read = now;
        const f32 hz =
            Nx2FgTiming::UsableDisplayHz(Nx2FgTiming::QueryDisplayRefreshHz(native_window))
                .value_or(0.0f);
        if (hz != timing.display_hz) {
            LOG_INFO(Render_Vulkan, "nx2:fg display refresh {} Hz{}", hz,
                     hz > 0.0f ? "" : " (unknown: generated frames keep the even weights)");
            timing.display_hz = hz;
        }
    }
    return timing.display_hz > 0.0f ? 1.0f / timing.display_hz : 0.0f;
}

// Before GenerateInto of this frame: B and V of its burst, whether the burst takes the timed
// weights, and the EDEN_NX2_FG_DIAG line. B comes from this frame's own interval, not the pacer's
// smoothed one (it lags each 60 <-> 30 change by several frames; on 30 -> 60 with three generated
// frames it would even time a burst that no longer drains), and from the previous burst's
// generated frames (Nx2FgTiming::DisplayGap): unknown when the previous Composite did not get here.
void FrameGen::UpdateTiming(bool use_fsr3) {
    const size_t count = generated ? last_generations : 0;
    const bool previous_known = timing.last_composite + 1 == timing.composites;
    const size_t previous = timing.previous_count;
    timing.last_composite = timing.composites;
    timing.previous_count = count;
    // frame_pacing_mode Target_30..120 holds every present (generated ones too) to its own rate
    // (Swapchain::AcquireNextImage): they no longer go out one refresh apart
    const bool display_paced =
        Settings::values.frame_pacing_mode.GetValue() == Settings::FramePacingMode::Target_Auto ||
        !Settings::values.use_speed_limit.GetValue();
    const f32 frame_interval = pacer.LastInterval();
    timing.period = RefreshPeriod();
    timing.interval = previous_known && frame_interval > 0.0f && timing.period > 0.0f
                          ? Nx2FgTiming::DisplayGap(frame_interval, count, previous, timing.period)
                          : 0.0f;
    timing.applies = timing.timed && use_fsr3 && display_paced &&
                     Nx2FgTiming::TimedApplies(count, timing.interval, timing.period);
    if (!timing.diag) {
        return;
    }

    auto& stats = timing.stats;
    ++stats.bursts[std::min<size_t>(count, stats.bursts.size() - 1)];
    if (count > 0) {
        if (timing.applies) {
            ++stats.timed_bursts;
        }
        stats.interval_min = stats.interval_count == 0
                                 ? frame_interval
                                 : std::min(stats.interval_min, frame_interval);
        stats.interval_max = std::max(stats.interval_max, frame_interval);
        stats.interval_sum += frame_interval;
        ++stats.interval_count;
        // The sample: the last burst with the most generated frames
        if (count >= stats.sample_count && count <= stats.sample_blend.size()) {
            stats.sample_count = count;
            for (size_t i = 0; i < count; ++i) {
                stats.sample_blend[i] =
                    timing.applies
                        ? Nx2FgTiming::TimedBlend(i, count, timing.interval, timing.period)
                        : Nx2FgTiming::EvenBlend(i, count);
            }
        }
    }
    if (++stats.frames < TIMING_DIAG_FRAMES) {
        return;
    }

    std::string bursts;
    for (size_t i = 0; i < stats.bursts.size(); ++i) {
        if (stats.bursts[i] != 0) {
            bursts += fmt::format("{}{}{}:{}", bursts.empty() ? "" : " ", i,
                                  i + 1 == stats.bursts.size() ? "+" : "", stats.bursts[i]);
        }
    }
    std::string sample;
    std::string even;
    for (size_t i = 0; i < stats.sample_count; ++i) {
        sample += fmt::format("{}{:.2f}", i == 0 ? "" : " ", stats.sample_blend[i]);
        even += fmt::format("{}{:.2f}", i == 0 ? "" : " ",
                            Nx2FgTiming::EvenBlend(i, stats.sample_count));
    }
    const f32 average = stats.interval_count > 0
                            ? stats.interval_sum / static_cast<f32>(stats.interval_count)
                            : 0.0f;
    // Its own line: "nx2:vi2 last 600 presents" is read by the measuring tools
    LOG_INFO(Render_Vulkan,
             "nx2:fg timing last {} frames ({}{}): generated per frame {}; frame interval {:.1f} ms "
             "(min {:.1f}, max {:.1f}); refresh {:.2f} ms ({}); timed weights in {} of {} bursts; "
             "sample n={} t {} (even {})",
             stats.frames, use_fsr3 ? "FSR3" : "not FSR3, weights fixed",
             display_paced ? "" : ", presents held by frame_pacing_mode, weights even", bursts,
             average * 1000.0f, stats.interval_min * 1000.0f, stats.interval_max * 1000.0f,
             timing.period * 1000.0f,
             timing.override_hz ? "EDEN_NX2_FG_REFRESH_HZ"
                                : (timing.display_hz > 0.0f ? "display" : "unknown"),
             stats.timed_bursts, stats.interval_count, stats.sample_count,
             sample.empty() ? "-" : sample, even.empty() ? "-" : even);
    stats = {};
}

void FrameGen::Rebuild(const Device& device, VkExtent2D extent, VkFormat format, f32 flow_scale) {
    scheduler.Finish();
    chain.reset();
    fsr3_chain.reset();

    built_flow_scale = flow_scale;

    if (Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::Fsr3) {
        fsr3_chain.emplace(device, memory_allocator, extent, format);
    } else {
        chain.emplace(device, memory_allocator, *shaders, extent, format, built_flow_scale);
    }
    built_extent = extent;
    built_format = format;
    frame_count = 0;
    warm_streak = 0;
    generated = false;
}

void FrameGen::DumpDebugImages(u64 count) {
    const std::filesystem::path directory =
        Common::FS::GetEdenPath(Common::FS::EdenPath::LosslessDir) / "debug";
    if (!Common::FS::CreateDirs(directory)) {
        return;
    }

    const auto dump = [&](const std::string& name, LsfgImage& image) {
        const VkExtent2D extent = image.Extent();
        const VkFormat format = image.Format();
        const VkDeviceSize texel_size = BytesPerTexel(format);
        const VkDeviceSize size =
            static_cast<VkDeviceSize>(extent.width) * extent.height * texel_size;

        vk::Buffer buffer = CreateWrappedBuffer(memory_allocator, size, MemoryUsage::Download);

        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record(
            [handle = image.Handle(), dst = *buffer, extent](vk::CommandBuffer cmdbuf) {
                DownloadColorImage(
                    cmdbuf, handle, dst,
                    VkExtent3D{.width = extent.width, .height = extent.height, .depth = 1});
            });
        scheduler.Finish();

        buffer.Invalidate();
        const std::span<u8> mapped = buffer.Mapped();

        if (format == LSFG_FLOW_FORMAT) {
            WriteGrayscalePgm(directory / (name + ".pgm"), extent, mapped);
        } else if (texel_size == COLOR_CHANNELS) {
            WriteColorPpm(directory / (name + ".ppm"), extent, mapped, IsBlueFirst(format));
        } else {
            WriteRaw(directory / (name + "_" + std::to_string(extent.width) + "x" +
                                  std::to_string(extent.height) + ".f16"),
                     mapped.subspan(0, std::min<size_t>(size, mapped.size())));
        }
    };

    dump("in0", chain->Input(0));
    dump("in1", chain->Input(1));

    for (size_t level = 0; level < LSFG_MIP_LEVELS; ++level) {
        dump("flow_mip" + std::to_string(level), chain->FlowLevel(level));
    }
    for (size_t index = 0; index < 2; ++index) {
        dump("alpha0_" + std::to_string(index), chain->AlphaOutput(0, count, index));
        dump("alpha6_" + std::to_string(index), chain->AlphaOutput(LSFG_MIP_LEVELS - 1, count,
                                                                  index));
    }
    for (size_t level = 0; level < LSFG_BETA_OUTPUTS; ++level) {
        dump("beta_" + std::to_string(level), chain->BetaOutput(level));
    }

    dump("gamma0", chain->GammaOutput(0));
    dump("gamma6", chain->GammaOutput(LSFG_MIP_LEVELS - 1));
    dump("delta2_out1", chain->DeltaOutput1(LSFG_DELTA_INSTANCES - 1));
    dump("delta2_out2", chain->DeltaOutput2(LSFG_DELTA_INSTANCES - 1));
}

} // namespace Vulkan
