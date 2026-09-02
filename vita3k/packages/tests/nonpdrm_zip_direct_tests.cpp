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

#include "../src/nonpdrm_zip_source.h"
#include "zip_test_builder.h"

#include <CryptoOperationsFactory.h>
#include <F00DKeyEncryptorFactory.h>
#include <FilesDbParser.h>
#include <HashTree.h>
#include <UnicvDbTypes.h>

#include <packages/nonpdrm_zip_direct.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::filesystem::path make_test_root() {
    std::random_device random;
    return std::filesystem::temp_directory_path()
        / ("vita3k-nonpdrm-zip-tests-" + std::to_string(random()) + '-' + std::to_string(random()));
}

void write_u16(std::vector<uint8_t> &bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void write_u32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    write_u16(bytes, offset, static_cast<uint16_t>(value));
    write_u16(bytes, offset + 2, static_cast<uint16_t>(value >> 16));
}

std::vector<uint8_t> make_sfo(std::string savedata, std::string addcont = "PCSE01305") {
    const std::array<std::pair<std::string, std::string>, 4> entries{ {
        { "TITLE_ID", "PCSE01305" },
        { "CONTENT_ID", "UP0438-PCSE01305_00-0000000000000001" },
        { "INSTALL_DIR_SAVEDATA", std::move(savedata) },
        { "INSTALL_DIR_ADDCONT", std::move(addcont) },
    } };
    constexpr size_t header_size = 20;
    constexpr size_t index_size = 16;
    std::vector<uint8_t> bytes(header_size + entries.size() * index_size);
    const auto key_table = static_cast<uint32_t>(bytes.size());
    std::array<uint16_t, entries.size()> key_offsets{};
    for (size_t index = 0; index < entries.size(); ++index) {
        key_offsets[index] = static_cast<uint16_t>(bytes.size() - key_table);
        bytes.insert(bytes.end(), entries[index].first.begin(), entries[index].first.end());
        bytes.push_back(0);
    }
    const auto data_table = static_cast<uint32_t>(bytes.size());
    std::array<uint32_t, entries.size()> data_offsets{};
    for (size_t index = 0; index < entries.size(); ++index) {
        data_offsets[index] = static_cast<uint32_t>(bytes.size() - data_table);
        bytes.insert(bytes.end(), entries[index].second.begin(), entries[index].second.end());
        bytes.push_back(0);
    }

    write_u32(bytes, 0, 0x46535000);
    write_u32(bytes, 4, 0x00000101);
    write_u32(bytes, 8, key_table);
    write_u32(bytes, 12, data_table);
    write_u32(bytes, 16, static_cast<uint32_t>(entries.size()));
    for (size_t index = 0; index < entries.size(); ++index) {
        const size_t offset = header_size + index * index_size;
        write_u16(bytes, offset, key_offsets[index]);
        write_u16(bytes, offset + 2, 0x0204);
        write_u32(bytes, offset + 4, static_cast<uint32_t>(entries[index].second.size() + 1));
        write_u32(bytes, offset + 8, static_cast<uint32_t>(entries[index].second.size() + 1));
        write_u32(bytes, offset + 12, data_offsets[index]);
    }
    return bytes;
}

std::vector<uint8_t> make_license(std::string_view content_id) {
    std::vector<uint8_t> bytes(512);
    if (content_id.size() >= 0x30)
        throw std::runtime_error("test content ID exceeds license field");
    std::memcpy(bytes.data() + 0x10, content_id.data(), content_id.size());
    return bytes;
}

