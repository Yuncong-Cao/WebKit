#pragma once

#include "config.h"

// Always compile disk data support
// #if ENABLE(PARKABLE_STRINGS)

#include "DiskDataMetadata.h"
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/FileSystem.h>
#include <wtf/FileHandle.h>
#include <span>
#include <map>

namespace WebCore {

/**
 * DiskDataAllocator - Centralized chunk-based disk storage allocator
 * 
 * Manages allocation and deallocation of chunks within a single disk file,
 * providing efficient storage for parkable strings and other data.
 * 
 * Key features:
 * - Single file backend with chunk-based allocation 
 * - Free list management with merging for fragmentation reduction  
 * - Thread-safe operations with WebKit's I/O patterns
 * - Configurable capacity limits with graceful degradation
 * - RAII ReservedChunk pattern to prevent leaks
 * 
 * Allocation strategy:
 * 1. Exact fit (reuse chunk of exact size)
 * 2. Worst fit (use largest available chunk to reduce fragmentation)
 * 3. Linear allocation at file tail
 * 
 * Configuration (WebKit pattern):
 * - Call setCapacityLimit(MB) before first use to set disk limits
 * - Or use ParkableStringManager::configureDiskCapacity() for external control
 * - Use disableCapacityLimit() for unlimited disk usage (testing)
 * 
 * WebKit I/O pattern: CacheStorageDiskStore
 */
class WEBCORE_EXPORT DiskDataAllocator {
    WTF_MAKE_NONCOPYABLE(DiskDataAllocator);
    WTF_MAKE_FAST_ALLOCATED;
public:
    virtual ~DiskDataAllocator();
    
    // Singleton access
    static DiskDataAllocator& instance();
    
    // Configuration - can be called before first use to set capacity limits
    // WebKit pattern: Configure via explicit methods rather than global flags
    void setCapacityLimit(size_t maxCapacityMB);
    void disableCapacityLimit();
    
    // Initialization - provides the backing file for storage
    // Must be called on the main thread
    void provideTemporaryFile(FileSystem::FileHandle&&);
    
    // Whether writes may succeed. This is not a guarantee. However, when this
    // returns false, writes will fail.
    bool mayWrite();
    
    // Chunk allocation interface
    // Returns valid ReservedChunk on success, nullptr if insufficient space
    // The ReservedChunk must be either written via Write() or will be
    // automatically discarded on destruction
    std::unique_ptr<ReservedChunk> tryReserveChunk(size_t size);
    
    // Writes data to a reserved chunk, returns metadata on success
    // Write(ReservedChunk, data) -> DiskDataMetadata
    std::unique_ptr<DiskDataMetadata> write(std::unique_ptr<ReservedChunk>, const Vector<uint8_t>& data);
    
    // Reads data from disk using metadata - blocking I/O operation
    // Read(metadata, output_vector)
    void read(const DiskDataMetadata&, Vector<uint8_t>& data);
    
    // Discards allocated space, making it available for reuse
    // Discard(unique_ptr<DiskDataMetadata>)
    void discard(std::unique_ptr<DiskDataMetadata>);
    
    // Statistics for monitoring and testing
    size_t diskFootprint() const;
    size_t freeChunksSize() const;
    
    // Testing support
    void setMayWriteForTesting(bool mayWrite);
    
    // Testing utilities for inspecting allocator state
    bool hasCapacityLimit() const;
    size_t maxCapacity() const;
    size_t allocatedChunksCount() const; // Number of allocated chunks
    
    // Advanced testing - check if allocator is in valid state
    bool isValidStateForTesting() const;

protected:
    DiskDataAllocator();
    
    // Actual implementations - to be overridden by test utilities
    virtual std::optional<size_t> doWrite(int64_t offset, std::span<const uint8_t> data);
    virtual void doRead(int64_t offset, std::span<uint8_t> data);
    
    // Allocation algorithms
    DiskDataMetadata findFreeChunk(size_t size) WTF_REQUIRES_LOCK(m_lock);
    void releaseChunk(const DiskDataMetadata&) WTF_REQUIRES_LOCK(m_lock);
    
    mutable Lock m_lock;
    
    // File backend - No lock needed for file I/O as files support concurrent access
    FileSystem::FileHandle m_file;
    
    // Allocation state
    // std::map for free chunks - required for lower_bound/upper_bound operations in allocation algorithm
    // We need the ordered nature and bound operations for chunk merging
    std::map<int64_t, size_t> m_freeChunks WTF_GUARDED_BY_LOCK(m_lock);
    size_t m_freeChunksSize WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    int64_t m_fileTail WTF_GUARDED_BY_LOCK(m_lock) { 0 };  // Next allocation offset
    
    // Feature flag support for capacity limits
    bool m_hasCapacityLimit { false };
    size_t m_maxCapacity { 0 };
    
    // Write permission tracking
    bool m_mayWrite { false };
    
#if ASSERT_ENABLED
    // Debug tracking of allocated chunks to catch double-free etc.
    // Use std::map for consistency
    std::map<int64_t, size_t> m_allocatedChunks WTF_GUARDED_BY_LOCK(m_lock);
#endif

    // Friend access for test utilities
    friend class InMemoryDataAllocator;

    friend class NeverDestroyed<DiskDataAllocator>;
};

} // namespace WebCore

// #endif // ENABLE(PARKABLE_STRINGS)

