#include "config.h"
#include "ParkableString.h"

#if ENABLE(PARKABLE_STRINGS)

#include "ParkableStringManager.h"
#include "DiskDataAllocator.h"
#include "DiskDataMetadata.h"
#include <wtf/WorkQueue.h>
#include <wtf/RunLoop.h>
#include <wtf/ThreadingPrimitives.h>
#include <wtf/Assertions.h>
#include <wtf/text/StringImpl.h>
#include <wtf/text/WTFString.h>
#include <wtf/Vector.h>
#include <JavaScriptCore/ArrayBuffer.h>
#include <WebCore/SharedBuffer.h>
#include <wtf/FileSystem.h>
#include <wtf/UUID.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/MakeString.h>
#include <wtf/HexNumber.h>
#include <wtf/MonotonicTime.h>
#include <wtf/CryptographicallyRandomNumber.h>

// Direct zlib for simple stateless compression/decompression
#include <zlib.h>

// WebKit's crypto support for SHA256
#if PLATFORM(COCOA)
#include <CommonCrypto/CommonDigest.h>
#define WTF_SHA256_CTX CC_SHA256_CTX
#define WTF_SHA256_DIGEST_LENGTH CC_SHA256_DIGEST_LENGTH
#define WTF_SHA256_Init CC_SHA256_Init
#define WTF_SHA256_Update CC_SHA256_Update
#define WTF_SHA256_Final CC_SHA256_Final
#else
// Fallback to a simple hash for non-Cocoa platforms
#include <wtf/HashFunctions.h>
#endif

namespace WebCore {

// ===== Utilities =====

enum class ParkingAction { Parked, Unparked, Written, Read };

/**
 * Records timing statistics for parking operations.
 * Currently unused but reserved for future histogram support.
 * 
 * @param size Size of the operation in bytes
 * @param duration Time taken for the operation
 * @param action Type of parking action performed
 */
static void recordStatistics(size_t size, Seconds duration, ParkingAction action)
{
    // TODO: Record statistics when WebKit has histogram support
    // For now, just log timing information for debugging
    UNUSED_PARAM(size);
    UNUSED_PARAM(duration);
    UNUSED_PARAM(action);
}

// Helper class to measure elapsed time using WebKit's MonotonicTime
class ElapsedTimer {
public:
    ElapsedTimer() : m_startTime(MonotonicTime::now()) { }
    
    Seconds elapsed() const 
    { 
        return MonotonicTime::now() - m_startTime;
    }
    
private:
    MonotonicTime m_startTime;
};

// ===== ASAN Support =====

#if defined(ADDRESS_SANITIZER)
extern "C" void __asan_poison_memory_region(void const volatile* addr, size_t size);
extern "C" void __asan_unpoison_memory_region(void const volatile* addr, size_t size);
#endif

/**
 * Marks string memory as poisoned for AddressSanitizer to detect use-after-free bugs.
 * Skips atomic strings since they remain in the AtomicStringTable for equality comparisons.
 * 
 * @param stringImpl The string whose memory should be poisoned
 */
static void asanPoisonString(const RefPtr<StringImpl>& stringImpl)
{
#if defined(ADDRESS_SANITIZER)
    if (!stringImpl)
        return;
    // Since string is not deallocated, it remains in the AtomicStringTable,
    // where its content can be accessed for equality comparison for instance,
    // triggering a poisoned memory access.
    if (stringImpl->isAtom())
        return;
    __asan_poison_memory_region(stringImpl->characters8(), stringImpl->sizeInBytes());
#else
    UNUSED_PARAM(stringImpl);
#endif // defined(ADDRESS_SANITIZER)
}

/**
 * Removes AddressSanitizer poisoning from string memory before access.
 * Must be called before accessing previously poisoned string content.
 * 
 * @param stringImpl The string whose memory should be unpoisoned
 */
static void asanUnpoisonString(const RefPtr<StringImpl>& stringImpl)
{
#if defined(ADDRESS_SANITIZER)
    if (!stringImpl)
        return;
    __asan_unpoison_memory_region(stringImpl->characters8(), stringImpl->sizeInBytes());
#else
    UNUSED_PARAM(stringImpl);
#endif // defined(ADDRESS_SANITIZER)
}

// ===== Background Task Parameters =====

// Created and destroyed on the same thread, accessed on a background thread as
// well. Object lifetime is managed by the RefPtr to keep the string alive
// during the entire background operation.
struct BackgroundTaskParams final {
    WTF_MAKE_FAST_ALLOCATED;
    
public:
    BackgroundTaskParams(
        RefPtr<ParkableStringImpl> string,
        Vector<uint8_t> data,
        ParkableStringImpl::ParkingMode parkingMode)
        : string(WTFMove(string))
        , data(WTFMove(data))
        , parkingMode(parkingMode)
        , elapsed(0_s)
    {
    }
    
    ~BackgroundTaskParams() { 
        ASSERT(isMainThread()); 
    }
    
    const RefPtr<ParkableStringImpl> string;  // Keeps object alive!
    Vector<uint8_t> data;
    ParkableStringImpl::ParkingMode parkingMode;
    Seconds elapsed;
    
    // Disk-specific fields for DiskDataAllocator integration
    std::unique_ptr<DiskDataMetadata> diskMetadata;
};

// ===== Hash-Based Deduplication =====

/**
 * Computes a SHA256 digest of the string content for deduplication.
 * This enables content-based deduplication across different StringImpl instances
 * that contain identical text. The digest includes encoding information to
 * differentiate between 8-bit and 16-bit strings with same byte content.
 * 
 * @param string The StringImpl to hash (can be null)
 * @return Unique pointer to SecureDigest containing SHA256 hash, or nullptr if string is null
 */
// static 
std::unique_ptr<ParkableStringImpl::SecureDigest> ParkableStringImpl::hashString(StringImpl* string)
{
    if (!string)
        return nullptr;
    
    auto digest = makeUnique<SecureDigest>();
    digest->resize(kDigestSize);
    
#if PLATFORM(COCOA)
    // Use CommonCrypto for SHA256 on Cocoa platforms
    WTF_SHA256_CTX context;
    WTF_SHA256_Init(&context);
    
    // Hash the string content using the correct StringImpl API
    if (string->is8Bit()) {
        // For 8-bit strings
        const char* characters = reinterpret_cast<const char*>(string->span8().data());
        WTF_SHA256_Update(&context, characters, string->length() * sizeof(LChar));
    } else {
        // For 16-bit strings  
        const char* characters = reinterpret_cast<const char*>(string->span16().data());
        WTF_SHA256_Update(&context, characters, string->length() * sizeof(UChar));
    }
    
    // Include encoding information to differentiate 8-bit vs 16-bit strings with same byte content
    uint8_t encodingByte = string->is8Bit() ? 1 : 0;
    WTF_SHA256_Update(&context, &encodingByte, 1);
    
    WTF_SHA256_Final(digest->mutableSpan().data(), &context);
#else
    // Fallback implementation for non-Cocoa platforms
    // Use a combination of string hash and encoding for reasonable distribution
    uint64_t hash1 = string->hash();
    uint64_t hash2 = string->is8Bit() ? 0x1111111111111111ULL : 0x2222222222222222ULL;
    uint64_t hash3 = static_cast<uint64_t>(string->length());
    
    // Fill digest with pattern based on hashes
    for (size_t i = 0; i < kDigestSize; i += 8) {
        uint64_t value = hash1 ^ (hash2 << (i % 32)) ^ (hash3 >> (i % 32));
        size_t copySize = std::min(sizeof(uint64_t), kDigestSize - i);
        memcpy(digest->mutableSpan().data() + i, &value, copySize);
    }
#endif
    
    return digest;
}

/**
 * Updates an existing digest with encoding information.
 * In our implementation, encoding is already included in hashString(),
 * so this is a no-op.
 * 
 * @param bytes The string bytes (unused in our implementation)
 * @param is8Bit Whether the string is 8-bit encoded (unused)
 * @param digest The digest to update (unused)
 */
// static
void ParkableStringImpl::updateDigestWithEncoding(const Vector<uint8_t>& bytes, bool is8Bit, SecureDigest& digest)
{
    // This is called if we need to update an existing digest
    // For our implementation, encoding is already included in hashString
    UNUSED_PARAM(bytes);
    UNUSED_PARAM(is8Bit);
    UNUSED_PARAM(digest);
}

// ===== ParkableStringImpl Implementation =====

// Minimum string size to consider for parking (10KB)
constexpr size_t kMinimumSizeForParking = 10 * 1000;
// Minimum size for disk storage (50KB) 
constexpr size_t kMinimumSizeForDisk = 50 * 1000;

/**
 * Returns singleton work queue for background compression and disk operations.
 * Uses lazy initialization with thread-safe once_flag pattern.
 * 
 * @return Reference to the background work queue
 */
static ConcurrentWorkQueue& backgroundWorkQueue()
{
    static LazyNeverDestroyed<Ref<ConcurrentWorkQueue>> workQueue;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        workQueue.construct(ConcurrentWorkQueue::create("org.webkit.ParkableString.background"_s));
    });
    return workQueue.get();
}

