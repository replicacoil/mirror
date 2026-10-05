// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Real libretro core entry points for Eden (RetroArch loads this .dll as a
// core, in contrast to core/libretro_wrapper.cpp which is Eden acting as a
// libretro *frontend* loading other cores - the two are separate features).
// Ported from suyu's libretro core (itself a fork of Eden) - see that
// project's src/libretro_core/ for the original.
//
// STATUS: boots and runs a game headlessly via Core::System, exactly like
// yuzu_cmd (src/yuzu_cmd/yuzu.cpp) does without Qt. System info, environment
// negotiation, load/unload/reset, and serialize size are real and correct.
//
// WIRED AND VERIFIED:
//   - Video: Vulkan renderer runs headless, rendering to a CPU buffer via
//     RenderToBuffer which retro_run reads back (with a B8G8R8A8 -> XRGB8888
//     channel swap). Falls back to a black frame before the first one lands.
//   - Audio: by default Eden opens a host audio device and plays directly,
//     which is what sounds correct. An "Audio Output" core option can instead
//     route samples through retro_audio_sample_batch via the
//     AudioEngine::Libretro sink - tidier in principle, but nothing paces the
//     emulated renderer here so it delivers in bursts.
//   - Input: retro_input_state_cb is bridged into InputCommon's
//     VirtualGamepad for up to 8 ports (16 buttons + both analog sticks
//     each), with per-port controller type selectable from RetroArch's own
//     Controls menu.
//   - Keys: prod/title/console.keys are picked up from the frontend's
//     system directory (<system>/eden/keys) if not already installed.
//   - Online: Eden's own room-based multiplayer is initialised here, so
//     online play works in the core the same way it does under Qt.
//
// NOT WIRED (deliberately, not silently faked):
//   - Save states: retro_serialize/unserialize return false and
//     retro_serialize_size() returns 0 - see the comment there. This also
//     rules out RetroArch netplay and rerecording, which are defined in
//     libretro.h as depending on serialization; the core declares
//     RETRO_SERIALIZATION_QUIRK_INCOMPLETE so the frontend reports them as
//     unavailable rather than offering them and failing later.

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <filesystem>
#include <vector>
#include "audio_core/sink/libretro_sink.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "network/network.h"
#include "libretro_core/libretro.h"
#include "libretro_core/retro_emu_window.h"
#include "video_core/renderer_base.h"

namespace {

std::unique_ptr<Core::System> g_system;
std::unique_ptr<LibretroCore::RetroEmuWindow> g_emu_window;
std::shared_ptr<InputCommon::InputSubsystem> g_input_subsystem;
std::string g_game_path;
bool g_game_loaded = false;

unsigned g_output_scale = 1;
bool g_geometry_dirty = false;

// False: Eden drives a host audio device directly (default, sounds correct).
// True: samples are handed to the frontend via retro_audio_sample_batch.
bool g_use_frontend_audio = false;
// Set once RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK registration succeeds (see
// retro_load_game()). When true, retro_run() leaves audio delivery to
// FrontendAudioCallback() instead of doing it inline, since both draining the
// same queue would just race pointlessly.
bool g_audio_callback_registered = false;

// Set from the "eden_log_fps" debugging option; see CheckForLiveOptionChanges
// and the [FPS] logging block in retro_run().
bool g_log_fps_enabled = false;

retro_environment_t g_environ_cb;
retro_video_refresh_t g_video_cb;
retro_audio_sample_t g_audio_sample_cb;
retro_audio_sample_batch_t g_audio_batch_cb;
retro_input_poll_t g_input_poll_cb;
retro_input_state_t g_input_state_cb;

constexpr unsigned kFrameWidth = 1280;
constexpr unsigned kFrameHeight = 720;

// Points a single Eden data directory at a subfolder of a frontend-provided
// base directory, creating it first since Common::FS::SetEdenPath silently
// refuses to point at a path that doesn't exist yet (or isn't a directory).
bool RedirectEdenDir(Common::FS::EdenPath path_id, const std::filesystem::path& base,
                      const char* sub_dir) {
    if (base.empty()) {
        return false;
    }
    const auto target = base / sub_dir;
    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    if (ec) {
        LOG_ERROR(Frontend, "libretro: failed to create {} ({})", target.string(), ec.message());
        return false;
    }
    Common::FS::SetEdenPath(path_id, target);
    return true;
}

// Redirects Eden's persistent data out of %APPDATA%\Eden (or <retroarch>\user
// in portable mode) and into the directories RetroArch itself hands the core:
//   - RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY for config/keys/cache/logs
//   - RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY for NAND/SDMC/saves/screenshots,
//     so RetroArch's own save handling (per-core folders, backups, cloud
//     sync, etc.) covers Eden's data the same way it covers any other core
// Each is placed directly in the frontend-provided directory (e.g.
// <system_directory>\config, <save_directory>\nand) with no extra "eden"
// subfolder, so per-core RetroArch overrides that already point
// system_directory/save_directory somewhere eden-specific (as set up in a
// core override .cfg) land exactly where they say, with nothing extra
// appended. If you share that base directory with another emulator, point
// the override somewhere eden-specific instead of relying on this code to
// namespace it for you.
// Controlled by the "eden_use_frontend_dirs" core option, enabled by default.
// Must run before anything (including keys import, below) touches
// Common::FS::GetEdenPath, or those uses will already have grabbed the old,
// non-redirected paths.
void RedirectEdenPathsToFrontend() {
    if (!g_environ_cb) {
        return;
    }

    struct retro_variable var {
        "eden_use_frontend_dirs", nullptr
    };
    // Frontends make the declared default available as soon as
    // RETRO_ENVIRONMENT_SET_VARIABLES has run, i.e. before retro_init() is
    // even called, so this reads correctly on a completely fresh install too.
    bool use_frontend_dirs = true;
    if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        use_frontend_dirs = std::strcmp(var.value, "Disabled") != 0;
    }
    if (!use_frontend_dirs) {
        LOG_INFO(Frontend, "libretro: eden_use_frontend_dirs disabled, keeping default data location");
        return;
    }

