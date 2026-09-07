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
#include <app/state.h>
#include <config/state.h>
#include <emuenv/state.h>
#include <io/state.h>
#include <packages/license.h>
#include <packages/sfo.h>

#include "fake_read_only_mount.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

namespace {

std::shared_ptr<DirectAppLaunch> make_direct_launch() {
    auto launch = std::make_shared<DirectAppLaunch>();
    launch->mount = std::make_shared<FakeReadOnlyMount>(
        std::initializer_list<FakeReadOnlyMount::File>{ { "eboot.bin", { 1, 2, 3 } } });
    launch->app_version = "1.00";
    launch->app_category = "gd";
    launch->content_id = "UP3643-PCSE01305_00-DOWNWELL00000000";
    launch->savedata = "PCSE01305";
    launch->short_title = "Downwell";
    launch->title = "Downwell";
    launch->title_id = "PCSE01305";
    std::ranges::fill(launch->license, uint8_t{ 0xA5 });
    return launch;
}

TEST(DirectAppLaunch, SetsTransientMetadataWithoutInstalledAppEntry) {
    // Given
    EmuEnvState emuenv;
    emuenv.app.user_list.users.emplace("00", app::User{ .id = "00" });
    emuenv.cfg.user_id = "00";
    emuenv.config_path = fs::temp_directory_path();
    const auto launch = make_direct_launch();
    const AppLaunchRequest request{
        .app_path = launch->title_id,
        .direct_app = launch,
    };

    // When
    const bool setup = app::setup_game_launch(emuenv, request, false);

    // Then
    ASSERT_TRUE(setup);
    EXPECT_EQ(emuenv.io.app_path, "PCSE01305");
    EXPECT_EQ(emuenv.io.title_id, "PCSE01305");
    EXPECT_EQ(emuenv.io.content_id, launch->content_id);
    EXPECT_EQ(emuenv.current_app_title, "Downwell");
    EXPECT_EQ(emuenv.app_info.app_category, "gd");
    EXPECT_EQ(emuenv.io.app0_mount, launch->mount);
    ASSERT_TRUE(emuenv.license.rif.contains("PCSE01305"));
    EXPECT_EQ(emuenv.license.rif.at("PCSE01305").key[0], uint8_t{ 0xA5 });
}

TEST(DirectAppLaunch, CleanupReleasesMountAndInsertedLicense) {
    // Given
    EmuEnvState emuenv;
    emuenv.app.user_list.users.emplace("00", app::User{ .id = "00" });
    emuenv.cfg.user_id = "00";
    emuenv.config_path = fs::temp_directory_path();
    const auto launch = make_direct_launch();
    const std::weak_ptr<const ReadOnlyMount> weak_mount = launch->mount;
    ASSERT_TRUE(app::setup_game_launch(emuenv, AppLaunchRequest{
                                                   .app_path = launch->title_id,
                                                   .direct_app = launch,
                                               },
        false));
    ASSERT_TRUE(emuenv.license.rif.contains("PCSE01305"));
    EXPECT_EQ(emuenv.license.rif.at("PCSE01305").key[0], uint8_t{ 0xA5 });

    // When
    app::abort_game_launch(emuenv);

    // Then
    EXPECT_FALSE(emuenv.io.app0_mount);
    EXPECT_FALSE(emuenv.direct_app);
    EXPECT_FALSE(emuenv.license.rif.contains("PCSE01305"));
    EXPECT_FALSE(weak_mount.expired());
}

TEST(DirectAppLaunch, CleanupPreservesExistingLicense) {
    // Given
    EmuEnvState emuenv;
    emuenv.app.user_list.users.emplace("00", app::User{ .id = "00" });
    emuenv.cfg.user_id = "00";
    emuenv.config_path = fs::temp_directory_path();
    auto &existing_license = emuenv.license.rif["PCSE01305"];
    existing_license.key[0] = 0x5A;
    const auto launch = make_direct_launch();
    ASSERT_TRUE(app::setup_game_launch(emuenv, AppLaunchRequest{
                                                   .app_path = launch->title_id,
                                                   .direct_app = launch,
                                               },
        false));
    EXPECT_EQ(emuenv.license.rif.at("PCSE01305").key[0], uint8_t{ 0xA5 });

    // When
    app::abort_game_launch(emuenv);

    // Then
    ASSERT_TRUE(emuenv.license.rif.contains("PCSE01305"));
    EXPECT_EQ(emuenv.license.rif.at("PCSE01305").key[0], uint8_t{ 0x5A });
}

} // namespace