/**
 * Constructor for ParkableMetadata - the parking-specific data structure.
 * This is only allocated for strings that are large enough to benefit from parking.
 * Caches essential string properties to avoid accessing StringImpl when parked.
 * 
 * @param string The original string (used to cache properties)
 * @param digest Unique SHA256 digest for deduplication
 */
// ParkableMetadata constructor
ParkableStringImpl::ParkableMetadata::ParkableMetadata(WTF::String string, std::unique_ptr<SecureDigest> digest)
    : lock()
    , lockCount(0)
    , state(State::Unparked)
    , age(Age::Young)
    , hasCompressedData(false)
    , hasOnDiskData(false)
    , backgroundTaskInProgress(false)
    , compressionFailed(false)
    , compressedData(nullptr)
    , diskMetadata(nullptr)
    , digest(WTFMove(digest))
    , is8Bit(string.is8Bit())
    , length(string.length())
{
}

/**
 * Creates a ParkableStringImpl with automatic parking eligibility determination.
 * Small strings (<10KB) will be made non-parkable for efficiency.
 * Large strings will be made parkable with computed digest.
 * 
 * @param string The StringImpl to wrap
 * @return Ref to new ParkableStringImpl
 */
Ref<ParkableStringImpl> ParkableStringImpl::create(RefPtr<StringImpl> string)
{
    return adoptRef(*new ParkableStringImpl(WTFMove(string)));
}

/**
 * Creates a non-parkable ParkableStringImpl.
 * Used for small strings that don't benefit from parking overhead.
 * No metadata is allocated, saving memory for small strings.
 * 
 * @param string The StringImpl to wrap
 * @return Ref to new non-parkable ParkableStringImpl
 */
// static
Ref<ParkableStringImpl> ParkableStringImpl::makeNonParkable(RefPtr<StringImpl> string)
{
    return adoptRef(*new ParkableStringImpl(WTFMove(string), false));
}

/**
 * Creates a parkable ParkableStringImpl with pre-computed digest.
 * Used by ParkableStringManager for deduplication - if we already computed
 * the digest for deduplication, we can reuse it instead of recomputing.
 * 
 * @param string The StringImpl to wrap
 * @param digest Pre-computed SHA256 digest for deduplication
 * @return Ref to new parkable ParkableStringImpl
 */
// static
Ref<ParkableStringImpl> ParkableStringImpl::makeParkable(RefPtr<StringImpl> string, std::unique_ptr<SecureDigest> digest)
{
    ASSERT(digest);
    return adoptRef(*new ParkableStringImpl(WTFMove(string), WTFMove(digest)));
}

/**
 * Constructor for non-parkable strings.
 * No metadata is allocated, saving memory for small strings that don't
 * benefit from parking. The string is isolated if it has multiple references
 * to ensure we have exclusive ownership.
 * 
 * @param string The StringImpl to wrap
 * @param parkable Must be false (ignored, for signature disambiguation)
 */
// Non-parkable constructor
ParkableStringImpl::ParkableStringImpl(RefPtr<StringImpl> string, bool parkable)
    : m_metadata(nullptr) // No metadata for non-parkable strings
{
    UNUSED_PARAM(parkable);
    // Store the string directly - query properties from StringImpl as needed
    if (string && string->refCount() > 1) {
        String copy { String(string.get()).isolatedCopy() };
        m_string = copy.impl();
    } else {
        m_string = string;
    }
}

/**
 * Constructor for parkable strings with pre-computed digest.
 * Allocates metadata structure for parking state management.
 * Caches string properties in metadata to avoid accessing StringImpl when parked.
 * 
 * @param string The StringImpl to wrap
 * @param digest Pre-computed SHA256 digest (null = non-parkable)
 */
// Parkable constructor with conditional metadata allocation
ParkableStringImpl::ParkableStringImpl(RefPtr<StringImpl> string, std::unique_ptr<SecureDigest> digest)
    : m_metadata(digest ? makeUnique<ParkableMetadata>(String(string.get()), WTFMove(digest)) : nullptr)
{
    // Store the string directly - query properties from StringImpl or use cached metadata
    if (string && string->refCount() > 1) {
        String copy { String(string.get()).isolatedCopy() };
        m_string = copy.impl();
    } else {
        m_string = string;
    }
    
    ASSERT(!digest || m_metadata); // If we had a digest, we should have metadata
}

/**
 * Destructor for ParkableStringImpl.
 * Cleans up any disk storage via IPC and ensures no background tasks are running.
 * Only parkable strings require cleanup - non-parkable strings just release StringImpl.
 */
ParkableStringImpl::~ParkableStringImpl()
{
    if (!mayBeParked())
        return;
    
    // There is nothing thread-hostile in this method, but the current design
    // should only reach this path through the main thread.
    assertOnValidThread();
    ASSERT(m_metadata->lockCount == 0);
    asanUnpoisonString(m_string);
    
    // Cannot destroy while parking is in progress, as the object is kept alive by
    // the background task.
    ASSERT(!m_metadata->backgroundTaskInProgress);
    ASSERT(!hasOnDiskData());
    
    // Clean up disk storage if we have it
    if (m_metadata->diskMetadata) {
        // For IPC-stored data, discard via IPC call
        String digest = digestString();
        if (!digest.isEmpty()) {
            ParkableStringManager::instance().discardStringViaIPC(digest);
        }
    }
}

// ===== Basic String Information =====

/**
 * Checks if the string is null (no content).
 * For non-parkable strings, checks if StringImpl is null.
 * For parkable strings, checks if we have no content in any form
 * (uncompressed, compressed, or on disk).
 * 
 * @return true if string has no content
 */
bool ParkableStringImpl::isNull() const
{
    if (!mayBeParked()) {
        return !m_string;
    }
    
    Locker locker { m_metadata->lock };
    return !m_string && !m_metadata->compressedData && !m_metadata->diskMetadata;
}

// Note: length() and is8Bit() are now inline methods in the header

