#include <vkutil/upload_buffer_allocator.h>

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <vector>

TEST(UploadBufferAllocator, RolloverPreservesVerticesOfEarlierUnsubmittedDraws) {
    vkutil::UploadBufferAllocator allocator(64);
    const auto first = allocator.allocate(48);
    const auto second = allocator.allocate(48);

    EXPECT_NE(first.buffer_index, second.buffer_index);
    EXPECT_EQ(first.offset, 0);
    EXPECT_EQ(second.offset, 0);
}

TEST(UploadBufferAllocator, MultipleWrapsNeverReuseStorageWithinFrame) {
    vkutil::UploadBufferAllocator allocator(64);
    std::vector<vkutil::UploadBufferAllocation> draws;
    for (int i = 0; i < 100; ++i) {
        const auto allocation = allocator.allocate(20);
        for (const auto &previous : draws) {
            if (previous.buffer_index == allocation.buffer_index)
                EXPECT_GE(allocation.offset, previous.offset + 20);
        }
        EXPECT_EQ(allocation.offset % 16, 0);
        EXPECT_LE(allocation.offset + 20, allocation.capacity);
        draws.push_back(allocation);
    }
}

TEST(UploadBufferAllocator, OversizedUploadGetsSufficientStorage) {
    vkutil::UploadBufferAllocator allocator(64);
    const auto large = allocator.allocate(129);
    EXPECT_GE(large.capacity, 129);
    EXPECT_EQ(large.offset, 0);
    const auto next = allocator.allocate(16);
    EXPECT_NE(large.buffer_index, next.buffer_index);
}

TEST(UploadBufferAllocator, CompletedFrameCanReuseItsChunks) {
    vkutil::UploadBufferAllocator allocator(64);
    const auto first = allocator.allocate(48);
    const auto second = allocator.allocate(48);
    allocator.reset();
    const auto reused_first = allocator.allocate(48);
    const auto reused_second = allocator.allocate(48);
    EXPECT_EQ(first.buffer_index, reused_first.buffer_index);
    EXPECT_EQ(second.buffer_index, reused_second.buffer_index);
    EXPECT_EQ(first.offset, reused_first.offset);
    EXPECT_EQ(second.offset, reused_second.offset);
}

TEST(UploadBufferAllocator, RecyclingOneFrameDoesNotResetOtherLiveFrames) {
    std::array<vkutil::UploadBufferAllocator, 3> frames{
        vkutil::UploadBufferAllocator(64), vkutil::UploadBufferAllocator(64), vkutil::UploadBufferAllocator(64)
    };
    for (auto &frame : frames)
        frame.allocate(48);
    frames[0].reset();
    EXPECT_EQ(frames[0].allocate(48).buffer_index, 0);
    EXPECT_EQ(frames[1].allocate(48).buffer_index, 1);
    EXPECT_EQ(frames[2].allocate(48).buffer_index, 1);
}

TEST(UploadBufferAllocator, MaximumGuestUploadDoesNotOverflowOffsetArithmetic) {
    vkutil::UploadBufferAllocator allocator(64);
    const auto large = allocator.allocate(std::numeric_limits<uint32_t>::max());
    EXPECT_GE(large.capacity, std::numeric_limits<uint32_t>::max());
    const auto next = allocator.allocate(16);
    EXPECT_NE(large.buffer_index, next.buffer_index);
    EXPECT_LE(next.offset + 16, next.capacity);
}

TEST(UploadBufferAllocator, RepeatedCompletedFramesReuseHighWaterCapacity) {
    vkutil::UploadBufferAllocator allocator(64);
    for (int frame = 0; frame < 100; ++frame) {
        for (int draw = 0; draw < 10; ++draw)
            allocator.allocate(48);
        EXPECT_EQ(allocator.buffer_count(), 10);
        allocator.reset();
    }
}

TEST(UploadBufferAllocator, ReusedChunkCanGrowWithoutChangingCurrentFrameAllocations) {
    vkutil::UploadBufferAllocator allocator(64);
    allocator.allocate(48);
    allocator.allocate(48);
    allocator.reset();
    const auto first = allocator.allocate(48);
    const auto large = allocator.allocate(129);
    EXPECT_NE(first.buffer_index, large.buffer_index);
    EXPECT_EQ(first.capacity, 64);
    EXPECT_GE(large.capacity, 129);
}

TEST(UploadBufferAllocator, ExactFitUsesWholeChunkBeforeAdvancing) {
    vkutil::UploadBufferAllocator allocator(64);
    const auto first = allocator.allocate(16);
    const auto second = allocator.allocate(48);
    EXPECT_EQ(first.buffer_index, second.buffer_index);
    EXPECT_EQ(second.offset, 16);
    EXPECT_EQ(allocator.allocate(1).buffer_index, 1);
}

TEST(UploadBufferAllocator, GravityRushSizedFramesDoNotAliasOrGrowAfterWarmup) {
    constexpr uint64_t chunk_size = 64ULL * 1024 * 1024;
    constexpr uint32_t upload_size = 550976;
    std::array<vkutil::UploadBufferAllocator, 3> frames{
        vkutil::UploadBufferAllocator(chunk_size), vkutil::UploadBufferAllocator(chunk_size), vkutil::UploadBufferAllocator(chunk_size)
    };
    for (int frame = 0; frame < 30; ++frame) {
        auto &allocator = frames[frame % frames.size()];
        allocator.reset();
        vkutil::UploadBufferAllocation previous{};
        for (int draw = 0; draw < 400; ++draw) {
            const auto allocation = allocator.allocate(upload_size);
            if (draw != 0 && allocation.buffer_index == previous.buffer_index)
                EXPECT_GE(allocation.offset, previous.offset + upload_size);
            EXPECT_LE(allocation.offset + upload_size, allocation.capacity);
            previous = allocation;
        }
        EXPECT_EQ(allocator.buffer_count(), 4);
    }
}
