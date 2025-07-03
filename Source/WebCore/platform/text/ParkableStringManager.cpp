#include "config.h"
#include "ParkableStringManager.h"

#if ENABLE(PARKABLE_STRINGS)

#include "ParkableString.h"
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/text/StringImpl.h>
#include <WebCore/Document.h>
#include <WebCore/SecurityOrigin.h>

// Forward declarations to avoid WebKit dependency from WebCore
namespace WebKit {
class WebParkableStringStorageConnection;
}

// Function pointer for IPC storage operations (set by WebProcess)
extern "C" {
    bool (*g_webkitStoreParkableString)(const String& digest, const Vector<uint8_t>& data) = nullptr;
    std::optional<Vector<uint8_t>> (*g_webkitRetrieveParkableString)(const String& digest) = nullptr;
    void (*g_webkitDiscardParkableString)(const String& digest) = nullptr;
}

namespace WebCore {

// Forward declare DiskStorageMetadata here since it's defined in ParkableString.h
struct DiskStorageMetadata;

/**
 * Returns singleton instance using WebKit's NeverDestroyed pattern.
 * Thread-safe initialization with static local variable.
 * 
 * @return Reference to the singleton manager instance
 */
ParkableStringManager& ParkableStringManager::instance()
{
    static NeverDestroyed<ParkableStringManager> manager;
    return manager;
}

/**
 * Constructor that initializes statistics counters.
 * Disk storage is initialized lazily when first needed.
 */
ParkableStringManager::ParkableStringManager() 
    : m_totalParkingThreadTime(0_s)
    , m_totalUnparkingTime(0_s)
    , m_totalDiskWriteTime(0_s)
    , m_totalDiskReadTime(0_s)
{
    // Lazy initialization - disk storage will be initialized when first needed
}

/**
 * Default destructor.
 * Hash maps and statistics are automatically cleaned up.
 */
ParkableStringManager::~ParkableStringManager() = default;

// static
bool ParkableStringManager::shouldPark(const StringImpl& string)
{
    // Don't attempt to park strings smaller than this size.
    static constexpr unsigned kSizeThreshold = 10000;
    return string.length() > kSizeThreshold && isMainThread();
}

RefPtr<ParkableStringImpl> ParkableStringManager::add(RefPtr<StringImpl> string)
{
    ASSERT(isMainThread());
    
    if (!string)
        return nullptr;

    // Only add parkable strings to the manager
    ASSERT(shouldPark(*string));
    
    // Compute digest for deduplication
    auto digest = ParkableStringImpl::hashString(string.get());
    if (!digest)
        return nullptr;
    
    return add(WTFMove(string), WTFMove(digest));
}

RefPtr<ParkableStringImpl> ParkableStringManager::add(RefPtr<StringImpl> string, std::unique_ptr<ParkableStringImpl::SecureDigest> digest)
{
    ASSERT(isMainThread());
    ASSERT(string);
    ASSERT(digest);
    
    // Deduplication logic: check all three maps for existing string with same digest
    Locker locker { m_lock };
    
    // Check unparked strings first (most common case)
    auto it = m_unparkedStrings.find(digest.get());
    if (it != m_unparkedStrings.end()) {
        // Found existing unparked string with same content
        return it->value;
    }
    
    // Check parked strings
    it = m_parkedStrings.find(digest.get());
    if (it != m_parkedStrings.end()) {
        // Found existing parked string with same content
        return it->value;
    }
    
    // Check on-disk strings
    it = m_onDiskStrings.find(digest.get());
    if (it != m_onDiskStrings.end()) {
        // Found existing on-disk string with same content
        return it->value;
    }
    
    // No deduplication hit - create new parkable string
    auto parkableString = ParkableStringImpl::makeParkable(WTFMove(string), WTFMove(digest));
    
    // Insert into unparked strings map using digest as key
    auto insertResult = m_unparkedStrings.set(parkableString->digest(), parkableString.copyRef());
    ASSERT_UNUSED(insertResult, insertResult.isNewEntry);
    
    return WTFMove(parkableString);
}

void ParkableStringManager::remove(ParkableStringImpl* string)
{
    if (!string)
        return;
    
    // If we're not on the main thread, post to main thread
    if (!isMainThread()) {
        RunLoop::main().dispatch([this, protectedString = RefPtr<ParkableStringImpl>(string)]() {
            removeOnMainThread(protectedString.get());
        });
        return;
    }
    
    removeOnMainThread(string);
}

/**
 * Main thread implementation of string removal from hash maps.
 * Searches all three state maps and removes the string using digest-based lookup.
 * 
 * @param string The parkable string to remove
 */
void ParkableStringManager::removeOnMainThread(ParkableStringImpl* string)
{
    ASSERT(isMainThread());
    
    if (!string || !string->mayBeParked() || !string->digest())
        return;
    
    Locker locker { m_lock };
    
    // Use digest-based lookup to find and remove from the appropriate map
    const auto* digest = string->digest();
    
    // Check which map contains the string and remove it
    if (m_unparkedStrings.remove(digest))
        return;
    if (m_parkedStrings.remove(digest))
        return;
    m_onDiskStrings.remove(digest);
}

// ===== Updated Map Operations (Digest-Based) =====

/**
 * Moves string between different state maps using digest-based lookup.
 * Used for state transitions (unparked -> parked -> on-disk).
 * 
 * @param string The string to move
 * @param from Source map
 * @param to Destination map
 * @return true if string was found and moved
 */
bool ParkableStringManager::moveString(ParkableStringImpl* string, StringMap* from, StringMap* to)
{
    ASSERT(string);
    ASSERT(string->digest());
    
    const auto* digest = string->digest();
    
    auto it = from->find(digest);
    if (it == from->end())
        return false;
    
    RefPtr<ParkableStringImpl> stringRef = it->value;
    from->remove(it);
    
    auto insertResult = to->set(digest, WTFMove(stringRef));
    ASSERT_UNUSED(insertResult, insertResult.isNewEntry);
    
    return true;
}

// ===== Thread-Routing Methods =====

void ParkableStringManager::completeParked(ParkableStringImpl* string)
{
    if (isMainThread()) {
        onParked(string);
        return;
    }
    
    // Post to main thread with retained reference
    callOnMainThread([protectedString = RefPtr<ParkableStringImpl>(string)]() {
        ParkableStringManager::instance().onParked(protectedString.get());
    });
}

void ParkableStringManager::completeUnpark(ParkableStringImpl* string, Seconds elapsed, Seconds diskElapsed)
{
    if (isMainThread()) {
        recordUnparkingTime(elapsed);
        recordDecompression();
        if (diskElapsed > 0_s)
            recordDiskReadTime(diskElapsed);
        onUnparked(string);
        return;
    }
    
    // Post to main thread with retained reference
    callOnMainThread([protectedString = RefPtr<ParkableStringImpl>(string), elapsed, diskElapsed]() {
        auto& manager = ParkableStringManager::instance();
        manager.recordUnparkingTime(elapsed);
        manager.recordDecompression();
        if (diskElapsed > 0_s)
            manager.recordDiskReadTime(diskElapsed);
        manager.onUnparked(protectedString.get());
    });
}

void ParkableStringManager::completeWrittenToDisk(ParkableStringImpl* string)
{
    if (isMainThread()) {
        onWrittenToDisk(string);
        return;
    }
    
    // Post to main thread with retained reference
    callOnMainThread([protectedString = RefPtr<ParkableStringImpl>(string)]() {
        ParkableStringManager::instance().onWrittenToDisk(protectedString.get());
    });
}

// ===== Main Thread Callbacks (Updated for Digest-Based Maps) =====

void ParkableStringManager::onParked(ParkableStringImpl* string)
{
    ASSERT(isMainThread());
    ASSERT(string);
    
    Locker locker { m_lock };
    moveString(string, &m_unparkedStrings, &m_parkedStrings);
}

void ParkableStringManager::onUnparked(ParkableStringImpl* string)
{
    ASSERT(isMainThread());
    ASSERT(string);
    
    Locker locker { m_lock };
    
    // String could be coming from either parked or on-disk state
    if (!moveString(string, &m_parkedStrings, &m_unparkedStrings))
        moveString(string, &m_onDiskStrings, &m_unparkedStrings);
}

void ParkableStringManager::onWrittenToDisk(ParkableStringImpl* string)
{
    ASSERT(isMainThread());
    ASSERT(string);
    
    Locker locker { m_lock };
    moveString(string, &m_parkedStrings, &m_onDiskStrings);
}

// Statistics and testing methods (updated for digest-based maps)

/**
 * Returns total count of tracked strings across all three state maps.
 * Must be called from main thread.
 * 
 * @return Total number of parkable strings being managed
 */
size_t ParkableStringManager::size() const
{
    ASSERT(isMainThread());
    Locker locker { m_lock };
    return m_unparkedStrings.size() + m_parkedStrings.size() + m_onDiskStrings.size();
}

/**
 * Test helper checking if string exists in parked strings map.
 * Uses digest-based lookup for consistent behavior.
 * 
 * @param string The string to check
 * @return true if string is in the parked map
 */
bool ParkableStringManager::isOnParkedMapForTesting(ParkableStringImpl* string)
{
    if (!string || !string->digest())
        return false;
        
    Locker locker { m_lock };
    return m_parkedStrings.contains(string->digest());
}

/**
 * Test helper checking if string exists in on-disk strings map.
 * Uses digest-based lookup for consistent behavior.
 * 
 * @param string The string to check
 * @return true if string is in the on-disk map
 */
bool ParkableStringManager::isOnDiskMapForTesting(ParkableStringImpl* string)
{
    if (!string || !string->digest())
        return false;
        
    Locker locker { m_lock };
    return m_onDiskStrings.contains(string->digest());
}

void ParkableStringManager::purgeMemory()
{
    ASSERT(isMainThread());
    
    Locker locker { m_lock };
    
    // Park all unparked strings
    Vector<RefPtr<ParkableStringImpl>> stringsToProcess;
    for (auto& pair : m_unparkedStrings)
        stringsToProcess.append(pair.value);
    
    locker.unlockEarly();
    
    for (auto& string : stringsToProcess) {
        if (string)
            string->park();
    }
}

/**
 * Test helper to clear all manager state and reset statistics.
 * Clears all three hash maps and zeroes all timing counters.
 */
void ParkableStringManager::resetForTesting()
{
    ASSERT(isMainThread());
    Locker locker { m_lock };
    
    m_unparkedStrings.clear();
    m_parkedStrings.clear();
    m_onDiskStrings.clear();
    
    // Reset statistics
    m_totalParkingThreadTime = 0_s;
    m_totalUnparkingTime = 0_s;
    m_totalDiskWriteTime = 0_s;
    m_totalDiskReadTime = 0_s;
    m_totalCompressions = 0;
    m_totalDecompressions = 0;
}

// ===== Network Process Storage Integration =====

bool ParkableStringManager::storeCompressedStringViaIPC(const String& digest, const Vector<uint8_t>& compressedData)
{
#if ENABLE(PARKABLE_STRINGS) && PLATFORM(COCOA)
    // Use function pointer to avoid WebKit dependency
    if (g_webkitStoreParkableString)
        return g_webkitStoreParkableString(digest, compressedData);
    
    WTFLogAlways("ParkableStringManager: WebKit IPC not initialized");
    return false;
#else
    // Fallback for non-WebKit platforms - use direct disk storage
    WTFLogAlways("ParkableStringManager: Direct disk storage not implemented for this platform");
    return false;
#endif
}

std::optional<Vector<uint8_t>> ParkableStringManager::retrieveCompressedStringViaIPC(const String& digest)
{
#if ENABLE(PARKABLE_STRINGS) && PLATFORM(COCOA)
    // Use function pointer to avoid WebKit dependency
    if (g_webkitRetrieveParkableString)
        return g_webkitRetrieveParkableString(digest);
    
    WTFLogAlways("ParkableStringManager: WebKit IPC not initialized");
    return std::nullopt;
#else
    // Fallback for non-WebKit platforms
    WTFLogAlways("ParkableStringManager: Direct disk storage not implemented for this platform");
    return std::nullopt;
#endif
}

void ParkableStringManager::discardStringViaIPC(const String& digest)
{
#if ENABLE(PARKABLE_STRINGS) && PLATFORM(COCOA)
    // Use function pointer to avoid WebKit dependency
    if (g_webkitDiscardParkableString)
        g_webkitDiscardParkableString(digest);
    else
        WTFLogAlways("ParkableStringManager: WebKit IPC not initialized");
#else
    // Fallback for non-WebKit platforms
    WTFLogAlways("ParkableStringManager: Direct disk storage not implemented for this platform");
#endif
}

// ===== Memory Dump Provider Implementation =====

ParkableStringManager::MemoryStatistics ParkableStringManager::getMemoryStatistics() const
{
    ASSERT(isMainThread());
    Locker locker { m_lock };
    
    MemoryStatistics stats;
    stats.unparkedStrings = m_unparkedStrings.size();
    stats.parkedStrings = m_parkedStrings.size();
    stats.onDiskStrings = m_onDiskStrings.size();
    stats.totalStrings = stats.unparkedStrings + stats.parkedStrings + stats.onDiskStrings;
    
    size_t totalCompressedBytes = 0;
    size_t totalUncompressedBytes = 0;
    size_t totalDiskBytes = 0;
    size_t metadataBytes = 0;
    
    // Calculate stats for all string maps
    auto calculateForMap = [&](const StringMap& map) {
        for (const auto& pair : map) {
            ParkableStringImpl* string = pair.value.get();
            if (!string)
                continue;
                
            auto usage = string->memoryUsageForSnapshot();
            metadataBytes += usage.thisSize;
            
            // For uncompressed size, use the logical string size, not just StringImpl overhead
            totalUncompressedBytes += string->sizeInBytes();
            
            if (string->hasCompressedData())
                totalCompressedBytes += string->compressedSize();
            
            if (string->hasOnDiskData())
                totalDiskBytes += string->onDiskSize();
        }
    };
    
    calculateForMap(m_unparkedStrings);
    calculateForMap(m_parkedStrings);
    calculateForMap(m_onDiskStrings);
    
    stats.totalUncompressedSize = totalUncompressedBytes;
    stats.totalCompressedSize = totalCompressedBytes;
    stats.totalDiskSize = totalDiskBytes;
    stats.metadataOverhead = metadataBytes;
    
    // Calculate compression ratio and savings
    if (totalUncompressedBytes > 0) {
        if (totalCompressedBytes > 0) {
            stats.averageCompressionRatio = static_cast<double>(totalCompressedBytes) / totalUncompressedBytes;
            stats.compressionSavings = (totalUncompressedBytes > totalCompressedBytes) ? 
                                     (totalUncompressedBytes - totalCompressedBytes) : 0;
        } else {
            stats.averageCompressionRatio = 0.0;
            stats.compressionSavings = 0;
        }
    }
    
    // Performance metrics
    stats.totalCompressions = m_totalCompressions;
    stats.totalDecompressions = m_totalDecompressions;
    stats.totalCompressionTime = m_totalParkingThreadTime;
    stats.totalDecompressionTime = m_totalUnparkingTime;
    stats.totalDiskWriteTime = m_totalDiskWriteTime;
    stats.totalDiskReadTime = m_totalDiskReadTime;
    
    return stats;
}

size_t ParkableStringManager::memoryFootprint() const
{
    auto stats = getMemoryStatistics();
    return stats.totalUncompressedSize + stats.totalCompressedSize + stats.metadataOverhead;
}

#if !LOG_DISABLED
void ParkableStringManager::dumpStatistics() const
{
    auto stats = getMemoryStatistics();
    
    WTFLogAlways("=== ParkableString Manager Statistics ===");
    WTFLogAlways("Total strings: %zu", stats.totalStrings);
    WTFLogAlways("  - Unparked: %zu", stats.unparkedStrings);
    WTFLogAlways("  - Parked: %zu", stats.parkedStrings);
    WTFLogAlways("  - On disk: %zu", stats.onDiskStrings);
    
    WTFLogAlways("Memory usage:");
    WTFLogAlways("  - Uncompressed: %.2f MB", stats.totalUncompressedSize / (1024.0 * 1024.0));
    WTFLogAlways("  - Compressed: %.2f MB", stats.totalCompressedSize / (1024.0 * 1024.0));
    WTFLogAlways("  - Metadata: %.2f MB", stats.metadataOverhead / (1024.0 * 1024.0));
    WTFLogAlways("  - Disk: %.2f MB", stats.totalDiskSize / (1024.0 * 1024.0));
    
    if (stats.averageCompressionRatio > 0) {
        WTFLogAlways("Compression ratio: %.1f%% (saved %.2f MB)", 
                    stats.averageCompressionRatio * 100.0,
                    stats.compressionSavings / (1024.0 * 1024.0));
    }
    
    WTFLogAlways("Performance:");
    WTFLogAlways("  - Compressions: %zu (%.2fs total)", stats.totalCompressions, stats.totalCompressionTime.seconds());
    WTFLogAlways("  - Decompressions: %zu (%.2fs total)", stats.totalDecompressions, stats.totalDecompressionTime.seconds());
    WTFLogAlways("  - Disk writes: %.2fs total", stats.totalDiskWriteTime.seconds());
    WTFLogAlways("  - Disk reads: %.2fs total", stats.totalDiskReadTime.seconds());
}

void ParkableStringManager::dumpDetailedStringBreakdown() const
{
    ASSERT(isMainThread());
    Locker locker { m_lock };
    
    WTFLogAlways("=== Detailed String Breakdown ===");
    
    auto dumpMap = [](const StringMap& map, const char* mapName) {
        WTFLogAlways("%s (%u strings):", mapName, static_cast<unsigned>(map.size()));
        for (const auto& pair : map) {
            ParkableStringImpl* string = pair.value.get();
            if (!string)
                continue;
                
            auto usage = string->memoryUsageForSnapshot();
            WTFLogAlways("  String: %p, Size: %zu bytes, Compressed: %zu bytes, State: %d",
                        string, usage.stringImplSize, string->compressedSize(), 
                        static_cast<int>(string->currentState()));
        }
    };
    
    dumpMap(m_unparkedStrings, "Unparked Strings");
    dumpMap(m_parkedStrings, "Parked Strings");
    dumpMap(m_onDiskStrings, "On-Disk Strings");
}
#endif // !LOG_DISABLED

} // namespace WebCore

#endif // ENABLE(PARKABLE_STRINGS)
