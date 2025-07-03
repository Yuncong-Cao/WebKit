#pragma once

#include "config.h"

#if ENABLE(PARKABLE_STRINGS)

#include <WebCore/DiskDataAllocator.h>
#include <span>
#include <map>
#include <algorithm>
#include <cstring>
#include <vector>

namespace TestWebKitAPI {

/**
 * InMemoryDataAllocator - Test implementation of DiskDataAllocator
 * 
 * Provides an in-memory implementation of the disk allocation interface
 * for testing purposes. This allows testing the allocation logic without
 * requiring actual disk I/O. Uses WebKit's FileHandle patterns.
 * 
 */
class InMemoryDataAllocator : public WebCore::DiskDataAllocator {
public:
    constexpr static size_t kMaxSize = 1 << 20; // 1MB
    
    InMemoryDataAllocator()
        : m_data(kMaxSize)
    {
        setMayWriteForTesting(true);
        // Enable capacity limits to match test expectations
        setCapacityLimit(1); // 1MB limit (kMaxSize = 1 << 20 = 1MB)
    }
    ~InMemoryDataAllocator() override = default;
    
    /**
     * Returns copy of free chunks map for test inspection.
     * Validates internal consistency before returning data.
     * 
     * @return Map of free chunk offsets to sizes
     */
    std::map<int64_t, size_t> FreeChunks()
    {
        WTF::Locker locker { m_lock };
        
        // Verify free chunks size tracking is correct
        size_t totalFreeSize = 0;
        for (const auto& entry : m_freeChunks)
            totalFreeSize += entry.second;
        
        ASSERT(totalFreeSize == m_freeChunksSize);
        
        // Convert std::map to std::map (they're the same type, so just return a copy)
        std::map<int64_t, size_t> result;
        for (const auto& entry : m_freeChunks)
            result[entry.first] = entry.second;
        
        return result;
    }
    
    /**
     * Validates allocator internal consistency for testing.
     * Checks free chunk tracking and state invariants.
     */
    void assertValidState() 
    {
        ASSERT(isValidStateForTesting());
    }
    
    /**
     * Returns count of allocated chunks for test verification.
     * Delegates to base class method for consistency.
     * 
     * @return Number of currently allocated chunks
     */
    size_t allocatedCount() const
    {
        return allocatedChunksCount();
    }
    
    // Static helper method - for use in tests
    static WTF::Vector<std::unique_ptr<WebCore::DiskDataMetadata>>
    Allocate(InMemoryDataAllocator* allocator, size_t size, size_t count)
    {
        WTF::Vector<std::unique_ptr<WebCore::DiskDataMetadata>> allMetadata;
        
        for (size_t i = 0; i < count; i++) {
            // Create test data
            WTF::Vector<uint8_t> testData(size);
            for (size_t j = 0; j < size; j++) {
                testData[j] = static_cast<uint8_t>((i * size + j) % 256);
            }
            
            auto reservedChunk = allocator->tryReserveChunk(size);
            ASSERT(reservedChunk);
            
            auto metadata = allocator->write(WTFMove(reservedChunk), testData);
            ASSERT(metadata);
            
            allMetadata.append(WTFMove(metadata));
        }
        
        return allMetadata;
    }

private:
    /**
     * In-memory implementation of disk write operation for mock testing.
     * Simulates disk I/O using memory buffer with size limits.
     * 
     * @param offset Byte offset in virtual file
     * @param data Data to write
     * @return Number of bytes written or nullopt on failure
     */
    std::optional<size_t> doWrite(int64_t offset,
                                std::span<const uint8_t> data) override
    {
        ASSERT(offset >= 0);
        int64_t endOffset = offset + data.size();
        if (endOffset > static_cast<int64_t>(kMaxSize)) {
            return std::nullopt;
        }

        // Use mutableSpan() instead of private data() method
        auto mutableData = m_data.mutableSpan();
        std::memcpy(mutableData.data() + static_cast<size_t>(offset), data.data(), data.size());
        m_maxOffset = std::max(endOffset, m_maxOffset);
        return data.size();
    }

    /**
     * In-memory implementation of disk read operation for mock testing.
     * Simulates disk I/O using memory buffer with bounds checking.
     * 
     * @param offset Byte offset in virtual file
     * @param data Buffer to read data into
     */
    void doRead(int64_t offset, std::span<uint8_t> data) override
    {
        ASSERT(offset >= 0);
        int64_t endOffset = offset + data.size();
        ASSERT(endOffset <= m_maxOffset);

        // Use span() instead of private data() method
        auto sourceData = m_data.span();
        std::memcpy(data.data(), sourceData.data() + static_cast<size_t>(offset), data.size());
    }
    
    int64_t m_maxOffset = 0;
    WTF::Vector<char> m_data;
};

} // namespace TestWebKitAPI

#endif // ENABLE(PARKABLE_STRINGS) 