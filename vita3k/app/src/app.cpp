// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <app/functions.h>

#include <camera/camera.h>
#include <config/functions.h>
#include <config/state.h>
#include <emuenv/state.h>
#include <io/functions.h>
#include <io/state.h>
#include <packages/license.h>
#include <packages/sfo.h>
#include <renderer/functions.h>
#include <renderer/shaders.h>
#include <renderer/state.h>
#include <util/fs.h>
#include <util/log.h>
#include <util/net_utils.h>

#include <SDL3/SDL_camera.h>
#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cstring>

namespace app {

namespace {

constexpr uint32_t perf_frames_size = 20;

bool has_installed_firmware_content(const fs::path &path) {
    return fs::exists(path) && fs::is_directory(path) && !fs::is_empty(path);
}

} // namespace

void reset_perf_metrics(EmuEnvState &emuenv) {
    emuenv.frame_count = 0;
    emuenv.fps = 0;
    emuenv.avg_fps = 0;
    emuenv.min_fps = 0;
    emuenv.max_fps = 0;
    std::fill_n(emuenv.fps_values, perf_frames_size, 0.0f);
    emuenv.current_fps_offset = 0;
    emuenv.ms_per_frame = 0;

    if (!emuenv.renderer)
        return;

    auto &renderer = *emuenv.renderer;
    renderer.perf_overlay.fps = 0;
    renderer.perf_overlay.avg_fps = 0;
    renderer.perf_overlay.min_fps = 0;
    renderer.perf_overlay.max_fps = 0;
    renderer.perf_overlay.ms_per_frame = 0;
    renderer.perf_overlay.fps_values.fill(0.0f);
    renderer.perf_overlay.fps_values_count = 0;
    renderer.perf_overlay.current_fps_offset = 0;
}

void sync_perf_overlay_config(EmuEnvState &emuenv) {
    if (!emuenv.renderer)
        return;

    auto &renderer = *emuenv.renderer;
    renderer.perf_overlay.enabled = emuenv.cfg.performance_overlay;
    renderer.perf_overlay.position = emuenv.cfg.performance_overlay_position;
    renderer.perf_overlay.detail = emuenv.cfg.performance_overlay_detail;
}

void reset_controller_binding(EmuEnvState &emuenv) {
    emuenv.cfg.controller_binds = {
        SDL_GAMEPAD_BUTTON_SOUTH,
        SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_GUIDE,
        SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_LEFT_STICK,
        SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_DPAD_UP,
        SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT
    };
    emuenv.cfg.controller_axis_binds = {
        SDL_GAMEPAD_AXIS_LEFTX,
        SDL_GAMEPAD_AXIS_LEFTY,
        SDL_GAMEPAD_AXIS_RIGHTX,
        SDL_GAMEPAD_AXIS_RIGHTY,
        SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
        SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
    };
    config::serialize_config(emuenv.cfg, emuenv.cfg.config_path);
}

int get_supported_memory_mapping_mask(const EmuEnvState &emuenv, int gpu_idx) {
    int mask = (1 << 0);

    if (gpu_idx < 0)
        gpu_idx = emuenv.cfg.gpu_idx;

    if (emuenv.vulkan_device_info && gpu_idx > 0
        && gpu_idx <= static_cast<int>(emuenv.vulkan_device_info->mapping_method_masks.size())) {
        mask = emuenv.vulkan_device_info->mapping_method_masks[gpu_idx - 1];
    } else if (emuenv.vulkan_device_info && !emuenv.vulkan_device_info->mapping_method_masks.empty()) {
        mask = emuenv.vulkan_device_info->mapping_method_masks[0];
    }

    return mask;
}

void ensure_camera_defaults(Config &cfg) {
    init_default_cameras(cfg);
}

std::vector<std::string> get_available_camera_names() {
    int camera_count = 0;
    auto *cameras = SDL_GetCameras(&camera_count);
    if (!cameras && camera_count <= 0) {
        SDL_ClearError();
        return {};
    }

    std::vector<std::string> camera_names;
    camera_names.reserve(static_cast<size_t>(std::max(camera_count, 0)));
    for (int index = 0; index < camera_count; ++index) {
        const char *camera_name = SDL_GetCameraName(cameras[index]);
        if (camera_name)
            camera_names.emplace_back(camera_name);
    }

    if (cameras)
        SDL_free(cameras);

    return camera_names;
}

std::vector<AdhocAddressOption> get_available_adhoc_address_options() {
    const auto addrs = net_utils::get_all_assigned_addrs();

    std::vector<AdhocAddressOption> options;
    options.reserve(addrs.size());
    for (const auto &addr : addrs) {
        options.push_back({
            addr.addr + " (" + addr.name + ")",
            addr.netMask,
        });
    }

    return options;
}

FirmwareState get_firmware_state(const EmuEnvState &emuenv) {
    return FirmwareState{
        .preinstalled_package = has_installed_firmware_content(emuenv.vita_fs_path / "pd0"),
        .main_firmware = has_installed_firmware_content(emuenv.vita_fs_path / "vs0"),
        .font_package = has_installed_firmware_content(emuenv.vita_fs_path / "sa0"),
    };
}

bool has_firmware_installed(const EmuEnvState &emuenv) {
    return has_installed_firmware_content(emuenv.vita_fs_path / "vs0");
}

bool ensure_current_user(EmuEnvState &emuenv) {
    const auto &users = emuenv.app.user_list.users;

    if (users.empty()) {
        const std::string id = create_user(emuenv, "Vita3K");
        if (id.empty() || !activate_user(emuenv, id))
            return false;

        emuenv.cfg.user_id = id;
        config::serialize_config(emuenv.cfg, emuenv.cfg.config_path);
        return true;
    }

    if (!emuenv.cfg.user_id.empty() && users.contains(emuenv.cfg.user_id))
        return activate_user(emuenv, emuenv.cfg.user_id);

    const auto &first = users.begin();
    if (!activate_user(emuenv, first->first))
        return false;

    emuenv.cfg.user_id = first->first;
    config::serialize_config(emuenv.cfg, emuenv.cfg.config_path);
    return true;
}

bool switch_emulator_path(EmuEnvState &emuenv, const fs::path &vita_fs_path) {
    const fs::path normalized_vita_fs_path = vita_fs_path / "";
    if (normalized_vita_fs_path.empty())
        return false;

    emuenv.vita_fs_path = normalized_vita_fs_path;
    emuenv.cfg.set_vita_fs_path(emuenv.vita_fs_path);

    ::io_deinit(emuenv.io);
    if (!::init(emuenv.io, emuenv.cache_path, emuenv.log_path, emuenv.vita_fs_path, emuenv.cfg.console)) {
        LOG_ERROR("Failed to initialize file system for switched emulator path '{}'.", emuenv.vita_fs_path);
        return false;
    }

    load_users(emuenv);
    emuenv.io.user_id.clear();
    emuenv.io.user_name.clear();

    const auto &users = emuenv.app.user_list.users;
    if (users.empty()) {
        const std::string id = create_user(emuenv, "Vita3K");
        if (id.empty() || !activate_user(emuenv, id))
            return false;
        emuenv.cfg.user_id = id;
    } else {
        const auto &first = users.begin();
        if (!activate_user(emuenv, first->first))
            return false;
        emuenv.cfg.user_id = first->first;
    }

    load_app_times(emuenv);
    if (!scan_apps(emuenv)) {
        LOG_ERROR("Failed to scan apps after switching emulator path to '{}'.", emuenv.vita_fs_path);
        return false;
    }

    set_current_config(emuenv, "");
    return true;
}

bool setup_game_launch(EmuEnvState &emuenv, const std::string &app_path, const bool update_last_time_used) {
    if (!ensure_current_user(emuenv))
        return false;

    if (!set_app_info(emuenv, app_path))
        return false;

    get_license(emuenv, emuenv.io.title_id, emuenv.io.content_id);
    if (update_last_time_used)
        update_last_time_app_used(emuenv, app_path);
    set_current_config(emuenv, app_path);
    reset_perf_metrics(emuenv);
    return true;
}

bool setup_game_launch(EmuEnvState &emuenv, const AppLaunchRequest &launch_request, const bool update_last_time_used) {
    if (!launch_request.direct_app)
        return setup_game_launch(emuenv, launch_request.app_path, update_last_time_used);

    const auto &direct_app = *launch_request.direct_app;
    if (!direct_app.mount || direct_app.title_id.empty() || launch_request.app_path != direct_app.title_id)
        return false;
    if (!ensure_current_user(emuenv))
        return false;

    emuenv.io.app_path = direct_app.title_id;
    emuenv.io.title_id = direct_app.title_id;
    emuenv.io.addcont = direct_app.addcont;
    emuenv.io.content_id = direct_app.content_id;
    emuenv.io.savedata = direct_app.savedata.empty() ? direct_app.title_id : direct_app.savedata;
    emuenv.current_app_title = direct_app.title;
    emuenv.app_info = {
        .app_version = direct_app.app_version,
        .app_category = direct_app.app_category,
        .app_content_id = direct_app.content_id,
        .app_addcont = direct_app.addcont,
        .app_savedata = direct_app.savedata,
        .app_parental_level = direct_app.parental_level,
        .app_short_title = direct_app.short_title,
        .app_title = direct_app.title,
        .app_title_id = direct_app.title_id,
    };
    emuenv.io.app0_mount = direct_app.mount;
    emuenv.direct_app = launch_request.direct_app;

    emuenv.previous_direct_license.reset();
    if (const auto existing = emuenv.license.rif.find(direct_app.title_id); existing != emuenv.license.rif.end()) {
        std::array<uint8_t, 512> previous{};
        std::memcpy(previous.data(), &existing->second, previous.size());
        emuenv.previous_direct_license = previous;
    }
    auto &license = emuenv.license.rif[direct_app.title_id];
    std::memcpy(&license, direct_app.license.data(), direct_app.license.size());

    set_current_config(emuenv, direct_app.title_id);
    reset_perf_metrics(emuenv);
    return true;
}

void prepare_game_launch_overlay(EmuEnvState &emuenv) {
    if (!emuenv.renderer)
        return;

    auto &renderer = *emuenv.renderer;
    renderer.precompile_bg_path.clear();
    renderer.precompile_queue.clear();
    renderer.precompile_total = 0;
    renderer.precompile_progress = 0;
    renderer.precompile_requested = false;
    renderer.precompile_complete.store(false, std::memory_order_relaxed);

    const auto bg_path = emuenv.vita_fs_path / "ux0/app" / emuenv.io.app_path / "sce_sys/pic0.png";
    if (fs::exists(bg_path))
        renderer.precompile_bg_path = fs_utils::path_to_utf8(bg_path);

    if (renderer::get_shaders_cache_hashs(renderer) && emuenv.cfg.shader_cache) {
        renderer.precompile_queue = renderer.shaders_cache_hashs;
        renderer.precompile_progress = 0;
        renderer.precompile_complete.store(false, std::memory_order_relaxed);
        renderer.precompile_requested = true;
    }
}

bool update_runtime_metrics(EmuEnvState &emuenv, LaunchRuntimeMetrics &metrics) {
    sync_perf_overlay_config(emuenv);

    if (emuenv.frame_count == 0)
        return false;

    if (!metrics.tracking_started) {
        metrics.tracking_started = true;
        metrics.last_fps_time = std::chrono::steady_clock::now();
        emuenv.frame_count = 0;
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const uint32_t ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - metrics.last_fps_time).count());
    if (ms < 1000)
        return false;