/**
 * Returns the logical size of the string content in bytes.
 * This is the size the string would occupy if fully uncompressed,
 * regardless of current parking state. Used for memory accounting.
 * 
 * @return Size in bytes of the string content
 */
size_t ParkableStringImpl::sizeInBytes() const
{
    if (!mayBeParked()) {
        return m_string ? m_string->sizeInBytes() : 0;
    }

    return length() * (is8Bit() ? sizeof(LChar) : sizeof(UChar));
}

// ===== State Management =====

/**
 * Checks if the string is currently parked (compressed in memory).
 * Non-parkable strings are never parked.
 * Thread-safe access to parking state.
 * 
 * @return true if string is compressed and uncompressed data is discarded
 */
bool ParkableStringImpl::isParked() const
{
    if (!mayBeParked()) {
        return false;
    }
        
    Locker locker { m_metadata->lock };
    bool result = m_metadata->state == State::Parked;
    return result;
}

/**
 * Checks if compression has failed for this string.
 * Returns true if a compression attempt was made but failed,
 * indicating the string should not be compressed again.
 * Non-parkable strings never fail compression.
 * 
 * @return true if compression has been attempted and failed
 */
bool ParkableStringImpl::isCompressionFailed() const
{
    if (!mayBeParked())
        return false;
        
    Locker locker { m_metadata->lock };
    return m_metadata->compressionFailed;
}

/**
 * Checks if the string is currently stored on disk.
 * Returns true if the string's compressed data has been written to disk
 * and may have been evicted from memory. Non-parkable strings are never on disk.
 * 
 * @return true if string is stored on disk (compressed data may be discarded from memory)
 */
bool ParkableStringImpl::isOnDisk() const
{
    if (!mayBeParked())
        return false;
        
    Locker locker { m_metadata->lock };
    return m_metadata->state == State::OnDisk;
}

/**
 * Returns the current parking state of the string.
 * The state indicates where the string's primary representation is stored:
 * - Unparked: In memory as normal StringImpl
 * * - Parked: Compressed in memory, StringImpl may be discarded
 * - OnDisk: Stored on disk, compressed data may be discarded
 * - DiskCorrupted: Was on disk but couldn't be read back
 * 
 * @return Current State enum value
 */
ParkableStringImpl::State ParkableStringImpl::currentState() const
{
    if (!mayBeParked())
        return State::Unparked;
        
    Locker locker { m_metadata->lock };
    return m_metadata->state;
}

/**
 * Returns the current age of the string for parking eligibility.
 * Age determines how aggressively the string should be parked:
 * - Young: Recently created or accessed, avoid parking
 * - Old: Eligible for compression
 * - VeryOld: Eligible for disk storage
 * 
 * @return Current Age enum value
 */
ParkableStringImpl::Age ParkableStringImpl::currentAge() const
{
    if (!mayBeParked())
        return Age::Young;
        
    Locker locker { m_metadata->lock };
    return m_metadata->age;
}

/**
 * Checks if compressed data is available for this string.
 * Returns true if the string has been compressed and the compressed
 * data is available in memory. This is independent of the parking state
 * - compressed data can exist alongside uncompressed data.
 * 
 * @return true if compressed data is available in memory
 */
bool ParkableStringImpl::hasCompressedData() const
{
    if (!mayBeParked())
        return false;
        
    Locker locker { m_metadata->lock };
    return m_metadata->hasCompressedData;
}

/**
 * Checks if compressed data is available (lock-free version).
 * This version assumes the caller already holds the metadata lock.
 * Used in internal functions where the lock is already acquired.
 * 
 * @return true if compressed data is available in memory
 */
bool ParkableStringImpl::hasCompressedDataNoLock() const
{
    // Lock must already be held by caller
    if (!mayBeParked())
        return false;
    return m_metadata->hasCompressedData;
}

/**
 * Returns the size of compressed data in bytes.
 * If the string has been compressed, returns the size of the compressed
 * representation. Returns 0 if no compressed data is available.
 * Used for memory accounting and compression ratio calculations.
 * 
 * @return Size of compressed data in bytes, or 0 if not compressed
 */
size_t ParkableStringImpl::compressedSize() const
{
    if (!mayBeParked())
        return 0;
        
    Locker locker { m_metadata->lock };
    return m_metadata->compressedData ? m_metadata->compressedData->size() : 0;
}

/**
 * Checks if the string has data stored on disk.
 * Returns true if the string's compressed data has been written to disk.
 * This is independent of whether the data is also available in memory.
 * 
 * @return true if compressed data is stored on disk
 */
bool ParkableStringImpl::hasOnDiskData() const
{
    if (!mayBeParked())
        return false;
        
    Locker locker { m_metadata->lock };
    return m_metadata->hasOnDiskData;
}

/**
 * Returns the size of data stored on disk in bytes.
 * If the string has been written to disk, returns the size of the disk
 * allocation. Returns 0 if no disk data is available.
 * 
 * @return Size of disk allocation in bytes, or 0 if not on disk
 */
size_t ParkableStringImpl::onDiskSize() const
{
    if (!mayBeParked())
        return 0;
        
    Locker locker { m_metadata->lock };
    return m_metadata->diskMetadata ? m_metadata->diskMetadata->size() : 0;
}

/**
 * Returns a description of the disk storage location.
 * With centralized DiskDataAllocator, individual file paths are not exposed.
 * Returns a general description of the storage system being used.
 * 
 * @return String describing the disk storage system
 */
const String& ParkableStringImpl::diskPath() const
{
    // With centralized DiskDataAllocator, individual file paths are not exposed
    // Return a description of the disk location instead
    static NeverDestroyed<String> centralizedPath("DiskDataAllocator"_s);
    return centralizedPath.get();
}

// ===== String Access =====

/**
 * Returns a String object containing the string content.
 * Automatically unparks the string if it's currently parked or on disk.
 * Marks the string as young (recently accessed) to prevent immediate re-parking.
 * This is the main method for accessing parkable string content.
 * 
 * @return String containing the uncompressed content
 */
String ParkableStringImpl::toString()
{
    // Non-parkable strings are handled directly
    if (!mayBeParked()) {
        return m_string ? String(m_string.get()) : String();
    }
    
    Locker locker { m_metadata->lock };
    
    // Make string young when accessed
    makeYoung();
    asanUnpoisonString(m_string);
    
    // Call unpark without additional lock acquisition
    unpark();
    
    return m_string ? String(m_string.get()) : String();
}

/**
 * Returns a RefPtr to the underlying StringImpl.
 * Convenience method that calls toString() and extracts the StringImpl.
 * Automatically unparks the string if needed.
 * 
 * @return RefPtr to StringImpl containing the content
 */
RefPtr<StringImpl> ParkableStringImpl::impl()
{
    String str = toString();
    return str.impl();
}

// ===== Locking =====

/**
 * Increments the lock count to prevent parking.
 * While locked, the string cannot be parked (compressed or written to disk).
 * Also marks the string as young since it's actively being used.
 * Non-parkable strings ignore lock operations.
 * 
 * Must be paired with unlock() calls to maintain proper reference counting.
 */
void ParkableStringImpl::lock()
{
    if (!mayBeParked())
        return;
        
    Locker locker { m_metadata->lock };
    ++m_metadata->lockCount;
    makeYoung();
    
    // External code may have a hard reference to the underlying StringImpl via
    // String::impl() for the sake of thread-safety. Unpoison the string so that
    // the external code can safely access the string.
    asanUnpoisonString(m_string);
}

