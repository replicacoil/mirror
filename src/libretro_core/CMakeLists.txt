# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(eden_libretro SHARED
    libretro.h
    retro_core.cpp
    retro_emu_window.cpp
    retro_emu_window.h
)

# video_core must be linked explicitly here, not just pulled in via core:
# core links video_core PRIVATE (see src/core/CMakeLists.txt), so its PUBLIC
# usage requirements - notably Vulkan::Headers' include path, which our
# VMA_IMPLEMENTATION block at the end of retro_core.cpp needs for
# <vulkan/vulkan.h> - don't propagate through core to us. yuzu_cmd's own
# CMakeLists.txt links video_core directly for the same reason.
target_link_libraries(eden_libretro PRIVATE common core input_common frontend_common audio_core network video_core)
target_link_libraries(eden_libretro PRIVATE ${PLATFORM_LIBRARIES} Threads::Threads)

set_target_properties(eden_libretro PROPERTIES
    PREFIX ""
    OUTPUT_NAME "eden_libretro"
)

create_target_directory_groups(eden_libretro)
