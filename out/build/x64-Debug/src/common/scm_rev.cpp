// SPDX-FileCopyrightText: 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/scm_rev.h"

#define GIT_REV "f8b76511041bb54070c52d1d09ea07d8108c592a"
#define GIT_BRANCH "master"
#define GIT_DESC "f8b7651104-master"
#define BUILD_NAME "Eden"
#define BUILD_DATE "2026-09-30T06:38:01Z"
#define BUILD_FULLNAME "Eden f8b7651104-master "
#define BUILD_VERSION "f8b7651104-master"
#define BUILD_ID "master"
#define TITLE_BAR_FORMAT_IDLE ""
#define TITLE_BAR_FORMAT_RUNNING ""
#define IS_DEV_BUILD true
#define COMPILER_ID "MSVC 19.51.36260.0"
#define BUILD_AUTO_UPDATE_WEBSITE "https://git.eden-emu.dev"
#define BUILD_AUTO_UPDATE_API "stable.eden-emu.dev"
#define BUILD_AUTO_UPDATE_API_PATH "/latest/release.json"
#define BUILD_AUTO_UPDATE_REPO "eden-emu/eden"
#define BUILD_AUTO_UPDATE_STABLE_API "git.eden-emu.dev"
#define BUILD_AUTO_UPDATE_STABLE_API_PATH "/api/v1/repos/"
#define BUILD_AUTO_UPDATE_STABLE_REPO "eden-emu/eden"
#define IS_NIGHTLY_BUILD false

namespace Common {

constexpr const char g_scm_rev[] = GIT_REV;
constexpr const char g_scm_branch[] = GIT_BRANCH;
constexpr const char g_scm_desc[] = GIT_DESC;
constexpr const char g_build_name[] = BUILD_NAME;
constexpr const char g_build_date[] = BUILD_DATE;
constexpr const char g_build_fullname[] = BUILD_FULLNAME;
constexpr const char g_build_version[] = BUILD_VERSION;
constexpr const char g_build_id[] = BUILD_ID;
constexpr const char g_title_bar_format_idle[] = TITLE_BAR_FORMAT_IDLE;
constexpr const char g_title_bar_format_running[] = TITLE_BAR_FORMAT_RUNNING;
constexpr const char g_compiler_id[] = COMPILER_ID;

constexpr const bool g_is_dev_build = IS_DEV_BUILD;
constexpr const bool g_is_nightly_build = IS_NIGHTLY_BUILD;

constexpr const char g_build_auto_update_website[] = BUILD_AUTO_UPDATE_WEBSITE;
constexpr const char g_build_auto_update_api[] = BUILD_AUTO_UPDATE_API;
constexpr const char g_build_auto_update_api_path[] = BUILD_AUTO_UPDATE_API_PATH;
constexpr const char g_build_auto_update_repo[] = BUILD_AUTO_UPDATE_REPO;
constexpr const char g_build_auto_update_stable_api[] = BUILD_AUTO_UPDATE_STABLE_API;
constexpr const char g_build_auto_update_stable_api_path[] = BUILD_AUTO_UPDATE_STABLE_API_PATH;
constexpr const char g_build_auto_update_stable_repo[] = BUILD_AUTO_UPDATE_STABLE_REPO;

} // namespace Common
