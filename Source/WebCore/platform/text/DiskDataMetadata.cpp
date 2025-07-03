#include "config.h"
#include "DiskDataMetadata.h"

#if ENABLE(PARKABLE_STRINGS)

#include "DiskDataAllocator.h"

namespace WebCore {

/**
 * Constructor initializing chunk reservation with metadata and allocator.
 * Provides RAII semantics for disk chunk allocation.
 * 
 * @param allocator The allocator that owns this chunk
 * @param metadata Unique metadata describing the allocated chunk
 */
ReservedChunk::ReservedChunk(DiskDataAllocator* allocator, std::unique_ptr<DiskDataMetadata> metadata)
    : m_allocator(allocator)
    , m_metadata(WTFMove(metadata))
{
    ASSERT(m_allocator);
    ASSERT(m_metadata);
}

/**
 * Destructor that auto-discards unrealized reservations to prevent leaks.
 * If take() was not called, the chunk is automatically returned to the free pool.
 */
ReservedChunk::~ReservedChunk()
{
    if (m_metadata) {
        // Auto-discard unrealized reservation
        m_allocator->discard(WTFMove(m_metadata));
    }
}

/**
 * Extracts metadata and prevents auto-discard for successful allocations.
 * After calling take(), the caller owns the metadata and must manage cleanup.
 * 
 * @return Unique pointer to the chunk metadata
 */
std::unique_ptr<DiskDataMetadata> ReservedChunk::take()
{
    return WTFMove(m_metadata);
}

} // namespace WebCore

#endif // ENABLE(PARKABLE_STRINGS) 
