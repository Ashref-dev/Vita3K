#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vkutil {

struct UploadBufferAllocation {
    std::size_t buffer_index;
    uint64_t offset;
    uint64_t capacity;
};

class UploadBufferAllocator {
    uint64_t chunk_capacity;
    std::vector<uint64_t> capacities;
    std::size_t buffer_index = 0;
    uint64_t cursor = 0;

public:
    explicit UploadBufferAllocator(uint64_t capacity)
        : chunk_capacity(capacity) {}

    UploadBufferAllocation allocate(uint32_t size) {
        uint64_t offset = (cursor + 15) & ~uint64_t{15};
        if (buffer_index < capacities.size() && (offset > capacities[buffer_index] || size > capacities[buffer_index] - offset)) {
            ++buffer_index;
            offset = 0;
        }
        if (buffer_index == capacities.size())
            capacities.push_back(std::max<uint64_t>(chunk_capacity, size));
        else if (size > capacities[buffer_index])
            capacities[buffer_index] = std::max<uint64_t>(chunk_capacity, size);

        cursor = offset + size;
        return { buffer_index, offset, capacities[buffer_index] };
    }

    // The owning frame must have completed all GPU work before resetting.
    void reset() {
        buffer_index = 0;
        cursor = 0;
    }

    std::size_t buffer_count() const { return capacities.size(); }

    void clear() {
        capacities.clear();
        reset();
    }
};

}
