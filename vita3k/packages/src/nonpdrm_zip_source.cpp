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

#include "nonpdrm_zip_source.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace packages::detail {
namespace {

constexpr uint64_t kMaximumCentralDirectorySize = 16 * 1024 * 1024;
constexpr uint32_t kMaximumEntryCount = std::numeric_limits<uint16_t>::max() - 1;
constexpr size_t kMaximumEntryNameSize = 4096;
constexpr size_t kEndRecordSize = 22;
constexpr size_t kMaximumEndSearchSize = kEndRecordSize + std::numeric_limits<uint16_t>::max();

uint16_t read_u16(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset])
        | static_cast<uint16_t>(bytes[offset + 1] << 8);
}

uint32_t read_u32(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint32_t>(read_u16(bytes, offset))
        | static_cast<uint32_t>(read_u16(bytes, offset + 2)) << 16;
}

std::string fold_path(std::string_view path) {
    std::string folded(path);
    std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return folded;
}

std::expected<std::vector<std::string>, NoNpDrmZipError> normalize_segments(std::string_view raw) {
    if (raw.empty() || raw.front() == '/' || raw.front() == '\\')
        return std::unexpected(NoNpDrmZipError{ NoNpDrmZipErrorCode::invalid_path, "absolute or empty ZIP entry path" });

    std::string path(raw);
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' && path[2] == '/')
        return std::unexpected(NoNpDrmZipError{ NoNpDrmZipErrorCode::invalid_path, "drive-absolute ZIP entry path" });

    std::vector<std::string> segments;
    for (size_t begin = 0; begin <= path.size();) {
        const auto end = path.find('/', begin);
        const auto length = (end == std::string::npos ? path.size() : end) - begin;
        const auto segment = path.substr(begin, length);
        if (segment == "..")
            return std::unexpected(NoNpDrmZipError{ NoNpDrmZipErrorCode::invalid_path, "parent component in ZIP entry path" });
        if (!segment.empty() && segment != ".")
            segments.push_back(segment);
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    if (segments.empty())
        return std::unexpected(NoNpDrmZipError{ NoNpDrmZipErrorCode::invalid_path, "ZIP entry has no title root" });
    return segments;
}

std::string join_segments(const std::vector<std::string> &segments, size_t first) {
    std::string path;
    for (size_t index = first; index < segments.size(); ++index) {
        if (!path.empty())
            path.push_back('/');
        path += segments[index];
    }
    return path;
}

NoNpDrmZipError zip_error(NoNpDrmZipErrorCode code, std::string message) {
    return { code, std::move(message) };
}

psvpfsparser::PfsError pfs_source_error(const NoNpDrmZipError &error) {
    if (error.code == NoNpDrmZipErrorCode::source_changed)
        return psvpfsparser::PfsError::io(error.message);
    return psvpfsparser::PfsError::io(error.message);
}

bool seek_file(std::FILE *file, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

} // namespace

NoNpDrmZipSource::NoNpDrmZipSource(std::filesystem::path path, std::FILE *file, Snapshot snapshot)
    : path_(std::move(path))
    , file_(file)
    , snapshot_(snapshot) {
    mz_zip_zero_struct(&archive_);
}

NoNpDrmZipSource::~NoNpDrmZipSource() {
    std::scoped_lock lock(mutex_);
    if (deflate_iterator_)
        mz_zip_reader_extract_iter_free(deflate_iterator_);
    if (archive_initialized_)
        mz_zip_reader_end(&archive_);
    if (file_)
        std::fclose(file_);
}

std::expected<std::shared_ptr<NoNpDrmZipSource>, NoNpDrmZipError> NoNpDrmZipSource::create(
    const std::filesystem::path &path, uint64_t maximum_small_metadata_size) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::io, "failed to stat ZIP: " + error.message()));
    const auto write_time = std::filesystem::last_write_time(path, error);
    if (error)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::io, "failed to read ZIP timestamp: " + error.message()));

#ifdef _WIN32
    std::FILE *file = _wfopen(path.c_str(), L"rb");
#else
    std::FILE *file = std::fopen(path.c_str(), "rb");
#endif
    if (!file)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::io, "failed to open ZIP read-only"));

    auto source = std::shared_ptr<NoNpDrmZipSource>(
        new NoNpDrmZipSource(path, file, Snapshot{ size, write_time }));
    if (auto initialized = source->initialize(maximum_small_metadata_size); !initialized)
        return std::unexpected(initialized.error());
    return source;
}