    const char* system_dir = nullptr;
    const char* save_dir = nullptr;
    g_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
    g_environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir);

    const std::filesystem::path system_base =
        (system_dir && *system_dir) ? std::filesystem::path(system_dir) : std::filesystem::path{};
    // Some setups (e.g. RetroArch's "Save files in content directory") can
    // report an empty save directory even though a system directory exists.
    // Fall back to the system directory as the root for save-type data too,
    // rather than silently reverting to %APPDATA% for just those folders.
    const std::filesystem::path save_base =
        (save_dir && *save_dir) ? std::filesystem::path(save_dir) : system_base;

    if (system_base.empty()) {
        LOG_WARNING(Frontend, "libretro: frontend gave no system directory; "
                              "keeping eden's default (%APPDATA%/portable) data location");
        return;
    }

    using Common::FS::EdenPath;

    // Save-type data: goes under RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY.
    RedirectEdenDir(EdenPath::NANDDir, save_base, "nand");
    RedirectEdenDir(EdenPath::SaveDir, save_base, "nand"); // Eden aliases SaveDir to the NAND dir
    RedirectEdenDir(EdenPath::SDMCDir, save_base, "sdmc");
    RedirectEdenDir(EdenPath::ScreenshotsDir, save_base, "screenshots");
    RedirectEdenDir(EdenPath::TASDir, save_base, "tas");
    RedirectEdenDir(EdenPath::PlayTimeDir, save_base, "play_time");
    RedirectEdenDir(EdenPath::AmiiboDir, save_base, "amiibo");
    RedirectEdenDir(EdenPath::LoadDir, save_base, "load");
    RedirectEdenDir(EdenPath::DumpDir, save_base, "dump");

    // Fixed/system-type data: goes under RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY.
    RedirectEdenDir(EdenPath::ConfigDir, system_base, "config");
    RedirectEdenDir(EdenPath::KeysDir, system_base, "keys");
    RedirectEdenDir(EdenPath::CacheDir, system_base, "cache");
    RedirectEdenDir(EdenPath::ShaderDir, system_base, "cache/shader");
    RedirectEdenDir(EdenPath::LogDir, system_base, "log");
    RedirectEdenDir(EdenPath::CrashDumpsDir, system_base, "crash_dumps");

    // Root path that a couple of rarely-used secondary subsystems read
    // directly (a nested-libretro-core loader's default firmware directory,
    // and the hactool integration helper's temp-extraction and tool-search
    // paths). Pointed at the system directory itself, with no subfolder, so
    // nothing is ever written under %APPDATA% even for these.
    {
        std::error_code ec;
        std::filesystem::create_directories(system_base, ec);
        Common::FS::SetEdenPath(EdenPath::EdenDir, system_base);
    }

    LOG_INFO(Frontend, "libretro: redirected eden data - system={} save={}", system_base.string(),
              save_base.string());
}


// Reads a single core option's current value, or empty if unavailable.
std::string ReadOption(const char* key) {
    struct retro_variable v {
        key, nullptr
    };
    if (g_environ_cb && g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value) {
        return v.value;
    }
    return {};
}

// Builds a Common::Log::Filter from the eden_log_* debugging options and
// actually applies it via Common::Log::SetGlobalFilter() - Settings::values
// .log_filter on its own does nothing; every other frontend (see
// yuzu/main_window.cpp) parses it into a real Filter and applies that
// explicitly, which this core never did until now (the previous hardcoded
// Settings::values.log_filter.SetValue() call in retro_init() was silently
// inert for that reason - it set a value nothing ever read).
void ApplyLogFilterFromOptions() {
    const std::string level_str = ReadOption("eden_log_level");
    const char* level_name = "Info";
    if (level_str == "Critical") level_name = "Critical";
    else if (level_str == "Error") level_name = "Error";
    else if (level_str == "Warning") level_name = "Warning";
    else if (level_str == "Debug") level_name = "Debug";
    else if (level_str == "Trace") level_name = "Trace";

    std::string filter_str = std::string("*:") + level_name;
    // Always-on: cheap, useful presentation/frame-pacing detail regardless of
    // the global level.
    filter_str += " Service.VI:Debug Service.AM:Debug Service.Nvnflinger:Debug";

    if (ReadOption("eden_log_render") == "On") {
        filter_str += " Render:Debug Render.Vulkan:Debug Render.OpenGL:Debug Render.Software:Debug";
    }
    if (ReadOption("eden_log_gpu") == "On") {
        filter_str += " HW.GPU:Debug";
    }

    Common::Log::Filter filter;
    filter.ParseFilterString(filter_str);
    Common::Log::SetGlobalFilter(filter);
    Settings::values.log_filter.SetValue(filter_str);

    LOG_INFO(Frontend, "libretro: applied log filter: {}", filter_str);
}

// Every Settings::ControllerType except Handheld, which isn't a per-port
// device choice at all - it's a separate, implicit NpadIdType::Handheld slot
// Eden's HID core manages itself based on the Docked Mode option, not
// something selected per-port here (see emulated_controller.cpp).
static const retro_controller_description pad_types[] = {
    {"Pro Controller", RETRO_DEVICE_JOYPAD},
    {"Joy-Con Pair", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0)},
    {"Joy-Con Left", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1)},
    {"Joy-Con Right", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 2)},
    {"GameCube Controller", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 3)},
    {"Poke Ball Plus", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 4)},
    {"NES Controller", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 5)},
    {"SNES Controller", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 6)},
    {"N64 Controller", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 7)},
    {"Sega Genesis Controller", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 8)},
    {nullptr, 0}
};
constexpr unsigned kNumPadTypes = 10;

static const struct retro_controller_info port_info[] = {
    {pad_types, kNumPadTypes}, {pad_types, kNumPadTypes}, {pad_types, kNumPadTypes},
    {pad_types, kNumPadTypes}, {pad_types, kNumPadTypes}, {pad_types, kNumPadTypes},
    {pad_types, kNumPadTypes}, {pad_types, kNumPadTypes}, {nullptr, 0}
};

unsigned g_port_device_type[8] = {};

// Decodes the libretro "device" value RetroArch passes to
// retro_set_controller_port_device() - driven by its own
// Quick Menu > Controls > Port N > Device Type menu, populated from the
// pad_types[] list above - into the Settings::ControllerType Eden's HID core
// actually wants. Defaults to Pro Controller for RETRO_DEVICE_NONE or
// anything unrecognised - it's accepted everywhere, including Docked mode
// (unlike Handheld, which real hardware and some games specifically reject
// while docked - see the pad_types[] comment above for why it's excluded).
Settings::ControllerType MapDeviceType(unsigned device) {
    switch (device) {
    case RETRO_DEVICE_JOYPAD:
        return Settings::ControllerType::ProController;
    case RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0):
        return Settings::ControllerType::DualJoyconDetached;
    case RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1):
        return Settings::ControllerType::LeftJoycon;
    case RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 2):
        return Settings::ControllerType::RightJoycon;
    case RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 3):
        return Settings::ControllerType::GameCube;
    case RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 4):
        return Settings::ControllerType::Pokeball;
    case RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 5):
        return Settings::ControllerType::NES;
    case RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 6):
        return Settings::ControllerType::SNES;
    case RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 7):
        return Settings::ControllerType::N64;
    case RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 8):
        return Settings::ControllerType::SegaGenesis;
    default:
        return Settings::ControllerType::ProController;
    }
}

