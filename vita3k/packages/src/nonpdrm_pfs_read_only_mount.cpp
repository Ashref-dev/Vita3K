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

#include "nonpdrm_pfs_read_only_mount.h"

#include <algorithm>
#include <expected>
#include <optional>
#include <utility>

namespace packages::detail {
namespace {

ReadOnlyMountError translate_error(psvpfsparser::PfsErrorCode code) {
    switch (code) {
    case psvpfsparser::PfsErrorCode::not_found:
        return ReadOnlyMountError::not_found;
    case psvpfsparser::PfsErrorCode::out_of_range:
        return ReadOnlyMountError::out_of_range;
    default:
        return ReadOnlyMountError::io_error;
    }
}

class PfsReadOnlyMount final : public ReadOnlyMount {
public:
    PfsReadOnlyMount(std::shared_ptr<const psvpfsparser::PfsMount> pfs,
        std::shared_ptr<const NoNpDrmZipSource> source)
        : pfs_(std::move(pfs))
        , source_(std::move(source)) {
    }

    std::expected<ReadOnlyMountStat, ReadOnlyMountError> stat(std::string_view path) const override {
        if (!source_->check_unchanged())
            return std::unexpected(ReadOnlyMountError::io_error);
        const auto result = pfs_->stat(path);
        if (!result)
            return std::unexpected(translate_error(result.error().code));
        if (!source_->check_unchanged())
            return std::unexpected(ReadOnlyMountError::io_error);
        return ReadOnlyMountStat{
            .type = result->directory ? ReadOnlyMountEntryType::directory : ReadOnlyMountEntryType::file,
            .size = result->size,
        };
    }

    std::expected<size_t, ReadOnlyMountError> read_at(
        std::string_view path, uint64_t offset, std::span<uint8_t> output) const override {
        if (!source_->check_unchanged())
            return std::unexpected(ReadOnlyMountError::io_error);
        const auto opened = pfs_->open(path);
        if (!opened)
            return std::unexpected(translate_error(opened.error().code));
        if (offset >= opened->size())
            return size_t{ 0 };
        const auto count = static_cast<size_t>(std::min<uint64_t>(output.size(), opened->size() - offset));
        if (count != 0) {
            const auto read = opened->read_at(offset, output.first(count));
            if (!read)
                return std::unexpected(translate_error(read.error().code));
        }
        if (!source_->check_unchanged())
            return std::unexpected(ReadOnlyMountError::io_error);
        return count;
    }

    std::expected<std::optional<ReadOnlyMountDirEntry>, ReadOnlyMountError> read_directory(
        std::string_view path, size_t index) const override {
        if (!source_->check_unchanged())
            return std::unexpected(ReadOnlyMountError::io_error);
        const auto entries = pfs_->list(path);
        if (!entries)
            return std::unexpected(translate_error(entries.error().code));
        if (index >= entries->size())
            return std::optional<ReadOnlyMountDirEntry>{};
        const auto &entry = (*entries)[index];
        const auto slash = entry.path.find_last_of('/');
        ReadOnlyMountDirEntry result{
            .name = entry.path.substr(slash == std::string::npos ? 0 : slash + 1),
            .stat = {
                .type = entry.directory ? ReadOnlyMountEntryType::directory : ReadOnlyMountEntryType::file,
                .size = entry.size,
            },
        };
        if (!source_->check_unchanged())
            return std::unexpected(ReadOnlyMountError::io_error);
        return std::optional<ReadOnlyMountDirEntry>(std::move(result));
    }

private:
    std::shared_ptr<const psvpfsparser::PfsMount> pfs_;
    std::shared_ptr<const NoNpDrmZipSource> source_;
};

} // namespace

std::shared_ptr<const ReadOnlyMount> make_pfs_read_only_mount(
    std::shared_ptr<const psvpfsparser::PfsMount> pfs,
    std::shared_ptr<const NoNpDrmZipSource> source) {
    return std::make_shared<PfsReadOnlyMount>(std::move(pfs), std::move(source));
}

} // namespace packages::detail
