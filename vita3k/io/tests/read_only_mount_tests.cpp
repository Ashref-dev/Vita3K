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

#include "fake_read_only_mount.h"

#include <io/functions.h>
#include <io/io.h>
#include <io/vfs.h>

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr char export_name[] = "io-test";

class IoTest : public testing::Test {
protected:
    void SetUp() override {
        root = fs::temp_directory_path() / fs::unique_path("vita3k-io-%%%%-%%%%");
        fs::create_directories(root);
        io.app_path = "TEST00001";
        init_device_paths(io);
    }

    void TearDown() override {
        io_deinit(io);
        fs::remove_all(root);
    }

    void write_host_file(const fs::path &relative, std::string_view contents) const {
        const auto path = root / "ux0" / "app" / io.app_path / relative;
        fs::create_directories(path.parent_path());
        std::ofstream output(path.generic_path().string(), std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    IOState io{};
    fs::path root;
};

std::shared_ptr<FakeReadOnlyMount> make_mount() {
    return std::make_shared<FakeReadOnlyMount>(std::initializer_list<FakeReadOnlyMount::File>{
        { "Data/Mixed.bin", { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 } },
        { "empty.txt", {} },
        { "Implicit/Child/leaf.txt", { 'l', 'e', 'a', 'f' } },
        { "alpha.txt", { 'a' } },
        { "Zoo/file.txt", { 'z' } },
    });
}

TEST_F(IoTest, mounted_random_reads_seeks_and_eof_are_bounded) {
    const auto mount = make_mount();
    io.app0_mount = mount;
    const auto fd = open_file(io, "app0:/data/mIXed.BIN", SCE_O_RDONLY, root, export_name);
    ASSERT_GE(fd, 0);

    std::array<uint8_t, 4> bytes{};
    EXPECT_EQ(read_file(bytes.data(), io, fd, bytes.size(), export_name), 4);
    EXPECT_EQ(bytes, (std::array<uint8_t, 4>{ 0, 1, 2, 3 }));
    EXPECT_EQ(tell_file(io, fd, export_name), 4);

    EXPECT_EQ(seek_file(fd, 7, SCE_SEEK_SET, io, export_name), 7);
    EXPECT_EQ(read_file(bytes.data(), io, fd, bytes.size(), export_name), 3);
    EXPECT_EQ((std::array<uint8_t, 3>{ bytes[0], bytes[1], bytes[2] }), (std::array<uint8_t, 3>{ 7, 8, 9 }));
    EXPECT_EQ(tell_file(io, fd, export_name), 10);
    EXPECT_EQ(read_file(bytes.data(), io, fd, bytes.size(), export_name), 0);

    EXPECT_EQ(seek_file(fd, -4, SCE_SEEK_CUR, io, export_name), 6);
    EXPECT_EQ(seek_file(fd, -2, SCE_SEEK_END, io, export_name), 8);
    EXPECT_EQ(seek_file(fd, -20, SCE_SEEK_END, io, export_name), SCE_ERROR_ERRNO_EBADFD);
    EXPECT_EQ(tell_file(io, fd, export_name), 8);
    EXPECT_LE(mount->largest_read_request(), bytes.size());
    EXPECT_EQ(close_file(io, fd, export_name), 0);
}

TEST_F(IoTest, mounted_reads_clip_request_before_touching_guest_buffer) {
    const auto mount = std::make_shared<FakeReadOnlyMount>(std::initializer_list<FakeReadOnlyMount::File>{
        { "tiny.bin", { 7 } },
    });
    io.app0_mount = mount;
    const auto fd = open_file(io, "app0:/tiny.bin", SCE_O_RDONLY, root, export_name);
    ASSERT_GE(fd, 0);

    std::array<uint8_t, 16> bytes;
    bytes.fill(0xAA);
    EXPECT_EQ(read_file(bytes.data(), io, fd, bytes.size(), export_name), 1);
    EXPECT_EQ(bytes.front(), 7);
    EXPECT_TRUE(std::ranges::all_of(bytes.begin() + 1, bytes.end(), [](uint8_t value) { return value == 0xAA; }));
    EXPECT_EQ(mount->largest_read_request(), 1);
}

TEST_F(IoTest, mounted_reads_fit_the_signed_io_return_range) {
    MountedFile file{
        .size = std::numeric_limits<uint32_t>::max(),
    };
    EXPECT_EQ(file.next_read_size(std::numeric_limits<uint32_t>::max()), std::numeric_limits<int>::max());
}

TEST_F(IoTest, mounted_reads_survive_a_concurrent_close) {
    io.app0_mount = make_mount();

    for (int iteration = 0; iteration < 500; ++iteration) {
        const auto fd = open_file(io, "app0:/Data/Mixed.bin", SCE_O_RDONLY, root, export_name);
        ASSERT_GE(fd, 0);

        std::array<uint8_t, 4> bytes{};
        std::thread reader([&] { read_file(bytes.data(), io, fd, bytes.size(), export_name); });
        std::thread closer([&] { close_file(io, fd, export_name); });
        reader.join();
        closer.join();
    }
}

TEST_F(IoTest, mounted_stat_by_path_and_descriptor_reports_read_only_types) {
    io.app0_mount = make_mount();
    SceIoStat path_stat{};
    ASSERT_EQ(stat_file(io, "app0:/IMPLICIT/child/LEAF.txt", &path_stat, root, export_name), 0);
    EXPECT_EQ(path_stat.st_size, 4);
    EXPECT_NE(path_stat.st_mode & SCE_S_IFREG, 0);
    EXPECT_EQ(path_stat.st_mode & SCE_S_IWUSR, 0);

    const auto fd = open_file(io, "app0:/implicit/child/leaf.txt", SCE_O_RDONLY, root, export_name);
    ASSERT_GE(fd, 0);
    SceIoStat fd_stat{};
    EXPECT_EQ(stat_file_by_fd(io, fd, &fd_stat, root, export_name), 0);
    EXPECT_EQ(fd_stat.st_size, path_stat.st_size);

    SceIoStat directory_stat{};
    EXPECT_EQ(stat_file(io, "app0:/implicit/child", &directory_stat, root, export_name), 0);
    EXPECT_NE(directory_stat.st_mode & SCE_S_IFDIR, 0);
}

TEST_F(IoTest, mounted_parent_components_do_not_resolve_to_root) {
    io.app0_mount = make_mount();
    SceIoStat stat{};

    EXPECT_EQ(stat_file(io, "app0:/implicit/../leaf.txt", &stat, root, export_name), SCE_ERROR_ERRNO_ENOENT);
    EXPECT_EQ(open_dir(io, "app0:/implicit/..", root, export_name), SCE_ERROR_ERRNO_ENOENT);
}

TEST_F(IoTest, mounted_bulk_app_read_uses_guest_io) {
    // Given
    io.app0_mount = make_mount();
    vfs::FileBuffer bytes;

    // When
    const bool read = vfs::read_app_file(bytes, io, root, "Data/Mixed.bin", 1024);

    // Then
    ASSERT_TRUE(read);
    EXPECT_EQ(bytes, (vfs::FileBuffer{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }));
}

TEST_F(IoTest, mounted_bulk_app_read_rejects_files_above_caller_limit) {
    io.app0_mount = make_mount();
    vfs::FileBuffer bytes{ 42 };

    EXPECT_FALSE(vfs::read_app_file(bytes, io, root, "Data/Mixed.bin", 9));
    EXPECT_EQ(bytes, (vfs::FileBuffer{ 42 }));
}

TEST_F(IoTest, mounted_implicit_directories_enumerate_deterministically) {
    io.app0_mount = make_mount();
    const auto fd = open_dir(io, "app0:/", root, export_name);
    ASSERT_GE(fd, 0);

    std::vector<std::string> names;
    SceIoDirent entry{};
    while (read_dir(io, fd, &entry, root, export_name) == 1) {
        names.emplace_back(entry.d_name);
        EXPECT_EQ(entry.d_name[sizeof(entry.d_name) - 1], '\0');
    }
    EXPECT_EQ(names, (std::vector<std::string>{ "alpha.txt", "Data", "empty.txt", "Implicit", "Zoo" }));
    EXPECT_EQ(close_dir(io, fd, export_name), 0);

    const auto implicit_fd = open_dir(io, "APP0:/implicit/CHILD", root, export_name);
    ASSERT_GE(implicit_fd, 0);
    ASSERT_EQ(read_dir(io, implicit_fd, &entry, root, export_name), 1);
    EXPECT_STREQ(entry.d_name, "leaf.txt");
    EXPECT_EQ(read_dir(io, implicit_fd, &entry, root, export_name), 0);
}

TEST_F(IoTest, mounted_teardown_releases_mount_and_invalidates_descriptors) {
    auto mount = make_mount();
    std::weak_ptr<const ReadOnlyMount> weak_mount = mount;
    io.app0_mount = mount;
    const auto file_fd = open_file(io, "app0:/alpha.txt", SCE_O_RDONLY, root, export_name);
    const auto dir_fd = open_dir(io, "app0:/", root, export_name);
    mount.reset();
    ASSERT_FALSE(weak_mount.expired());

    io_deinit(io);
    EXPECT_TRUE(weak_mount.expired());
    std::array<uint8_t, 1> byte{};
    EXPECT_EQ(read_file(byte.data(), io, file_fd, byte.size(), export_name), SCE_ERROR_ERRNO_EBADFD);
    SceIoDirent entry{};
    EXPECT_EQ(read_dir(io, dir_fd, &entry, root, export_name), SCE_ERROR_ERRNO_EBADFD);
}

TEST_F(IoTest, mounted_app0_rejects_every_mutation) {
    io.app0_mount = make_mount();
    constexpr int write_flags[] = { SCE_O_WRONLY, SCE_O_RDWR, SCE_O_RDONLY | SCE_O_CREAT, SCE_O_RDONLY | SCE_O_TRUNC, SCE_O_RDONLY | SCE_O_APPEND };
    for (const auto flags : write_flags)
        EXPECT_EQ(open_file(io, "app0:/alpha.txt", flags, root, export_name), SCE_ERROR_ERRNO_EOPNOTSUPP);

    const auto fd = open_file(io, "app0:/alpha.txt", SCE_O_RDONLY, root, export_name);
    ASSERT_GE(fd, 0);
    const uint8_t value = 7;
    EXPECT_EQ(write_file(fd, &value, 1, io, export_name), SCE_ERROR_ERRNO_EOPNOTSUPP);
    EXPECT_EQ(truncate_file(fd, 0, io, export_name), SCE_ERROR_ERRNO_EOPNOTSUPP);
    EXPECT_EQ(remove_file(io, "app0:/alpha.txt", root, export_name), SCE_ERROR_ERRNO_EOPNOTSUPP);
    EXPECT_EQ(rename(io, "app0:/alpha.txt", "ux0:/moved.txt", root, export_name), SCE_ERROR_ERRNO_EOPNOTSUPP);
    EXPECT_EQ(rename(io, "ux0:/source.txt", "app0:/moved.txt", root, export_name), SCE_ERROR_ERRNO_EOPNOTSUPP);
    EXPECT_EQ(create_dir(io, "app0:/new", 0777, root, export_name), SCE_ERROR_ERRNO_EOPNOTSUPP);
    EXPECT_EQ(remove_dir(io, "app0:/Implicit", root, export_name), SCE_ERROR_ERRNO_EOPNOTSUPP);
}

TEST_F(IoTest, null_mount_preserves_host_app0_file_and_directory_behavior) {
    write_host_file("host.txt", "host-data");
    ASSERT_FALSE(io.app0_mount);

    const auto fd = open_file(io, "app0:/host.txt", SCE_O_RDWR, root, export_name);
    ASSERT_GE(fd, 0);
    std::array<char, 9> bytes{};
    EXPECT_EQ(read_file(bytes.data(), io, fd, bytes.size(), export_name), 9);
    EXPECT_EQ(std::string_view(bytes.data(), bytes.size()), "host-data");
    EXPECT_EQ(seek_file(fd, 0, SCE_SEEK_SET, io, export_name), 0);
    const char replacement = 'H';
    EXPECT_EQ(write_file(fd, &replacement, 1, io, export_name), 1);

    SceIoStat stat{};
    EXPECT_EQ(stat_file(io, "app0:/host.txt", &stat, root, export_name), 0);
    EXPECT_EQ(stat.st_size, 9);
    EXPECT_EQ(stat_file_by_fd(io, fd, &stat, root, export_name), 0);
    EXPECT_EQ(close_file(io, fd, export_name), 0);

    const auto dir_fd = open_dir(io, "app0:/", root, export_name);
    ASSERT_GE(dir_fd, 0);
    SceIoDirent entry{};
    ASSERT_EQ(read_dir(io, dir_fd, &entry, root, export_name), 1);
    EXPECT_STREQ(entry.d_name, "host.txt");
    EXPECT_EQ(close_dir(io, dir_fd, export_name), 0);
}

TEST_F(IoTest, null_mount_bulk_app_read_preserves_host_behavior) {
    // Given
    write_host_file("host.txt", "host-data");
    vfs::FileBuffer bytes;

    // When
    const bool read = vfs::read_app_file(bytes, io, root, "host.txt", 1024);

    // Then
    ASSERT_TRUE(read);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), "host-data");
}

TEST_F(IoTest, null_mount_preserves_host_app0_mutations) {
    write_host_file("source.txt", "source");
    ASSERT_EQ(rename(io, "app0:/source.txt", "ux0:/app/TEST00001/renamed.txt", root, export_name), 0);
    EXPECT_EQ(create_dir(io, "app0:/new-dir", 0777, root, export_name), 0);

    const auto created_fd = open_file(io, "app0:/created.txt", SCE_O_WRONLY | SCE_O_CREAT, root, export_name);
    ASSERT_GE(created_fd, 0);
    EXPECT_EQ(truncate_file(created_fd, 0, io, export_name), 0);
    EXPECT_EQ(close_file(io, created_fd, export_name), 0);
    EXPECT_EQ(remove_file(io, "app0:/created.txt", root, export_name), 0);
    EXPECT_EQ(remove_dir(io, "app0:/new-dir", root, export_name), 0);
}

} // namespace