// Connects and (re)types every player port, then tells Eden's HID core to
// pick the change up. Shared by retro_load_game() (first load) and
// retro_set_controller_port_device() (RetroArch calls this live whenever the
// user changes Port N's Device Type in Quick Menu > Controls - including
// *after* retro_load_game() has already run, which is exactly why a type
// change only ever took effect on the next full session rather than
// immediately or even "on next restart": the stored g_port_device_type was
// being applied once at load, but the frontend's actual call frequently
// arrives after that point has already passed).
void ApplyControllerPorts() {
    if (!g_system) {
        return;
    }
    for (int i = 0; i < 8; ++i) {
        auto& p = Settings::values.players.GetValue()[i];
        p.connected = true;
        p.controller_type = MapDeviceType(g_port_device_type[i]);
    }
    g_system->HIDCore().ReloadInputDevices();
}

} // namespace

extern "C" {

RETRO_API void retro_set_environment(retro_environment_t cb) {
    g_environ_cb = cb;

    bool no_content = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

    // Core Options V2, with everything new (the debugging tools below) under
    // its own "Debugging" category - the older flat list further down is
    // only a fallback for frontends that don't support V2 (unlikely for any
    // RetroArch from the last several years, but costs little to keep).
    static const struct retro_core_option_v2_category categories[] = {
        {"debugging", "Debugging",
         "Logging controls for comparing this core's behaviour against standalone Eden - "
         "e.g. tracking down a scene that runs fine standalone but not here."},
        {nullptr, nullptr, nullptr},
    };

    static const struct retro_core_option_v2_definition definitions[] = {
        {"eden_renderer", "Renderer", nullptr, nullptr, nullptr, nullptr,
         {{"Vulkan", nullptr}, {"OpenGL", nullptr}, {"Software", nullptr}, {nullptr, nullptr}},
         "Vulkan"},
        {"eden_resolution", "Internal Resolution", nullptr, nullptr, nullptr, nullptr,
         {{"1x (Native)", nullptr}, {"2x (~4K)", nullptr}, {"3x (~6K)", nullptr},
          {"4x (~8K)", nullptr}, {nullptr, nullptr}},
         "1x (Native)"},
        {"eden_scaling_filter", "Window Adapting Filter", nullptr, nullptr, nullptr, nullptr,
         {{"Bilinear", nullptr}, {"Bicubic", nullptr}, {"Lanczos", nullptr},
          {"ScaleForce", nullptr}, {"FSR", nullptr}, {"NearestNeighbor", nullptr},
          {nullptr, nullptr}},
         "Bilinear"},
        {"eden_anti_aliasing", "Anti-Aliasing", nullptr, nullptr, nullptr, nullptr,
         {{"None", nullptr}, {"FXAA", nullptr}, {"SMAA", nullptr}, {nullptr, nullptr}},
         "None"},
        {"eden_cpu_accuracy", "CPU Accuracy", nullptr, nullptr, nullptr, nullptr,
         {{"Auto", nullptr}, {"Accurate", nullptr}, {"Unsafe", nullptr}, {nullptr, nullptr}},
         "Auto"},
        {"eden_use_docked", "Docked Mode", nullptr, nullptr, nullptr, nullptr,
         {{"Yes", nullptr}, {"No", nullptr}, {nullptr, nullptr}},
         "Yes"},
        {"eden_fastmem", "Fastmem", nullptr, nullptr, nullptr, nullptr,
         {{"Enabled", nullptr}, {"Disabled", nullptr}, {nullptr, nullptr}},
         "Enabled"},
        {"eden_use_frontend_dirs", "Use RetroArch System/Save Directories", nullptr,
         "Redirects Eden's data (NAND/SDMC/saves/config/keys/cache/logs) into RetroArch's own "
         "system/save directories instead of Eden's normal %APPDATA%/Eden (or <retroarch>/user "
         "in portable mode) location. Disable to use a standalone Eden install's existing data.",
         nullptr, nullptr,
         {{"Enabled", nullptr}, {"Disabled", nullptr}, {nullptr, nullptr}},
         "Enabled"},
        {"eden_audio_output", "Audio Output", nullptr, nullptr, nullptr, nullptr,
         {{"Host (direct)", nullptr}, {"Frontend (libretro)", nullptr}, {nullptr, nullptr}},
         "Host (direct)"},
        {"eden_online_enable", "Eden Online Play", nullptr,
         "Eden's own room-based multiplayer. RetroArch's netplay can't drive this core (no save "
         "state support), but this tunnels the game's own LAN multiplayer between peers instead.",
         nullptr, nullptr,
         {{"Disabled", nullptr}, {"Enabled", nullptr}, {nullptr, nullptr}},
         "Disabled"},
        {"eden_online_server", "Eden Room Server", nullptr, nullptr, nullptr, nullptr,
         {{"127.0.0.1", nullptr}, {nullptr, nullptr}},
         "127.0.0.1"},
        {"eden_online_port", "Eden Room Port", nullptr, nullptr, nullptr, nullptr,
         {{"24872", nullptr}, {nullptr, nullptr}},
         "24872"},
        {"eden_online_nickname", "Eden Online Nickname", nullptr, nullptr, nullptr, nullptr,
         {{"Player", nullptr}, {nullptr, nullptr}},
         "Player"},
        // --- Debugging category ---
        {"eden_log_level", "Global Log Level", "Log Level",
         "Minimum severity logged for every category not overridden below. Trace is extremely "
         "verbose and will noticeably slow emulation down; Debug is the usual choice for "
         "troubleshooting.", nullptr, "debugging",
         {{"Critical", nullptr}, {"Error", nullptr}, {"Warning", nullptr}, {"Info", nullptr},
          {"Debug", nullptr}, {"Trace", nullptr}, {nullptr, nullptr}},
         "Info"},
        {"eden_log_render", "Rendering/Pipeline Log", "Rendering/Pipeline",
         "Debug-level logging for Render, Render.Vulkan, Render.OpenGL and Render.Software - "
         "shader/pipeline creation, renderer init, per-frame renderer messages.", nullptr,
         "debugging",
         {{"Off", nullptr}, {"On", nullptr}, {nullptr, nullptr}},
         "Off"},
        {"eden_log_gpu", "GPU/Engine Log", "GPU/Engine",
         "Debug-level logging for HW.GPU - the Maxwell command processor and GPU thread, "
         "including per-frame timing-relevant messages.", nullptr, "debugging",
         {{"Off", nullptr}, {"On", nullptr}, {nullptr, nullptr}},
         "Off"},
        {"eden_log_fps", "Frame Timing Log", "Frame Timing",
         "Logs this core's own measured frame interval/FPS periodically to eden_log.txt (tagged "
         "[FPS]), independent of Eden's internal emulation speed - for comparing a slow scene "
         "against standalone Eden's own FPS counter to see whether the slowdown is in Eden's "
         "emulation or specific to this core's render/readback path.", nullptr, "debugging",
         {{"Off", nullptr}, {"On", nullptr}, {nullptr, nullptr}},
         "Off"},
        {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr},
    };

    static struct retro_core_options_v2 options_v2 {
        const_cast<struct retro_core_option_v2_category*>(categories),
        const_cast<struct retro_core_option_v2_definition*>(definitions),
    };

    unsigned options_version = 0;
    if (!cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &options_version)) {
        options_version = 0;
    }
    if (options_version >= 2) {
        cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_v2);
    } else {
        // Fallback for anything that predates Core Options V2: same option
        // keys/values, just flattened and with no Debugging grouping.
        static const struct retro_variable vars[] = {
            {"eden_renderer", "Renderer; Vulkan|OpenGL|Software"},
            {"eden_resolution", "Internal Resolution; 1x (Native)|2x (~4K)|3x (~6K)|4x (~8K)"},
            {"eden_scaling_filter",
             "Window Adapting Filter; Bilinear|Bicubic|Lanczos|ScaleForce|FSR|NearestNeighbor"},
            {"eden_anti_aliasing", "Anti-Aliasing; None|FXAA|SMAA"},
            {"eden_cpu_accuracy", "CPU Accuracy; Auto|Accurate|Unsafe"},
            {"eden_use_docked", "Docked Mode; Yes|No"},
            {"eden_fastmem", "Fastmem; Enabled|Disabled"},
            {"eden_use_frontend_dirs", "Use RetroArch System/Save Directories; Enabled|Disabled"},
            {"eden_audio_output", "Audio Output; Host (direct)|Frontend (libretro)"},
            {"eden_online_enable", "Eden Online Play; Disabled|Enabled"},
            {"eden_online_server", "Eden Room Server; 127.0.0.1"},
            {"eden_online_port", "Eden Room Port; 24872"},
            {"eden_online_nickname", "Eden Online Nickname; Player"},
            {"eden_log_level", "Debugging > Global Log Level; Info|Debug|Trace|Warning|Error|Critical"},
            {"eden_log_render", "Debugging > Rendering/Pipeline Log; Off|On"},
            {"eden_log_gpu", "Debugging > GPU/Engine Log; Off|On"},
            {"eden_log_fps", "Debugging > Frame Timing Log; Off|On"},
            {nullptr, nullptr},
        };
        cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
    }

    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)port_info);
    // Tell the frontend up front that state serialization is not usable for
    // frame-sensitive features. RetroArch keys netplay and rerecording off
    // this, so declaring it means those are cleanly reported as unavailable
    // instead of being offered and then failing mid-session.
    uint64_t quirks = RETRO_SERIALIZATION_QUIRK_INCOMPLETE;
    cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) {
    g_video_cb = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) {
    g_audio_sample_cb = cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    g_audio_batch_cb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb) {
    g_input_poll_cb = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb) {
    g_input_state_cb = cb;
}

