#include <vkutil/vertex_buffer_pool.h>

#include <cstring>
#include <util/log.h>

namespace vkutil {

std::pair<vk::Buffer, vk::DeviceSize> VertexBufferPool::upload(vma::Allocator allocator, uint32_t size, const void *data) {
    const auto allocation = offsets.allocate(size);
    if (allocation.buffer_index == chunks.size())
        chunks.emplace_back();
    auto &chunk = chunks[allocation.buffer_index];

    // Match the vertex ring's padding for drivers that fetch RGB16 as RGBA16.
    const uint64_t padded_capacity = allocation.capacity + 2;
    if (chunk.buffer.size < padded_capacity) {
        chunk.buffer.destroy();
        chunk.buffer = Buffer(padded_capacity);
        chunk.buffer.init_buffer(vk::BufferUsageFlagBits::eVertexBuffer, vma_mapped_alloc);
        const auto properties = allocator.getAllocationMemoryProperties(chunk.buffer.allocation);
        chunk.coherent = static_cast<bool>(properties & vk::MemoryPropertyFlagBits::eHostCoherent);
        LOG_INFO("Frame-owned vertex upload storage: chunk={} capacity={} bytes", allocation.buffer_index, allocation.capacity);
    }

    if (size != 0) {
        memcpy(static_cast<uint8_t *>(chunk.buffer.mapped_data) + allocation.offset, data, size);
        if (!chunk.coherent)
            allocator.flushAllocation(chunk.buffer.allocation, allocation.offset, size);
    }
    return { chunk.buffer.buffer, allocation.offset };
}

}
