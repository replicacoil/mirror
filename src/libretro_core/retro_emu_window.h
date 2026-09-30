// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/frontend/emu_window.h"

namespace LibretroCore {

// Minimal EmuWindow for the libretro core frontend. Runs the video backend
// in WindowSystemType::Headless mode (render_surface = nullptr), which
// video_core already supports natively - see core/frontend/emu_window.h's
// WindowSystemInfo::render_surface doc comment. This lets Core::System boot
// and run a game with no real window at all.
//
// Frames reach retro_video_refresh via a CPU-side readback path added to
// RendererBase/RendererVulkan for this port: Composite() detects the headless
// window and copies the presented image to host memory
// (vkCmdCopyImageToBuffer) instead of presenting to a swapchain, then
// RendererBase::GetLastRenderedFrame() exposes it. See renderer_vulkan.cpp's
// is_headless branch in Composite().
class RetroEmuWindow final : public Core::Frontend::EmuWindow {
public:
    RetroEmuWindow();
    ~RetroEmuWindow() override;

    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override;
    bool IsShown() const override;
};

} // namespace LibretroCore