    const uint32_t frame_count = static_cast<uint32_t>(emuenv.frame_count);
    if (frame_count == 0) {
        metrics.last_fps_time = now;
        return false;
    }

    emuenv.fps = (frame_count * 1000 + ms / 2) / ms;
    emuenv.ms_per_frame = (ms + frame_count / 2) / frame_count;
    metrics.last_fps_time = now;
    emuenv.frame_count = 0;

    emuenv.fps_values[emuenv.current_fps_offset] = static_cast<float>(emuenv.fps);
    emuenv.current_fps_offset = (emuenv.current_fps_offset + 1) % perf_frames_size;

    float avg_fps = 0.0f;
    for (uint32_t i = 0; i < perf_frames_size; i++)
        avg_fps += emuenv.fps_values[i];
    emuenv.avg_fps = static_cast<uint32_t>(avg_fps) / perf_frames_size;
    emuenv.min_fps = static_cast<uint32_t>(*std::min_element(std::begin(emuenv.fps_values), std::end(emuenv.fps_values)));
    emuenv.max_fps = static_cast<uint32_t>(*std::max_element(std::begin(emuenv.fps_values), std::end(emuenv.fps_values)));

    if (!emuenv.renderer)
        return true;

    auto &renderer = *emuenv.renderer;
    renderer.perf_overlay.fps = emuenv.fps;
    renderer.perf_overlay.avg_fps = emuenv.avg_fps;
    renderer.perf_overlay.min_fps = emuenv.min_fps;
    renderer.perf_overlay.max_fps = emuenv.max_fps;
    renderer.perf_overlay.ms_per_frame = emuenv.ms_per_frame;
    std::copy(std::begin(emuenv.fps_values), std::end(emuenv.fps_values), renderer.perf_overlay.fps_values.begin());
    renderer.perf_overlay.fps_values_count = perf_frames_size;
    renderer.perf_overlay.current_fps_offset = emuenv.current_fps_offset;