RETRO_API void retro_init() {
    // Must happen before any Common::FS::GetEdenPath() call (including the
    // key-import block just below), or those calls will already have latched
    // onto the old %APPDATA%/portable paths.
    RedirectEdenPathsToFrontend();

    Common::Log::Initialize();
    Common::Log::Start();

    LOG_INFO(Frontend, "libretro core: retro_init() starting");

    g_system = std::make_unique<Core::System>();
    g_emu_window = std::make_unique<LibretroCore::RetroEmuWindow>();
    g_input_subsystem = std::make_shared<InputCommon::InputSubsystem>();
    g_input_subsystem->Initialize();

    g_system->Initialize();
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    // Audio output path. Default is "host": Eden opens its own audio device
    // and plays directly, which is how this core behaved before the libretro
    // route existed and is what actually sounds correct today.
    //
    // The libretro route hands samples to the frontend instead, which is
    // tidier in principle (frontend volume, recording, per-core mixing) but
    // sounds worse in practice: nothing throttles the emulated renderer here
    // the way a real device's buffer does, so it produces audio in bursts
    // rather than at a steady rate. It's offered as an option rather than
    // imposed, and the choice is re-read in retro_load_game.
    g_use_frontend_audio = false;
    Settings::values.sink_id.SetValue(Settings::AudioEngine::Auto);
    Settings::values.cpuopt_fastmem.SetValue(true);
    Settings::values.cpuopt_fastmem_exclusives.SetValue(true);
    Settings::values.log_flush_line.SetValue(true);
    // Actually applies the eden_log_* debugging options (see
    // ApplyLogFilterFromOptions' comment) - the hardcoded Settings::values
    // .log_filter.SetValue() this replaces never did anything, since nothing
    // ever called Common::Log::SetGlobalFilter() to make it take effect.
    ApplyLogFilterFromOptions();
    g_system->ApplySettings();
    g_system->SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    g_system->SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    g_system->GetFileSystemController().CreateFactories(*g_system->GetFilesystem());
    g_system->GetUserChannel().clear();

    // Adopt keys from any other Switch emulator installed on this machine.
    // A RetroArch user may well have never run Eden itself, but is likely to
    // have one of these already set up; copying rather than reading in place
    // keeps Eden's own key directory the single source of truth afterwards.
    {
        const auto keys_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
        // .../<roaming>/eden/keys -> .../<roaming>
        const auto roaming = keys_dir.parent_path().parent_path();
        for (const auto& emu : {"suyu", "yuzu", "sudachi", "citron", "Ryujinx"}) {
            const auto src_dir = roaming / emu / "keys";
            std::error_code ec;
            if (!std::filesystem::exists(src_dir, ec)) {
                continue;
            }
            LOG_INFO(Frontend, "libretro: found {} key directory at {}", emu, src_dir.string());
            Common::FS::CreateDir(keys_dir);
            for (const auto& name : {"prod.keys", "title.keys", "console.keys"}) {
                const auto src = src_dir / name;
                const auto dst = keys_dir / name;
                // Never overwrite: Eden's own keys, and anything adopted from
                // an earlier emulator in this list, take precedence.
                if (std::filesystem::exists(src, ec) && !std::filesystem::exists(dst, ec)) {
                    std::filesystem::copy_file(src, dst, ec);
                    if (!ec) {
                        LOG_INFO(Frontend, "libretro: adopted {} from {}", name, emu);
                    }
                }
            }
        }
    }

    // Load keys from RetroArch system directory if available
    // Users can place prod.keys and title.keys in <system_dir>/eden/keys/
    if (g_environ_cb) {
        const char* system_dir = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir) {
            const auto src_dir = std::filesystem::path(system_dir) / "eden" / "keys";
            const auto dst_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
            LOG_INFO(Frontend, "libretro: checking for keys in: {}", src_dir.string());
            if (std::filesystem::exists(src_dir)) {
                Common::FS::CreateDir(dst_dir);
                for (const auto& name : {"prod.keys", "title.keys", "console.keys"}) {
                    auto src = src_dir / name;
                    auto dst = dst_dir / name;
                    if (std::filesystem::exists(src) && !std::filesystem::exists(dst)) {
                        std::error_code ec;
                        std::filesystem::copy_file(src, dst, ec);
                        if (!ec) {
                            LOG_INFO(Frontend, "libretro: copied {} from RetroArch system dir", name);
                        }
                    }
                }
            }
        }
    }

    // Bring up Eden's own room-based multiplayer. RetroArch's netplay can't
    // drive this core (see retro_serialize_size() for why), but Eden's online
    // play is peer-to-peer at the emulated-console level and doesn't depend on
    // frontend savestates, so it works here exactly as it does in the Qt
    // frontend once the user joins a room.
    if (!Network::Init()) {
        LOG_WARNING(Frontend, "libretro: could not initialise the network layer; "
                              "eden online play will be unavailable in this session");
    } else {
        LOG_INFO(Frontend, "libretro: eden room networking initialised");
    }

    g_system->HIDCore().ReloadInputDevices();
    LOG_INFO(Frontend, "libretro core: retro_init() complete");
}

