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

#include <miniz.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

class ZipTestBuilder {
public:
    struct EntryOptions {
        uint16_t flags = 0;
        uint16_t method = 0;
        std::optional<uint32_t> uncompressed_size;
        std::optional<std::vector<uint8_t>> local_name;
    };

    void add(std::string_view name, std::string_view contents = {}) {
        add(name, contents, EntryOptions{});
    }

    void add(std::string_view name, std::string_view contents, EntryOptions options) {
        add(std::vector<uint8_t>(name.begin(), name.end()),
            std::vector<uint8_t>(contents.begin(), contents.end()), std::move(options));
    }

    void add(std::string_view name, std::vector<uint8_t> contents) {
        add(std::vector<uint8_t>(name.begin(), name.end()), std::move(contents), EntryOptions{});
    }

    void add(std::vector<uint8_t> name, std::vector<uint8_t> contents) {
        add(std::move(name), std::move(contents), EntryOptions{});
    }

    void add(std::vector<uint8_t> name, std::vector<uint8_t> contents, EntryOptions options) {
        entries_.push_back({ std::move(name), std::move(contents), std::move(options) });
    }

    void write(const std::filesystem::path &path) const {
        std::vector<uint8_t> bytes;
        std::vector<uint32_t> local_offsets;
        std::vector<std::vector<uint8_t>> payloads;
        payloads.reserve(entries_.size());
        for (const auto &entry : entries_)
            payloads.push_back(entry.options.method == 8 ? deflate(entry.contents) : entry.contents);

        for (size_t index = 0; index < entries_.size(); ++index) {
            const auto &entry = entries_[index];
            const auto &payload = payloads[index];
            local_offsets.push_back(static_cast<uint32_t>(bytes.size()));
            const auto &local_name = entry.options.local_name.value_or(entry.name);
            append_u32(bytes, 0x04034b50);
            append_u16(bytes, 20);
            append_u16(bytes, entry.options.flags);
            append_u16(bytes, entry.options.method);
            append_u16(bytes, 0);
            append_u16(bytes, 0);
            append_u32(bytes, crc(entry.contents));
            append_u32(bytes, static_cast<uint32_t>(payload.size()));
            append_u32(bytes, entry.options.uncompressed_size.value_or(static_cast<uint32_t>(entry.contents.size())));
            append_u16(bytes, static_cast<uint16_t>(local_name.size()));
            append_u16(bytes, 0);
            append(bytes, local_name);
            append(bytes, payload);
        }

        const auto central_offset = static_cast<uint32_t>(bytes.size());
        for (size_t index = 0; index < entries_.size(); ++index) {
            const auto &entry = entries_[index];
            append_u32(bytes, 0x02014b50);
            append_u16(bytes, 20);
            append_u16(bytes, 20);
            append_u16(bytes, entry.options.flags);
            append_u16(bytes, entry.options.method);
            append_u16(bytes, 0);
            append_u16(bytes, 0);
            append_u32(bytes, crc(entry.contents));
            append_u32(bytes, static_cast<uint32_t>(payloads[index].size()));
            append_u32(bytes, entry.options.uncompressed_size.value_or(static_cast<uint32_t>(entry.contents.size())));
            append_u16(bytes, static_cast<uint16_t>(entry.name.size()));
            append_u16(bytes, 0);
            append_u16(bytes, 0);
            append_u16(bytes, 0);
            append_u16(bytes, 0);
            append_u32(bytes, 0);
            append_u32(bytes, local_offsets[index]);
            append(bytes, entry.name);
        }

        const auto central_size = static_cast<uint32_t>(bytes.size()) - central_offset;
        append_u32(bytes, 0x06054b50);
        append_u16(bytes, 0);
        append_u16(bytes, 0);
        append_u16(bytes, static_cast<uint16_t>(entries_.size()));
        append_u16(bytes, static_cast<uint16_t>(entries_.size()));
        append_u32(bytes, central_size);
        append_u32(bytes, central_offset);
        append_u16(bytes, 0);

        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

private:
    struct Entry {
        std::vector<uint8_t> name;
        std::vector<uint8_t> contents;
        EntryOptions options;
    };

    static uint32_t crc(std::span<const uint8_t> bytes) {
        return static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, bytes.data(), bytes.size()));
    }

    static std::vector<uint8_t> deflate(std::span<const uint8_t> bytes) {
        std::vector<uint8_t> output(mz_compressBound(bytes.size()));
        mz_stream stream{};
        if (mz_deflateInit2(&stream, MZ_BEST_COMPRESSION, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY) != MZ_OK)
            throw std::runtime_error("failed to initialize raw DEFLATE fixture");
        stream.next_in = bytes.data();
        stream.avail_in = static_cast<unsigned int>(bytes.size());
        stream.next_out = output.data();
        stream.avail_out = static_cast<unsigned int>(output.size());
        const auto result = mz_deflate(&stream, MZ_FINISH);
        mz_deflateEnd(&stream);
        if (result != MZ_STREAM_END)
            throw std::runtime_error("failed to build raw DEFLATE fixture");
        output.resize(stream.total_out);
        return output;
    }

    static void append(std::vector<uint8_t> &output, std::span<const uint8_t> value) {
        output.insert(output.end(), value.begin(), value.end());
    }

    static void append_u16(std::vector<uint8_t> &output, uint16_t value) {
        output.push_back(static_cast<uint8_t>(value));
        output.push_back(static_cast<uint8_t>(value >> 8));
    }

    static void append_u32(std::vector<uint8_t> &output, uint32_t value) {
        append_u16(output, static_cast<uint16_t>(value));
        append_u16(output, static_cast<uint16_t>(value >> 16));
    }

    std::vector<Entry> entries_;
};
