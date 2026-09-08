#pragma once

#include <io/read_only_mount.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace packages::detail {

class ReadOnlyBlockCache {
    using Key = std::pair<std::string, uint64_t>;
    using Block = std::shared_ptr<const std::vector<uint8_t>>;
    using Lru = std::list<std::pair<Key, Block>>;
    const size_t maximum_blocks_;
    mutable std::mutex mutex_;
    mutable Lru blocks_;
    mutable std::map<Key, Lru::iterator> index_;
    mutable size_t resident_bytes_ = 0;

public:
    static constexpr size_t block_size = 64 * 1024;
    explicit ReadOnlyBlockCache(size_t byte_budget)
        : maximum_blocks_(byte_budget / block_size) {}

    // The caller owns source-immutability checks, including reads served from cache.
    template <typename Reader>
    std::expected<size_t, ReadOnlyMountError> read(std::string_view path, uint64_t file_size,
        uint64_t offset, std::span<uint8_t> output, Reader &&reader) const {
        if (offset >= file_size)
            return size_t{0};
        const auto count = static_cast<size_t>(std::min<uint64_t>(output.size(), file_size - offset));
        if (count == 0)
            return size_t{0};
        if (maximum_blocks_ == 0)
            return reader(offset, output.first(count));

        size_t copied = 0;
        while (copied < count) {
            const auto position = offset + copied;
            const auto start = position - position % block_size;
            const Key key{std::string(path), start};
            Block block;
            {
                const std::scoped_lock lock(mutex_);
                const auto found = index_.find(key);
                if (found != index_.end()) {
                    blocks_.splice(blocks_.begin(), blocks_, found->second);
                    block = found->second->second;
                }
            }
            if (!block) {
                const auto load_size = static_cast<size_t>(std::min<uint64_t>(block_size, file_size - start));
                auto loaded = std::make_shared<std::vector<uint8_t>>(load_size);
                // Never hold the cache lock over drive I/O or PFS decryption.
                const auto result = reader(start, std::span<uint8_t>(*loaded));
                if (!result)
                    return std::unexpected(result.error());
                if (*result != load_size)
                    return std::unexpected(ReadOnlyMountError::io_error);

                const std::scoped_lock lock(mutex_);
                const auto existing = index_.find(key);
                if (existing != index_.end()) {
                    blocks_.splice(blocks_.begin(), blocks_, existing->second);
                    block = existing->second->second;
                } else {
                    while (blocks_.size() >= maximum_blocks_) {
                        resident_bytes_ -= blocks_.back().second->size();
                        index_.erase(blocks_.back().first);
                        blocks_.pop_back();
                    }
                    block = std::move(loaded);
                    blocks_.emplace_front(key, block);
                    index_.emplace(key, blocks_.begin());
                    resident_bytes_ += block->size();
                }
            }
            const auto within = static_cast<size_t>(position - start);
            const auto length = std::min(count - copied, block->size() - within);
            std::copy_n(block->begin() + within, length, output.begin() + copied);
            copied += length;
        }
        return count;
    }

    size_t resident_bytes() const {
        const std::scoped_lock lock(mutex_);
        return resident_bytes_;
    }
};

}
