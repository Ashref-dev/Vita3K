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

#include "nonpdrm_pfs_read_only_mount.h"
#include "nonpdrm_zip_source.h"

#include <CryptoOperationsFactory.h>
#include <F00DKeyEncryptorFactory.h>
#include <PfsMount.h>

#include <util/bytes.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string_view>

namespace packages {
namespace {

bool is_safe_component(std::string_view value, size_t maximum_size) {
    if (value.empty() || value.size() > maximum_size || value == "." || value == ".." || value.find('\0') != std::string_view::npos)
        return false;
    return value.find_first_of("/\\:") == std::string_view::npos;
}

bool is_valid_title_id(std::string_view value) {
    return value.size() == 9 && is_safe_component(value, 9)
        && std::ranges::all_of(value, [](unsigned char character) {
               return std::isdigit(character) || (character >= 'A' && character <= 'Z');
           });
}

std::expected<sfo::SfoAppInfo, NoNpDrmZipError> parse_app_info(
    const std::vector<uint8_t> &content, int language) {
    sfo::SfoAppInfo info;
    if (!sfo::get_param_info(info, content, language))
        return std::unexpected(NoNpDrmZipError{ NoNpDrmZipErrorCode::invalid_sfo, "invalid param.sfo structure" });
    if (info.app_title_id.empty() || info.app_content_id.empty())
        return std::unexpected(NoNpDrmZipError{ NoNpDrmZipErrorCode::invalid_sfo, "param.sfo lacks title identity" });
    if (!is_valid_title_id(info.app_title_id)
        || !is_safe_component(info.app_savedata, 64)
        || !is_safe_component(info.app_addcont, 64))
        return std::unexpected(NoNpDrmZipError{ NoNpDrmZipErrorCode::invalid_sfo, "param.sfo contains an invalid title or install directory" });
    return info;
}

} // namespace

NoNpDrmZipResult open_nonpdrm_zip_direct(
    const std::filesystem::path &archive_path, NoNpDrmZipOptions options) {
    if (options.maximum_small_metadata_size == 0 || options.maximum_pfs_metadata_size == 0)
        return std::unexpected(NoNpDrmZipError{ NoNpDrmZipErrorCode::metadata_too_large, "metadata bounds must be nonzero" });
    auto source = detail::NoNpDrmZipSource::create(archive_path, options.maximum_small_metadata_size);
    if (!source)
        return std::unexpected(source.error());

    auto license_bytes = (*source)->read_small_file("sce_sys/package/work.bin", options.maximum_small_metadata_size);
    if (!license_bytes || license_bytes->size() != sizeof(SceNpDrmLicense))
        return std::unexpected(NoNpDrmZipError{ NoNpDrmZipErrorCode::invalid_license,
            license_bytes ? "work.bin is not a 512-byte license" : license_bytes.error().message });
    SceNpDrmLicense license{};
    static_assert(sizeof(license) == 512);
    std::memcpy(&license, license_bytes->data(), sizeof(license));

    auto param = (*source)->read_small_file("sce_sys/param.sfo", options.maximum_small_metadata_size);
    if (!param)
        return std::unexpected(param.error());
    auto app_info = parse_app_info(*param, options.sys_language);
    if (!app_info)
        return std::unexpected(app_info.error());

    const auto license_content_end = std::find(std::begin(license.content_id), std::end(license.content_id), '\0');
    if (license_content_end == std::end(license.content_id)
        || std::string_view(license.content_id, static_cast<size_t>(license_content_end - std::begin(license.content_id))) != app_info->app_content_id)
        return std::unexpected(NoNpDrmZipError{ NoNpDrmZipErrorCode::invalid_license,
            "work.bin content ID does not match param.sfo" });
    license.sku_flag = byte_swap(license.sku_flag);

    auto cryptops = CryptoOperationsFactory::create(CryptoOperationsTypes::openssl);
    auto f00d = F00DKeyEncryptorFactory::create(F00DEncryptorTypes::native, cryptops);
    std::ostringstream pfs_log;
    psvpfsparser::PfsMountOptions pfs_options;
    pfs_options.maximum_metadata_file_size = options.maximum_pfs_metadata_size;
    const std::span<const uint8_t, 0x10> klicensee(license.key);
    auto pfs = psvpfsparser::PfsMount::create(*source, cryptops, f00d, klicensee, pfs_log, pfs_options);
    if (!pfs)
        return std::unexpected(NoNpDrmZipError{ NoNpDrmZipErrorCode::pfs,
            pfs.error().message + (pfs_log.str().empty() ? "" : ": " + pfs_log.str()) });

    return NoNpDrmZipDirectPlay{
        .mount = detail::make_pfs_read_only_mount(*pfs, *source),
        .app_info = std::move(*app_info),
        .license = license,
        .title_root = (*source)->title_root(),
    };
}

} // namespace packages
