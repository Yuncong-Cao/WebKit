#pragma once

#include "config.h"

// Always compile parkable strings support
// #if ENABLE(PARKABLE_STRINGS)

#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>
#include <wtf/FileSystem.h>
#include <WebCore/DiskDataMetadata.h>

namespace WebCore {
struct ClientOrigin;
class DiskDataAllocator;
}

namespace WebKit {

/**
 * NetworkParkableStringStorage - Network Process storage for parkable strings
 * 
 * Manages compressed string data storage for Web Content Processes.
 * This class runs in the Network Process and provides disk-backed storage
 * for parkable strings, following WebKit's process isolation model.
 * 
 * Architecture:
 * - Web Content Process: ParkableStringManager sends IPC requests
 * - Network Process: NetworkParkableStringStorage handles actual disk I/O
 * - Per-origin storage with quota management integration
 */
class NetworkParkableStringStorage {
    WTF_MAKE_FAST_ALLOCATED;
    WTF_MAKE_NONCOPYABLE(NetworkParkableStringStorage);
    
public:
    NetworkParkableStringStorage();
    ~NetworkParkableStringStorage();
    
    // Storage operations (called from NetworkStorageManager IPC handlers)
    bool storeCompressedString(const WebCore::ClientOrigin&, const String& digest, const Vector<uint8_t>& compressedData);
    std::optional<Vector<uint8_t>> retrieveCompressedString(const WebCore::ClientOrigin&, const String& digest);
    void discardString(const WebCore::ClientOrigin&, const String& digest);
    void clearOrigin(const WebCore::ClientOrigin&);
    
    // Lifecycle management
    void initialize();
    void shutdown();
    
    // Statistics and monitoring
    size_t diskFootprint() const;
    size_t memoryFootprint() const;
    
    // Testing support
    void clearAllForTesting();
    bool hasStringForTesting(const WebCore::ClientOrigin&, const String& digest) const;

private:
    struct StoredStringMetadata {
        WTF_MAKE_FAST_ALLOCATED;
    public:
        String digest;
        std::unique_ptr<WebCore::DiskDataMetadata> diskMetadata;
        size_t compressedSize;
        
        StoredStringMetadata(String&& digest, std::unique_ptr<WebCore::DiskDataMetadata>&& metadata, size_t size)
            : digest(WTFMove(digest)), diskMetadata(WTFMove(metadata)), compressedSize(size) { }
    };
    
    using OriginStringMap = HashMap<String, std::unique_ptr<StoredStringMetadata>>; // digest -> metadata
    using OriginStorageMap = HashMap<String, std::unique_ptr<OriginStringMap>>; // origin -> strings
    
    String originKey(const WebCore::ClientOrigin&) const;
    OriginStringMap& ensureOriginStorage(const WebCore::ClientOrigin&) WTF_REQUIRES_LOCK(m_lock);
    
    // Initialize disk storage allocator
    void initializeDiskAllocator();
    
    mutable Lock m_lock;
    OriginStorageMap m_originStorage WTF_GUARDED_BY_LOCK(m_lock);
    
    // Disk allocator reference (singleton from WebCore)
    WebCore::DiskDataAllocator* m_diskAllocator { nullptr };
    bool m_initialized { false };
};

} // namespace WebKit

// #endif // ENABLE(PARKABLE_STRINGS)