    return true;
}

void release_direct_app(EmuEnvState &emuenv) {
    if (emuenv.direct_app) {
        if (emuenv.previous_direct_license) {
            auto &license = emuenv.license.rif[emuenv.direct_app->title_id];
            std::memcpy(&license, emuenv.previous_direct_license->data(), emuenv.previous_direct_license->size());
        } else {
            emuenv.license.rif.erase(emuenv.direct_app->title_id);
        }
    }
    emuenv.previous_direct_license.reset();
    emuenv.direct_app.reset();
    emuenv.io.app0_mount.reset();
}

void abort_game_launch(EmuEnvState &emuenv) {
    release_direct_app(emuenv);
    emuenv.io.mounted_files.clear();
    emuenv.io.mounted_directories.clear();
    emuenv.io.app_path.clear();
    emuenv.io.title_id.clear();
    emuenv.io.addcont.clear();
    emuenv.io.content_id.clear();
    emuenv.io.savedata.clear();
    emuenv.current_app_title.clear();
    emuenv.app_path.clear();
    emuenv.license_content_id.clear();
    emuenv.license_title_id.clear();
    emuenv.self_name.clear();
    emuenv.self_path.clear();
    emuenv.main_thread_id = 0;
    emuenv.app_info = {};
    emuenv.drop_inputs = false;
    reset_perf_metrics(emuenv);
    set_current_config(emuenv, "");
}

void request_in_process_launch(EmuEnvState &emuenv, AppLaunchRequest request) {
    if (request.app_path.empty())
        request.app_path = emuenv.io.app_path;
    if (!request.direct_app && request.app_path == emuenv.io.app_path)
        request.direct_app = emuenv.direct_app;

    emuenv.post_app_launch_request(std::move(request));
    if (emuenv.renderer)
        emuenv.renderer->should_display = true;
}

} // namespace app
