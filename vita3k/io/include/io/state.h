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

#include <io/filesystem.h>
#include <io/read_only_mount.h>
#include <io/types.h>
#include <io/util.h>

#include <map>
#include <memory>
#include <unordered_map>

// Class for all needed information to access files on Vita3K.
class FileStats : public VitaStats {
    // Shared file pointer
    FilePtr wrapped_file;

public:
    // Constructor used for files
    // Based on https://codereview.stackexchange.com/questions/4679/
    explicit FileStats(const char *vita, const std::string &t, const fs::path &file, const int open) {
        wrapped_file = create_shared_file(file, open);

        file_info.vita_loc = vita;
        file_info.translated = t;
        file_info.sys_loc = file;
        file_info.open_mode = open;
        file_info.file_mode = SCE_SO_IFREG | SCE_SO_IROTH;
        file_info.access_mode = SCE_S_IFREG;
    }

    bool is_regular_file() const {
        return file_info.file_mode & SCE_SO_IFREG;
    }

    // Check if the file is writable
    bool can_write_file() const {
        if (!is_regular_file())
            return false;

        return can_write(file_info.open_mode);
    }

    // File operations
    FILE *get_file_pointer() const {
        return wrapped_file.get();
    }

    // File functions
    SceOff read(void *input_data, int element_size, SceSize element_count) const;
    SceOff write(const void *data, SceSize size, int count) const;
    int truncate(const SceSize size) const;
    bool seek(SceOff offset, SceIoSeekMode seek_mode) const;
    SceOff tell() const;
};

// Class for implementing Directory structure; path names are wide for Windows, normal for else
class DirStats : public VitaStats {
    // Shared directory pointer
    DirPtr dir_ptr;

public:
    DirStats(const char *vita, const std::string &t, const fs::path &file, DirPtr ptr) {
        dir_ptr = std::move(ptr);

        file_info.vita_loc = vita;
        file_info.translated = t;
        file_info.sys_loc = file;
        file_info.open_mode = SCE_O_RDONLY;
        file_info.file_mode = SCE_SO_IFDIR | SCE_SO_IROTH;
        file_info.access_mode = SCE_S_IFDIR | SCE_S_IRUSR;
    }

    auto get_dir_ptr() const {
        return get_system_dir_ptr(dir_ptr);
    }

    bool is_directory() const {
        return file_info.file_mode & SCE_SO_IFDIR;
    }
};

typedef std::map<SceUID, TtyType> TtyFiles;
typedef std::map<SceUID, FileStats> StdFiles;
typedef std::map<SceUID, DirStats> DirEntries;

struct MountedFile {
    std::shared_ptr<const ReadOnlyMount> mount;
    std::string path;
    uint64_t size;
    uint64_t cursor = 0;
    std::mutex mutex;

    [[nodiscard]] SceSize next_read_size(SceSize read_size) const;
    SceOff read(void *data, SceSize read_size);
    bool seek(SceOff offset, SceIoSeekMode seek_mode);
    [[nodiscard]] SceOff tell() const;
};

struct MountedDirectory {
    std::shared_ptr<const ReadOnlyMount> mount;
    std::string path;
    size_t cursor = 0;
    std::mutex mutex;
};

using MountedFiles = std::map<SceUID, std::shared_ptr<MountedFile>>;
using MountedDirectories = std::map<SceUID, std::shared_ptr<MountedDirectory>>;

struct IOState {
    struct DevicePaths {
        std::string app0;
        std::string savedata0;
        std::string addcont0;
    } device_paths;

    std::string addcont;
    std::string content_id;
    std::string savedata;
    std::string title_id;
    std::string app_path;

    std::string user_id;
    std::string user_name;

    bool redirect_stdio;

    SceUID next_fd = 0;
    TtyFiles tty_files;
    StdFiles std_files;
    DirEntries dir_entries;
    std::shared_ptr<const ReadOnlyMount> app0_mount;
    // Guards the mounted descriptor tables; handles are refcounted so a concurrent close cannot
    // destroy a descriptor that another guest thread is still reading through.
    mutable std::mutex mounted_mutex;
    MountedFiles mounted_files;
    MountedDirectories mounted_directories;

    std::unordered_map<std::string, std::string> cachemap;
    bool case_isens_find_enabled = false;

    std::mutex overlay_mutex;
    SceUID next_overlay_id = 1;
    // overlay in the order they should be applied
    std::vector<FiosOverlay> overlays;
};
