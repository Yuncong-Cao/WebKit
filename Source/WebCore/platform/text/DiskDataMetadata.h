#pragma once

#include "config.h"

// Always compile disk data support
// #if ENABLE(PARKABLE_STRINGS)

#include <wtf/Forward.h>

namespace WebCore {

class DiskDataAllocator;

/**
 * DiskDataMetadata - Represents a chunk of allocated disk space
 * 
 * Simple POD class that tracks the location and size of data stored
 * in the shared disk file managed by DiskDataAllocator.
 */
class WEBCORE_EXPORT DiskDataMetadata {
    WTF_MAKE_FAST_ALLOCATED;
public:
    DiskDataMetadata(int64_t startOffset, size_t size)
        : m_startOffset(startOffset), m_size(size) { }
    DiskDataMetadata(const DiskDataMetadata&) = default;
    DiskDataMetadata(DiskDataMetadata&&) = default;
    DiskDataMetadata& operator=(const DiskDataMetadata&) = default;

    int64_t startOffset() const { return m_startOffset; }
    size_t size() const { return m_size; }

    // Static factory method for DiskDataAllocator
    static std::unique_ptr<DiskDataMetadata> create(int64_t startOffset, size_t size)
    {
        return makeUnique<DiskDataMetadata>(startOffset, size);
    }

private:
    int64_t m_startOffset;
    mutable size_t m_size;  // Allow direct modification for allocation algorithms

    friend class DiskDataAllocator;
};

/**
 * ReservedChunk - RAII wrapper for disk chunk reservations
 * 
 * Holds a DiskDataMetadata representing a reserved chunk until it's
 * either written to (via take()) or automatically discarded on destruction.
 * This prevents memory leaks from unrealized reservations.
 */
class WEBCORE_EXPORT ReservedChunk {
    WTF_MAKE_FAST_ALLOCATED;
    WTF_MAKE_NONCOPYABLE(ReservedChunk);
public:
    ReservedChunk(DiskDataAllocator* allocator, std::unique_ptr<DiskDataMetadata>);
    ~ReservedChunk();

    // Takes ownership of the reserved chunk. After this call,
    // the chunk will NOT be auto-discarded on destruction.
    std::unique_ptr<DiskDataMetadata> take();

private:
    DiskDataAllocator* m_allocator;
    std::unique_ptr<DiskDataMetadata> m_metadata;
};

} // namespace WebCore

// #endif // ENABLE(PARKABLE_STRINGS) 