/**
 * Decrements the lock count, potentially allowing parking.
 * When the lock count reaches zero and no external references exist,
 * the string becomes eligible for parking. Non-parkable strings ignore unlock operations.
 * 
 * Must be paired with lock() calls to maintain proper reference counting.
 */
void ParkableStringImpl::unlock()
{
    if (!mayBeParked())
        return;
        
    Locker locker { m_metadata->lock };
    ASSERT(m_metadata->lockCount > 0);
    --m_metadata->lockCount;
    
#if defined(ADDRESS_SANITIZER) && ASSERT_ENABLED
    // There are no external references to the data, nobody should touch the data.
    //
    // Note: Only poison the memory if this is on the owning thread, as this is
    // otherwise racy. Indeed |unlock()| may be called on any thread, and
    // the owning thread may concurrently call |toString()|. It is then allowed
    // to use the string until the end of the current owning thread task.
    //
    // Checking the owning thread first as |currentStatus()| can only be called
    // from the owning thread.
    if (isMainThread() && currentStatus() == Status::UnreferencedExternally) {
        asanPoisonString(m_string);
    }
#endif // defined(ADDRESS_SANITIZER) && ASSERT_ENABLED
}

/**
 * Increments the lock count without marking the string as young.
 * Used internally during background compression tasks where we need to
 * prevent parking but don't want to reset the age (which would interfere
 * with the compression eligibility that triggered the task).
 */
void ParkableStringImpl::lockWithoutMakingYoung()
{
    if (!mayBeParked())
        return;
        
    Locker locker { m_metadata->lock };
    ++m_metadata->lockCount;
    // Don't make young - used in background compression
}

// ===== Core State Management =====

/**
 * Checks if the string can be parked immediately.
 * Returns true if all conditions for parking are met:
 * - No external references (only this ParkableStringImpl references the StringImpl)
 * - String is not locked
 * - String is not Young (has aged sufficiently)
 * - Compression has not failed for this string
 * 
 * @return true if string is eligible for immediate parking
 */
bool ParkableStringImpl::canParkNow() const
{
    return currentStatus() == Status::UnreferencedExternally 
        && m_metadata->age != Age::Young 
        && !m_metadata->compressionFailed;
}

/**
 * Determines the current reference status for parking eligibility.
 * Checks various conditions that prevent parking:
 * - Locked: String has been explicitly locked via lock()
 * - TooManyReferences: External code holds references to the StringImpl
 * - UnreferencedExternally: String is eligible for parking
 * 
 * @return Status enum indicating why parking may or may not be allowed
 */
ParkableStringImpl::Status ParkableStringImpl::currentStatus() const
{
    ASSERT(isMainThread());
    ASSERT(mayBeParked());
    
    // Can park iff:
    // - |this| is not locked.
    // - There are no external reference to |string_|. Since |this| holds a
    //   reference to |string_|, it must be the only one.
    if (m_metadata->lockCount != 0)
        return Status::Locked;
    
    // Can be null if it is compressed or on disk.
    if (!m_string)
        return Status::UnreferencedExternally;
        
    if (!m_string->hasOneRef())
        return Status::TooManyReferences;
        
    return Status::UnreferencedExternally;
}

/**
 * Discards the uncompressed StringImpl while keeping compressed data.
 * This transitions the string to Parked state where only compressed data is available.
 * The compressed data is retained for fast synchronous re-parking later.
 * Updates the manager's hash maps to reflect the new state.
 */
void ParkableStringImpl::discardUncompressedData()
{
    // Only discard uncompressed data, keep compressed data for performance
    // Must unpoison the memory before releasing it.
    asanUnpoisonString(m_string);
    m_string = nullptr;
    m_metadata->state = State::Parked;
    
    // Note: We deliberately keep m_metadata->compressedData and hasCompressedData=true
    // This allows for fast synchronous re-parking later
    
    // Notify manager (thread-safe)
    ParkableStringManager::instance().completeParked(this);
}

/**
 * Discards compressed data after it has been written to disk.
 * This transitions the string to OnDisk state where only disk storage is available.
 * Frees memory by releasing the compressed data once disk storage is confirmed.
 * Updates the manager's hash maps to reflect the new state.
 */
void ParkableStringImpl::discardCompressedData()
{
    // Discard compressed data, update flags
    m_metadata->compressedData = nullptr;
    m_metadata->hasCompressedData = false;  // Update flag
    m_metadata->state = State::OnDisk;
    
    // Note: hasOnDiskData should already be true from onDiskWriteComplete()
    
    // Notify manager (thread-safe)
    ParkableStringManager::instance().completeWrittenToDisk(this);
}

// ===== Parking Operations =====

/**
 * Unparks the string by restoring uncompressed StringImpl data.
 * Assumes the metadata lock is already held by the caller.
 * Handles various unparking scenarios:
 * - From compressed data in memory (fast path)
 * - From disk storage via IPC (slower path)
 * - From corrupted disk data (fallback path)
 */
void ParkableStringImpl::unpark()
{
    // Assume lock is already held by caller
    if (m_metadata->state == State::Unparked) {
        return;
    }
    
    // Call unparkInternal which does the actual work
    String result = unparkInternal();
    
    if (!result.isNull()) {
        m_string = result.impl();
    }
}

/**
 * Parks the string using the specified parking mode.
 * This is the main public interface for parking strings. It performs
 * eligibility checks and delegates to parkInternal() for the actual work.
 * 
 * @param mode The parking mode to use (compression, disk, etc.)
 * @return true if parking was initiated or string was already parked
 */
bool ParkableStringImpl::park(ParkingMode mode)
{
    if (!mayBeParked()) {
        return false;
    }
    
    Locker locker { m_metadata->lock };
    
    assertOnValidThread();
    
    if (m_metadata->state == State::Parked) {
        return true;
    }
    
    // Making the string old to cancel parking if it is accessed/locked before parking is complete.
    m_metadata->age = Age::Old;
    
    if (!canParkNow()) {
        return false;
    }
    
    return parkInternal(mode);
}

/**
 * Attempts to park the string based on automatic aging policies.
 * This is used by the ParkableStringManager for background parking.
 * Implements age-based parking strategy:
 * - Young strings: Just age them, don't park
 * - Old strings: Compress only
 * - VeryOld large strings: Compress then write to disk
 * 
 * @return true if parking was initiated
 */
bool ParkableStringImpl::maybeParkString()
{
    if (!mayBeParked())
        return false;
    
    Locker locker { m_metadata->lock };
    
    // For automatic parking, we need to check and age the string first
    if (m_metadata->age == Age::Young) {
        if (canParkNow()) {
            ageString();
        }
        return false; // Young strings don't get parked, just aged
    }
    
    if (!canParkNow())
        return false;
    
    // Choose parking mode based on age and size
    ParkingMode mode;
    if (m_metadata->age == Age::VeryOld && sizeInBytes() >= kMinimumSizeForDisk) {
        mode = m_metadata->hasCompressedData ? ParkingMode::WriteToDisk : ParkingMode::CompressThenWriteToDisk;
    } else {
        mode = ParkingMode::CompressOnly;
    }
    
    return parkInternal(mode);
}

/**
 * Internal parking implementation that handles different parking modes.
 * This method assumes all eligibility checks have been performed and the
 * metadata lock is held. Coordinates between synchronous and asynchronous
 * parking strategies based on available cached data.
 * 
 * @param mode The parking mode specifying the target storage type
 * @return true if parking was initiated or completed
 */
