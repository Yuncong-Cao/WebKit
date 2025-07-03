#pragma once

#include "config.h"

#if ENABLE(PARKABLE_STRINGS)

#include <wtf/Forward.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/RefPtr.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#include <wtf/text/StringImpl.h>
#include <wtf/text/WTFString.h>
#include <wtf/ThreadingPrimitives.h>
#include <wtf/MonotonicTime.h>
#include <wtf/FileSystem.h>
#include <JavaScriptCore/ArrayBuffer.h>

namespace JSC {
class ArrayBuffer;
}

namespace WebCore {

class ParkableStringManager;

// Forward declarations
struct BackgroundTaskParams;

// Forward declaration of DiskDataMetadata from our centralized allocator
class DiskDataMetadata;

/**
 * ParkableStringImpl - The core implementation of a string that can be "parked"
 * (compressed and moved to reduce memory usage).
 * 
 * This class provides the main functionality:
 * - Background compression of large strings
 * - Synchronous parking/unparking
 * - Thread-safe access to the string data
 * - Lock mechanism to prevent parking during active use
 * - Hash-based deduplication for memory efficiency
 * 
 * State transitions are managed by ParkableStringManager to ensure
 * proper main thread synchronization and string deduplication.
 */
class WEBCORE_EXPORT ParkableStringImpl final : public WTF::ThreadSafeRefCounted<ParkableStringImpl> {
    friend class ParkableStringManager;
public:
    enum class ParkingMode {
        SynchronousOnly,
        CompressOnly,
        WriteToDisk,
        CompressThenWriteToDisk,
        CompressAndSwap
    };
    
    enum class State {
        Unparked,     // String is in memory, uncompressed
        Parked,       // String is compressed in memory
        OnDisk,       // String is written to disk (compressed data may be discarded)
        DiskCorrupted // String was on disk but couldn't be read back
    };
    
    enum class Age {
        Young = 0,
        Old = 1,
        VeryOld = 2
    };
    
    enum class Status {
        UnreferencedExternally,
        TooManyReferences,
        Locked
    };

    // Hash-based deduplication support
    static constexpr size_t kDigestSize = 32; // SHA256
    using SecureDigest = WTF::Vector<uint8_t, kDigestSize>;
    
    // Computes a secure hash of a string for deduplication
    static std::unique_ptr<SecureDigest> hashString(WTF::StringImpl* string);
    
    // Updates a digest to include string encoding information
    static void updateDigestWithEncoding(const WTF::Vector<uint8_t>& bytes, bool is8Bit, SecureDigest& digest);

    static WTF::Ref<ParkableStringImpl> create(WTF::RefPtr<WTF::StringImpl>);
    
    // Factory methods
    static WTF::Ref<ParkableStringImpl> makeNonParkable(WTF::RefPtr<WTF::StringImpl>);
    static WTF::Ref<ParkableStringImpl> makeParkable(WTF::RefPtr<WTF::StringImpl>, std::unique_ptr<SecureDigest> digest);

    ParkableStringImpl(const ParkableStringImpl&) = delete;
    ParkableStringImpl& operator=(const ParkableStringImpl&) = delete;
    
    ~ParkableStringImpl();

    // Basic string properties (always available, no metadata required)
    bool isNull() const;
    size_t length() const {
        if (!mayBeParked())
            return m_string ? m_string->length() : 0;
        return m_metadata->length;
    }
    bool is8Bit() const {
        if (!mayBeParked())
            return m_string ? m_string->is8Bit() : true;
        return m_metadata->is8Bit;
    }
    size_t sizeInBytes() const;
    
    // Memory profiling support
    struct MemoryUsage {
        size_t thisSize;              // Size of ParkableStringImpl + metadata + compressed data
        const void* stringImpl;      // Pointer to underlying StringImpl (for tracking) 
        size_t stringImplSize;       // Size of StringImpl + character data
    };
    MemoryUsage memoryUsageForSnapshot() const;
    size_t memoryFootprintForDump() const;
    
    // Parking eligibility check - metadata != nullptr means parkable
    bool mayBeParked() const { return !!m_metadata; }
    
    // Parking-related queries (require metadata)
    bool hasCompressedData() const;
    bool hasCompressedDataNoLock() const WTF_REQUIRES_LOCK(m_metadata->lock);
    bool isParked() const;     // Returns true if currently parked (compressed)
    bool isCompressionFailed() const; // Returns true if compression has failed
    size_t compressedSize() const; // Returns size of compressed data in bytes
    
