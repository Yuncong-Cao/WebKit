#include "config.h"
#include "WebParkableStringStorageConnection.h"

#if ENABLE(PARKABLE_STRINGS)

#include "NetworkProcessConnection.h"
#include "NetworkStorageManagerMessages.h"
#include "WebProcess.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>

namespace WebKit {

/**
 * Singleton accessor using NeverDestroyed pattern.
 * Thread-safe initialization with static local variable.
 * 
 * @return Reference to the singleton connection instance
 */
WebParkableStringStorageConnection& WebParkableStringStorageConnection::singleton()
{
    static NeverDestroyed<WebParkableStringStorageConnection> connection;
    return connection;
}

/**
 * Gets IPC connection to Network Process from WebProcess.
 * Ensures connection is established before returning reference.
 * 
 * @return Reference to the network process IPC connection
 */
IPC::Connection& WebParkableStringStorageConnection::networkProcessConnection()
{
    return WebProcess::singleton().ensureNetworkProcessConnection().connection();
}

void WebParkableStringStorageConnection::storeCompressedString(const WebCore::ClientOrigin& origin, const String& digest, Vector<uint8_t>&& compressedData, CompletionHandler<void(bool)>&& completionHandler)
{
    networkProcessConnection().sendWithAsyncReply(
        Messages::NetworkStorageManager::ParkableStringStore(origin, digest, WTFMove(compressedData)),
        WTFMove(completionHandler)
    );
}

void WebParkableStringStorageConnection::retrieveCompressedString(const WebCore::ClientOrigin& origin, const String& digest, CompletionHandler<void(std::optional<Vector<uint8_t>>)>&& completionHandler)
{
    networkProcessConnection().sendWithAsyncReply(
        Messages::NetworkStorageManager::ParkableStringRetrieve(origin, digest),
        WTFMove(completionHandler)
    );
}

void WebParkableStringStorageConnection::discardString(const WebCore::ClientOrigin& origin, const String& digest, CompletionHandler<void()>&& completionHandler)
{
    networkProcessConnection().sendWithAsyncReply(
        Messages::NetworkStorageManager::ParkableStringDiscard(origin, digest),
        WTFMove(completionHandler)
    );
}

void WebParkableStringStorageConnection::clearOrigin(const WebCore::ClientOrigin& origin, CompletionHandler<void()>&& completionHandler)
{
    networkProcessConnection().sendWithAsyncReply(
        Messages::NetworkStorageManager::ParkableStringClearOrigin(origin),
        WTFMove(completionHandler)
    );
}

// Synchronous versions - These block the calling thread waiting for IPC response
// Note: Generally not recommended for WebKit, but needed for compatibility with
// existing ParkableStringManager synchronous API

bool WebParkableStringStorageConnection::storeCompressedStringSync(const WebCore::ClientOrigin& origin, const String& digest, const Vector<uint8_t>& compressedData)
{
    bool result = false;
    bool responseReceived = false;
    
    Vector<uint8_t> dataCopy = compressedData; // Make a copy since we can't move in sync call
    storeCompressedString(origin, digest, WTFMove(dataCopy), [&](bool success) {
        result = success;
        responseReceived = true;
    });
    
    // Wait for response - this is not ideal but necessary for sync API compatibility
    while (!responseReceived) {
        RunLoop::current().cycle();
    }
    
    return result;
}

std::optional<Vector<uint8_t>> WebParkableStringStorageConnection::retrieveCompressedStringSync(const WebCore::ClientOrigin& origin, const String& digest)
{
    std::optional<Vector<uint8_t>> result;
    bool responseReceived = false;
    
    retrieveCompressedString(origin, digest, [&](std::optional<Vector<uint8_t>>&& data) {
        result = WTFMove(data);
        responseReceived = true;
    });
    
    // Wait for response
    while (!responseReceived) {
        RunLoop::current().cycle();
    }
    
    return result;
}

void WebParkableStringStorageConnection::discardStringSync(const WebCore::ClientOrigin& origin, const String& digest)
{
    bool responseReceived = false;
    
    discardString(origin, digest, [&]() {
        responseReceived = true;
    });
    
    // Wait for response
    while (!responseReceived) {
        RunLoop::current().cycle();
    }
}

void WebParkableStringStorageConnection::clearOriginSync(const WebCore::ClientOrigin& origin)
{
    bool responseReceived = false;
    
    clearOrigin(origin, [&]() {
        responseReceived = true;
    });
    
    // Wait for response
    while (!responseReceived) {
        RunLoop::current().cycle();
    }
}

} // namespace WebKit

// Function pointer declarations for WebCore integration
#if ENABLE(PARKABLE_STRINGS) && PLATFORM(COCOA)
extern "C" {
    // Function pointers that WebCore can call without including WebKit headers
    extern bool (*g_webkitStoreParkableString)(const String& digest, const Vector<uint8_t>& data);
    extern std::optional<Vector<uint8_t>> (*g_webkitRetrieveParkableString)(const String& digest);
    extern void (*g_webkitDiscardParkableString)(const String& digest);
}

namespace WebKit {

/**
 * Creates synthetic origin for grouping parkable strings by process.
 * Uses special webkit-parkable-strings protocol to isolate from web content.
 * 
 * @return ClientOrigin for parkable string storage operations
 */
static WebCore::ClientOrigin getCurrentParkableStringOrigin()
{
    // Use a special origin for parkable strings that groups all strings by process
    WebCore::SecurityOriginData processOrigin { "webkit-parkable-strings"_s, "webprocess"_s, std::nullopt };
    WebCore::SecurityOriginData clientOrigin = processOrigin;
    return WebCore::ClientOrigin { WTFMove(processOrigin), WTFMove(clientOrigin) };
}

/**
 * Function pointer implementation for IPC storage operations.
 * Bridges WebCore function pointer calls to WebKit IPC system.
 * 
 * @param digest String digest for deduplication
 * @param data Compressed string data to store
 * @return true if storage succeeded
 */
static bool webkitStoreParkableStringImpl(const String& digest, const Vector<uint8_t>& data)
{
    auto& connection = WebParkableStringStorageConnection::singleton();
    return connection.storeCompressedStringSync(getCurrentParkableStringOrigin(), digest, data);
}

/**
 * Function pointer implementation for IPC retrieval operations.
 * Bridges WebCore function pointer calls to WebKit IPC system.
 * 
 * @param digest String digest to retrieve
 * @return Compressed data vector or nullopt if not found
 */
static std::optional<Vector<uint8_t>> webkitRetrieveParkableStringImpl(const String& digest)
{
    auto& connection = WebParkableStringStorageConnection::singleton();
    return connection.retrieveCompressedStringSync(getCurrentParkableStringOrigin(), digest);
}

/**
 * Function pointer implementation for IPC discard operations.
 * Bridges WebCore function pointer calls to WebKit IPC system.
 * 
 * @param digest String digest to discard from storage
 */
static void webkitDiscardParkableStringImpl(const String& digest)
{
    auto& connection = WebParkableStringStorageConnection::singleton();
    connection.discardStringSync(getCurrentParkableStringOrigin(), digest);
}

/**
 * Sets up function pointers for WebCore integration.
 * Must be called during WebProcess initialization to enable parkable strings.
 */
void initializeParkableStringFunctionPointers()
{
    g_webkitStoreParkableString = webkitStoreParkableStringImpl;
    g_webkitRetrieveParkableString = webkitRetrieveParkableStringImpl;
    g_webkitDiscardParkableString = webkitDiscardParkableStringImpl;
}

} // namespace WebKit
#endif

#endif // ENABLE(PARKABLE_STRINGS)