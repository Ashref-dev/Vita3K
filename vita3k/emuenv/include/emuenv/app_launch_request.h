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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ReadOnlyMount;

inline constexpr size_t DIRECT_APP_LICENSE_SIZE = 0x200;

struct DirectAppLaunch {
    std::shared_ptr<const ReadOnlyMount> mount;
    std::string app_version;
    std::string app_category;
    std::string content_id;
    std::string addcont;
    std::string savedata;
    std::string parental_level;
    std::string short_title;
    std::string title;
    std::string title_id;
    std::array<uint8_t, DIRECT_APP_LICENSE_SIZE> license{};
};

enum class AppLaunchReason {
    User,
    LoadExec,
    ProcessExit,
};

struct AppLaunchRequest {
    std::string app_path{};
    std::string self_path{};
    std::vector<std::string> argv{};
    AppLaunchReason reason = AppLaunchReason::User;
    std::shared_ptr<const DirectAppLaunch> direct_app;
};