    // Hash-based deduplication support
    const SecureDigest* digest() const { 
        return m_metadata ? m_metadata->digest.get() : nullptr; 
    }
    
    // Get digest as hex string for IPC storage key
    String digestString() const;
    
    // String access
    WTF::String toString();
    WTF::RefPtr<WTF::StringImpl> impl();
    
    // Locking mechanism to prevent parking (requires metadata)
    void lock();   // Increment lock count, prevents parking
    void unlock(); // Decrement lock count
    void lockWithoutMakingYoung(); // Used in background tasks
    
    // Parking operations (require metadata)
    bool park(ParkingMode mode = ParkingMode::CompressOnly);
    bool maybeParkString();

    // Disk storage info (require metadata)
    bool isOnDisk() const;
    State currentState() const;
    Age currentAge() const;
    
    // Disk storage operations (require metadata)
    bool hasOnDiskData() const;
    size_t onDiskSize() const;
    const String& diskPath() const;

    // Core parking operations (require metadata)
    bool parkInternal(ParkingMode mode) WTF_REQUIRES_LOCK(m_metadata->lock);
    void unpark() WTF_REQUIRES_LOCK(m_metadata->lock);
    WTF::String unparkInternal() WTF_REQUIRES_LOCK(m_metadata->lock);
    WTF::String unparkFromCompressed() WTF_REQUIRES_LOCK(m_metadata->lock);
    WTF::String unparkFromDisk() WTF_REQUIRES_LOCK(m_metadata->lock);

    // State management (require metadata)
    bool canParkNow() const WTF_REQUIRES_LOCK(m_metadata->lock);
    void discardUncompressedData() WTF_REQUIRES_LOCK(m_metadata->lock);
    void discardCompressedData() WTF_REQUIRES_LOCK(m_metadata->lock);
    void makeYoung() WTF_REQUIRES_LOCK(m_metadata->lock);
    void ageString() WTF_REQUIRES_LOCK(m_metadata->lock);
    Status currentStatus() const WTF_REQUIRES_LOCK(m_metadata->lock);
    
    // Helper methods for memory usage calculation
    bool isParkedNoLock() const WTF_REQUIRES_LOCK(m_metadata->lock);
    bool isOnDiskNoLock() const WTF_REQUIRES_LOCK(m_metadata->lock);

private:
    explicit ParkableStringImpl(WTF::RefPtr<WTF::StringImpl>, bool parkable = true);
    explicit ParkableStringImpl(WTF::RefPtr<WTF::StringImpl>, std::unique_ptr<SecureDigest> digest);
    
    // Conditional metadata allocation for parkable strings only
    struct ParkableMetadata {
        WTF_MAKE_FAST_ALLOCATED;
    public:
        ParkableMetadata(WTF::String string, std::unique_ptr<SecureDigest> digest);
        ParkableMetadata(const ParkableMetadata&) = delete;
        ParkableMetadata& operator=(const ParkableMetadata&) = delete;
        
        // Thread synchronization
        mutable WTF::Lock lock;
        
        // Lock count to prevent parking while string is in use
        unsigned lockCount WTF_GUARDED_BY_LOCK(lock) { 0 };
        
        // Primary parking state (for main state tracking)
        State state WTF_GUARDED_BY_LOCK(lock) { State::Unparked };
        Age age WTF_GUARDED_BY_LOCK(lock) { Age::Young };
        
        // Separate boolean flags for data availability
        // These can be independently true, allowing intermediate states
        bool hasCompressedData WTF_GUARDED_BY_LOCK(lock) { false };
        bool hasOnDiskData WTF_GUARDED_BY_LOCK(lock) { false };
        
        // Background task tracking
        bool backgroundTaskInProgress WTF_GUARDED_BY_LOCK(lock) { false };
        bool compressionFailed WTF_GUARDED_BY_LOCK(lock) { false };
        
        // Data storage - can coexist for performance
        std::unique_ptr<WTF::Vector<uint8_t>> compressedData WTF_GUARDED_BY_LOCK(lock);
        std::unique_ptr<DiskDataMetadata> diskMetadata WTF_GUARDED_BY_LOCK(lock);
        
        // Deduplication digest (immutable after construction)
        const std::unique_ptr<SecureDigest> digest;
        
        // Cached string properties from construction (immutable)
        const bool is8Bit;
        const unsigned length;
    };
    