RETRO_API void retro_deinit() {
    Network::Shutdown();
    g_emu_window.reset();
    g_system.reset();
    if (g_input_subsystem) { g_input_subsystem->Shutdown(); g_input_subsystem.reset(); }
    Common::Log::Stop();
}

RETRO_API unsigned retro_api_version() {
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
    std::memset(info, 0, sizeof(*info));
    info->library_name = "eden";
    info->library_version = "0.04";
    info->valid_extensions = "nsp|xci|nca|nro";
    info->need_fullpath = true;
    info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
    info->geometry.base_width = kFrameWidth;
    info->geometry.base_height = kFrameHeight;
    // Allow up to 4x so raising the internal resolution option doesn't get
    // clamped back down to 720p by the frontend. RetroArch sizes its texture
    // from max_*, and a core may deliver anything up to it.
    info->geometry.max_width = kFrameWidth * 4;
    info->geometry.max_height = kFrameHeight * 4;
    info->geometry.aspect_ratio = static_cast<float>(kFrameWidth) / static_cast<float>(kFrameHeight);
    info->timing.fps = 60.0;
    info->timing.sample_rate = 48000.0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    if (port >= 8) {
        return;
    }
    g_port_device_type[port] = device;
    // RetroArch calls this both before AND after retro_load_game() - before,
    // on initial content load with a saved Device Type; after, any time the
    // user changes it live from Quick Menu > Controls. g_system only exists
    // once retro_init() has run, so this is a no-op (safely deferred to
    // retro_load_game()'s own call to ApplyControllerPorts()) for the
    // before-load case, and applies immediately for the after-load case.
    ApplyControllerPorts();
}

RETRO_API void retro_reset() {
    LOG_WARNING(Frontend, "libretro core: retro_reset() requested but not yet implemented "
                          "(would need Core::System restart without a full unload/reload)");
}

namespace {
using VB = InputCommon::VirtualGamepad::VirtualButton;
struct RetroToVirtual {
    unsigned retro_id;
    VB virtual_button;
};
constexpr RetroToVirtual kButtonMap[] = {
    {RETRO_DEVICE_ID_JOYPAD_A, VB::ButtonA},
    {RETRO_DEVICE_ID_JOYPAD_B, VB::ButtonB},
    {RETRO_DEVICE_ID_JOYPAD_X, VB::ButtonX},
    {RETRO_DEVICE_ID_JOYPAD_Y, VB::ButtonY},
    {RETRO_DEVICE_ID_JOYPAD_L, VB::TriggerL},
    {RETRO_DEVICE_ID_JOYPAD_R, VB::TriggerR},
    {RETRO_DEVICE_ID_JOYPAD_L2, VB::TriggerZL},
    {RETRO_DEVICE_ID_JOYPAD_R2, VB::TriggerZR},
    {RETRO_DEVICE_ID_JOYPAD_L3, VB::StickL},
    {RETRO_DEVICE_ID_JOYPAD_R3, VB::StickR},
    {RETRO_DEVICE_ID_JOYPAD_START, VB::ButtonPlus},
    {RETRO_DEVICE_ID_JOYPAD_SELECT, VB::ButtonMinus},
    {RETRO_DEVICE_ID_JOYPAD_UP, VB::ButtonUp},
    {RETRO_DEVICE_ID_JOYPAD_DOWN, VB::ButtonDown},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, VB::ButtonLeft},
    {RETRO_DEVICE_ID_JOYPAD_RIGHT, VB::ButtonRight},
};
bool g_prev_buttons[8][20] = {};


} // namespace

namespace {

// Drains whatever the emulated audio renderer produced since the last call
// and hands it to the frontend. upload_batch takes frames (L+R pairs), not
// individual samples. Feed the frontend a steady ~1 frame of audio per call
// rather than whatever has piled up: RetroArch resamples against its own
// clock and expects roughly sample_rate/fps frames each call, and handing it
// a quarter second in one lump and nothing for the next 15 calls is what made
// the output screech. Anything beyond a small backlog is dropped so latency
// can't creep up instead.
//
// Called either from retro_run() (frontends too old to support
// RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK) or from FrontendAudioCallback()
// below, never both - see g_audio_callback_registered.
void DeliverPendingAudio() {
    if (!g_use_frontend_audio || !g_audio_batch_cb || !g_game_loaded) {
        return;
    }

    constexpr size_t kFramesPerCall = 48000 / 60; // stereo frames
    constexpr size_t kMaxBacklogFrames = kFramesPerCall * 6;

    static std::vector<s16> pending; // interleaved L,R awaiting delivery
    std::vector<s16> drained;
    AudioCore::Sink::LibretroSampleQueue::Instance().Drain(drained);
    if (!drained.empty()) {
        pending.insert(pending.end(), drained.begin(), drained.end());
    }

    // Trim from the front if we've fallen behind; stale audio is worse than a
    // short gap.
    if (pending.size() > kMaxBacklogFrames * 2) {
        const size_t excess = pending.size() - kMaxBacklogFrames * 2;
        pending.erase(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(excess));
    }

    const size_t frames = std::min(kFramesPerCall, pending.size() / 2);
    if (frames > 0) {
        g_audio_batch_cb(pending.data(), frames);
        pending.erase(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(frames * 2));
    }
}

// Called by the frontend's own audio thread once RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK
// is registered, independently of whether retro_run() is being called at all.
void RETRO_CALLCONV FrontendAudioCallback() {
    DeliverPendingAudio();
}

// The actual reason to register SET_AUDIO_CALLBACK at all: its set_state
// half is the one libretro hook that still fires while the frontend has
// stopped calling retro_run() entirely (RetroArch's menu open, content
// paused, rewind, etc.). Without this, Eden's own CPU/GPU threads - kicked
// off once by g_system->Run() in retro_load_game() and never revisited -
// keep running regardless of whether the frontend is still ticking us, which
// is why the game previously kept advancing behind the RetroArch menu.
// enabled=false means "the frontend has gone quiet"; enabled=true means
// "resume normal operation".
void RETRO_CALLCONV FrontendAudioSetState(bool enabled) {
    if (!g_system || !g_game_loaded) {
        return;
    }
    if (enabled) {
        if (g_system->IsPaused()) {
            g_system->Run();
        }
    } else {
        if (!g_system->IsPaused()) {
            g_system->Pause();
        }
    }
}

} // namespace

