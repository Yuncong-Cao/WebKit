#pragma once

#include "config.h"

#if ENABLE(PARKABLE_STRINGS)

#include <wtf/CompletionHandler.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>
#include <WebCore/ClientOrigin.h>

namespace WebKit {

/**
 * WebParkableStringStorageConnection - WebProcess connection for parkable string storage
 * 
 * Handles IPC communication between WebProcess ParkableStringManager and
 * NetworkProcess NetworkStorageManager for parkable string storage operations.
 * 
 * This class provides the bridge between WebCore's parkable string system
 * and WebKit's Network Process storage infrastructure.
 */
class WebParkableStringStorageConnection {
    WTF_MAKE_FAST_ALLOCATED;
    WTF_MAKE_NONCOPYABLE(WebParkableStringStorageConnection);
    
public:
    static WebParkableStringStorageConnection& singleton();
    
    // Storage operations
    void storeCompressedString(const WebCore::ClientOrigin&, const String& digest, Vector<uint8_t>&& compressedData, CompletionHandler<void(bool)>&&);
    void retrieveCompressedString(const WebCore::ClientOrigin&, const String& digest, CompletionHandler<void(std::optional<Vector<uint8_t>>)>&&);
    void discardString(const WebCore::ClientOrigin&, const String& digest, CompletionHandler<void()>&&);
    void clearOrigin(const WebCore::ClientOrigin&, CompletionHandler<void()>&&);
    
    // Synchronous versions for compatibility with existing ParkableStringManager API
    bool storeCompressedStringSync(const WebCore::ClientOrigin&, const String& digest, const Vector<uint8_t>& compressedData);
    std::optional<Vector<uint8_t>> retrieveCompressedStringSync(const WebCore::ClientOrigin&, const String& digest);
    void discardStringSync(const WebCore::ClientOrigin&, const String& digest);
    void clearOriginSync(const WebCore::ClientOrigin&);

private:
    WebParkableStringStorageConnection() = default;
    ~WebParkableStringStorageConnection() = default;
    
    // Helper to get NetworkProcess connection
    class IPC::Connection& networkProcessConnection();
    
    friend class WTF::NeverDestroyed<WebParkableStringStorageConnection>;
};

// Initialize function pointers for WebCore integration
#if PLATFORM(COCOA)
void initializeParkableStringFunctionPointers();
#endif

} // namespace WebKit

#endif // ENABLE(PARKABLE_STRINGS)