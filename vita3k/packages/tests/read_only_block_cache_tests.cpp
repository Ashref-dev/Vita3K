#include "../src/read_only_block_cache.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <future>
#include <vector>

using packages::detail::ReadOnlyBlockCache;
constexpr auto block_size = ReadOnlyBlockCache::block_size;

struct MemoryReader {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(block_size * 4 + 7);
    size_t calls = 0;

    MemoryReader() {
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<uint8_t>((i * 17 + i / block_size) % 251);
    }

    std::expected<size_t, ReadOnlyMountError> operator()(uint64_t offset, std::span<uint8_t> output) {
        ++calls;
        if (offset > bytes.size() || output.size() > bytes.size() - offset)
            return std::unexpected(ReadOnlyMountError::io_error);
        std::copy_n(bytes.begin() + offset, output.size(), output.begin());
        return output.size();
    }
};

TEST(ReadOnlyBlockCache, NearbyReadsReuseVerifiedBlock) {
    ReadOnlyBlockCache cache(block_size * 2);
    MemoryReader source;
    std::array<uint8_t, 16> output{};
    ASSERT_EQ(cache.read("file", source.bytes.size(), 10, output, source), output.size());
    ASSERT_EQ(cache.read("file", source.bytes.size(), 100, output, source), output.size());
    EXPECT_EQ(source.calls, 1);
    EXPECT_TRUE(std::equal(output.begin(), output.end(), source.bytes.begin() + 100));
}

TEST(ReadOnlyBlockCache, CrossBlockAndShortFinalBlockAreExact) {
    ReadOnlyBlockCache cache(block_size * 2);
    MemoryReader source;
    std::array<uint8_t, 16> output{};
    ASSERT_EQ(cache.read("file", source.bytes.size(), block_size - 8, output, source), output.size());
    EXPECT_TRUE(std::equal(output.begin(), output.end(), source.bytes.begin() + block_size - 8));
    ASSERT_EQ(cache.read("file", source.bytes.size(), source.bytes.size() - 7, output, source), 7);
    EXPECT_TRUE(std::equal(output.begin(), output.begin() + 7, source.bytes.end() - 7));
    EXPECT_EQ(cache.read("file", source.bytes.size(), source.bytes.size(), output, source), 0);
    EXPECT_EQ(cache.read("file", source.bytes.size(), UINT64_MAX, output, source), 0);
}

TEST(ReadOnlyBlockCache, EvictsLeastRecentlyUsedWithinBudget) {
    ReadOnlyBlockCache cache(block_size * 2);
    MemoryReader source;
    std::array<uint8_t, 1> output{};
    for (const auto offset : std::array<size_t, 4>{0, block_size, 0, block_size * 2})
        ASSERT_EQ(cache.read("file", source.bytes.size(), offset, output, source), 1);
    EXPECT_EQ(source.calls, 3);
    EXPECT_LE(cache.resident_bytes(), block_size * 2);
    ASSERT_EQ(cache.read("file", source.bytes.size(), 0, output, source), 1);
    EXPECT_EQ(source.calls, 3);
    ASSERT_EQ(cache.read("file", source.bytes.size(), block_size, output, source), 1);
    EXPECT_EQ(source.calls, 4);
}

TEST(ReadOnlyBlockCache, FileIdentityIsPartOfCacheKey) {
    ReadOnlyBlockCache cache(block_size * 2);
    MemoryReader first, second;
    second.bytes[0] = 253;
    std::array<uint8_t, 1> output{};
    ASSERT_EQ(cache.read("first", first.bytes.size(), 0, output, first), 1);
    ASSERT_EQ(cache.read("second", second.bytes.size(), 0, output, second), 1);
    EXPECT_EQ(output[0], 253);
    ASSERT_EQ(cache.read("first", first.bytes.size(), 0, output, first), 1);
    EXPECT_EQ(output[0], first.bytes[0]);
    EXPECT_EQ(first.calls, 1);
}

TEST(ReadOnlyBlockCache, FailedOrShortFillNeverPoisonsCache) {
    ReadOnlyBlockCache cache(block_size);
    MemoryReader source;
    std::array<uint8_t, 16> output{};
    const auto failing = [](uint64_t, std::span<uint8_t>) -> std::expected<size_t, ReadOnlyMountError> {
        return std::unexpected(ReadOnlyMountError::io_error);
    };
    const auto short_read = [](uint64_t, std::span<uint8_t>) -> std::expected<size_t, ReadOnlyMountError> { return 1; };
    EXPECT_FALSE(cache.read("file", source.bytes.size(), 0, output, failing));
    EXPECT_FALSE(cache.read("file", source.bytes.size(), 0, output, short_read));
    ASSERT_EQ(cache.read("file", source.bytes.size(), 0, output, source), output.size());
    EXPECT_TRUE(std::equal(output.begin(), output.end(), source.bytes.begin()));
}

TEST(ReadOnlyBlockCache, CacheHitDoesNotWaitForUnrelatedSlowFill) {
    ReadOnlyBlockCache cache(block_size * 2);
    MemoryReader source;
    std::array<uint8_t, 1> output{};
    ASSERT_EQ(cache.read("ready", source.bytes.size(), 0, output, source), 1);
    std::promise<void> started, release;
    auto released = release.get_future().share();
    auto miss = std::async(std::launch::async, [&] {
        std::array<uint8_t, 1> local{};
        auto slow = [&](uint64_t offset, std::span<uint8_t> target) {
            started.set_value();
            released.wait();
            return source(offset, target);
        };
        return cache.read("slow", source.bytes.size(), 0, local, slow);
    });
    started.get_future().wait();
    auto hit = std::async(std::launch::async, [&] {
        std::array<uint8_t, 1> local{};
        return cache.read("ready", source.bytes.size(), 0, local, source);
    });
    const auto status = hit.wait_for(std::chrono::seconds(2));
    release.set_value();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_EQ(hit.get(), 1);
    EXPECT_EQ(miss.get(), 1);
}

TEST(ReadOnlyBlockCache, EmptyReadsAndDisabledCacheStayBounded) {
    ReadOnlyBlockCache cache(0);
    MemoryReader source;
    std::array<uint8_t, 1> output{};
    EXPECT_EQ(cache.read("file", source.bytes.size(), 0, std::span<uint8_t>{}, source), 0);
    EXPECT_EQ(source.calls, 0);
    ASSERT_EQ(cache.read("file", source.bytes.size(), 0, output, source), 1);
    ASSERT_EQ(cache.read("file", source.bytes.size(), 0, output, source), 1);
    EXPECT_EQ(source.calls, 2);
    EXPECT_EQ(cache.resident_bytes(), 0);
}
