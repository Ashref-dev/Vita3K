#pragma once

#include <vkutil/objects.h>
#include <vkutil/upload_buffer_allocator.h>

#include <utility>
#include <vector>

namespace vkutil {

class VertexBufferPool {
    struct Chunk {
        Buffer buffer;
        bool coherent = false;
    };

    UploadBufferAllocator offsets{64ULL * 1024 * 1024};
    std::vector<Chunk> chunks;

public:
    std::pair<vk::Buffer, vk::DeviceSize> upload(vma::Allocator allocator, uint32_t size, const void *data);

    // Called only after the owning frame's submission fences have completed.
    void reset() { offsets.reset(); }

    void clear() {
        chunks.clear();
        offsets.clear();
    }
};

}