bool ParkableStringImpl::parkInternal(ParkingMode mode)
{
    ASSERT(m_metadata->state == State::Unparked || m_metadata->state == State::Parked);
    ASSERT(m_metadata->age != Age::Young);
    ASSERT(canParkNow());
    
    // No concurrent background tasks.
    if (m_metadata->backgroundTaskInProgress) {
        return true;
    }
    
    switch (mode) {
    case ParkingMode::SynchronousOnly:
        if (m_metadata->hasCompressedData) {
            discardUncompressedData(); // Handles notification internally
        } else {
            return false; // Cannot park synchronously without compressed data
        }
        break;
        
    case ParkingMode::CompressOnly:
        if (m_metadata->hasCompressedData) {
            // Synchronous parking using cached compressed data!
            discardUncompressedData(); // Handles notification internally
        } else {
            scheduleCompressionTask(mode);
        }
        break;
        
    case ParkingMode::WriteToDisk:
        if (m_metadata->hasOnDiskData) {
            discardCompressedData(); // Handles notification internally
        } else {
            // Check if we have compressed data first
            if (!m_metadata->hasCompressedData) {
                return false;
            }
            scheduleDiskWriteTask();
        }
        break;
        
    case ParkingMode::CompressThenWriteToDisk:
        if (m_metadata->hasOnDiskData) {
            discardUncompressedData(); // Handles notification internally
            discardCompressedData(); // Handles notification internally
            ASSERT(m_metadata->state == State::OnDisk);
        } else if (m_metadata->hasCompressedData) {
            discardUncompressedData(); // Handles notification internally
            return parkInternal(ParkingMode::WriteToDisk);
        } else {
            scheduleCompressionTask(mode);
        }
        break;
        
    case ParkingMode::CompressAndSwap:
        // Same as CompressThenWriteToDisk for now
        if (m_metadata->hasOnDiskData) {
            discardUncompressedData(); // Handles notification internally
            discardCompressedData(); // Handles notification internally
        } else if (m_metadata->hasCompressedData) {
            discardUncompressedData(); // Handles notification internally
            return parkInternal(ParkingMode::WriteToDisk);
        } else {
            scheduleCompressionTask(mode);
        }
        break;
    }
    
    return true;
}

/**
 * Internal unparking implementation that routes to appropriate unpark method.
 * Assumes the metadata lock is held and determines the correct unparking
 * strategy based on current state:
 * - OnDisk: Read from disk via IPC then decompress
 * - Parked: Decompress from memory
 * 
 * @return Uncompressed String content, or empty String on failure
 */
String ParkableStringImpl::unparkInternal()
{
    if (m_metadata->state == State::OnDisk) {
        return unparkFromDisk();
    }
    
    if (m_metadata->state == State::Parked) {
        return unparkFromCompressed();
    }
    
    return String();
}

/**
 * Unparks a string from compressed data stored in memory.
 * Decompresses the cached compressed data and restores the string to
 * Unparked state. Retains the compressed data for fast re-parking.
 * Records timing information for performance monitoring.
 * 
 * @return Decompressed String content, or empty String on failure
 */
String ParkableStringImpl::unparkFromCompressed()
{
    if (m_metadata->state != State::Parked) {
        return String();
    }
    
    if (!m_metadata->hasCompressedData) {
        return String();
    }
    
    // Time the decompression
    ElapsedTimer timer;
    
    // Use cached metadata values since m_string might be null when parked
    auto result = decompressData(*m_metadata->compressedData, m_metadata->is8Bit, m_metadata->length);
    
    if (!result.has_value()) {
        return String();
    }
    
    String decompressedString = result.value();
    Seconds elapsed = timer.elapsed();
    
    // Change state to unparked, but keep compressed data for fast re-parking!
    m_metadata->state = State::Unparked;
    // Note: We deliberately keep m_metadata->compressedData and hasCompressedData=true
    
    // Notify manager with timing information (thread-safe)
    ParkableStringManager::instance().completeUnpark(this, elapsed);
    
    asanUnpoisonString(m_string);
    return decompressedString;
}

/**
 * Unparks a string from disk storage via IPC.
 * Reads the compressed data from the Network Process, then decompresses it.
 * This is a synchronous operation that blocks until IPC completes.
 * Records separate timing for disk I/O and decompression.
 * 
 * @return Decompressed String content, or empty String on failure
 */
String ParkableStringImpl::unparkFromDisk()
{
    // Must be called with lock held
    if (!m_metadata->diskMetadata)
        return String();
    
    // Time the disk read
    ElapsedTimer diskTimer;
    
    // Read compressed data from disk synchronously via IPC
    String digest = digestString();
    if (digest.isEmpty())
        return String();
    
    auto compressedDataOpt = ParkableStringManager::instance().retrieveCompressedStringViaIPC(digest);
    if (!compressedDataOpt)
        return String();
    
    auto compressedData = makeUnique<Vector<uint8_t>>(WTFMove(*compressedDataOpt));
    
    Seconds diskElapsed = diskTimer.elapsed();
    
    // Restore compressed data for future use
    m_metadata->compressedData = WTFMove(compressedData);
    
    // Time the decompression
    ElapsedTimer decompressTimer;
    
    // Use our working static decompression method
    // Use cached metadata values since m_string might be null when on disk
    auto result = decompressData(*m_metadata->compressedData, m_metadata->is8Bit, m_metadata->length);
    
    if (!result.has_value()) {
        return String();
    }
    
    String decompressedString = result.value();
    Seconds decompressElapsed = decompressTimer.elapsed();
    
    // Change state, keep compressed data for fast re-parking
    m_metadata->state = State::Unparked;
    m_metadata->hasCompressedData = true;  // Set flag
    
    // Notify manager with both disk and decompression timing (thread-safe)
    ParkableStringManager::instance().completeUnpark(this, decompressElapsed, diskElapsed);
    
    asanUnpoisonString(m_string);
    return decompressedString;
}

// ===== Background Task Scheduling =====

/**
 * Schedules background compression with parameter setup and callback routing.
 * Poisons string memory, copies string data, and dispatches compression task to background queue.
 * 
 * @param mode The parking mode (compress-only or compress-then-disk)
 */
void ParkableStringImpl::scheduleCompressionTask(ParkingMode mode)
{
    ASSERT(!m_metadata->backgroundTaskInProgress);
    
    // |string_|'s data should not be touched except in the compression task.
    asanPoisonString(m_string);
    m_metadata->backgroundTaskInProgress = true;
    
    // Get data for compression
    Vector<uint8_t> data;
    if (m_string) {
        if (m_string->is8Bit()) {
            auto chars = m_string->span8();
            data.reserveInitialCapacity(chars.size());
            for (auto ch : chars) {
                data.append(static_cast<uint8_t>(ch));
            }
    } else {
            auto chars = m_string->span16();
            data.reserveInitialCapacity(chars.size() * sizeof(UChar));
            for (auto ch : chars) {
                data.append(static_cast<uint8_t>(ch & 0xFF));
                data.append(static_cast<uint8_t>((ch >> 8) & 0xFF));
        }
    }
    }
    
    // Create task parameters using the constructor
    auto params = makeUnique<BackgroundTaskParams>(
        RefPtr<ParkableStringImpl>(this),
        WTFMove(data),
        mode
    );
    
    // Post task to background queue
    WorkQueue::create("com.apple.WebKit.ParkableString.compression"_s)->dispatch([params = WTFMove(params)]() mutable {
        compressInBackground(WTFMove(params));
    });
}

/**
 * Schedules background disk write for already compressed data.
 * Copies compressed data and dispatches write task to background queue.
 */
