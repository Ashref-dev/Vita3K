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

#include <io/read_only_mount.h>
#include <packages/license.h>
#include <packages/sfo.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace packages {

inline constexpr size_t NONPDRM_ZIP_REPLAY_SCRATCH_SIZE = 64 * 1024;

enum class NoNpDrmZipErrorCode {
    io,
    invalid_zip,
    invalid_path,
    duplicate_path,
    multiple_roots,
    encrypted_entry,
    unsupported_compression,
    metadata_too_large,
    invalid_license,
    invalid_sfo,
    pfs,
    source_changed,
};

struct NoNpDrmZipError {
    NoNpDrmZipErrorCode code;
    std::string message;
};

struct NoNpDrmZipOptions {
    int sys_language = 1;
    uint64_t maximum_small_metadata_size = 8 * 1024 * 1024;
    uint64_t maximum_pfs_metadata_size = 64 * 1024 * 1024;
};

struct NoNpDrmZipDirectPlay {
    std::shared_ptr<const ReadOnlyMount> mount;
    sfo::SfoAppInfo app_info;
    SceNpDrmLicense license;
    std::string title_root;
};

using NoNpDrmZipResult = std::expected<NoNpDrmZipDirectPlay, NoNpDrmZipError>;

NoNpDrmZipResult open_nonpdrm_zip_direct(
    const std::filesystem::path &archive_path, NoNpDrmZipOptions options = {});

} // namespace packages