namespace {

// Polls for any core-option change once per frame and applies whichever of
// our live-updatable options actually moved. A single
// RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE check covers all of them - no need
// for one call per option.
void CheckForLiveOptionChanges() {
    bool updated = false;
    if (!g_environ_cb || !g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) ||
        !updated) {
        return;
    }

    // Docked Mode: lets Quick Menu > Options > Docked Mode take effect
    // immediately on a running game, the same way Eden's own Qt UI applies
    // it live - without this, eden_use_docked is only ever read once in
    // retro_load_game(), so toggling it mid-session would silently do
    // nothing until the next reload. Mirrors OnDockedModeChanged() in
    // yuzu/configuration/configure_input.cpp, which is Qt-widget code we
    // can't link from a libretro core; the actual logic there has no Qt
    // dependency, so it's reproduced directly here instead.
    if (g_system && g_game_loaded) {
        const std::string docked_str = ReadOption("eden_use_docked");
        if (!docked_str.empty()) {
            const bool new_docked = docked_str == "Yes";
            const bool was_docked = Settings::values.use_docked_mode.GetValue() ==
                                     Settings::ConsoleMode::Docked;
            if (new_docked != was_docked) {
                Settings::values.use_docked_mode.SetValue(
                    new_docked ? Settings::ConsoleMode::Docked : Settings::ConsoleMode::Handheld);
                if (g_system->IsPoweredOn()) {
                    g_system->GetAppletManager().OperationModeChanged();
                    LOG_INFO(Frontend, "libretro: docked mode changed live -> {}",
                             new_docked ? "Docked" : "Handheld");
                }
            }
        }
    }

    ApplyLogFilterFromOptions();
    g_log_fps_enabled = ReadOption("eden_log_fps") == "On";
}

} // namespace

RETRO_API void retro_run() {
    CheckForLiveOptionChanges();

    if (g_geometry_dirty && g_environ_cb) {
        retro_game_geometry geom{};
        geom.base_width = kFrameWidth * g_output_scale;
        geom.base_height = kFrameHeight * g_output_scale;
        geom.max_width = kFrameWidth * 4;
        geom.max_height = kFrameHeight * 4;
        geom.aspect_ratio = (float)kFrameWidth / (float)kFrameHeight;
        g_environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
        g_geometry_dirty = false;
    }
    if (g_input_poll_cb) {
        g_input_poll_cb();
    }

    // Bridge libretro input → Eden HID via VirtualGamepad
    if (g_input_state_cb && g_input_subsystem && g_game_loaded) {
        auto* vgp = g_input_subsystem->GetVirtualGamepad();
        if (vgp) {
            for (unsigned port = 0; port < 8; ++port) {
                for (const auto& m : kButtonMap) {
                    const bool pressed = g_input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, m.retro_id) != 0;
                    const int idx = static_cast<int>(m.virtual_button);
                    if (pressed != g_prev_buttons[port][idx]) {
                        g_prev_buttons[port][idx] = pressed;
                        vgp->SetButtonState(port, m.virtual_button, pressed);
                    }
                }
                // Left analog stick
                const float lx = g_input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) /
                    32768.0f;
                const float ly =
                    g_input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                     RETRO_DEVICE_ID_ANALOG_Y) /
                    -32768.0f;
                vgp->SetStickPosition(port, InputCommon::VirtualGamepad::VirtualStick::Left, lx, ly);
                // Right analog stick
                const float rx =
                    g_input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                                     RETRO_DEVICE_ID_ANALOG_X) /
                    32768.0f;
                const float ry =
                    g_input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                                     RETRO_DEVICE_ID_ANALOG_Y) /
                    -32768.0f;
                vgp->SetStickPosition(port, InputCommon::VirtualGamepad::VirtualStick::Right, rx, ry);
            }
        }
    }

    static unsigned frame_counter = 0;
    ++frame_counter;

    if (frame_counter <= 3 || (frame_counter % 600) == 0) {
        LOG_INFO(Frontend, "libretro: retro_run frame {}, game_loaded={}", frame_counter, g_game_loaded);
        fprintf(stderr, "[eden-libretro] retro_run frame %u, game_loaded=%d\n", frame_counter, g_game_loaded);
        fflush(stderr);
    }

    // "eden_log_fps" debugging option: measures this core's own actual
    // retro_run() call interval (i.e. the rate RetroArch is driving us at,
    // and the time our own readback/bridge work takes within that), entirely
    // independent of whatever FPS counter Eden's internals may report. The
    // point is comparison against standalone Eden during the same scene: if
    // both report the same drop, the slowdown is in Eden's emulation itself;
    // if this core reports noticeably worse than standalone's own counter
    // for the same moment, the overhead is specific to this core's
    // render-readback/bridge path rather than Eden proper.
    if (g_log_fps_enabled && g_game_loaded) {
        static auto last_time = std::chrono::steady_clock::now();
        static unsigned sample_count = 0;
        static double accum_ms = 0.0;
        static double worst_ms = 0.0;

        const auto now = std::chrono::steady_clock::now();
        const double delta_ms = std::chrono::duration<double, std::milli>(now - last_time).count();
        last_time = now;

        // Skip the first sample after enabling/loading - it measures however
        // long content load or the option toggle itself took, not a frame.
        if (sample_count > 0 || frame_counter > 1) {
            accum_ms += delta_ms;
            worst_ms = std::max(worst_ms, delta_ms);
            ++sample_count;
        }

        if (sample_count >= 60) {
            const double avg_ms = accum_ms / sample_count;
            const double avg_fps = avg_ms > 0.0 ? 1000.0 / avg_ms : 0.0;
            const double worst_fps = worst_ms > 0.0 ? 1000.0 / worst_ms : 0.0;
            LOG_INFO(Frontend,
                     "[FPS] frame {}: {} samples, avg {:.2f} ms ({:.1f} fps), "
                     "worst {:.2f} ms ({:.1f} fps)",
                     frame_counter, sample_count, avg_ms, avg_fps, worst_ms, worst_fps);
            sample_count = 0;
            accum_ms = 0.0;
            worst_ms = 0.0;
        }
    }

    // Only handle audio here ourselves if no async audio callback took over
    // in retro_load_game() - otherwise FrontendAudioCallback() is already
    // draining the same queue from the frontend's audio thread, and doing it
    // in both places would just race for no benefit.
    if (!g_audio_callback_registered) {
        DeliverPendingAudio();
    }

    if (g_video_cb && g_system && g_game_loaded) {
        auto& renderer = g_system->Renderer();
        if (renderer.IsHeadless()) {
            const auto& frame = renderer.GetLastRenderedFrame();
            if (!frame.empty()) {
                // No channel swap: the renderer produces VK_FORMAT_B8G8R8A8,
                // i.e. B,G,R,A in ascending byte order, and libretro's
                // XRGB8888 is the 32-bit word 0xXXRRGGBB, which on a
                // little-endian host is that same B,G,R,X byte order. Passing
                // the buffer straight through is correct; an earlier R<->B
                // swap here was the cause of red rendering as blue.
                g_video_cb(frame.data(), renderer.GetHeadlessWidth(),
                           renderer.GetHeadlessHeight(),
                           renderer.GetHeadlessWidth() * 4);
                return;
            }
        }
        if (frame_counter <= 5 || (frame_counter % 300) == 0) {
            LOG_WARNING(Frontend, "libretro: frame {} - no rendered frame available, sending black",
                        frame_counter);
        }
        static const std::vector<u32> black_frame(
            static_cast<size_t>(kFrameWidth) * kFrameHeight, 0xFF000000);
        g_video_cb(black_frame.data(), kFrameWidth, kFrameHeight, kFrameWidth * sizeof(u32));
    }
}