std::expected<void, NoNpDrmZipError> NoNpDrmZipSource::validate_archive_bounds() {
    if (snapshot_.size < kEndRecordSize)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "ZIP is too small"));
    const auto search_size = static_cast<size_t>(std::min<uintmax_t>(snapshot_.size, kMaximumEndSearchSize));
    std::vector<uint8_t> tail(search_size);
    if (!seek_file(file_, snapshot_.size - search_size)
        || std::fread(tail.data(), 1, tail.size(), file_) != tail.size())
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::io, "failed to read ZIP end record"));

    std::optional<size_t> record;
    for (size_t offset = tail.size() - kEndRecordSize + 1; offset-- > 0;) {
        if (read_u32(tail, offset) == 0x06054b50) {
            record = offset;
            break;
        }
    }
    if (!record)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "missing ZIP end record"));
    const auto offset = *record;
    const auto comment_size = read_u16(tail, offset + 20);
    if (offset + kEndRecordSize + comment_size != tail.size())
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "invalid ZIP end record length"));
    const auto entries_on_disk = read_u16(tail, offset + 8);
    const auto entries = read_u16(tail, offset + 10);
    const auto central_size = read_u32(tail, offset + 12);
    const auto central_offset = read_u32(tail, offset + 16);
    if (read_u16(tail, offset + 4) != 0 || read_u16(tail, offset + 6) != 0 || entries_on_disk != entries)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "multi-disk ZIP is unsupported"));
    if (entries == std::numeric_limits<uint16_t>::max() || central_size == std::numeric_limits<uint32_t>::max()
        || central_offset == std::numeric_limits<uint32_t>::max())
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "ZIP64 central directory is unsupported"));
    if (entries == 0 || entries > kMaximumEntryCount || central_size > kMaximumCentralDirectorySize)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "ZIP directory exceeds fixed bounds"));
    const auto record_file_offset = snapshot_.size - tail.size() + offset;
    if (static_cast<uint64_t>(central_offset) + central_size != record_file_offset)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "ZIP central directory bounds are inconsistent"));
    return {};
}

std::expected<void, NoNpDrmZipError> NoNpDrmZipSource::initialize(uint64_t maximum_small_metadata_size) {
    if (auto bounds = validate_archive_bounds(); !bounds)
        return bounds;
    if (!seek_file(file_, 0) || !mz_zip_reader_init_cfile(&archive_, file_, snapshot_.size, 0))
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip,
            std::string("miniz rejected ZIP: ") + mz_zip_get_error_string(mz_zip_peek_last_error(&archive_))));
    archive_initialized_ = true;

    const auto count = mz_zip_reader_get_num_files(&archive_);
    if (count == 0 || count > kMaximumEntryCount)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "ZIP entry count exceeds fixed bounds"));

    std::set<std::string> seen_paths;
    std::set<std::string> directory_paths;
    for (uint32_t index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive_, index, &stat))
            return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "failed to read ZIP entry metadata"));
        const auto name_buffer_size = mz_zip_reader_get_filename(&archive_, index, nullptr, 0);
        if (name_buffer_size <= 1 || name_buffer_size > kMaximumEntryNameSize + 1)
            return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_path, "ZIP entry name exceeds fixed bound"));
        std::vector<char> name_buffer(name_buffer_size);
        if (mz_zip_reader_get_filename(&archive_, index, name_buffer.data(), name_buffer_size) != name_buffer_size)
            return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "failed to read complete ZIP entry name"));
        const std::string_view raw_name(name_buffer.data(), name_buffer_size - 1);
        if (raw_name.find('\0') != std::string_view::npos)
            return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_path, "NUL in ZIP entry name"));
        if (stat.m_is_encrypted || (stat.m_bit_flag & 1) != 0)
            return std::unexpected(zip_error(NoNpDrmZipErrorCode::encrypted_entry, "encrypted ZIP entries are unsupported"));
        if ((stat.m_method != 0 && stat.m_method != 8) || !stat.m_is_supported)
            return std::unexpected(zip_error(NoNpDrmZipErrorCode::unsupported_compression, "unsupported ZIP compression method"));

        auto segments = normalize_segments(raw_name);
        if (!segments)
            return std::unexpected(segments.error());
        const auto folded_root = fold_path((*segments)[0]);
        if (title_root_.empty())
            title_root_ = (*segments)[0];
        else if (fold_path(title_root_) != folded_root)
            return std::unexpected(zip_error(NoNpDrmZipErrorCode::multiple_roots, "ZIP contains multiple title roots"));

        const auto logical_path = join_segments(*segments, 1);
        const auto folded_logical_path = fold_path(logical_path);
        if (!seen_paths.insert(folded_logical_path).second)
            return std::unexpected(zip_error(NoNpDrmZipErrorCode::duplicate_path, "duplicate normalized ZIP path: " + logical_path));
        if (logical_path.empty())
            continue;
        if ((folded_logical_path == "sce_sys/param.sfo" || folded_logical_path == "sce_sys/package/work.bin")
            && stat.m_uncomp_size > maximum_small_metadata_size)
            return std::unexpected(zip_error(NoNpDrmZipErrorCode::metadata_too_large, "small metadata entry exceeds configured bound"));

        auto data_offset = parse_data_offset(stat, raw_name);
        if (!data_offset)
            return std::unexpected(data_offset.error());
        if (stat.m_method == 0 && stat.m_comp_size != stat.m_uncomp_size)
            return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "stored ZIP entry sizes differ"));

        const bool directory = stat.m_is_directory != 0;
        const auto entry_index = entries_.size();
        entries_.push_back({ logical_path, folded_logical_path, std::string(raw_name), stat.m_uncomp_size,
            stat.m_comp_size, *data_offset, index, stat.m_method, directory });
        entry_indices_.emplace(folded_logical_path, entry_index);
        if (directory)
            directory_paths.insert(folded_logical_path);

        for (size_t slash = logical_path.find('/'); slash != std::string::npos; slash = logical_path.find('/', slash + 1))
            directory_paths.insert(fold_path(logical_path.substr(0, slash)));
    }

    source_entries_.reserve(entries_.size() + directory_paths.size());
    for (const auto &entry : entries_)
        source_entries_.push_back({ entry.path, entry.size, entry.directory });
    for (const auto &folded_directory : directory_paths) {
        if (entry_indices_.contains(folded_directory))
            continue;
        source_entries_.push_back({ folded_directory, 0, true });
    }
    return {};
}

