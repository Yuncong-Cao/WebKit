#include "config.h"
#include "DiskDataAllocator.h"

// Always compile disk data support
// #if ENABLE(PARKABLE_STRINGS)

#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/FileSystem.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>
#include <span>
#include <algorithm>
#include <map>

// Platform-specific includes for high-performance positioned I/O
#if OS(UNIX)
#include <unistd.h>    // For pread(), pwrite()
#include <errno.h>     // For errno, EINTR
#endif

namespace WebCore {

namespace {
constexpr size_t MB = 1024 * 1024;

// Default capacity for parkable strings when enabled
// Can be overridden via setCapacityLimit()
constexpr size_t kDefaultParkableStringDiskCapacityMB = 50;
}

/**
 * Constructor that initializes without capacity limits by default.
 * Capacity limits can be configured later via setCapacityLimit() if needed.
 */
DiskDataAllocator::DiskDataAllocator()
{
    // Initialize without capacity limits by default
    // Capacity limits can be configured via setCapacityLimit() if needed
}

/**
 * Default destructor.
 * Free chunks map and other resources are automatically cleaned up.
 */
DiskDataAllocator::~DiskDataAllocator() = default;

/**
 * Singleton accessor using NeverDestroyed pattern.
 * Thread-safe initialization with static local variable.
 * 
 * @return Reference to the singleton allocator instance
 */
// static
DiskDataAllocator& DiskDataAllocator::instance()
{
    static NeverDestroyed<DiskDataAllocator> allocator;
    return allocator.get();
}

/**
 * Configures maximum disk usage limit in megabytes.
 * Prevents allocations when total usage would exceed this limit.
 * 
 * @param maxCapacityMB Maximum capacity in megabytes
 */
void DiskDataAllocator::setCapacityLimit(size_t maxCapacityMB)
{
    Locker locker { m_lock };
    m_hasCapacityLimit = true;
    m_maxCapacity = maxCapacityMB * MB;
}

/**
 * Removes disk usage restrictions for unlimited allocation.
 * Useful for testing or when disk space management is handled externally.
 */
void DiskDataAllocator::disableCapacityLimit()
{
    Locker locker { m_lock };
    m_hasCapacityLimit = false;
    m_maxCapacity = 0;
}

/**
 * Returns whether disk writes are currently permitted by policy.
 * Can be disabled for testing or when disk access should be prevented.
 * 
 * @return true if writes are allowed
 */
bool DiskDataAllocator::mayWrite()
{
    Locker locker { m_lock };
    return m_mayWrite;
}

/**
 * Test helper to control write permissions.
 * Allows testing error handling when disk writes are disabled.
 * 
 * @param mayWrite Whether writes should be permitted
 */
void DiskDataAllocator::setMayWriteForTesting(bool mayWrite)
{
    Locker locker { m_lock };
    m_mayWrite = mayWrite;
}

DiskDataMetadata DiskDataAllocator::findFreeChunk(size_t size)
{
    // 1. Exact fit (reuse chunk of exact size)
    // 2. Worst fit (use largest available chunk to reduce fragmentation)
    
    DiskDataMetadata chosenChunk { -1, 0 };
    size_t worstFitSize = 0;
    
    for (const auto& chunk : m_freeChunks) {
        size_t chunkSize = chunk.second;
        if (size == chunkSize) {
            // Exact fit - use immediately
            chosenChunk = DiskDataMetadata { chunk.first, chunk.second };
            break;
        } else if (chunkSize > size && chunkSize >= worstFitSize) {
            // Worst fit candidate - prefer largest available space
            // When sizes are equal, prefer higher offsets for consistent behavior
            chosenChunk = DiskDataMetadata { chunk.first, chunk.second };
            worstFitSize = chunkSize;
        }
    }
    
    if (chosenChunk.startOffset() != -1) {
        // Remove chosen chunk from free list
        m_freeChunksSize -= size;
        m_freeChunks.erase(chosenChunk.startOffset());
        
        // Split chunk if necessary
        if (chosenChunk.size() > size) {
            int64_t remainderOffset = chosenChunk.startOffset() + static_cast<int64_t>(size);
            size_t remainderSize = chosenChunk.size() - size;
            
            auto insertResult = m_freeChunks.insert({remainderOffset, remainderSize});
            ASSERT_UNUSED(insertResult, insertResult.second);
            
            // Update chosen chunk size to requested size
            chosenChunk = DiskDataMetadata { chosenChunk.startOffset(), size };
        }
    }
    
    return chosenChunk;
}

void DiskDataAllocator::releaseChunk(const DiskDataMetadata& metadata)
{
    DiskDataMetadata chunk { metadata.startOffset(), metadata.size() };
    
    ASSERT(m_freeChunks.find(chunk.startOffset()) == m_freeChunks.end());
    
    // Use lower_bound for efficient left neighbor detection
    auto lowerBound = m_freeChunks.lower_bound(chunk.startOffset());
    ASSERT(m_freeChunks.upper_bound(chunk.startOffset()) == 
           m_freeChunks.lower_bound(chunk.startOffset()));
    
    if (lowerBound != m_freeChunks.begin()) {
        // There is a chunk to the left
        auto left = --lowerBound;
        // Can merge with the left chunk?
        int64_t leftChunkEnd = left->first + static_cast<int64_t>(left->second);
        ASSERT(leftChunkEnd <= chunk.startOffset());
        if (leftChunkEnd == chunk.startOffset()) {
            chunk = DiskDataMetadata { left->first, left->second + chunk.size() };
            m_freeChunksSize -= left->second;
            m_freeChunks.erase(left);
        }
    }
    
    auto right = m_freeChunks.upper_bound(chunk.startOffset());
    if (right != m_freeChunks.end()) {
        ASSERT(right->first != chunk.startOffset());
        int64_t chunkEnd = chunk.startOffset() + static_cast<int64_t>(chunk.size());
        ASSERT(chunkEnd <= right->first);
        if (right->first == chunkEnd) {
            chunk = DiskDataMetadata { chunk.startOffset(), chunk.size() + right->second };
            m_freeChunksSize -= right->second;
            m_freeChunks.erase(right);
        }
    }
    
    auto insertResult = m_freeChunks.insert({chunk.startOffset(), chunk.size()});
    ASSERT_UNUSED(insertResult, insertResult.second);
    m_freeChunksSize += chunk.size();
}

std::unique_ptr<ReservedChunk> DiskDataAllocator::tryReserveChunk(size_t size)
{
    Locker locker { m_lock };
    if (!m_mayWrite) {
        return nullptr;
    }

    DiskDataMetadata chosenChunk = findFreeChunk(size);
    if (chosenChunk.startOffset() < 0) {
        if (m_hasCapacityLimit && m_fileTail + size > m_maxCapacity) {
            return nullptr;
        }
        chosenChunk = DiskDataMetadata { m_fileTail, size };
        m_fileTail += size;
    }

#if ASSERT_ENABLED
    m_allocatedChunks.insert({chosenChunk.startOffset(), chosenChunk.size()});
#endif

    return makeUnique<ReservedChunk>(
        this, makeUnique<DiskDataMetadata>(
                chosenChunk.startOffset(), chosenChunk.size()));
}

std::unique_ptr<DiskDataMetadata> DiskDataAllocator::write(
    std::unique_ptr<ReservedChunk> chunk,
    const Vector<uint8_t>& data)
{
    std::unique_ptr<DiskDataMetadata> metadata = chunk->take();
    ASSERT(metadata);

    auto written = doWrite(metadata->startOffset(), data.span().first(metadata->size()));

    if (metadata->size() != written) {
        discard(WTFMove(metadata));

        // Assume that the error is not transient. This can happen if the disk is
        // full for instance, in which case it is likely better not to try writing
        // later.
        Locker locker { m_lock };
        m_mayWrite = false;
        return nullptr;
    }

    return metadata;
}

void DiskDataAllocator::read(const DiskDataMetadata& metadata,
                             Vector<uint8_t>& data)
{
    // Doesn't need locking as files support concurrent access, and we don't
    // update metadata.
    doRead(metadata.startOffset(), data.mutableSpan().first(metadata.size()));

#if ASSERT_ENABLED
    {
        Locker locker { m_lock };
        auto iterator = m_allocatedChunks.find(metadata.startOffset());
        ASSERT(iterator != m_allocatedChunks.end());
        ASSERT(metadata.size() == iterator->second);
    }
#endif
}

void DiskDataAllocator::discard(std::unique_ptr<DiskDataMetadata> metadata)
{
    Locker locker { m_lock };
    ASSERT(m_mayWrite || m_file.isValid());

#if ASSERT_ENABLED
    auto iterator = m_allocatedChunks.find(metadata->startOffset());
    ASSERT(iterator != m_allocatedChunks.end());
    ASSERT(metadata->size() == iterator->second);
    m_allocatedChunks.erase(iterator);
#endif

    releaseChunk(*metadata);
}

/**
 * Returns total allocated disk space across all chunks.
 * Includes both used and free chunks in the file.
 * 
 * @return Total disk footprint in bytes
 */
size_t DiskDataAllocator::diskFootprint() const
{
    Locker locker { m_lock };
    return m_fileTail;
}

/**
 * Returns combined size of all free chunks available for reuse.
 * Useful for monitoring fragmentation and available space.
 * 
 * @return Total size of free chunks in bytes
 */
size_t DiskDataAllocator::freeChunksSize() const
{
    Locker locker { m_lock };
    return m_freeChunksSize;
}

/**
 * Sets the backing file used for disk storage operations.
 * Must be called from main thread before any allocations.
 * 
 * @param file Valid file handle for storage operations
 */
void DiskDataAllocator::provideTemporaryFile(FileSystem::FileHandle&& file)
{
    Locker locker { m_lock };
    ASSERT(isMainThread());
    ASSERT(!m_file.isValid());
    ASSERT(!m_mayWrite);

    m_file = WTFMove(file);
    m_mayWrite = m_file.isValid();
}

std::optional<size_t> DiskDataAllocator::doWrite(int64_t offset, std::span<const uint8_t> data)
{
    if (!m_file.isValid())
        return std::nullopt;
    
#if OS(UNIX)
    // OPTIMIZATION: Use positioned I/O (pwrite) on UNIX platforms
    // This avoids the 2x syscall overhead of seek() + write()
    int fd = m_file.platformHandle();
    ssize_t written;
    do {
        written = ::pwrite(fd, data.data(), data.size(), offset);
    } while (written == -1 && errno == EINTR);
    
    if (written >= 0) {
        return static_cast<size_t>(written);
    }
    
    // WebKit pattern: Detailed error logging
    WTFLogAlways("DISK: pwrite failed. errno = %d, written = %zd, expected = %zu", 
                errno, written, data.size());
    return std::nullopt;
#else
    // Fallback: Use WebKit's FileHandle seek() + write() for non-UNIX platforms
    auto seekResult = m_file.seek(offset, FileSystem::FileSeekOrigin::Beginning);
    if (!seekResult) {
        WTFLogAlways("DISK: Cannot seek to offset %lld for write", static_cast<long long>(offset));
        return std::nullopt;
    }
    
    auto written = m_file.write(data);
    
    if (!written || written.value() != data.size()) {
        WTFLogAlways("DISK: Cannot write to disk. written = %zu, expected = %zu", 
                    static_cast<size_t>(written.value_or(0)), data.size());
    }
    
    return written;
#endif
}

void DiskDataAllocator::doRead(int64_t offset, std::span<uint8_t> data)
{
    if (!m_file.isValid()) {
        WTFLogAlways("DISK: File not valid for reading");
        ASSERT_NOT_REACHED();
        return;
    }
    
#if OS(UNIX)
    // OPTIMIZATION: Use positioned I/O (pread) on UNIX platforms
    // This avoids the 2x syscall overhead of seek() + read()
    int fd = m_file.platformHandle();
    ssize_t totalRead = 0;
    
    // Handle partial reads (common with large data)
    while (totalRead < static_cast<ssize_t>(data.size())) {
        ssize_t currentRead;
        auto remainingData = data.subspan(totalRead);
        do {
            currentRead = ::pread(fd, 
                                remainingData.data(), 
                                remainingData.size(), 
                                offset + totalRead);
        } while (currentRead == -1 && errno == EINTR);
        
        if (currentRead <= 0) {
            WTFLogAlways("DISK: pread failure - likely file corruption. read = %zd, expected = %zu, errno = %d",
                        currentRead, data.size(), errno);
            ASSERT_NOT_REACHED();
            return;
        }
        
        totalRead += currentRead;
    }
#else
    // Fallback: Use WebKit's FileHandle seek() + read() for non-UNIX platforms
    auto seekResult = m_file.seek(offset, FileSystem::FileSeekOrigin::Beginning);
    if (!seekResult) {
        WTFLogAlways("DISK: Cannot seek to offset %lld for read", static_cast<long long>(offset));
        ASSERT_NOT_REACHED();
        return;
    }
    
    auto read = m_file.read(data);
    
    if (!read || read.value() != data.size()) {
        WTFLogAlways("DISK: Read failure - likely file corruption. read = %zu, expected = %zu",
                    static_cast<size_t>(read.value_or(0)), data.size());
        ASSERT_NOT_REACHED();
    }
#endif
}

// Testing utilities implementation

/**
 * Returns whether capacity limits are currently enabled.
 * Used to check if allocations are size-constrained.
 * 
 * @return true if capacity limits are active
 */
bool DiskDataAllocator::hasCapacityLimit() const
{
    Locker locker { m_lock };
    return m_hasCapacityLimit;
}

/**
 * Returns maximum capacity limit in bytes.
 * Only meaningful when hasCapacityLimit() returns true.
 * 
 * @return Maximum capacity in bytes
 */
size_t DiskDataAllocator::maxCapacity() const
{
    Locker locker { m_lock };
    return m_maxCapacity;
}

/**
 * Returns count of allocated chunks for debugging.
 * Only tracked in debug builds for performance reasons.
 * 
 * @return Number of allocated chunks (0 in release builds)
 */
size_t DiskDataAllocator::allocatedChunksCount() const
{
#if ASSERT_ENABLED
    Locker locker { m_lock };
    return m_allocatedChunks.size();
#else
    return 0; // Not tracked in release builds
#endif
}

bool DiskDataAllocator::isValidStateForTesting() const
{
    Locker locker { m_lock };
    
    // Verify internal consistency
    size_t totalFreeSize = 0;
    for (const auto& chunk : m_freeChunks) {
        totalFreeSize += chunk.second;
        
        // Verify no chunk has zero size
        if (chunk.second == 0)
            return false;
            
        // Verify offset is reasonable
        if (chunk.first < 0 || chunk.first >= m_fileTail)
            return false;
    }
    
    // Verify free chunks size tracking
    if (totalFreeSize != m_freeChunksSize)
        return false;
    
#if ASSERT_ENABLED
    // Verify no overlap between allocated and free chunks
    for (const auto& allocatedChunk : m_allocatedChunks) {
        int64_t allocStart = allocatedChunk.first;
        int64_t allocEnd = allocStart + static_cast<int64_t>(allocatedChunk.second);
        
        for (const auto& freeChunk : m_freeChunks) {
            int64_t freeStart = freeChunk.first;
            int64_t freeEnd = freeStart + static_cast<int64_t>(freeChunk.second);
            
            // Check for any overlap
            if (!(allocEnd <= freeStart || freeEnd <= allocStart))
                return false;
        }
    }
#endif
    
    return true;
}

} // namespace WebCore

// #endif // ENABLE(PARKABLE_STRINGS) 