    // Compression/decompression helpers
    static std::unique_ptr<WTF::Vector<uint8_t>> compressData(const WTF::Vector<uint8_t>& data);
    std::unique_ptr<WTF::Vector<uint8_t>> compressDataSynchronously() WTF_REQUIRES_LOCK(m_metadata->lock);
    std::optional<WTF::String> decompress() WTF_REQUIRES_LOCK(m_metadata->lock);
    
    // Background task scheduling (require metadata)
    void scheduleCompressionTask(ParkingMode mode) WTF_REQUIRES_LOCK(m_metadata->lock);
    void scheduleDiskWriteTask() WTF_REQUIRES_LOCK(m_metadata->lock);
    void scheduleDiskWriteFromCompression();
    void scheduleDiskReadTask() WTF_REQUIRES_LOCK(m_metadata->lock);
    
    // Static background methods
    static void compressInBackground(std::unique_ptr<BackgroundTaskParams> params);
    static void writeToDiskInBackground(RefPtr<ParkableStringImpl>, const WTF::Vector<uint8_t>& compressedData);
    static void readFromDiskInBackground(RefPtr<ParkableStringImpl>, const DiskDataMetadata&);

    // Background operations callback
    void onCompressionCompleteOnMainThread(std::unique_ptr<BackgroundTaskParams> params, std::unique_ptr<WTF::Vector<uint8_t>> compressedData);
    void onDiskWriteComplete(std::unique_ptr<DiskDataMetadata> metadata);
    void onDiskReadComplete(std::unique_ptr<WTF::Vector<uint8_t>> compressedData);
    
    // Disk operations now handled by centralized DiskDataAllocator
    
    // Compression operations  
    static std::optional<WTF::String> decompressData(const WTF::Vector<uint8_t>& compressedData, bool is8Bit, unsigned length);
    
    // Always-present lightweight fields
    // The actual string data (null when parked - for parkable strings this access requires metadata->lock)
    WTF::RefPtr<WTF::StringImpl> m_string;
    
    // Conditionally allocated metadata for parkable strings only
    // nullptr = non-parkable string, non-null = parkable string
    const std::unique_ptr<ParkableMetadata> m_metadata;
    
    // Parking eligibility constants
    static constexpr size_t kMinimumSizeForParking = 10 * 1024; // 10KB
    static constexpr size_t kMinimumSizeForDisk = 50 * 1024;    // 50KB for disk storage
    
#if ASSERT_ENABLED
    void assertOnValidThread() const { ASSERT(isMainThread()); }
#else
    void assertOnValidThread() const { }
#endif
};

/**
 * ParkableString - A string wrapper that provides memory-efficient storage
 * for large strings through compression.
 * 
 * This is the main public interface that users should interact with.
 * It behaves like a normal string but can automatically compress itself
 * to save memory when not actively being used.
 * 
 * Example usage:
 *   ParkableString str(someImpl);
 *   str.lock();         // Prevent parking during active use
 *   auto text = str.toString();  // Access the string
 *   str.unlock();       // Allow parking again
 *   str.park();         // Manually trigger parking
 */
class WEBCORE_EXPORT ParkableString final {
public:
    ParkableString();
    explicit ParkableString(WTF::RefPtr<WTF::StringImpl>);
    
    // Copy/move constructors and assignment operators
    ParkableString(const ParkableString&);
    ParkableString& operator=(const ParkableString&);
    ParkableString(ParkableString&&);
    ParkableString& operator=(ParkableString&&);
    
    ~ParkableString();

    // Basic properties
    bool isNull() const;
    size_t length() const;
    bool is8Bit() const;
    size_t sizeInBytes() const;
    
    // Parking-related
    bool mayBeParked() const;  // True if string is large enough to benefit from parking
    bool isParked() const;     // True if currently compressed
    size_t compressedSize() const; // Size of compressed data in bytes (0 if not compressed)
    
    // String access
    WTF::String toString() const;
    WTF::RefPtr<WTF::StringImpl> impl() const;
    
    // Access to ParkableStringImpl for testing and deduplication
    ParkableStringImpl* Impl() const;
    
    // Memory profiling support (delegates to implementation)
    size_t memoryFootprintForDump() const;
    
    // Locking to prevent parking during active use
    void lock();     // Increment lock, prevents parking
    void unlock();   // Decrement lock
    
    // Parking control
    bool park(ParkableStringImpl::ParkingMode mode = ParkableStringImpl::ParkingMode::CompressOnly);

    // Disk storage info
    bool isOnDisk() const;
    size_t onDiskSize() const;

private:
    WTF::RefPtr<ParkableStringImpl> m_impl;
};

} // namespace WebCore

#endif // ENABLE(PARKABLE_STRINGS)
