#pragma once

#include "config.h"

#if ENABLE(PARKABLE_STRINGS)

#include <wtf/Forward.h>
#include <wtf/HashMap.h>
#include <wtf/HashTraits.h>
#include <wtf/Lock.h>
#include <wtf/RefPtr.h>
#include <wtf/RunLoop.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#include <wtf/text/StringImpl.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

class ParkableStringImpl;

/**
 * ParkableStringManager - Centralized manager for all parkable strings.
 * 
 * This singleton manages the lifecycle and state transitions of all
 * ParkableStringImpl instances. All background task completions are
 * routed through this manager to ensure proper main thread synchronization.
 * 
 * Features hash-based string deduplication to ensure identical
 * strings (by content and encoding) are only stored once in memory, providing
 * significant memory savings in real-world applications.
 * 
 * This design is inspired by Chromium's approach and solves the timeout
 * issue by ensuring all async callbacks are processed through the main
 * thread's RunLoop.
 */
class WEBCORE_EXPORT ParkableStringManager {
public:
    // Hash traits for secure digest-based deduplication
    // Compares digest contents, not pointers, and uses first 4 bytes as hash
    struct SecureDigestHashTraits : WTF::GenericHashTraits<const ParkableStringImpl::SecureDigest*> {
        static unsigned hash(const ParkableStringImpl::SecureDigest* digest) {
            // Use first 4 bytes of SHA256 digest as hash key
            if (!digest || digest->size() < 4)
                return 0;
            
            // Safe approach: extract bytes individually to build hash
            unsigned result = 0;
            auto& vector = *digest;
            result |= static_cast<unsigned>(vector[0]);
            result |= static_cast<unsigned>(vector[1]) << 8;
            result |= static_cast<unsigned>(vector[2]) << 16;
            result |= static_cast<unsigned>(vector[3]) << 24;
            return result;
        }
        
        static bool equal(const ParkableStringImpl::SecureDigest* const a, 
                         const ParkableStringImpl::SecureDigest* const b) {
            // Support both pointer equality and content equality
            return a == b || (a && b && *a == *b);
        }
        
        static constexpr bool safeToCompareToEmptyOrDeleted = false;
    };
    
    // Hash-based string map for deduplication
    // Maps secure digest to string implementation for memory-efficient deduplication
    using StringMap = WTF::HashMap<const ParkableStringImpl::SecureDigest*, 
                                   WTF::RefPtr<ParkableStringImpl>, 
                                   SecureDigestHashTraits>;

    static ParkableStringManager& instance();
    
    ~ParkableStringManager();
    
    // String lifecycle management with deduplication
    WTF::RefPtr<ParkableStringImpl> add(WTF::RefPtr<WTF::StringImpl>);
    WTF::RefPtr<ParkableStringImpl> add(WTF::RefPtr<WTF::StringImpl>, std::unique_ptr<ParkableStringImpl::SecureDigest> digest);
    void remove(ParkableStringImpl*);
    
    // Determine if a string should be parkable
    static bool shouldPark(const WTF::StringImpl& string);
    
    // State transition callbacks (called from ParkableStringImpl)
    // These methods handle thread routing automatically
    void completeParked(ParkableStringImpl*);
    void completeUnpark(ParkableStringImpl*, Seconds elapsed = 0_s, Seconds diskElapsed = 0_s);
    void completeWrittenToDisk(ParkableStringImpl*);
    
    // Main thread callbacks (private)
    void onParked(ParkableStringImpl*);
    void onUnparked(ParkableStringImpl*);
    void onWrittenToDisk(ParkableStringImpl*);
    
    // Network Process storage integration (WebKit IPC pattern)
    // Stores compressed string data via IPC to NetworkStorageManager
    bool storeCompressedStringViaIPC(const String& digest, const Vector<uint8_t>& compressedData);
    std::optional<Vector<uint8_t>> retrieveCompressedStringViaIPC(const String& digest);
    void discardStringViaIPC(const String& digest);
    
    // Storage configuration - Note: Actual limits are enforced in Network Process
    void configureDiskCapacity(size_t) { /* TODO: Send to Network Process if needed */ }
    void disableDiskCapacityLimit() { /* TODO: Send to Network Process if needed */ }
    
    // Statistics and testing
    void recordParkingThreadTime(Seconds time) { m_totalParkingThreadTime += time; }
    void recordUnparkingTime(Seconds time) { m_totalUnparkingTime += time; }
    void recordDiskWriteTime(Seconds time) { m_totalDiskWriteTime += time; }
    void recordDiskReadTime(Seconds time) { m_totalDiskReadTime += time; }
    void recordCompression() { ++m_totalCompressions; }
    void recordDecompression() { ++m_totalDecompressions; }
    
    size_t size() const;
    bool isOnParkedMapForTesting(ParkableStringImpl* string);
    bool isOnDiskMapForTesting(ParkableStringImpl* string);
    void resetForTesting();
    
    // Memory management
    void purgeMemory();
    
    // Memory reporting and dump provider functionality
    struct MemoryStatistics {
        size_t totalStrings { 0 };
        size_t unparkedStrings { 0 };
        size_t parkedStrings { 0 };
        size_t onDiskStrings { 0 };
        size_t totalUncompressedSize { 0 };
        size_t totalCompressedSize { 0 };
        size_t totalDiskSize { 0 };
        size_t metadataOverhead { 0 };
        double averageCompressionRatio { 0.0 };
        size_t compressionSavings { 0 };
        
        // Performance metrics
        size_t totalCompressions { 0 };
        size_t totalDecompressions { 0 };
        Seconds totalCompressionTime { 0_s };
        Seconds totalDecompressionTime { 0_s };
        Seconds totalDiskWriteTime { 0_s };
        Seconds totalDiskReadTime { 0_s };
    };
    
    MemoryStatistics getMemoryStatistics() const;
    size_t memoryFootprint() const;
    
#if !LOG_DISABLED
    void dumpStatistics() const;
    void dumpDetailedStringBreakdown() const;
#endif
    
private:
    ParkableStringManager();
    
    // Friend class to allow NeverDestroyed access to constructor
    friend class WTF::NeverDestroyed<ParkableStringManager>;
    
    // Move string between different state maps using digest-based lookup
    bool moveString(ParkableStringImpl*, StringMap* from, StringMap* to);
    
    void removeOnMainThread(ParkableStringImpl*);
    
    mutable WTF::Lock m_lock;
    
    // Hash-based maps tracking strings in different states
    // These provide automatic deduplication by content hash
    StringMap m_unparkedStrings;
    StringMap m_parkedStrings;
    StringMap m_onDiskStrings;
    
    // Statistics tracking (extended for memory dump provider)
    Seconds m_totalParkingThreadTime;
    Seconds m_totalUnparkingTime;
    Seconds m_totalDiskWriteTime;
    Seconds m_totalDiskReadTime;
    
    // Performance counters
    mutable size_t m_totalCompressions { 0 };
    mutable size_t m_totalDecompressions { 0 };
    
    static ParkableStringManager* s_instance;
};

} // namespace WebCore

#endif // ENABLE(PARKABLE_STRINGS)
