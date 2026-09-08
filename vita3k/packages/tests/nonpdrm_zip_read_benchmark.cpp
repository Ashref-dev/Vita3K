#include <packages/nonpdrm_zip_direct.h>
#include "../src/nonpdrm_zip_source.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

using Reader = std::function<bool(uint64_t, std::span<uint8_t>)>;

static void measure(const char *label, const Reader &read, const std::vector<uint64_t> &offsets, size_t size) {
    std::vector<uint8_t> buffer(size);
    std::vector<double> times;
    uint64_t digest = 14695981039346656037ULL;
    double total = 0;
    for (const auto offset : offsets) {
        const auto started = std::chrono::steady_clock::now();
        if (!read(offset, buffer))
            throw std::runtime_error("read failed at " + std::to_string(offset));
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
        times.push_back(us);
        total += us;
        for (const auto byte : buffer)
            digest = (digest ^ byte) * 1099511628211ULL;
    }
    std::sort(times.begin(), times.end());
    std::cout << label << " reads=" << offsets.size() << " bytes-per-read=" << size
              << " total-ms=" << total / 1000 << " p50-us=" << times[times.size() / 2]
              << " p95-us=" << times[times.size() * 95 / 100] << " max-us=" << times.back()
              << " digest=" << std::hex << digest << std::dec << '\n';
}

int main(int argc, char **argv) {
    try {
        if (argc != 3) {
            std::cerr << "usage: packages-nonpdrm-zip-benchmark <NoNpDrm.zip> <relative-file>\n";
            return 2;
        }
        const std::filesystem::path path(argv[1]);
        const auto before_size = std::filesystem::file_size(path);
        const auto before_time = std::filesystem::last_write_time(path);
        const auto opened = packages::open_nonpdrm_zip_direct(path);
        if (!opened)
            throw std::runtime_error(opened.error().message);
        const auto raw = packages::detail::NoNpDrmZipSource::create(path, 64 * 1024 * 1024);
        if (!raw)
            throw std::runtime_error(raw.error().message);
        const std::string file(argv[2]);
        const auto stat = opened->mount->stat(file);
        if (!stat || stat->size < 16 * 1024 * 1024)
            throw std::runtime_error("benchmark requires a file of at least 16 MiB");
        const Reader mounted = [&](uint64_t offset, std::span<uint8_t> output) {
            return opened->mount->read_at(file, offset, output) == output.size();
        };
        const Reader stored = [&](uint64_t offset, std::span<uint8_t> output) {
            return (*raw)->read_at(file, offset, output).has_value();
        };
        std::vector<uint64_t> sequential, repeated, scattered;
        const uint64_t base = 4 * 1024 * 1024;
        const uint64_t windows = std::min<uint64_t>((stat->size - 65536 - base) / (4 * 1024 * 1024), 128);
        for (uint64_t i = 0; i < 256; ++i) {
            sequential.push_back(base + i * 4096);
            repeated.push_back(base + (i % 16) * 4096);
            scattered.push_back(base + ((i * 2654435761ULL) % windows) * (4 * 1024 * 1024));
        }
        std::cout << "title=" << opened->app_info.app_title_id << " file=" << file << " size=" << stat->size << '\n';
        measure("raw-sequential", stored, sequential, 4096);
        measure("pfs-sequential", mounted, sequential, 4096);
        measure("raw-repeated", stored, repeated, 4096);
        measure("pfs-repeated", mounted, repeated, 4096);
        measure("raw-scattered", stored, scattered, 4096);
        measure("pfs-scattered", mounted, scattered, 4096);
        measure("pfs-scattered-warm", mounted, scattered, 4096);
        if (std::filesystem::file_size(path) != before_size || std::filesystem::last_write_time(path) != before_time)
            throw std::runtime_error("archive changed during benchmark");
        std::cout << "source-unchanged=true writes-or-extractions=0\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