class TestContext {
public:
    TestContext()
        : root_(make_test_root()) {
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    ~TestContext() {
        std::filesystem::remove_all(root_);
    }

    packages::NoNpDrmZipError open_error(const ZipTestBuilder &builder,
        packages::NoNpDrmZipOptions options = {}) const {
        const auto path = write(builder);
        const auto result = packages::open_nonpdrm_zip_direct(path, options);
        if (result)
            throw std::runtime_error("invalid archive unexpectedly opened");
        return result.error();
    }

    std::filesystem::path write(const ZipTestBuilder &builder) const {
        const auto path = root_ / "input.zip";
        builder.write(path);
        return path;
    }

private:
    std::filesystem::path root_;
};

void require_code(packages::NoNpDrmZipError actual, packages::NoNpDrmZipErrorCode expected) {
    if (actual.code != expected)
        throw std::runtime_error("unexpected error code: " + actual.message);
}

void rejects_empty_archives() {
    require_code(TestContext{}.open_error({}), packages::NoNpDrmZipErrorCode::invalid_zip);
}

void rejects_absolute_and_parent_paths() {
    TestContext context;
    ZipTestBuilder absolute;
    absolute.add("/TITLE/eboot.bin", "x");
    require_code(context.open_error(absolute), packages::NoNpDrmZipErrorCode::invalid_path);
    ZipTestBuilder parent;
    parent.add("TITLE/../escape.bin", "x");
    require_code(context.open_error(parent), packages::NoNpDrmZipErrorCode::invalid_path);
}

void rejects_nul_in_entry_name() {
    ZipTestBuilder builder;
    builder.add({ 'T', 'I', 'T', 'L', 'E', '/', 'a', '\0', 'b' }, { 'x' });
    require_code(TestContext{}.open_error(builder), packages::NoNpDrmZipErrorCode::invalid_path);
}

void rejects_multiple_title_roots() {
    ZipTestBuilder builder;
    builder.add("TITLE_A/eboot.bin", "a");
    builder.add("TITLE_B/eboot.bin", "b");
    require_code(TestContext{}.open_error(builder), packages::NoNpDrmZipErrorCode::multiple_roots);
}

void rejects_duplicate_normalized_and_case_folded_names() {
    TestContext context;
    ZipTestBuilder duplicate;
    duplicate.add("TITLE/Data//file.bin", "a");
    duplicate.add("TITLE/Data/file.bin", "b");
    require_code(context.open_error(duplicate), packages::NoNpDrmZipErrorCode::duplicate_path);
    ZipTestBuilder collision;
    collision.add("TITLE/Data/File.bin", "a");
    collision.add("TITLE/data/file.BIN", "b");
    require_code(context.open_error(collision), packages::NoNpDrmZipErrorCode::duplicate_path);
}

void rejects_encrypted_and_unsupported_entries() {
    TestContext context;
    ZipTestBuilder encrypted;
    ZipTestBuilder::EntryOptions encrypted_options;
    encrypted_options.flags = 1;
    encrypted.add("TITLE/eboot.bin", "x", encrypted_options);
    require_code(context.open_error(encrypted), packages::NoNpDrmZipErrorCode::encrypted_entry);
    ZipTestBuilder unsupported;
    ZipTestBuilder::EntryOptions unsupported_options;
    unsupported_options.method = 12;
    unsupported.add("TITLE/eboot.bin", "x", unsupported_options);
    require_code(context.open_error(unsupported), packages::NoNpDrmZipErrorCode::unsupported_compression);
}

void rejects_oversized_small_metadata_before_extraction() {
    ZipTestBuilder builder;
    builder.add("TITLE/sce_sys/param.sfo", std::string(4097, 'x'));
    packages::NoNpDrmZipOptions options;
    options.maximum_small_metadata_size = 4096;
    require_code(TestContext{}.open_error(builder, options), packages::NoNpDrmZipErrorCode::metadata_too_large);
}

void rejects_mismatched_local_header_names() {
    ZipTestBuilder builder;
    ZipTestBuilder::EntryOptions options;
    options.local_name = std::vector<uint8_t>{ 'T', 'I', 'T', 'L', 'E', '/', 'o', 't', 'h', 'e', 'r' };
    builder.add("TITLE/eboot.bin", "x", options);
    require_code(TestContext{}.open_error(builder), packages::NoNpDrmZipErrorCode::invalid_zip);
}

void accepts_utf8_entry_names() {
    TestContext context;
    ZipTestBuilder builder;
    builder.add("TITLE/テスト.txt", "data");
    auto source = packages::detail::NoNpDrmZipSource::create(context.write(builder), 4096);
    if (!source)
        throw std::runtime_error("UTF-8 ZIP entry was rejected: " + source.error().message);

    std::array<uint8_t, 4> content{};
    if (auto read = (*source)->read_at("テスト.txt", 0, content); !read || content != std::array<uint8_t, 4>{ 'd', 'a', 't', 'a' })
        throw std::runtime_error("UTF-8 ZIP entry could not be read");
}

void reads_deflated_entries_forward_and_backward() {
    TestContext context;
    ZipTestBuilder builder;
    ZipTestBuilder::EntryOptions options;
    options.method = 8;
    const std::string content = "0123456789abcdefghijklmnopqrstuvwxyz";
    builder.add("TITLE/data.bin", content, options);
    auto source = packages::detail::NoNpDrmZipSource::create(context.write(builder), 4096);
    if (!source)
        throw std::runtime_error("deflated ZIP entry was rejected: " + source.error().message);

    std::array<uint8_t, 6> bytes{};
    if (auto read = (*source)->read_at("data.bin", 0, bytes); !read || std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != "012345")
        throw std::runtime_error("deflated ZIP entry start read differed");
    if (auto read = (*source)->read_at("data.bin", 20, bytes); !read || std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != "klmnop")
        throw std::runtime_error("deflated ZIP entry forward read differed");
    if (auto read = (*source)->read_at("data.bin", 8, bytes); !read || std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != "89abcd")
        throw std::runtime_error("deflated ZIP entry backward read differed");
}

void rejects_sfo_install_directory_traversal() {
    ZipTestBuilder builder;
    builder.add("TITLE/sce_sys/package/work.bin", make_license("UP0438-PCSE01305_00-0000000000000001"));
    builder.add("TITLE/sce_sys/param.sfo", make_sfo("../../outside"));
    require_code(TestContext{}.open_error(builder), packages::NoNpDrmZipErrorCode::invalid_sfo);
}

void rejects_out_of_bounds_sfo_entries() {
    auto param = make_sfo("PCSE01305");
    write_u32(param, 20 + 12, std::numeric_limits<uint32_t>::max());
    ZipTestBuilder builder;
    builder.add("TITLE/sce_sys/package/work.bin", make_license("UP0438-PCSE01305_00-0000000000000001"));
    builder.add("TITLE/sce_sys/param.sfo", param);
    require_code(TestContext{}.open_error(builder), packages::NoNpDrmZipErrorCode::invalid_sfo);
}

void rejects_nul_in_sfo_install_directories() {
    TestContext context;
    ZipTestBuilder savedata;
    savedata.add("TITLE/sce_sys/package/work.bin", make_license("UP0438-PCSE01305_00-0000000000000001"));
    savedata.add("TITLE/sce_sys/param.sfo", make_sfo(std::string("..\0x", 4)));
    require_code(context.open_error(savedata), packages::NoNpDrmZipErrorCode::invalid_sfo);

    ZipTestBuilder addcont;
    addcont.add("TITLE/sce_sys/package/work.bin", make_license("UP0438-PCSE01305_00-0000000000000001"));
    addcont.add("TITLE/sce_sys/param.sfo", make_sfo("PCSE01305", std::string("..\0x", 4)));
    require_code(context.open_error(addcont), packages::NoNpDrmZipErrorCode::invalid_sfo);
}

void rejects_mismatched_license_identity() {
    ZipTestBuilder builder;
    builder.add("TITLE/sce_sys/package/work.bin", make_license("UP0000-PCSE01305_00-MISMATCH000000000"));
    builder.add("TITLE/sce_sys/param.sfo", make_sfo("PCSE01305"));
    require_code(TestContext{}.open_error(builder), packages::NoNpDrmZipErrorCode::invalid_license);
}

void rejects_unbounded_files_db_page_size_before_crypto() {
    sce_ng_pfs_header_t header{};
    std::memcpy(header.magic, MAGIC_WORD, 8);
    header.pageSize = std::numeric_limits<uint32_t>::max();
    std::string bytes(reinterpret_cast<const char *>(&header), sizeof(header));
    std::istringstream input(bytes, std::ios::in | std::ios::binary);
    std::ostringstream output;
    std::array<uint8_t, 0x10> klicensee{};
    auto cryptops = CryptoOperationsFactory::create(CryptoOperationsTypes::openssl);
    auto f00d = F00DKeyEncryptorFactory::create(F00DEncryptorTypes::native, cryptops);
    FilesDbParser parser(cryptops, f00d, output, klicensee.data(), psvpfs::path{});
    if (parser.parse(input, true) >= 0)
        throw std::runtime_error("files.db with an unbounded page size unexpectedly parsed");
}

void rejects_wrapped_unicv_data_size() {
    sce_irodb_header_t header{};
    std::memcpy(header.magic, DB_MAGIC_WORD, 8);
    header.version = UNICV_EXPECTED_VERSION_2;
    header.blockSize = EXPECTED_PAGE_SIZE;
    header.unk2 = std::numeric_limits<uint32_t>::max();
    header.unk3 = std::numeric_limits<uint32_t>::max();
    header.dataSize = std::numeric_limits<uint64_t>::max() - EXPECTED_PAGE_SIZE + 1;
    std::string bytes(reinterpret_cast<const char *>(&header), sizeof(header));
    std::istringstream input(bytes, std::ios::in | std::ios::binary);
    std::ostringstream output;
    sce_irodb_t database(output);
    if (database.read(input))
        throw std::runtime_error("unicv.db with wrapped data size unexpectedly parsed");
}

void rejects_icv_sector_count_above_merkle_tail_capacity() {
    std::vector<uint8_t> bytes(EXPECTED_PAGE_SIZE);
    sce_icvdb_header_t header{};
    std::memcpy(header.magic, CV_DB_MAGIC_WORD, 8);
    header.version = ICV_EXPECTED_VERSION_2;
    header.fileSectorSize = EXPECTED_FILE_SECTOR_SIZE;
    header.pageSize = EXPECTED_PAGE_SIZE;
    header.unk0 = std::numeric_limits<uint32_t>::max();
    header.unk1 = std::numeric_limits<uint32_t>::max();
    header.nSectors = (ICV_NUM_ENTRIES + 1) / 2 + 1;
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::string content(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    std::istringstream input(content, std::ios::in | std::ios::binary);
    std::ostringstream output;
    sce_icvdb_header_proxy_t proxy(output);
    if (!proxy.read(input) || proxy.validate())
        throw std::runtime_error("icv.db sector count above tail capacity unexpectedly validated");
}

void validates_deep_hash_tree_without_recursion() {
    constexpr uint32_t page_count = 32768;
    std::vector<sce_ng_pfs_block_t> blocks(page_count);
    std::multimap<uint32_t, page_icv_data> page_icvs;
    for (uint32_t page = 0; page < page_count; ++page) {
        blocks[page].page = page;
        page_icv_data child{};
        child.page = page;
        std::fill(std::begin(child.icv), std::end(child.icv), static_cast<uint8_t>(page));
        page_icvs.emplace(page == 0 ? std::numeric_limits<uint32_t>::max() : page - 1, child);
        if (page != 0) {
            sce_ng_pfs_hash_t hash{};
            std::copy(std::begin(child.icv), std::end(child.icv), std::begin(hash.data));
            blocks[page - 1].hashes.push_back(hash);
        }
    }
    if (!validate_hash_tree(0, 0, blocks, page_icvs))
        throw std::runtime_error("deep acyclic hash tree failed iterative validation");
}

} // namespace

int main() {
    using Test = std::pair<std::string_view, void (*)()>;
    const std::array<Test, 18> tests{ {
        { "rejects_empty_archives", rejects_empty_archives },
        { "rejects_absolute_and_parent_paths", rejects_absolute_and_parent_paths },
        { "rejects_nul_in_entry_name", rejects_nul_in_entry_name },
        { "rejects_multiple_title_roots", rejects_multiple_title_roots },
        { "rejects_duplicate_normalized_and_case_folded_names", rejects_duplicate_normalized_and_case_folded_names },
        { "rejects_encrypted_and_unsupported_entries", rejects_encrypted_and_unsupported_entries },
        { "rejects_oversized_small_metadata_before_extraction", rejects_oversized_small_metadata_before_extraction },
        { "rejects_mismatched_local_header_names", rejects_mismatched_local_header_names },
        { "accepts_utf8_entry_names", accepts_utf8_entry_names },
        { "reads_deflated_entries_forward_and_backward", reads_deflated_entries_forward_and_backward },
        { "rejects_sfo_install_directory_traversal", rejects_sfo_install_directory_traversal },
        { "rejects_out_of_bounds_sfo_entries", rejects_out_of_bounds_sfo_entries },
        { "rejects_nul_in_sfo_install_directories", rejects_nul_in_sfo_install_directories },
        { "rejects_mismatched_license_identity", rejects_mismatched_license_identity },
        { "rejects_unbounded_files_db_page_size_before_crypto", rejects_unbounded_files_db_page_size_before_crypto },
        { "rejects_wrapped_unicv_data_size", rejects_wrapped_unicv_data_size },
        { "rejects_icv_sector_count_above_merkle_tail_capacity", rejects_icv_sector_count_above_merkle_tail_capacity },
        { "validates_deep_hash_tree_without_recursion", validates_deep_hash_tree_without_recursion },
    } };
    try {
        for (const auto &[name, test] : tests) {
            test();
            std::cout << "PASS: " << name << '\n';
        }
        std::cout << "PASSED: " << tests.size() << " tests\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