void ParkableStringImpl::scheduleDiskWriteTask()
{
    ASSERT(!m_metadata->backgroundTaskInProgress);
    ASSERT(m_metadata->state == State::Parked);
    ASSERT(m_metadata->hasCompressedData);
    
    m_metadata->backgroundTaskInProgress = true;
    
    // Copy compressed data for background task
    Vector<uint8_t> compressedDataCopy = *m_metadata->compressedData;
    
    // Post disk write task to background queue
    backgroundWorkQueue().dispatch([protectedThis = RefPtr<ParkableStringImpl>(this), compressedDataCopy = WTFMove(compressedDataCopy)]() mutable {
        writeToDiskInBackground(WTFMove(protectedThis), compressedDataCopy);
    });
}

/**
 * Chains disk write immediately after compression completion.
 * Used for CompressThenWriteToDisk mode to avoid intermediate state.
 */
void ParkableStringImpl::scheduleDiskWriteFromCompression()
{
    if (!mayBeParked())
        return;
    
    Locker locker { m_metadata->lock };
    
    // Only proceed if we have compressed data and no background task is in progress
    if (!m_metadata->compressedData || m_metadata->backgroundTaskInProgress)
        return;
    
    m_metadata->backgroundTaskInProgress = true;
    
    Vector<uint8_t> compressedDataCopy = *m_metadata->compressedData;
    
    // Keep ourselves alive during background task
    Ref<ParkableStringImpl> protectedThis(*this);
    
    backgroundWorkQueue().dispatch([protectedThis = WTFMove(protectedThis), compressedDataCopy = WTFMove(compressedDataCopy)]() mutable {
        writeToDiskInBackground(WTFMove(protectedThis), compressedDataCopy);
    });
}

/**
 * Schedules background disk read when unparking from disk storage.
 * Copies disk metadata and dispatches read task to background queue.
 */
void ParkableStringImpl::scheduleDiskReadTask()
{
    // Must be called with lock held
    ASSERT(!m_metadata->backgroundTaskInProgress);
    ASSERT(m_metadata->diskMetadata);
    
    m_metadata->backgroundTaskInProgress = true;
    
    // Copy disk metadata values for background task to avoid copy constructor issues
    int64_t startOffset = m_metadata->diskMetadata->startOffset();
    size_t size = m_metadata->diskMetadata->size();
    
    // Keep ourselves alive during background task
    Ref<ParkableStringImpl> protectedThis(*this);
    
    backgroundWorkQueue().dispatch([protectedThis = WTFMove(protectedThis), startOffset, size]() mutable {
        DiskDataMetadata diskMetadata { startOffset, size };
        readFromDiskInBackground(WTFMove(protectedThis), diskMetadata);
    });
}

// ===== Background Static Methods =====

// Performs actual compression in background thread.
void ParkableStringImpl::compressInBackground(std::unique_ptr<BackgroundTaskParams> params)
{
    ElapsedTimer timer;

    std::unique_ptr<Vector<uint8_t>> compressedData = nullptr;
    
    if (params->data.size() > 0) {
        compressedData = compressData(params->data);
    }
    
    Seconds elapsed = timer.elapsed();
    
    recordStatistics(params->data.size(), elapsed, ParkingAction::Parked);
    
    // Complete compression on main thread
    callOnMainThread([parkableString = params->string, params = WTFMove(params), compressedData = WTFMove(compressedData)]() mutable {
        parkableString->onCompressionCompleteOnMainThread(WTFMove(params), WTFMove(compressedData));
    });
}

// Writes compressed data to disk storage using DiskDataAllocator.
void ParkableStringImpl::writeToDiskInBackground(RefPtr<ParkableStringImpl> parkableString, const Vector<uint8_t>& compressedData)
{
    ElapsedTimer timer;
    
    auto& manager = ParkableStringManager::instance();
    
    // Use digest as the storage key for IPC-based storage
    String digest = parkableString->digestString();
    if (digest.isEmpty()) {
        // Can't store without a digest
        callOnMainThread([parkableString = WTFMove(parkableString)]() {
            parkableString->onDiskWriteComplete(nullptr);
        });
        return;
    }
    
    // Store via IPC to Network Process
    bool success = manager.storeCompressedStringViaIPC(digest, compressedData);
    Seconds elapsed = timer.elapsed();
    
    // Record statistics
    recordStatistics(compressedData.size(), elapsed, ParkingAction::Written);
    
    // Create synthetic metadata indicating successful storage
    std::unique_ptr<DiskDataMetadata> metadata;
    if (success) {
        // Use digest as a synthetic identifier for IPC-stored data
        // startOffset = -1 indicates IPC storage (not file-based)
        metadata = makeUnique<DiskDataMetadata>(-1, compressedData.size());
    }
    
    // Complete on main thread
    callOnMainThread([parkableString = WTFMove(parkableString), metadata = WTFMove(metadata)]() mutable {
        parkableString->onDiskWriteComplete(WTFMove(metadata));
    });
}

void ParkableStringImpl::readFromDiskInBackground(RefPtr<ParkableStringImpl> parkableString, const DiskDataMetadata& diskMetadata)
{
    auto& manager = ParkableStringManager::instance();
    
    // For IPC-stored data (indicated by startOffset == -1), retrieve via IPC
    if (diskMetadata.startOffset() == -1) {
        String digest = parkableString->digestString();
        if (digest.isEmpty()) {
            // Can't retrieve without digest
            callOnMainThread([parkableString = WTFMove(parkableString)]() {
                parkableString->onDiskReadComplete(nullptr);
            });
            return;
        }
        
        // Retrieve via IPC from Network Process
        auto compressedData = manager.retrieveCompressedStringViaIPC(digest);
        auto dataPtr = compressedData ? makeUnique<Vector<uint8_t>>(WTFMove(*compressedData)) : nullptr;
        
        callOnMainThread([parkableString = WTFMove(parkableString), compressedData = WTFMove(dataPtr)]() mutable {
            parkableString->onDiskReadComplete(WTFMove(compressedData));
        });
    } else {
        // Legacy direct disk access is not supported in IPC-only implementation
        WTFLogAlways("ParkableString: Legacy disk access not supported in IPC implementation");
        callOnMainThread([parkableString = WTFMove(parkableString)]() mutable {
            parkableString->onDiskReadComplete(nullptr);
        });
    }
}

// ===== Background Completion Callbacks =====

// Handles completion of background compression.
void ParkableStringImpl::onCompressionCompleteOnMainThread(std::unique_ptr<BackgroundTaskParams> params, std::unique_ptr<WTF::Vector<uint8_t>> compressedData)
{
    if (!mayBeParked())
        return;
    
    Locker locker { m_metadata->lock };
    
    ASSERT(m_metadata->backgroundTaskInProgress);
    ASSERT(m_metadata->state == State::Unparked);
    
    m_metadata->backgroundTaskInProgress = false;
            
    // Always keep the compressed data. Compression is expensive, so even if the
    // uncompressed representation cannot be discarded now, avoid compressing
    // multiple times. This will allow synchronous parking next time.
    ASSERT(!m_metadata->compressedData);
    if (compressedData) {
        m_metadata->compressedData = WTFMove(compressedData);
        m_metadata->hasCompressedData = true;  // Set flag
        // Track successful compression
        ParkableStringManager::instance().recordCompression();
    } else {
        m_metadata->compressionFailed = true;
    }
    
    // Between |park()| and now, things may have happened:
    // 1. |toString()| or
    // 2. |lock()| may have been called.
    //
    // Both of these will make the string young again, and if so we don't
    // discard the compressed representation yet.
    bool canParkNow = this->canParkNow();
    
    if (canParkNow && m_metadata->hasCompressedData) {
        // Actually park now - discard uncompressed but keep compressed
        discardUncompressedData();
        params->data = {}; // Clear uncompressed data copy to reduce memory pressure
    } else {
        // Cancel parking but keep compressed data for next time!
        m_metadata->state = State::Unparked;
        // Note: We keep both m_string AND m_metadata->compressedData
        //params->data = {};  // Clear uncompressed data copy (no longer needed)
    }
    
    // Check if we need to continue to disk
    if (params->parkingMode == ParkingMode::CompressThenWriteToDisk && isParkedNoLock()) {
        parkInternal(ParkingMode::WriteToDisk);
    }
}

