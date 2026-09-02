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

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class FakeReadOnlyMount final : public ReadOnlyMount {
public:
    using File = std::pair<std::string, std::vector<uint8_t>>;

    explicit FakeReadOnlyMount(std::initializer_list<File> files) {
        for (auto [path, data] : files) {
            normalize(path);
            files_.emplace(lower(path), StoredFile{ std::move(path), std::move(data) });
        }
        build_directories();
    }

    std::expected<ReadOnlyMountStat, ReadOnlyMountError> stat(std::string_view path) const override {
        const auto key = normalized_key(path);
        if (const auto file = files_.find(key); file != files_.end())
            return ReadOnlyMountStat{ ReadOnlyMountEntryType::file, file->second.data.size() };
        if (directories_.contains(key))
            return ReadOnlyMountStat{ ReadOnlyMountEntryType::directory, 0 };
        return std::unexpected(ReadOnlyMountError::not_found);
    }

    std::expected<size_t, ReadOnlyMountError> read_at(std::string_view path, uint64_t offset, std::span<uint8_t> output) const override {
        largest_read_request_ = std::max(largest_read_request_, output.size());
        const auto file = files_.find(normalized_key(path));
        if (file == files_.end())
            return std::unexpected(ReadOnlyMountError::not_found);
        if (offset >= file->second.data.size())
            return size_t{ 0 };

        const auto available = file->second.data.size() - static_cast<size_t>(offset);
        const auto count = std::min(output.size(), available);
        std::copy_n(file->second.data.data() + offset, count, output.data());
        return count;
    }

    std::expected<std::optional<ReadOnlyMountDirEntry>, ReadOnlyMountError> read_directory(std::string_view path, size_t index) const override {
        const auto directory = children_.find(normalized_key(path));
        if (directory == children_.end())
            return std::unexpected(ReadOnlyMountError::not_found);
        if (index >= directory->second.size())
            return std::optional<ReadOnlyMountDirEntry>{};
        return directory->second[index];
    }

    [[nodiscard]] size_t largest_read_request() const {
        return largest_read_request_;
    }

private:
    struct StoredFile {
        std::string path;
        std::vector<uint8_t> data;
    };

    static std::string lower(std::string_view value) {
        std::string result(value);
        std::ranges::transform(result, result.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return result;
    }

    static void normalize(std::string &path) {
        std::replace(path.begin(), path.end(), '\\', '/');
        while (path.starts_with('/'))
            path.erase(path.begin());
        while (path.ends_with('/'))
            path.pop_back();
    }

    static std::string normalized_key(std::string_view path) {
        std::string normalized(path);
        normalize(normalized);
        return lower(normalized);
    }

    void build_directories() {
        directories_.insert("");
        std::map<std::string, std::map<std::string, ReadOnlyMountDirEntry>> child_maps;

        for (const auto &[key, file] : files_) {
            size_t slash = 0;
            std::string parent;
            while ((slash = file.path.find('/', slash)) != std::string::npos) {
                const auto component = file.path.substr(parent.empty() ? 0 : parent.size() + 1, slash - (parent.empty() ? 0 : parent.size() + 1));
                const auto child_path = file.path.substr(0, slash);
                child_maps[lower(parent)].emplace(lower(component), ReadOnlyMountDirEntry{ component, { ReadOnlyMountEntryType::directory, 0 } });
                directories_.insert(lower(child_path));
                parent = child_path;
                ++slash;
            }

            const auto name_start = file.path.find_last_of('/');
            const auto name = file.path.substr(name_start == std::string::npos ? 0 : name_start + 1);
            parent = name_start == std::string::npos ? "" : file.path.substr(0, name_start);
            child_maps[lower(parent)].emplace(lower(name), ReadOnlyMountDirEntry{ name, { ReadOnlyMountEntryType::file, file.data.size() } });
        }

        for (auto &[directory, entries] : child_maps) {
            auto &children = children_[directory];
            for (auto &[unused, entry] : entries)
                children.push_back(std::move(entry));
        }
        for (const auto &directory : directories_)
            children_.try_emplace(directory);
    }

    std::map<std::string, StoredFile> files_;
    std::set<std::string> directories_;
    std::map<std::string, std::vector<ReadOnlyMountDirEntry>> children_;
    mutable size_t largest_read_request_ = 0;
};
