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

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

enum class ReadOnlyMountError {
    not_found,
    io_error,
    out_of_range,
};

enum class ReadOnlyMountEntryType {
    file,
    directory,
};

struct ReadOnlyMountStat {
    ReadOnlyMountEntryType type;
    uint64_t size;
};

struct ReadOnlyMountDirEntry {
    std::string name;
    ReadOnlyMountStat stat;
};

class ReadOnlyMount {
public:
    virtual ~ReadOnlyMount() = default;

    virtual std::expected<ReadOnlyMountStat, ReadOnlyMountError> stat(std::string_view path) const = 0;
    virtual std::expected<size_t, ReadOnlyMountError> read_at(std::string_view path, uint64_t offset, std::span<uint8_t> output) const = 0;
    virtual std::expected<std::optional<ReadOnlyMountDirEntry>, ReadOnlyMountError> read_directory(std::string_view path, size_t index) const = 0;
};
