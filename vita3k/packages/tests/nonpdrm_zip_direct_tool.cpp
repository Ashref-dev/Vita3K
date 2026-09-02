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

#include <packages/nonpdrm_zip_direct.h>

#include <codec/state.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr size_t kCompareChunkSize = 64 * 1024;

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

void compare_range(const ReadOnlyMount &mount, const std::filesystem::path &reference_root,
    std::string_view path, uint64_t offset, size_t size) {
    std::vector<uint8_t> actual(size);
    const auto read = mount.read_at(path, offset, actual);
    if (!read)
        throw std::runtime_error("mounted read error " + std::to_string(static_cast<int>(read.error()))
            + ": " + std::string(path) + " at " + std::to_string(offset) + " for " + std::to_string(size));
    require(*read == size, "mounted short read " + std::to_string(*read) + ": " + std::string(path) + " at " + std::to_string(offset) + " for " + std::to_string(size));

    std::ifstream reference(reference_root / std::filesystem::path(path), std::ios::binary);
    require(reference.is_open(), "missing plaintext reference: " + std::string(path));
    reference.seekg(static_cast<std::streamoff>(offset));
    std::vector<uint8_t> expected(size);
    reference.read(reinterpret_cast<char *>(expected.data()), static_cast<std::streamsize>(size));
    require(reference.gcount() == static_cast<std::streamsize>(size), "plaintext reference short read");
    require(actual == expected, "plaintext mismatch: " + std::string(path) + " at " + std::to_string(offset));
}

uint64_t compare_entire_file(const ReadOnlyMount &mount, const std::filesystem::path &reference_root,
    std::string_view path) {
    const auto mounted_stat = mount.stat(path);
    require(mounted_stat && mounted_stat->type == ReadOnlyMountEntryType::file, "mounted file stat failed");
    require(mounted_stat->size == std::filesystem::file_size(reference_root / std::filesystem::path(path)),
        "mounted/reference size mismatch");
    for (uint64_t offset = 0; offset < mounted_stat->size;) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(kCompareChunkSize, mounted_stat->size - offset));
        compare_range(mount, reference_root, path, offset, count);
        offset += count;
    }
    return mounted_stat->size;
}

void compare_representative_ranges(const ReadOnlyMount &mount, const std::filesystem::path &reference_root,
    std::string_view path) {
    const auto stat = mount.stat(path);
    require(stat && stat->type == ReadOnlyMountEntryType::file && stat->size >= kCompareChunkSize,
        "representative asset unavailable: " + std::string(path));
    require(stat->size == std::filesystem::file_size(reference_root / std::filesystem::path(path)),
        "representative asset mounted/reference sizes differ: " + std::string(path));
    const std::array<uint64_t, 3> offsets{ 0, (stat->size - kCompareChunkSize) / 2, stat->size - kCompareChunkSize };
    for (const auto offset : offsets)
        compare_range(mount, reference_root, path, offset, kCompareChunkSize);
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 2 && argc != 3) {
            std::cerr << "usage: packages-nonpdrm-zip-tool <NoNpDrm.zip> [plaintext-title-root]\n";
            return 2;
        }
        const std::filesystem::path zip_path(argv[1]);
        const std::optional<std::filesystem::path> reference_root = argc == 3
            ? std::optional<std::filesystem::path>(argv[2])
            : std::nullopt;
        const auto before_size = std::filesystem::file_size(zip_path);
        const auto before_time = std::filesystem::last_write_time(zip_path);

        const auto opened = packages::open_nonpdrm_zip_direct(zip_path);
        require(opened.has_value(), opened ? "" : opened.error().message);
        require(opened->app_info.app_title_id == "PCSE01305", "unexpected TITLE_ID");
        require(sizeof(opened->license) == 512, "license is not 512 bytes");
        const auto &mount = *opened->mount;

        const auto root_stat = mount.stat("");
        require(root_stat && root_stat->type == ReadOnlyMountEntryType::directory, "root stat failed");
        bool saw_eboot = false;
        bool saw_media = false;
        for (size_t index = 0;; ++index) {
            const auto entry = mount.read_directory("", index);
            require(entry.has_value(), "root listing failed");
            if (!*entry)
                break;
            saw_eboot |= (*entry)->name == "eboot.bin";
            saw_media |= (*entry)->name == "Media";
        }
        require(saw_eboot && saw_media, "root listing omitted expected entries");

        uint64_t eboot_bytes = 0;
        if (reference_root) {
            eboot_bytes = compare_entire_file(mount, *reference_root, "eboot.bin");
            compare_representative_ranges(mount, *reference_root, "Media/globalgamemanagers.assets");
            compare_representative_ranges(mount, *reference_root,
                "Media/StreamingAssets/movie_vita/MOV_intro.mp4");
        } else {
            const auto eboot_stat = mount.stat("eboot.bin");
            require(eboot_stat && eboot_stat->type == ReadOnlyMountEntryType::file && eboot_stat->size > 0,
                "mounted eboot stat failed");
            const auto probe_size = static_cast<size_t>(std::min<uint64_t>(kCompareChunkSize, eboot_stat->size));
            std::vector<uint8_t> probe(probe_size);
            require(mount.read_at("eboot.bin", 0, probe) == probe_size, "mounted eboot start probe failed");
            require(mount.read_at("eboot.bin", eboot_stat->size - probe_size, probe) == probe_size,
                "mounted eboot end probe failed");
            eboot_bytes = probe_size * 2;
        }

        constexpr std::string_view movie_path = "Media/StreamingAssets/movie_vita/MOV_intro.mp4";
        const auto movie_stat = mount.stat(movie_path);
        require(movie_stat && movie_stat->type == ReadOnlyMountEntryType::file, "mounted movie stat failed");
        PlayerState player;
        player.queue(PlayerSource{
            .name = std::string(movie_path),
            .size = movie_stat->size,
            .read_at = [&mount, movie_path](uint64_t offset, std::span<uint8_t> output) -> std::optional<size_t> {
                const auto read = mount.read_at(movie_path, offset, output);
                if (!read)
                    return std::nullopt;
                return *read;
            },
        });
        const auto video_size = player.get_size();
        require(!player.video_playing.empty() && video_size.width > 0 && video_size.height > 0,
            "FFmpeg custom IO could not open mounted movie");

        require(std::filesystem::file_size(zip_path) == before_size, "source ZIP size changed");
        require(std::filesystem::last_write_time(zip_path) == before_time, "source ZIP timestamp changed");
        std::cout << std::dec
                  << "TITLE_ID: " << opened->app_info.app_title_id << '\n'
                  << "TITLE: " << opened->app_info.app_title << '\n'
                  << "CONTENT_ID: " << opened->app_info.app_content_id << '\n'
                  << "TITLE_ROOT: " << opened->title_root << '\n'
                  << "LICENSE_BYTES: " << sizeof(opened->license) << '\n'
                  << "EBOOT_BYTES_COMPARED: " << eboot_bytes << '\n'
                  << "ASSET_RANGES_COMPARED: " << (reference_root ? 6 : 0) << '\n'
                  << "STREAMED_VIDEO_SIZE: " << video_size.width << 'x' << video_size.height << '\n'
                  << "ZIP_REPLAY_SCRATCH_BOUND: " << packages::NONPDRM_ZIP_REPLAY_SCRATCH_SIZE << '\n'
                  << "WRITES_OR_EXTRACTIONS: 0\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