RETRO_API size_t retro_serialize_size() {
    // Returning 0 disables savestates (and, through them, RetroArch netplay
    // and rerecording) in the frontend UI.
    //
    // This isn't a shortcut in the libretro layer: Eden has no state
    // serialization at all, and neither does any other yuzu-derived emulator.
    // A Switch savestate has to capture several GB of guest RAM plus dynarmic
    // CPU state for every guest thread, the whole GPU pipeline (Maxwell
    // registers, in-flight command buffers, texture/shader caches), the audio
    // renderer, and the entire HLE kernel object graph - threads, mutexes,
    // events, shared memory, open filesystem handles and live IPC sessions.
    // Until that exists in core/, there is nothing here to hand back, and
    // faking a partial state would corrupt saves rather than fail cleanly.
    return 0;
}

RETRO_API bool retro_serialize(void* /*data*/, size_t /*size*/) {
    return false;
}

RETRO_API bool retro_unserialize(const void* /*data*/, size_t /*size*/) {
    return false;
}

RETRO_API void retro_cheat_reset() {}

RETRO_API void retro_cheat_set(unsigned /*index*/, bool /*enabled*/, const char* /*code*/) {}

RETRO_API bool retro_load_game(const struct retro_game_info* game) {
    if (!g_system || !g_emu_window || !game || !game->path) {
        LOG_CRITICAL(Frontend, "libretro core: retro_load_game null check failed "
                               "(system={} window={} game={} path={})",
                     !!g_system, !!g_emu_window, !!game, game ? !!game->path : false);
        return false;
    }

    g_game_path = game->path;
    LOG_INFO(Frontend, "libretro core: loading game: {}", g_game_path);

    // Apply core options before loading the game
    if (g_environ_cb) {
        struct retro_variable var;
        var.key = "eden_use_docked";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            Settings::values.use_docked_mode.SetValue(
                std::string(var.value) == "Yes" ? Settings::ConsoleMode::Docked
                                                : Settings::ConsoleMode::Handheld);
        }
        var.key = "eden_cpu_accuracy";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            std::string v(var.value);
            if (v == "Accurate")
                Settings::values.cpu_accuracy.SetValue(Settings::CpuAccuracy::Accurate);
            else if (v == "Unsafe")
                Settings::values.cpu_accuracy.SetValue(Settings::CpuAccuracy::Unsafe);
            else
                Settings::values.cpu_accuracy.SetValue(Settings::CpuAccuracy::Auto);
        }
        var.key = "eden_resolution";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            // Matches by leading token so this doesn't depend on the exact
            // "(~4K)"-style suffix text in the option strings above.
            const std::string v(var.value);
            auto res = Settings::ResolutionSetup::Res1X;
            if (v.rfind("2x", 0) == 0) {
                res = Settings::ResolutionSetup::Res2X;
                g_output_scale = 2;
            } else if (v.rfind("3x", 0) == 0) {
                res = Settings::ResolutionSetup::Res3X;
                g_output_scale = 3;
            } else if (v.rfind("4x", 0) == 0) {
                res = Settings::ResolutionSetup::Res4X;
                g_output_scale = 4;
            }
            Settings::values.resolution_setup.SetValue(res);
        }
        var.key = "eden_scaling_filter";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            const std::string v(var.value);
            auto f = Settings::ScalingFilter::Bilinear;
            if (v == "Bicubic") f = Settings::ScalingFilter::Bicubic;
            else if (v == "Lanczos") f = Settings::ScalingFilter::Lanczos;
            else if (v == "ScaleForce") f = Settings::ScalingFilter::ScaleForce;
            else if (v == "FSR") f = Settings::ScalingFilter::Fsr;
            else if (v == "NearestNeighbor") f = Settings::ScalingFilter::NearestNeighbor;
            Settings::values.scaling_filter.SetValue(f);
        }
        var.key = "eden_anti_aliasing";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            const std::string v(var.value);
            auto aa = Settings::AntiAliasing::None;
            if (v == "FXAA") aa = Settings::AntiAliasing::Fxaa;
            else if (v == "SMAA") aa = Settings::AntiAliasing::Smaa;
            Settings::values.anti_aliasing.SetValue(aa);
        }
        var.key = "eden_audio_output";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            g_use_frontend_audio = std::string(var.value).rfind("Frontend", 0) == 0;
            Settings::values.sink_id.SetValue(g_use_frontend_audio
                                                  ? Settings::AudioEngine::Libretro
                                                  : Settings::AudioEngine::Auto);
            LOG_INFO(Frontend, "libretro: audio output = {}",
                     g_use_frontend_audio ? "frontend" : "host");
        }
        var.key = "eden_fastmem";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            const bool enabled = std::string(var.value) == "Enabled";
            Settings::values.cpuopt_fastmem.SetValue(enabled);
            Settings::values.cpuopt_fastmem_exclusives.SetValue(enabled);
        }
        ApplyLogFilterFromOptions();
        g_log_fps_enabled = ReadOption("eden_log_fps") == "On";
        g_system->ApplySettings();

        // Explicitly connect every player slot before loading. Eden (unlike
        // suyu, which auto-connects Player1 as a fallback regardless of this
        // setting - see hid_core/frontend/emulated_controller.cpp) only
        // connects a controller when Settings::values.players[i].connected is
        // true, and nothing else in this minimal embedding ever sets that.
        // Without this, games that strictly check IsConnected() before
        // accepting input (Smash Ultimate, confirmed by testing) silently
        // never receive a single button press, while games that just read
        // raw HID state regardless happen to work anyway - which is exactly
        // the split we saw between titles before this fix.
        ApplyControllerPorts();

        g_emu_window->UpdateCurrentFramebufferLayout(kFrameWidth * g_output_scale, kFrameHeight * g_output_scale);
        g_geometry_dirty = true;

        // Join an Eden room if the user configured one. Done here rather than
        // in retro_init so the options the frontend collected are already
        // available, and so a failed join can't stop the game from booting
        // single-player.
        // If the frontend has a netplay session running, treat that as a
        // request for online play even when the core option is off. RetroArch's
        // own netplay can't synchronise this core (see retro_serialize_size),
        // but a user who started one has clearly asked to play with someone,
        // and Eden's room system can carry that - so the session is used as
        // the trigger and the client index decides who hosts.
        unsigned netplay_index = 0;
        const bool frontend_netplay =
            g_environ_cb(RETRO_ENVIRONMENT_GET_NETPLAY_CLIENT_INDEX, &netplay_index);
        if (frontend_netplay) {
            LOG_INFO(Frontend, "libretro: frontend netplay active (client index {}); "
                               "bringing up eden online play", netplay_index);
        }

        var.key = "eden_online_enable";
        var.value = nullptr;
        const bool option_enabled =
            g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
            std::string(var.value) == "Enabled";
        if (option_enabled || frontend_netplay) {
            std::string server = "127.0.0.1";
            std::string nickname = "Player";
            u16 port = 24872;

            var.key = "eden_online_server";
            var.value = nullptr;
            if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
                server = var.value;
            }
            var.key = "eden_online_nickname";
            var.value = nullptr;
            if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
                nickname = var.value;
            }
            var.key = "eden_online_port";
            var.value = nullptr;
            if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
                port = static_cast<u16>(std::strtoul(var.value, nullptr, 10));
            }

            // Keep peers in one netplay session from colliding on nickname,
            // which the room rejects as a duplicate.
            if (frontend_netplay && netplay_index > 0) {
                nickname += "_" + std::to_string(netplay_index);
            }

            if (auto member = Network::GetRoomMember().lock()) {
                LOG_INFO(Frontend, "libretro: joining eden room {}:{} as '{}'", server, port,
                         nickname);
                member->Join(nickname, server.c_str(), port);
            } else {
                LOG_WARNING(Frontend, "libretro: online play requested but the room member is "
                                      "unavailable; continuing single-player");
            }
        }
    }

    Service::AM::FrontendAppletParameters load_parameters{};
    load_parameters.applet_id = Service::AM::AppletId::Application;

    const Core::SystemResultStatus result =
        g_system->Load(*g_emu_window, g_game_path, load_parameters);
    if (result != Core::SystemResultStatus::Success) {
        LOG_CRITICAL(Frontend, "libretro core: Load() failed with status {}",
                     static_cast<u32>(result));
        return false;
    }

    // The GUI's EmuThread does these steps between Load and Run — without them the GPU thread
    // never starts and the CPU manager doesn't know the GPU is ready, causing the game to stall
    // before it ever reaches display setup.
    auto& gpu = g_system->GPU();
    gpu.ObtainContext();
    gpu.ReleaseContext();
    gpu.Start();
    g_system->GetCpuManager().OnGpuReady();

    g_system->Run();
    g_game_loaded = true;

    // Register for the frontend's audio-pause notification (see
    // FrontendAudioSetState's comment) so opening the RetroArch menu, or
    // pausing content, actually pauses Eden's own CPU/GPU threads instead of
    // letting them run on unattended in the background.
    struct retro_audio_callback audio_cb {};
    audio_cb.callback = FrontendAudioCallback;
    audio_cb.set_state = FrontendAudioSetState;
    g_audio_callback_registered = g_environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &audio_cb);
    if (g_audio_callback_registered) {
        LOG_INFO(Frontend, "libretro core: registered async audio callback - "
                            "frontend pause/menu will now pause emulation");
    } else {
        LOG_WARNING(Frontend, "libretro core: frontend doesn't support "
                               "RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK - opening the menu or "
                               "pausing content won't pause eden's own emulation");
    }

    LOG_INFO(Frontend, "libretro core: game loaded and running");
    return true;
}