std::expected<uint64_t, NoNpDrmZipError> NoNpDrmZipSource::parse_data_offset(
    const mz_zip_archive_file_stat &stat, std::string_view archive_name) {
    std::array<uint8_t, 30> header{};
    if (mz_zip_read_archive_data(&archive_, stat.m_local_header_ofs, header.data(), header.size()) != header.size())
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "short ZIP local header"));
    if (read_u32(header, 0) != 0x04034b50 || read_u16(header, 6) != stat.m_bit_flag || read_u16(header, 8) != stat.m_method)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "ZIP local and central headers disagree"));
    const auto name_size = read_u16(header, 26);
    const auto extra_size = read_u16(header, 28);
    if (name_size != archive_name.size())
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "ZIP local filename length differs"));
    std::vector<uint8_t> local_name(name_size);
    if (mz_zip_read_archive_data(&archive_, stat.m_local_header_ofs + header.size(), local_name.data(), local_name.size()) != local_name.size()
        || !std::equal(local_name.begin(), local_name.end(), reinterpret_cast<const uint8_t *>(archive_name.data())))
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "ZIP local filename differs"));
    const uint64_t data_offset = stat.m_local_header_ofs + header.size() + name_size + extra_size;
    if (data_offset > archive_.m_central_directory_file_ofs
        || stat.m_comp_size > archive_.m_central_directory_file_ofs - data_offset)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, "ZIP entry data exceeds archive bounds"));
    return data_offset;
}

std::expected<void, NoNpDrmZipError> NoNpDrmZipSource::check_unchanged_unlocked() const {
    std::error_code error;
    const auto size = std::filesystem::file_size(path_, error);
    if (error || size != snapshot_.size)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::source_changed, "source ZIP size changed after mount"));
    const auto write_time = std::filesystem::last_write_time(path_, error);
    if (error || write_time != snapshot_.write_time)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::source_changed, "source ZIP timestamp changed after mount"));
    return {};
}

std::expected<void, NoNpDrmZipError> NoNpDrmZipSource::check_unchanged() const {
    std::scoped_lock lock(mutex_);
    return check_unchanged_unlocked();
}

