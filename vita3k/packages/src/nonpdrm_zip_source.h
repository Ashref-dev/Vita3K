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

#include <packages/nonpdrm_zip_direct.h>

#include <PfsMount.h>
#include <miniz.h>

#include <cstdio>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace packages::detail {

class NoNpDrmZipSource final : public psvpfsparser::PfsReadOnlySource {
public:
    static std::expected<std::shared_ptr<NoNpDrmZipSource>, NoNpDrmZipError> create(
        const std::filesystem::path &path, uint64_t maximum_small_metadata_size);

    ~NoNpDrmZipSource() override;

    NoNpDrmZipSource(const NoNpDrmZipSource &) = delete;
    NoNpDrmZipSource &operator=(const NoNpDrmZipSource &) = delete;

    psvpfsparser::PfsResult<std::vector<psvpfsparser::PfsSourceEntry>> entries() const override;
    psvpfsparser::PfsResult<uint64_t> file_size(std::string_view path) const override;
    psvpfsparser::PfsResult<void> read_at(
        std::string_view path, uint64_t offset, std::span<uint8_t> destination) const override;

    std::expected<std::vector<uint8_t>, NoNpDrmZipError> read_small_file(
        std::string_view path, uint64_t maximum_size) const;
    std::expected<void, NoNpDrmZipError> check_unchanged() const;
    const std::string &title_root() const noexcept;

private:
    struct Entry {
        std::string path;
        std::string folded_path;
        std::string archive_name;
        uint64_t size;
        uint64_t compressed_size;
        uint64_t data_offset;
        uint32_t archive_index;
        uint16_t method;
        bool directory;
    };

    struct Snapshot {
        uintmax_t size;
        std::filesystem::file_time_type write_time;
    };

    NoNpDrmZipSource(std::filesystem::path path, std::FILE *file, Snapshot snapshot);

    std::expected<void, NoNpDrmZipError> initialize(uint64_t maximum_small_metadata_size);
    std::expected<void, NoNpDrmZipError> validate_archive_bounds();
    std::expected<std::string, NoNpDrmZipError> read_entry_name(uint32_t index) const;
    std::expected<size_t, NoNpDrmZipError> find_root_depth(uint32_t count) const;
    std::expected<uint64_t, NoNpDrmZipError> parse_data_offset(
        const mz_zip_archive_file_stat &stat, std::string_view archive_name);
    std::expected<void, NoNpDrmZipError> check_unchanged_unlocked() const;
    const Entry *find_entry(std::string_view path) const;

    std::filesystem::path path_;
    std::FILE *file_;
    Snapshot snapshot_;
    mutable std::mutex mutex_;
    mutable mz_zip_archive archive_{};
    mutable mz_zip_reader_extract_iter_state *deflate_iterator_ = nullptr;
    mutable uint32_t deflate_archive_index_ = std::numeric_limits<uint32_t>::max();
    mutable uint64_t deflate_position_ = 0;
    bool archive_initialized_ = false;
    std::string title_root_;
    std::vector<Entry> entries_;
    std::vector<psvpfsparser::PfsSourceEntry> source_entries_;
    std::unordered_map<std::string, size_t> entry_indices_;
};

} // namespace packages::detail