RETRO_API bool retro_load_game_special(unsigned /*game_type*/, const struct retro_game_info* /*info*/,
                                       size_t /*num_info*/) {
    return false;
}

RETRO_API void retro_unload_game() {
    if (g_system && g_game_loaded) {
        g_output_scale = 1;
        g_geometry_dirty = false;
        g_system->ShutdownMainProcess();
    }
    g_game_loaded = false;
    g_game_path.clear();
    // Leave any room we joined for this game; the next one loaded in this
    // session gets to make its own decision from its own core options.
    if (auto member = Network::GetRoomMember().lock()) {
        if (member->IsConnected()) {
            member->Leave();
        }
    }
    // Drop any audio still queued from the game that just went away, so it
    // can't leak into the next one loaded in this session.
    AudioCore::Sink::LibretroSampleQueue::Instance().Clear();
}

RETRO_API unsigned retro_get_region() {
    return RETRO_REGION_NTSC;
}

RETRO_API void* retro_get_memory_data(unsigned /*id*/) {
    return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned /*id*/) {
    return 0;
}

} // extern "C"

// Eden compat: unlike suyu, Eden's video_core only declares the VMA
// (Vulkan Memory Allocator) interface - it never instantiates the actual
// implementation itself, so every final linked binary that pulls in
// video_core is individually responsible for doing so exactly once (see
// yuzu_cmd/yuzu.cpp and yuzu/main_window.cpp for the same pattern). Without
// this, linking fails with unresolved externals for every vma* symbol
// video_core.lib calls (vmaCreateBuffer, vmaFlushAllocation, etc).
#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