const NoNpDrmZipSource::Entry *NoNpDrmZipSource::find_entry(std::string_view path) const {
    const auto found = entry_indices_.find(fold_path(path));
    if (found == entry_indices_.end())
        return nullptr;
    return &entries_[found->second];
}

psvpfsparser::PfsResult<std::vector<psvpfsparser::PfsSourceEntry>> NoNpDrmZipSource::entries() const {
    if (auto unchanged = check_unchanged(); !unchanged)
        return std::unexpected(pfs_source_error(unchanged.error()));
    return source_entries_;
}

psvpfsparser::PfsResult<uint64_t> NoNpDrmZipSource::file_size(std::string_view path) const {
    if (auto unchanged = check_unchanged(); !unchanged)
        return std::unexpected(pfs_source_error(unchanged.error()));
    const auto *entry = find_entry(path);
    if (!entry || entry->directory)
        return std::unexpected(psvpfsparser::PfsError::not_found("ZIP file not found: " + std::string(path)));
    return entry->size;
}

psvpfsparser::PfsResult<void> NoNpDrmZipSource::read_at(
    std::string_view path, uint64_t offset, std::span<uint8_t> destination) const {
    const auto *entry = find_entry(path);
    if (!entry || entry->directory)
        return std::unexpected(psvpfsparser::PfsError::not_found("ZIP file not found: " + std::string(path)));
    if (offset > entry->size || destination.size() > entry->size - offset)
        return std::unexpected(psvpfsparser::PfsError::out_of_range("ZIP entry read exceeds file size"));

    std::scoped_lock lock(mutex_);
    if (auto unchanged = check_unchanged_unlocked(); !unchanged)
        return std::unexpected(pfs_source_error(unchanged.error()));
    if (destination.empty())
        return {};

    if (entry->method == 0) {
        if (mz_zip_read_archive_data(&archive_, entry->data_offset + offset, destination.data(), destination.size()) != destination.size())
            return std::unexpected(psvpfsparser::PfsError::io("short stored ZIP entry read"));
    } else {
        if (!deflate_iterator_ || deflate_archive_index_ != entry->archive_index || offset < deflate_position_) {
            if (deflate_iterator_)
                mz_zip_reader_extract_iter_free(deflate_iterator_);
            deflate_iterator_ = mz_zip_reader_extract_iter_new(&archive_, entry->archive_index, 0);
            deflate_archive_index_ = entry->archive_index;
            deflate_position_ = 0;
        }
        if (!deflate_iterator_)
            return std::unexpected(psvpfsparser::PfsError::io("failed to create deflate iterator"));
        std::array<uint8_t, NONPDRM_ZIP_REPLAY_SCRATCH_SIZE> scratch{};
        while (deflate_position_ < offset) {
            const auto request = static_cast<size_t>(std::min<uint64_t>(scratch.size(), offset - deflate_position_));
            const auto count = mz_zip_reader_extract_iter_read(deflate_iterator_, scratch.data(), request);
            if (count == 0)
                return std::unexpected(psvpfsparser::PfsError::io("short read while replaying deflated ZIP entry"));
            deflate_position_ += count;
        }
        size_t copied = 0;
        while (copied < destination.size()) {
            const auto count = mz_zip_reader_extract_iter_read(
                deflate_iterator_, destination.data() + copied, destination.size() - copied);
            if (count == 0)
                return std::unexpected(psvpfsparser::PfsError::io("short deflated ZIP entry read"));
            copied += count;
            deflate_position_ += count;
        }
    }
    if (auto unchanged = check_unchanged_unlocked(); !unchanged)
        return std::unexpected(pfs_source_error(unchanged.error()));
    return {};
}

std::expected<std::vector<uint8_t>, NoNpDrmZipError> NoNpDrmZipSource::read_small_file(
    std::string_view path, uint64_t maximum_size) const {
    const auto size = file_size(path);
    if (!size)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::invalid_zip, size.error().message));
    if (*size > maximum_size || *size > std::numeric_limits<size_t>::max())
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::metadata_too_large, "metadata entry exceeds configured bound"));
    std::vector<uint8_t> content(static_cast<size_t>(*size));
    if (auto read = read_at(path, 0, content); !read)
        return std::unexpected(zip_error(NoNpDrmZipErrorCode::io, read.error().message));
    return content;
}

const std::string &NoNpDrmZipSource::title_root() const noexcept {
    return title_root_;
}

} // namespace packages::detail