// Handles completion of disk write operation.
void ParkableStringImpl::onDiskWriteComplete(std::unique_ptr<DiskDataMetadata> metadata)
{
    if (!mayBeParked())
        return;
    
    Locker locker { m_metadata->lock };
    
    ASSERT(m_metadata->backgroundTaskInProgress);
    ASSERT(!m_metadata->diskMetadata);
    
    m_metadata->backgroundTaskInProgress = false;
    
    // Writing failed.
    if (!metadata) {
        return;
    }
    
    m_metadata->diskMetadata = WTFMove(metadata);
    m_metadata->hasOnDiskData = true;  // Set flag
    
    // State can be:
    // - Parked: unparking didn't happen in the meantime.
    // - Unparked: unparking happened in the meantime.
    ASSERT(m_metadata->state == State::Unparked || m_metadata->state == State::Parked);
    
    if (m_metadata->state == State::Parked) {
        // discardCompressedData() handles manager notification internally
        discardCompressedData();
        ASSERT(m_metadata->state == State::OnDisk);
    }
    
    // Record disk write timing (this was already paid for)
    // TODO: Add disk write timing parameter to this method
}

void ParkableStringImpl::onDiskReadComplete(std::unique_ptr<Vector<uint8_t>> compressedData)
{
    if (!mayBeParked())
        return;
    
    Locker locker { m_metadata->lock };
    
    m_metadata->backgroundTaskInProgress = false;
    
    if (compressedData) {
        m_metadata->compressedData = WTFMove(compressedData);
        m_metadata->hasCompressedData = true;  // Set flag
        // Don't change state here - let unpark() handle it
    }
}

// ===== Disk Operations =====
// Note: Individual file operations removed - now using centralized DiskDataAllocator

// ===== Compression Operations (existing, keeping for compatibility) =====

/**
 * Performs immediate compression without background threading.
 * Used for synchronous compression when background tasks are not suitable.
 * 
 * @return Compressed data vector or nullptr if compression fails
 */
std::unique_ptr<Vector<uint8_t>> ParkableStringImpl::compressDataSynchronously()
{
    if (!mayBeParked())
        return nullptr;
    
    if (!m_string)
        return nullptr;
    
    // Prepare string data for compression
    Vector<uint8_t> stringData;
    if (is8Bit()) {
        auto span = m_string->span8();
        stringData.reserveInitialCapacity(span.size());
        stringData.append(span);
    } else {
        auto span = m_string->span16();
        stringData.reserveInitialCapacity(span.size() * sizeof(UChar));
        for (auto ch : span) {
            stringData.append(static_cast<uint8_t>(ch & 0xFF));
            stringData.append(static_cast<uint8_t>((ch >> 8) & 0xFF));
        }
    }
    
    return compressData(stringData);
}

std::unique_ptr<Vector<uint8_t>> ParkableStringImpl::compressData(const Vector<uint8_t>& data)
{
    // Direct ZStream usage for stateless compression
    // Avoid CompressionStreamEncoder to prevent Web API overhead and state management
    
    if (data.isEmpty())
        return nullptr;
    
    // Use raw zlib instead of ZStream to avoid destructor issues
    z_stream stream = { };
    
    // Initialize compression
    int result = deflateInit2(&stream, 5, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY);
    if (result != Z_OK)
        return nullptr;
    
    // Prepare input
    stream.next_in = const_cast<Bytef*>(data.span().data());
    stream.avail_in = data.size();
    
    // Start with output buffer sized for good compression (75% of input)
    size_t outputCapacity = std::max(data.size() * 3 / 4, static_cast<size_t>(16384));
    auto outputData = makeUnique<Vector<uint8_t>>();
    outputData->reserveInitialCapacity(outputCapacity);
    
    // Compress in chunks
    int deflateResult;
    do {
        // Ensure we have output space
        size_t currentSize = outputData->size();
        outputData->resize(currentSize + 16384); // 16KB chunks
        
        stream.next_out = &(*outputData)[currentSize];
        stream.avail_out = 16384;
        
        deflateResult = deflate(&stream, Z_FINISH);
        
        if (deflateResult != Z_OK && deflateResult != Z_STREAM_END) {
            // Compression failed
            deflateEnd(&stream);
            return nullptr;
        }
        
        // Adjust result size to actual compressed data
        size_t compressedBytes = 16384 - stream.avail_out;
        outputData->resize(currentSize + compressedBytes);
            
    } while (deflateResult != Z_STREAM_END && stream.avail_out == 0);
    
    // Clean up compression state
    deflateEnd(&stream);
    
    // Check compression effectiveness (at least 20% reduction as per Part 5)
    if (outputData->size() >= data.size() * 0.8) {
        return nullptr;
    }
    
    return outputData;
}

std::optional<String> ParkableStringImpl::decompressData(const Vector<uint8_t>& compressedData, bool is8Bit, unsigned length)
{
    // Use raw zlib to avoid ZStream destructor issues
    // Simple function call semantics, no object lifetime management
    
    if (compressedData.isEmpty())
        return std::nullopt;
    
    // Use raw zlib instead of ZStream to avoid destructor calling wrong cleanup
    z_stream stream = { };
    
    // Initialize decompression
    int result = inflateInit2(&stream, 15);
    if (result != Z_OK)
        return std::nullopt;
    
    // Prepare input
    stream.next_in = const_cast<Bytef*>(compressedData.span().data());
    stream.avail_in = compressedData.size();
    
    // Calculate expected output size (bytes needed for the string)
    size_t expectedBytes = length * (is8Bit ? sizeof(LChar) : sizeof(UChar));
    Vector<uint8_t> decompressedBytes;
    decompressedBytes.reserveInitialCapacity(expectedBytes);
    
    // Decompress in chunks
    int inflateResult;
    do {
        // Ensure we have output space
        size_t currentSize = decompressedBytes.size();
        decompressedBytes.resize(currentSize + 16384); // 16KB chunks
        
        stream.next_out = &decompressedBytes[currentSize];
        stream.avail_out = 16384;
        
        inflateResult = inflate(&stream, Z_NO_FLUSH);
        
        if (inflateResult != Z_OK && inflateResult != Z_STREAM_END) {
            // Decompression failed
            inflateEnd(&stream);
            return std::nullopt;
        }
        
        // Adjust result size to actual decompressed data
        size_t decompressedChunkBytes = 16384 - stream.avail_out;
        decompressedBytes.resize(currentSize + decompressedChunkBytes);
            
    } while (inflateResult != Z_STREAM_END && stream.avail_out == 0);
    
    // Clean up decompression state
    inflateEnd(&stream);
    
    // Verify we got the expected amount of data
    if (decompressedBytes.size() != expectedBytes) {
        return std::nullopt;
    }
    
    auto decompressedSpan = decompressedBytes.span();
    
    // Create string according to original format
    if (is8Bit) {
    auto stringImpl = StringImpl::create(byteCast<LChar>(decompressedSpan));
    return String(WTFMove(stringImpl));
    } else {
        // Verify even number of bytes for UChar
        if (decompressedSpan.size() % sizeof(UChar) != 0)
            return std::nullopt;
        
        // Convert bytes to UChar using safe buffer operations
        Vector<UChar> ucharVector;
        ucharVector.reserveInitialCapacity(length);
        
        for (size_t i = 0; i < length; ++i) {
            size_t byteIndex = i * sizeof(UChar);
            UChar ch = static_cast<UChar>(decompressedSpan[byteIndex]) | 
                      (static_cast<UChar>(decompressedSpan[byteIndex + 1]) << 8);
            ucharVector.append(ch);
        }
        
        auto stringImpl = StringImpl::create(ucharVector.span());
        return String(WTFMove(stringImpl));
    }
}

