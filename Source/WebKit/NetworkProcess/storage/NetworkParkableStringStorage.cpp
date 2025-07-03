#include "config.h"
#include "NetworkParkableStringStorage.h"

// Always compile parkable strings support
// #if ENABLE(PARKABLE_STRINGS)

#include <WebCore/DiskDataAllocator.h>
#include <WebCore/DiskDataMetadata.h>
#include <WebCore/SecurityOriginData.h>
#include <wtf/FileSystem.h>
#include <wtf/Locker.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/StringBuilder.h>

namespace WebKit {

/**
 * Constructor that asserts main thread usage.
 * Network Process storage must be initialized on the main thread.
 */
NetworkParkableStringStorage::NetworkParkableStringStorage()
{
    // Initialize on Network Process main thread
    ASSERT(isMainThread());
}

/**
 * Destructor that calls shutdown to clean up resources.
 * Ensures proper cleanup of origin storage maps and allocator references.
 */
NetworkParkableStringStorage::~NetworkParkableStringStorage()
{
    shutdown();
}

/**
 * Sets up disk allocator and temporary file for network storage.
 * Called lazily when first storage operation is requested.
 */
void NetworkParkableStringStorage::initialize()
{
    ASSERT(isMainThread());
    
    if (m_initialized)
        return;
        
    initializeDiskAllocator();
    m_initialized = true;
}

/**
 * Configures disk allocator with capacity limits and temp file.
 * Sets up the underlying storage infrastructure for parkable strings.
 */
void NetworkParkableStringStorage::initializeDiskAllocator()
{
    // Use the singleton DiskDataAllocator instance
    m_diskAllocator = &WebCore::DiskDataAllocator::instance();
    
    // Configure capacity limits (50MB default)
    m_diskAllocator->setCapacityLimit(50);
    
    // Create temporary file for disk storage
    auto [tempFilePath, tempFileHandle] = FileSystem::openTemporaryFile("NetworkParkableStrings"_s, ".data"_s);
    
    if (tempFileHandle.isValid()) {
        m_diskAllocator->provideTemporaryFile(WTFMove(tempFileHandle));
    } else {
        WTFLogAlways("NetworkParkableStringStorage: Failed to create temporary file for disk storage");
    }
}

/**
 * Cleans up origin storage maps and releases allocator reference.
 * Called during shutdown to ensure proper resource cleanup.
 */
void NetworkParkableStringStorage::shutdown()
{
    Locker locker { m_lock };
    m_originStorage.clear();
    m_diskAllocator = nullptr;
    m_initialized = false;
}

String NetworkParkableStringStorage::originKey(const WebCore::ClientOrigin& origin) const
{
    // Create a unique key for the origin
    StringBuilder builder;
    builder.append(origin.topOrigin.protocol());
    builder.append("://"_s);
    builder.append(origin.topOrigin.host());
    if (origin.topOrigin.port())
        builder.append(':', *origin.topOrigin.port());
    
    // Include client origin if different from top origin
    if (origin.clientOrigin != origin.topOrigin) {
        builder.append("_client_"_s);
        builder.append(origin.clientOrigin.protocol());
        builder.append("://"_s);
        builder.append(origin.clientOrigin.host());
        if (origin.clientOrigin.port())
            builder.append(':', *origin.clientOrigin.port());
    }
    
    return builder.toString();
}

/**
 * Gets existing or creates new origin-specific string storage map.
 * Each origin gets isolated storage to maintain security boundaries.
 * 
 * @param origin The client origin requesting storage
 * @return Reference to the origin's string storage map
 */
NetworkParkableStringStorage::OriginStringMap& NetworkParkableStringStorage::ensureOriginStorage(const WebCore::ClientOrigin& origin)
{
    String key = originKey(origin);
    auto iterator = m_originStorage.find(key);
    if (iterator != m_originStorage.end())
        return *iterator->value;
    
    auto newMap = makeUnique<OriginStringMap>();
    auto* mapPtr = newMap.get();
    m_originStorage.set(key, WTFMove(newMap));
    return *mapPtr;
}

bool NetworkParkableStringStorage::storeCompressedString(const WebCore::ClientOrigin& origin, const String& digest, const Vector<uint8_t>& compressedData)
{
    if (!m_initialized) {
        initialize();
        if (!m_initialized)
            return false;
    }
    
    if (!m_diskAllocator || !m_diskAllocator->mayWrite())
        return false;
    
    Locker locker { m_lock };
    
    auto& originStrings = ensureOriginStorage(origin);
    
    // Check if string already exists
    if (originStrings.contains(digest))
        return true; // Already stored
    
    locker.unlockEarly();
    
    // Reserve chunk for compressed data
    auto reservedChunk = m_diskAllocator->tryReserveChunk(compressedData.size());
    if (!reservedChunk)
        return false;
    
    // Write compressed data to disk
    auto diskMetadata = m_diskAllocator->write(WTFMove(reservedChunk), compressedData);
    if (!diskMetadata)
        return false;
    
    // Store metadata
    Locker newLocker { m_lock };
    auto& newOriginStrings = ensureOriginStorage(origin);
    auto storedMetadata = makeUnique<StoredStringMetadata>(String { digest }, WTFMove(diskMetadata), compressedData.size());
    newOriginStrings.set(digest, WTFMove(storedMetadata));
    
    return true;
}

std::optional<Vector<uint8_t>> NetworkParkableStringStorage::retrieveCompressedString(const WebCore::ClientOrigin& origin, const String& digest)
{
    if (!m_initialized || !m_diskAllocator)
        return std::nullopt;
    
    Locker locker { m_lock };
    
    String key = originKey(origin);
    auto originIterator = m_originStorage.find(key);
    if (originIterator == m_originStorage.end())
        return std::nullopt;
    
    auto& originStrings = *originIterator->value;
    auto stringIterator = originStrings.find(digest);
    if (stringIterator == originStrings.end())
        return std::nullopt;
    
    auto& metadata = *stringIterator->value;
    auto* diskMetadata = metadata.diskMetadata.get();
    size_t compressedSize = metadata.compressedSize;
    
    locker.unlockEarly();
    
    // Read compressed data from disk
    Vector<uint8_t> compressedData;
    compressedData.resize(compressedSize);
    
    m_diskAllocator->read(*diskMetadata, compressedData);
    
    return compressedData;
}

void NetworkParkableStringStorage::discardString(const WebCore::ClientOrigin& origin, const String& digest)
{
    if (!m_initialized || !m_diskAllocator)
        return;
    
    Locker locker { m_lock };
    
    String key = originKey(origin);
    auto originIterator = m_originStorage.find(key);
    if (originIterator == m_originStorage.end())
        return;
    
    auto& originStrings = *originIterator->value;
    auto stringIterator = originStrings.find(digest);
    if (stringIterator == originStrings.end())
        return;
    
    auto metadata = WTFMove(stringIterator->value);
    originStrings.remove(stringIterator);
    
    locker.unlockEarly();
    
    // Discard disk space
    m_diskAllocator->discard(WTFMove(metadata->diskMetadata));
}

void NetworkParkableStringStorage::clearOrigin(const WebCore::ClientOrigin& origin)
{
    if (!m_initialized || !m_diskAllocator)
        return;
    
    Locker locker { m_lock };
    
    String key = originKey(origin);
    auto originIterator = m_originStorage.find(key);
    if (originIterator == m_originStorage.end())
        return;
    
    auto originStorage = WTFMove(originIterator->value);
    m_originStorage.remove(originIterator);
    
    locker.unlockEarly();
    
    // Discard all disk data for this origin
    for (auto& pair : *originStorage) {
        m_diskAllocator->discard(WTFMove(pair.value->diskMetadata));
    }
}

/**
 * Returns total disk usage across all origin storages.
 * Delegates to the underlying disk allocator for accurate measurement.
 * 
 * @return Total disk usage in bytes
 */
size_t NetworkParkableStringStorage::diskFootprint() const
{
    if (!m_diskAllocator)
        return 0;
    return m_diskAllocator->diskFootprint();
}

/**
 * Returns memory usage of storage metadata structures.
 * Calculates total size of origin keys, digest keys, and metadata objects.
 * 
 * @return Total memory footprint in bytes
 */
size_t NetworkParkableStringStorage::memoryFootprint() const
{
    Locker locker { m_lock };
    
    size_t total = 0;
    for (const auto& originPair : m_originStorage) {
        total += originPair.key.sizeInBytes();
        for (const auto& stringPair : *originPair.value) {
            total += stringPair.key.sizeInBytes();
            total += sizeof(StoredStringMetadata);
        }
    }
    return total;
}

/**
 * Test helper performing complete shutdown and cleanup.
 * Clears all storage and resets initialization state.
 */
void NetworkParkableStringStorage::clearAllForTesting()
{
    shutdown();
}

/**
 * Test helper checking if string exists for specific origin.
 * Used to verify storage operations in tests.
 * 
 * @param origin The origin to check
 * @param digest The string digest to look for
 * @return true if string exists in storage
 */
bool NetworkParkableStringStorage::hasStringForTesting(const WebCore::ClientOrigin& origin, const String& digest) const
{
    Locker locker { m_lock };
    
    String key = originKey(origin);
    auto originIterator = m_originStorage.find(key);
    if (originIterator == m_originStorage.end())
        return false;
    
    return originIterator->value->contains(digest);
}

} // namespace WebKit

// #endif // ENABLE(PARKABLE_STRINGS)