/**
 * Resets string age to Young state for recently accessed strings.
 * Prevents parking until string ages through Old to VeryOld states.
 */
void ParkableStringImpl::makeYoung()
{
    if (mayBeParked())
        m_metadata->age = Age::Young;
}

/**
 * Advances string through age progression from Young to Old to VeryOld.
 * Only VeryOld strings are eligible for parking to avoid parking frequently used strings.
 */
void ParkableStringImpl::ageString()
{
    if (!mayBeParked())
        return;
    
    switch (m_metadata->age) {
    case Age::Young:
        m_metadata->age = Age::Old;
        break;
    case Age::Old:
        m_metadata->age = Age::VeryOld;
        break;
    case Age::VeryOld:
        break;
    }
}

// ===== Memory Profiling Support =====

namespace {
void recordStringImplMemoryUsage(ParkableStringImpl::MemoryUsage* result, const RefPtr<StringImpl>& stringImpl)
{
    if (stringImpl) {
        result->stringImpl = stringImpl.get();
        result->stringImplSize = sizeof(StringImpl) + stringImpl->sizeInBytes();
    }
}
} // namespace

ParkableStringImpl::MemoryUsage ParkableStringImpl::memoryUsageForSnapshot() const
{
    assertOnValidThread();
    MemoryUsage result = {0, nullptr, 0};
    
    // Base size of ParkableStringImpl
    result.thisSize = sizeof(ParkableStringImpl);
    
    if (!mayBeParked()) {
        // Non-parkable string: just include StringImpl
        recordStringImplMemoryUsage(&result, m_string);
        return result;
    }
    
    // Parkable string: add metadata overhead
    result.thisSize += sizeof(ParkableMetadata);
    
    Locker locker { m_metadata->lock };
    
    // Include StringImpl if NOT parked AND NOT on disk
    if (!isParkedNoLock() && !isOnDiskNoLock()) {
        recordStringImplMemoryUsage(&result, m_string);
    }
    
    // Trust the compressed data pointer directly
    if (m_metadata->compressedData) {
        result.thisSize += m_metadata->compressedData->size();
    }
    
    // WebKit-specific: Track disk metadata overhead
    // we store file paths per-string (~100+ bytes each)
    // This is a real memory cost of our architectural choice and should be accounted for
    if (m_metadata->hasOnDiskData && m_metadata->diskMetadata) {
        result.thisSize += sizeof(DiskDataMetadata);
        // Note: This includes the String filePath overhead, which can be substantial
    }
    
    return result;
}

size_t ParkableStringImpl::memoryFootprintForDump() const
{
    MemoryUsage usage = memoryUsageForSnapshot();
    return usage.thisSize + usage.stringImplSize;
}

// ===== ParkableString Implementation =====

ParkableString::ParkableString() = default;

ParkableString::ParkableString(RefPtr<StringImpl> string)
{
    if (!string) {
        m_impl = nullptr;
        return;
    }
    
    bool isParkable = ParkableStringManager::shouldPark(*string);
    
    if (isParkable) {
        // Use manager for parkable strings - this enables deduplication
        m_impl = ParkableStringManager::instance().add(WTFMove(string));
    } else {
        // Create non-parkable string directly
        m_impl = ParkableStringImpl::makeNonParkable(WTFMove(string));
    }
}

ParkableString::ParkableString(const ParkableString& other) = default;
ParkableString& ParkableString::operator=(const ParkableString& other) = default;
ParkableString::ParkableString(ParkableString&& other) = default;
ParkableString& ParkableString::operator=(ParkableString&& other) = default;
ParkableString::~ParkableString() = default;

bool ParkableString::isNull() const
{
    return !m_impl || m_impl->isNull();
}

size_t ParkableString::length() const
{
    return m_impl ? m_impl->length() : 0;
}

bool ParkableString::is8Bit() const
{
    return !m_impl || m_impl->is8Bit();
}

size_t ParkableString::sizeInBytes() const
{
    return m_impl ? m_impl->sizeInBytes() : 0;
}

bool ParkableString::mayBeParked() const
{
    return m_impl && m_impl->mayBeParked();
}

bool ParkableString::isParked() const
{
    return m_impl && m_impl->isParked();
}

bool ParkableString::isOnDisk() const
{
    return m_impl && m_impl->isOnDisk();
}

size_t ParkableString::compressedSize() const
{
    return m_impl ? m_impl->compressedSize() : 0;
}

size_t ParkableString::onDiskSize() const
{
    return m_impl ? m_impl->onDiskSize() : 0;
}

String ParkableString::toString() const
{
    return m_impl ? m_impl->toString() : String();
}

RefPtr<StringImpl> ParkableString::impl() const
{
    return m_impl ? m_impl->impl() : nullptr;
}

ParkableStringImpl* ParkableString::Impl() const
{
    return m_impl.get();
}

void ParkableString::lock()
{
    if (m_impl)
        m_impl->lock();
}

void ParkableString::unlock()
{
    if (m_impl)
        m_impl->unlock();
}

bool ParkableString::park(ParkableStringImpl::ParkingMode mode)
{
    return m_impl && m_impl->park(mode);
}

size_t ParkableString::memoryFootprintForDump() const
{
    return m_impl ? m_impl->memoryFootprintForDump() : 0;
}

/**
 * Returns parking state without acquiring the string's lock.
 * Used in contexts where the caller already holds the lock.
 * 
 * @return true if string is in Parked state
 */
bool ParkableStringImpl::isParkedNoLock() const
{
    if (!mayBeParked())
        return false;
    return m_metadata->state == State::Parked;
}

/**
 * Returns disk state without acquiring the string's lock.
 * Used in contexts where the caller already holds the lock.
 * 
 * @return true if string is in OnDisk state
 */
bool ParkableStringImpl::isOnDiskNoLock() const
{
    if (!mayBeParked())
        return false;
    return m_metadata->state == State::OnDisk;
}

/**
 * Converts binary SHA256 digest to hex string for IPC storage keys.
 * Used for network process storage where string keys are required.
 * 
 * @return Hex string representation of the digest or empty string if no digest
 */
String ParkableStringImpl::digestString() const
{
    const auto* digestPtr = digest();
    if (!digestPtr)
        return emptyString();
    
    // Convert digest bytes to hex string
    StringBuilder builder;
    builder.reserveCapacity(digestPtr->size() * 2);
    
    for (uint8_t byte : *digestPtr) {
        builder.append(hex(byte, 2, Lowercase));
    }
    
    return builder.toString();
}

} // namespace WebCore

#endif // ENABLE(PARKABLE_STRINGS)
