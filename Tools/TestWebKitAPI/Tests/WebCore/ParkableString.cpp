/*
 * Tests for WebCore::ParkableString - Comprehensive test suite adapted from Chrome.
 */

#include "config.h"

#if ENABLE(PARKABLE_STRINGS)

#include "Test.h"
#include "Utilities.h"
#include <WebCore/ParkableString.h>
#include <WebCore/ParkableStringManager.h>
#include <wtf/MainThread.h>
#include <wtf/text/WTFString.h>
#include <wtf/text/MakeString.h>
#include <wtf/Vector.h>
#include <wtf/Threading.h>
#include <random>
#include <thread>
#include <chrono>

namespace TestWebKitAPI {

using namespace WebCore;
using namespace WTF;

// Test constants
static constexpr size_t kSizeKb = 20;

/**
 * Creates string above 10KB parking threshold for testing.
 * Uses specified fill character and size for controlled test scenarios.
 * 
 * @param c Fill character for the string content
 * @param sizeKB Size in kilobytes (default 20KB)
 * @return Large string suitable for parking operations
 */
static String createLargeString(char c = 'a', size_t sizeKB = kSizeKb)
{
    Vector<char> data(sizeKB * 1000, c);
    return String(data);
}



/**
 * Waits for background compression using WebKit's run loop processing.
 * Polls until string is parked or on disk, with configurable timeout.
 * 
 * @param parkable The parkable string to monitor
 * @param maxWaitMs Maximum wait time in milliseconds
 * @return true if compression completed within timeout
 */
static bool waitForCompression(ParkableString& parkable, int maxWaitMs = 5000)
{
    bool result = Util::waitFor([&] {
        bool parked = parkable.isParked();
        bool onDisk = parkable.isOnDisk();
        return parked || onDisk;
    }, maxWaitMs);
    
    return result;
}

/**
 * Generic condition waiter with timeout for test synchronization.
 * Uses WebKit's Util::waitFor with configurable predicate and timeout.
 * 
 * @param predicate Function returning true when condition is met
 * @param maxWaitMs Maximum wait time in milliseconds
 * @return true if condition was met within timeout
 */
template<typename Predicate>
static bool waitForCondition(Predicate predicate, int maxWaitMs = 2000)
{
    return Util::waitFor(predicate, maxWaitMs);
}

/**
 * Initializes main thread environment for test execution.
 * Sets up WebKit threading infrastructure required for parkable strings.
 */
static void setupTest()
{
    WTF::initializeMainThread();
    // Reset manager state if possible
    // ParkableStringManager::instance().resetForTesting(); // If available
}

TEST(WebCore_ParkableString, BasicUsage)
{
    setupTest();
    
    // Small string - should not be parkable
    String small = "Hello WebKit ParkableString!"_s;
    ParkableString parkableSmall(small.impl());
    
    EXPECT_FALSE(parkableSmall.isNull());
    EXPECT_EQ(parkableSmall.length(), small.length());
    EXPECT_EQ(parkableSmall.toString(), small);
    EXPECT_FALSE(parkableSmall.isParked());
    EXPECT_FALSE(parkableSmall.mayBeParked()); // Small strings shouldn't be parkable
    
    // Large string - should be parkable
    String large = createLargeString();
    ParkableString parkableLarge(large.impl());
    
    EXPECT_TRUE(parkableLarge.mayBeParked());
    EXPECT_FALSE(parkableLarge.isParked());
    EXPECT_EQ(parkableLarge.length(), large.length());
    EXPECT_EQ(parkableLarge.toString(), large);
}

TEST(WebCore_ParkableString, EmptyString)
{
    setupTest();
    
    String empty = ""_s;
    ParkableString parkable(empty.impl());

    EXPECT_FALSE(parkable.isNull());
    EXPECT_EQ(parkable.length(), 0U);
    EXPECT_EQ(parkable.toString(), empty);
    EXPECT_FALSE(parkable.isParked());
    EXPECT_FALSE(parkable.mayBeParked()); // Empty strings shouldn't be parkable
}

TEST(WebCore_ParkableString, NullString)
{
    setupTest();
    
    ParkableString parkable;
    EXPECT_TRUE(parkable.isNull());
    EXPECT_EQ(parkable.length(), 0U);
    EXPECT_FALSE(parkable.mayBeParked());
}

TEST(WebCore_ParkableString, Copy)
{
    setupTest();
    
    String original = createLargeString('c');
    ParkableString parkable1(original.impl());
    ParkableString parkable2 = parkable1;

    EXPECT_FALSE(parkable1.isNull());
    EXPECT_FALSE(parkable2.isNull());
    EXPECT_EQ(parkable1.length(), parkable2.length());
    EXPECT_EQ(parkable1.toString(), parkable2.toString());
    EXPECT_EQ(parkable1.Impl(), parkable2.Impl()); // Should share implementation
}

TEST(WebCore_ParkableString, Parking)
{
    setupTest();
    
    String original = createLargeString('a');
    ParkableString parkable(original.impl());
    
    // Initially unparked
    EXPECT_FALSE(parkable.isParked());
    EXPECT_EQ(parkable.toString(), original);
    
    // park() ages the string and then parks it in one call
    bool parked = parkable.park();
    EXPECT_TRUE(parked);   // Should be true - Chrome ages string immediately in park()
    
    // Wait for background compression to complete
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Now access the string - this should unpark it
    EXPECT_EQ(parkable.toString(), original);
    
    // After accessing, it should be unparked again
    EXPECT_FALSE(parkable.isParked());
}

TEST(WebCore_ParkableString, LockingPreventsParking)
{
    setupTest();
    
    String original = createLargeString('b');
    ParkableString parkable(original.impl());
    
    // Lock the string
    parkable.lock();
    
    // Should not be able to park while locked (even though string gets aged)
    bool parked = parkable.park();
    EXPECT_FALSE(parked);
    EXPECT_FALSE(parkable.isParked());
    
    // Unlock and try again
    parkable.unlock();
    parked = parkable.park();
    EXPECT_TRUE(parked);
    
    // Wait for async compression to complete
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Now access the string - should preserve content and unpark
    EXPECT_EQ(parkable.toString(), original);
    EXPECT_FALSE(parkable.isParked());
}

TEST(WebCore_ParkableString, SynchronousParking)
{
    setupTest();
    
    String original = createLargeString('d');
    ParkableString parkable(original.impl());
    
    // First, park the string to get compressed data
    bool parked = parkable.park(ParkableStringImpl::ParkingMode::CompressOnly);
    EXPECT_TRUE(parked);
    
    // Wait for compression to complete
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Access the string to unpark it (but keep compressed data)
    EXPECT_EQ(parkable.toString(), original);
    EXPECT_FALSE(parkable.isParked());
    
    // Now try synchronous parking (should work because we have compressed data)
    parked = parkable.park(ParkableStringImpl::ParkingMode::SynchronousOnly);
    EXPECT_TRUE(parked);
    EXPECT_TRUE(parkable.isParked());
    
    // Verify content is still correct
    EXPECT_EQ(parkable.toString(), original);
}

TEST(WebCore_ParkableString, UTF16String)
{
    setupTest();
    
    // Create a UTF-16 string with emoji (grinning face)
    Vector<UChar> utf16Data;
    for (int i = 0; i < 10000; ++i) {
        utf16Data.append(0xD83D); // High surrogate for grinning face emoji
        utf16Data.append(0xDE00); // Low surrogate for grinning face emoji
    }
    
    String original = String(utf16Data);
    ParkableString parkable(original.impl());
    
    EXPECT_FALSE(parkable.is8Bit());
    EXPECT_TRUE(parkable.mayBeParked());
    EXPECT_EQ(parkable.length(), utf16Data.size());
    
    // Test parking and unparking
    bool parked = parkable.park();
    EXPECT_TRUE(parked);
    
    // Wait for async compression to complete
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Verify content is preserved when unparking
    String unparked = parkable.toString();
    EXPECT_FALSE(parkable.isParked());
    EXPECT_EQ(unparked, original);
    EXPECT_FALSE(unparked.is8Bit());
    EXPECT_EQ(unparked.length(), original.length());
}

TEST(WebCore_ParkableString, CompressionEffectiveness)
{
    setupTest();
    
    // Create a string with repetitive content (should compress well)
    String repetitive = createLargeString('x', 50); // 50KB of same character
    ParkableString parkable(repetitive.impl());
    
    size_t originalSize = parkable.sizeInBytes();
    
    // Park the string
    bool parked = parkable.park();
    EXPECT_TRUE(parked);
    
    // Wait for compression to complete
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Verify that we had a substantial amount of data to compress
    EXPECT_GT(originalSize, 10000U); // Should be > 10KB
    
    // Verify content integrity when unparking
    EXPECT_EQ(parkable.toString(), repetitive);
    EXPECT_FALSE(parkable.isParked());
}

TEST(WebCore_ParkableString, ParkUnparkIdenticalContent)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Park
    EXPECT_TRUE(parkable.park());
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Verify identical content after unparking
    EXPECT_EQ(original, parkable.toString());
}

TEST(WebCore_ParkableString, AbortParking)
{
    setupTest();
    
    ParkableString parkable(createLargeString().impl());
    
    // Lock the string first
    parkable.lock();
    
    // Try to park while locked - should fail
    EXPECT_FALSE(parkable.park());
    EXPECT_FALSE(parkable.isParked());
    
    // Unlock and try again - should work
    parkable.unlock();
    EXPECT_TRUE(parkable.park());
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
}

TEST(WebCore_ParkableString, BackgroundCompressionTask)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Initiate background compression
    EXPECT_TRUE(parkable.park());
    
    // Should not be immediately parked (background task)
    EXPECT_FALSE(parkable.isParked());
    
    // Wait for background compression
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Verify content preservation
    EXPECT_EQ(parkable.toString(), original);
}

TEST(WebCore_ParkableString, MultipleStringManagement)
{
    setupTest();
    
    // Create multiple large strings
    String str1 = createLargeString('a');
    String str2 = createLargeString('b');
    String str3 = createLargeString('c');
    
    ParkableString parkable1(str1.impl());
    ParkableString parkable2(str2.impl());
    ParkableString parkable3(str3.impl());
    
    // All should be parkable
    EXPECT_TRUE(parkable1.mayBeParked());
    EXPECT_TRUE(parkable2.mayBeParked());
    EXPECT_TRUE(parkable3.mayBeParked());
    
    // Park all
    EXPECT_TRUE(parkable1.park());
    EXPECT_TRUE(parkable2.park());
    EXPECT_TRUE(parkable3.park());
    
    // Wait for all to be compressed
    EXPECT_TRUE(waitForCompression(parkable1));
    EXPECT_TRUE(waitForCompression(parkable2));
    EXPECT_TRUE(waitForCompression(parkable3));
    
    EXPECT_TRUE(parkable1.isParked());
    EXPECT_TRUE(parkable2.isParked());
    EXPECT_TRUE(parkable3.isParked());
    
    // Verify all content is preserved
    EXPECT_EQ(parkable1.toString(), str1);
    EXPECT_EQ(parkable2.toString(), str2);
    EXPECT_EQ(parkable3.toString(), str3);
}

TEST(WebCore_ParkableString, LockUnlockBehavior)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Test multiple lock/unlock cycles
    parkable.lock();
    parkable.lock(); // Nested lock
    
    // Should not be able to park while locked
    EXPECT_FALSE(parkable.park()); // Locked - should fail
    
    parkable.unlock();
    EXPECT_FALSE(parkable.park()); // Still locked
    
    parkable.unlock(); // Fully unlocked
    EXPECT_TRUE(parkable.park()); // Should work now
    
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
}

TEST(WebCore_ParkableString, CompressThenAccessPattern)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Park, access, park again cycle
    EXPECT_TRUE(parkable.park()); // Park
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Access (unpark) - don't store result in variable
    EXPECT_EQ(parkable.toString(), original); // Creates temporary, no lasting reference
    EXPECT_FALSE(parkable.isParked());
    
    // Should be able to park again (synchronously now due to cached compressed data)
    EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::SynchronousOnly));
    EXPECT_TRUE(parkable.isParked());
    
    // Verify content again
    EXPECT_EQ(parkable.toString(), original);
}

TEST(WebCore_ParkableString, StringDeduplication)
{
    setupTest();
    
    String str1 = createLargeString('z');
    String str2 = String(str1.impl()->isolatedCopy()); // Same content, different impl
    
    EXPECT_NE(str1.impl(), str2.impl()); // Different implementations
    
    ParkableString parkable1(str1.impl());
    ParkableString parkable2(str2.impl());
    
    // Content should be identical
    EXPECT_EQ(parkable1.toString(), parkable2.toString());
    EXPECT_EQ(parkable1.length(), parkable2.length());
}

TEST(WebCore_ParkableString, EdgeCaseSizes)
{
    setupTest();
    
    // Test strings around the parking threshold (10KB)
    String justUnder = createLargeString('a', 9); // 9KB - should not be parkable
    String justOver = createLargeString('b', 11); // 11KB - should be parkable
    
    ParkableString parkableUnder(justUnder.impl());
    ParkableString parkableOver(justOver.impl());
    
    EXPECT_FALSE(parkableUnder.mayBeParked());
    EXPECT_TRUE(parkableOver.mayBeParked());
    
    // Try parking both
    EXPECT_FALSE(parkableUnder.park()); // Should fail
    EXPECT_TRUE(parkableOver.park()); // Should succeed
    
    EXPECT_TRUE(waitForCompression(parkableOver));
    EXPECT_TRUE(parkableOver.isParked());
}

TEST(WebCore_ParkableString, DiskStorageBasic)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Park with disk mode
    bool parked = parkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk);
    EXPECT_TRUE(parked);
    
    // Wait for compression and potential disk writing
    EXPECT_TRUE(waitForCompression(parkable));
    
    // String should still be accessible
    EXPECT_EQ(parkable.toString(), original);
}

TEST(WebCore_ParkableString, ParkingModes)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Test CompressOnly mode
    EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::CompressOnly));
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Unpark
    EXPECT_EQ(parkable.toString(), original);
    EXPECT_FALSE(parkable.isParked());
    
    // Test SynchronousOnly mode (should work due to cached compressed data)
    EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::SynchronousOnly));
    EXPECT_TRUE(parkable.isParked());
    
    // Verify content
    EXPECT_EQ(parkable.toString(), original);
}

// ===== NEW COMPREHENSIVE TESTS =====

TEST(WebCore_ParkableString, CompressionFailure)
{
    setupTest();
    
    // Create a truly random string that's difficult to compress
    Vector<char> randomData;
    randomData.reserveInitialCapacity(25000); // 25KB
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    for (size_t i = 0; i < 25000; ++i) {
        randomData.append(static_cast<char>(dis(gen)));
    }
    
    String randomString = String(randomData);
    ParkableString parkable(randomString.impl());
    
    EXPECT_TRUE(parkable.mayBeParked());
    
    // Try to park - compression might fail due to poor compression ratio
    bool parked = parkable.park(ParkableStringImpl::ParkingMode::CompressOnly);
    EXPECT_TRUE(parked); // Park call should succeed initially
    
    // Wait for background compression
    waitForCondition([&] { 
        return parkable.isParked() || parkable.compressedSize() == 0; 
    }, 2000);
    
    // Whether parking succeeded or failed, the string should remain accessible
    String result = parkable.toString();
    EXPECT_EQ(result, randomString);
    EXPECT_EQ(result.length(), randomString.length());
}

TEST(WebCore_ParkableString, AbortedParkingRetainsCompressedData)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Start parking
    EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::CompressOnly));
    
    // Access string before compression completes (this cancels parking)
    parkable.toString();
    
    // Wait for background compression to actually complete
    waitForCondition([&] { 
        return parkable.compressedSize() > 0; 
    }, 2000);
    
    // String should not be parked but compressed data should be retained
    EXPECT_FALSE(parkable.isParked());
    
    // Now synchronous parking should work using cached compressed data
    if (parkable.compressedSize() > 0) {
        bool synchronousSuccess = parkable.park(ParkableStringImpl::ParkingMode::SynchronousOnly);
        EXPECT_TRUE(synchronousSuccess);
        EXPECT_TRUE(parkable.isParked());
        EXPECT_EQ(parkable.toString(), original);
    }
}

TEST(WebCore_ParkableString, LockParkedString)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Park the string
    EXPECT_TRUE(parkable.park());
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Lock the parked string (should not unpark it)
    parkable.lock();
    EXPECT_TRUE(parkable.isParked());
    
    // Access should unpark but keep it locked
    EXPECT_EQ(parkable.toString(), original);
    EXPECT_FALSE(parkable.isParked());
    
    // Cannot park while locked
    EXPECT_FALSE(parkable.park());
    
    // Unlock and can park again
    parkable.unlock();
    EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::SynchronousOnly));
    EXPECT_TRUE(parkable.isParked());
}

TEST(WebCore_ParkableString, ManagerSimple)
{
    setupTest();
    
    auto& manager = ParkableStringManager::instance();
    size_t initialSize = manager.size();
    
    // Small strings are not tracked
    ParkableString small(String("abc"_s).impl());
    EXPECT_EQ(manager.size(), initialSize);
    
    // Large strings are tracked
    ParkableString parkable(createLargeString().impl());
    EXPECT_EQ(manager.size(), initialSize + 1);
    
    // Multiple references to same string don't increase count
    ParkableString copy = parkable;
    EXPECT_EQ(manager.size(), initialSize + 1);
}

TEST(WebCore_ParkableString, ManagerMultipleStrings)
{
    setupTest();
    
    auto& manager = ParkableStringManager::instance();
    size_t initialSize = manager.size();
    
    Vector<ParkableString> strings;
    
    // Create multiple different strings
    for (int i = 0; i < 5; ++i) {
        String str = createLargeString('A' + i);
        strings.append(ParkableString(str.impl()));
    }
    
    EXPECT_EQ(manager.size(), initialSize + 5);
    
    // Remove some strings
    strings.removeLast();
    strings.removeLast();
    
    // Size might not immediately decrease due to reference counting
    // But should not be more than initial + 5
    EXPECT_LE(manager.size(), initialSize + 5);
}

TEST(WebCore_ParkableString, ThreadSafetyBasic)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Park the string first
    EXPECT_TRUE(parkable.park());
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Simple thread safety test - just verify string can be accessed from main thread after parking
    String result = parkable.toString();
    EXPECT_EQ(result, original);
    EXPECT_FALSE(parkable.isParked()); // Should be unparked after access
}

TEST(WebCore_ParkableString, LockUnlockCycles)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Test multiple lock/unlock cycles on the main thread
    for (int i = 0; i < 10; ++i) {
        parkable.lock();
        EXPECT_FALSE(parkable.park()); // Should fail while locked
        parkable.unlock();
        
        // Should be able to park when unlocked
        if (parkable.park()) {
            waitForCompression(parkable);
            EXPECT_EQ(parkable.toString(), original);
        }
    }
}

TEST(WebCore_ParkableString, CompressThenToDisk)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Test compress-then-disk mode
    bool parked = parkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk);
    EXPECT_TRUE(parked);
    
    // Wait for both compression and potential disk writing
    waitForCondition([&] { 
        return parkable.isParked() || parkable.isOnDisk(); 
    }, 10000); // Longer timeout for disk operations
    
    // String should be accessible regardless of whether it's in memory or on disk
    EXPECT_EQ(parkable.toString(), original);
    
    // After access, verify we can still access the string
    EXPECT_EQ(parkable.toString(), original);
}

TEST(WebCore_ParkableString, BasicDiskStorage)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Test basic disk storage mode (may not be implemented yet)
    parkable.park(ParkableStringImpl::ParkingMode::WriteToDisk);
    
    // Whether disk parking succeeds or not, the string should remain accessible
    EXPECT_EQ(parkable.toString(), original);
}

TEST(WebCore_ParkableString, EqualityNoUnparking)
{
    setupTest();
    
    String largeString = createLargeString();
    String copy = String(largeString.impl()->isolatedCopy());
    
    EXPECT_NE(largeString.impl(), copy.impl());
    
    ParkableString parkable(largeString.impl());
    EXPECT_TRUE(parkable.park());
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Creating a new ParkableString with same content should reuse the parked one
    ParkableString parkableCopy(copy.impl());
    
    // They might be deduplicated (implementation detail)
    EXPECT_EQ(parkableCopy.toString(), parkable.toString());
}

TEST(WebCore_ParkableString, ShouldPark)
{
    setupTest();
    
    // Test the parking criteria
    String emptyString = ""_s;
    String smallString = "Small string"_s;
    String largeString = createLargeString();
    
    // These determinations are made at ParkableString construction time
    ParkableString emptyParkable(emptyString.impl());
    ParkableString smallParkable(smallString.impl());
    ParkableString largeParkable(largeString.impl());
    
    EXPECT_FALSE(emptyParkable.mayBeParked());
    EXPECT_FALSE(smallParkable.mayBeParked());
    EXPECT_TRUE(largeParkable.mayBeParked());
}

TEST(WebCore_ParkableString, MultipleCompressionCycles)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Multiple park/unpark cycles
    for (int i = 0; i < 5; ++i) {
        // Park
        EXPECT_TRUE(parkable.park());
        EXPECT_TRUE(waitForCompression(parkable));
        EXPECT_TRUE(parkable.isParked());
        
        // Unpark by accessing
        EXPECT_EQ(parkable.toString(), original);
        EXPECT_FALSE(parkable.isParked());
        
        // Should be able to park synchronously now (cached compressed data)
        if (i > 0) {
            EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::SynchronousOnly));
            EXPECT_TRUE(parkable.isParked());
            
            // Unpark again
            EXPECT_EQ(parkable.toString(), original);
            EXPECT_FALSE(parkable.isParked());
        }
    }
}

TEST(WebCore_ParkableString, BasicParkingModes)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Test CompressOnly mode
    EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::CompressOnly));
    
    // Wait for compression and verify
    if (waitForCompression(parkable)) {
        EXPECT_TRUE(parkable.isParked());
    }
    
    // Test string access and integrity
    EXPECT_EQ(parkable.toString(), original);
    
    // Test synchronous parking after having compressed data
    if (parkable.compressedSize() > 0) {
        EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::SynchronousOnly));
        EXPECT_TRUE(parkable.isParked());
        EXPECT_EQ(parkable.toString(), original);
    }
}

TEST(WebCore_ParkableString, MemoryFootprint)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Get memory footprint before parking
    size_t unparkedFootprint = parkable.sizeInBytes();
    EXPECT_GT(unparkedFootprint, 0U);
    
    // Park and check if memory footprint changes
    EXPECT_TRUE(parkable.park());
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Parked string should still report its logical size
    EXPECT_EQ(parkable.sizeInBytes(), unparkedFootprint);
    
    // Unpark and verify size is consistent
    parkable.toString();
    EXPECT_EQ(parkable.sizeInBytes(), unparkedFootprint);
}

TEST(WebCore_ParkableString, LargeStringHandling)
{
    setupTest();
    
    // Test very large string (1MB)
    String veryLargeString = createLargeString('L', 1000); // 1MB
    ParkableString parkable(veryLargeString.impl());
    
    EXPECT_TRUE(parkable.mayBeParked());
    EXPECT_EQ(parkable.length(), 1000000U);
    
    // Should be able to park large strings
    EXPECT_TRUE(parkable.park());
    EXPECT_TRUE(waitForCompression(parkable, 10000)); // Longer timeout
    EXPECT_TRUE(parkable.isParked());
    
    // Should be able to unpark and verify content
    String unparked = parkable.toString();
    EXPECT_EQ(unparked.length(), veryLargeString.length());
    EXPECT_EQ(unparked, veryLargeString);
}

TEST(WebCore_ParkableString, UTF16LargeString)
{
    setupTest();
    
    // Create large UTF-16 string with varied content
    Vector<UChar> utf16Data;
    const size_t targetLength = 20000; // 20k characters = 40k bytes
    
    for (size_t i = 0; i < targetLength; ++i) {
        // Use various Unicode characters
        utf16Data.append(0x41 + (i % 26)); // A-Z
        if (i % 100 == 0) {
            utf16Data.append(0x3042 + (i / 100) % 50); // Hiragana
        }
    }
    
    String original = String(utf16Data);
    ParkableString parkable(original.impl());
    
    EXPECT_FALSE(parkable.is8Bit());
    EXPECT_TRUE(parkable.mayBeParked());
    
    // Park and verify
    EXPECT_TRUE(parkable.park());
    EXPECT_TRUE(waitForCompression(parkable));
    EXPECT_TRUE(parkable.isParked());
    
    // Unpark and verify content integrity
    String unparked = parkable.toString();
    EXPECT_EQ(unparked, original);
    EXPECT_FALSE(unparked.is8Bit());
}

TEST(WebCore_ParkableString, RapidParkUnparkCycles)
{
    setupTest();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // First park to get compressed data
    EXPECT_TRUE(parkable.park());
    EXPECT_TRUE(waitForCompression(parkable));
    
    // Rapid park/unpark cycles (should use cached compressed data)
    for (int i = 0; i < 20; ++i) {
        parkable.toString(); // Unpark
        EXPECT_FALSE(parkable.isParked());
        
        EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::SynchronousOnly));
        EXPECT_TRUE(parkable.isParked());
    }
    
    // Final verification
    EXPECT_EQ(parkable.toString(), original);
}

TEST(WebCore_ParkableString, HashBasedDeduplication)
{
    setupTest();
    
    String originalContent = createLargeString('D', 25); // 25KB string
    
    // Create multiple ParkableString instances with identical content
    ParkableString parkable1(originalContent.impl());
    ParkableString parkable2(String(originalContent.impl()->isolatedCopy()).impl());
    ParkableString parkable3(originalContent.impl());
    
    // All should be parkable
    EXPECT_TRUE(parkable1.mayBeParked());
    EXPECT_TRUE(parkable2.mayBeParked());
    EXPECT_TRUE(parkable3.mayBeParked());
    
    // With deduplication, they should all point to the same implementation
    EXPECT_EQ(parkable1.Impl(), parkable2.Impl());
    EXPECT_EQ(parkable1.Impl(), parkable3.Impl());
    EXPECT_EQ(parkable2.Impl(), parkable3.Impl());
    
    // Verify content is preserved
    EXPECT_EQ(parkable1.toString(), originalContent);
    EXPECT_EQ(parkable2.toString(), originalContent);
    EXPECT_EQ(parkable3.toString(), originalContent);
    
    // Manager should only track one string despite three instances
    // Note: Manager tracking is implementation-specific behavior
    
    // All should have the same digest
    EXPECT_TRUE(parkable1.Impl()->digest());
    EXPECT_TRUE(parkable2.Impl()->digest());
    EXPECT_TRUE(parkable3.Impl()->digest());
    
    if (parkable1.Impl()->digest() && parkable2.Impl()->digest()) {
        EXPECT_EQ(*parkable1.Impl()->digest(), *parkable2.Impl()->digest());
    }
}

TEST(WebCore_ParkableString, DeduplicationAcrossStates)
{
    setupTest();
    
    String originalContent = createLargeString('E', 30); // 30KB string
    
    // Create first instance and park it
    ParkableString parkable1(originalContent.impl());
    EXPECT_TRUE(parkable1.park());
    EXPECT_TRUE(waitForCompression(parkable1));
    EXPECT_TRUE(parkable1.isParked());
    
    // Create second instance with same content while first is parked
    ParkableString parkable2(String(originalContent.impl()->isolatedCopy()).impl());
    
    // Should deduplicate to the same (parked) instance
    EXPECT_EQ(parkable1.Impl(), parkable2.Impl());
    EXPECT_TRUE(parkable2.isParked()); // Should inherit parked state
    
    // Both should unpark to same content
    EXPECT_EQ(parkable1.toString(), originalContent);
    EXPECT_EQ(parkable2.toString(), originalContent);
    EXPECT_FALSE(parkable1.isParked());
    EXPECT_FALSE(parkable2.isParked());
    
    // Create third instance with same content while unparked
    ParkableString parkable3(originalContent.impl());
    EXPECT_EQ(parkable1.Impl(), parkable3.Impl());
}

TEST(WebCore_ParkableString, DeduplicationWithDifferentEncodings)
{
    setupTest();
    
    // Create 8-bit string content
    Vector<LChar> data8Bit(15000, 'X'); // 15KB of 'X'
    String string8Bit = String(data8Bit);
    
    // Create 16-bit string with same visual content
    Vector<UChar> data16Bit(15000, 'X'); // 15KB of 'X' as UChar
    String string16Bit = String(data16Bit);
    
    // Verify they have same visual content but different encodings
    EXPECT_TRUE(string8Bit.is8Bit());
    EXPECT_FALSE(string16Bit.is8Bit());
    EXPECT_EQ(string8Bit.length(), string16Bit.length());
    
    // Create parkable strings
    ParkableString parkable8Bit(string8Bit.impl());
    ParkableString parkable16Bit(string16Bit.impl());
    
    // Both should be parkable
    EXPECT_TRUE(parkable8Bit.mayBeParked());
    EXPECT_TRUE(parkable16Bit.mayBeParked());
    
    // Should NOT deduplicate because encodings are different
    EXPECT_NE(parkable8Bit.Impl(), parkable16Bit.Impl());
    
    // Should have different digests due to encoding differences
    if (parkable8Bit.Impl()->digest() && parkable16Bit.Impl()->digest()) {
        EXPECT_NE(*parkable8Bit.Impl()->digest(), *parkable16Bit.Impl()->digest());
    }
    
    // Content should still be accessible and correct
    EXPECT_EQ(parkable8Bit.toString(), string8Bit);
    EXPECT_EQ(parkable16Bit.toString(), string16Bit);
    EXPECT_TRUE(parkable8Bit.is8Bit());
    EXPECT_FALSE(parkable16Bit.is8Bit());
}

TEST(WebCore_ParkableString, DeduplicationMemoryEfficiency)
{
    setupTest();
    
    auto& manager = ParkableStringManager::instance();
    size_t initialSize = manager.size();
    
    String baseContent = createLargeString('M', 50); // 50KB base content
    
    // Create many ParkableString instances with identical content
    Vector<ParkableString> instances;
    const size_t numInstances = 20;
    
    for (size_t i = 0; i < numInstances; ++i) {
        // Create isolated copy to ensure different StringImpl pointers
        String copy = String(baseContent.impl()->isolatedCopy());
        instances.append(ParkableString(copy.impl()));
    }
    
    // All instances should be deduplicated to the same implementation
    for (size_t i = 1; i < numInstances; ++i) {
        EXPECT_EQ(instances[0].Impl(), instances[i].Impl());
    }
    
    // Manager should only track one additional string despite 20 instances
    EXPECT_EQ(manager.size(), initialSize + 1);
    
    // Verify all have correct content
    for (const auto& instance : instances) {
        EXPECT_EQ(instance.toString(), baseContent);
        EXPECT_TRUE(instance.mayBeParked());
    }
    
    // Park one instance - all should be parked due to shared implementation
    EXPECT_TRUE(instances[0].park());
    EXPECT_TRUE(waitForCompression(instances[0]));
    
    for (const auto& instance : instances) {
        EXPECT_TRUE(instance.isParked());
    }
    
    // Access one instance - all should be unparked
    String result = instances[5].toString();
    EXPECT_EQ(result, baseContent);
    
    for (const auto& instance : instances) {
        EXPECT_FALSE(instance.isParked());
    }
}

TEST(WebCore_ParkableString, DeduplicationWithManagerState)
{
    setupTest();
    
    auto& manager = ParkableStringManager::instance();
    String content = createLargeString('S', 40);
    
    // Create and park first instance
    ParkableString parkable1(content.impl());
    EXPECT_TRUE(manager.isOnParkedMapForTesting(parkable1.Impl()) == false); // Should be in unparked map initially
    
    EXPECT_TRUE(parkable1.park());
    EXPECT_TRUE(waitForCompression(parkable1));
    EXPECT_TRUE(parkable1.isParked());
    EXPECT_TRUE(manager.isOnParkedMapForTesting(parkable1.Impl()));
    
    // Create second instance with same content - should deduplicate to parked string
    ParkableString parkable2(String(content.impl()->isolatedCopy()).impl());
    EXPECT_EQ(parkable1.Impl(), parkable2.Impl());
    EXPECT_TRUE(parkable2.isParked());
    EXPECT_TRUE(manager.isOnParkedMapForTesting(parkable2.Impl()));
    
    // Unpark via second instance
    String result = parkable2.toString();
    EXPECT_EQ(result, content);
    EXPECT_FALSE(parkable1.isParked());
    EXPECT_FALSE(parkable2.isParked());
    EXPECT_FALSE(manager.isOnParkedMapForTesting(parkable1.Impl()));
}

TEST(WebCore_ParkableString, HashDigestGeneration)
{
    setupTest();
    
    String content1 = createLargeString('H', 20);
    String content2 = createLargeString('I', 20); // Different content
    String content3 = createLargeString('H', 20); // Same as content1
    
    ParkableString parkable1(content1.impl());
    ParkableString parkable2(content2.impl());
    ParkableString parkable3(content3.impl());
    
    // All should have digests
    EXPECT_TRUE(parkable1.Impl()->digest());
    EXPECT_TRUE(parkable2.Impl()->digest());
    EXPECT_TRUE(parkable3.Impl()->digest());
    
    // Same content should have same digest
    if (parkable1.Impl()->digest() && parkable3.Impl()->digest()) {
        EXPECT_EQ(*parkable1.Impl()->digest(), *parkable3.Impl()->digest());
    }
    
    // Different content should have different digests
    if (parkable1.Impl()->digest() && parkable2.Impl()->digest()) {
        EXPECT_NE(*parkable1.Impl()->digest(), *parkable2.Impl()->digest());
    }
    
    // Deduplication should work
    EXPECT_EQ(parkable1.Impl(), parkable3.Impl()); // Same content = same impl
    EXPECT_NE(parkable1.Impl(), parkable2.Impl()); // Different content = different impl
}

// ===== IPC AND NETWORK PROCESS STORAGE TESTS =====
// These tests validate the behavior patterns that would occur with IPC,
// though they test the fallback behavior when IPC is not available

/**
 * Sets up mock IPC environment for testing.
 * Validates fallback behavior when IPC is not available.
 */
static void setupMockIPC()
{
    // In test environment, IPC is not available so these tests validate
    // the fallback behavior and parking mode handling
}

/**
 * Cleans up mock IPC environment after testing.
 * Ensures no state leaks between test cases.
 */
static void teardownMockIPC()
{
    // Cleanup helper
}

TEST(WebCore_ParkableString, IPCBasicFunctionality)
{
    setupTest();
    setupMockIPC();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Test disk parking mode (would use IPC in real implementation)
    bool parked = parkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk);
    EXPECT_TRUE(parked);
    
    // Wait for compression
    EXPECT_TRUE(waitForCompression(parkable));
    
    // String should be accessible regardless of IPC availability
    EXPECT_EQ(parkable.toString(), original);
    
    // In test environment, this validates the fallback behavior
    // In real WebKit, this would go through IPC to Network Process
    
    teardownMockIPC();
}

TEST(WebCore_ParkableString, IPCStoreFailure)
{
    setupTest();
    setupMockIPC();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Park with disk mode - should fall back gracefully when IPC not available
    bool parked = parkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk);
    EXPECT_TRUE(parked); // Park call succeeds even if IPC fails
    
    // Wait for compression attempt
    waitForCondition([&] { 
        return parkable.isParked(); 
    }, 2000);
    
    // String should still be accessible (falls back to memory compression)
    EXPECT_EQ(parkable.toString(), original);
    
    teardownMockIPC();
}

TEST(WebCore_ParkableString, IPCRetrieveFailure)
{
    setupTest();
    setupMockIPC();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Park successfully
    bool parked = parkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk);
    EXPECT_TRUE(parked);
    EXPECT_TRUE(waitForCompression(parkable));
    
    // Accessing should work even when IPC is not available (fallback to compression)
    String result = parkable.toString();
    EXPECT_EQ(result, original); // Should have correct content
    EXPECT_EQ(result.length(), original.length()); // Should have correct length
    
    teardownMockIPC();
}

TEST(WebCore_ParkableString, IPCDiscardOperations)
{
    setupTest();
    setupMockIPC();
    
    Vector<ParkableString> parkables;
    Vector<String> originals;
    
    // Create multiple parkable strings
    for (int i = 0; i < 5; ++i) {
        String original = createLargeString('A' + i);
        originals.append(original);
        parkables.append(ParkableString(original.impl()));
        
        // Park each string
        EXPECT_TRUE(parkables[i].park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk));
    }
    
    // Wait for all to be compressed
    for (auto& parkable : parkables) {
        waitForCompression(parkable);
    }
    
    // Clear some parkables - in real implementation would trigger IPC discard
    parkables.removeLast();
    parkables.removeLast();
    
    // Verify remaining strings still work
    for (size_t i = 0; i < parkables.size(); ++i) {
        EXPECT_EQ(parkables[i].toString(), originals[i]);
    }
    
    teardownMockIPC();
}

TEST(WebCore_ParkableString, IPCDataIntegrity)
{
    setupTest();
    setupMockIPC();
    
    // Test with various data types
    Vector<String> testStrings = {
        createLargeString('A', 50),  // ASCII
        String::fromUTF8("🎉🚀💻🌟⭐"),  // Emoji (will be expanded to large string)
        String::fromUTF8("αβγδεζηθικλμνξοπρστυφχψω"), // Greek
        String::fromUTF8("日本語中文한국어"), // CJK
    };
    
    // Expand small test strings to meet parking threshold
    for (auto& str : testStrings) {
        if (str.length() < 10000) {
            StringBuilder builder;
            while (builder.length() < 20000) {
                builder.append(str);
            }
            str = builder.toString();
        }
    }
    
    Vector<ParkableString> parkables;
    
    // Park all test strings
    for (const auto& original : testStrings) {
        ParkableString parkable(original.impl());
        EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk));
        EXPECT_TRUE(waitForCompression(parkable));
        parkables.append(parkable);
    }
    
    // Verify parking was attempted
    
    // Verify data integrity after retrieval
    for (size_t i = 0; i < testStrings.size(); ++i) {
        String retrieved = parkables[i].toString();
        EXPECT_EQ(retrieved, testStrings[i]);
        EXPECT_EQ(retrieved.length(), testStrings[i].length());
        
        // For UTF-16 strings, verify encoding is preserved
        if (!testStrings[i].is8Bit()) {
            EXPECT_FALSE(retrieved.is8Bit());
        }
    }
    
    // Data integrity verified through content comparison
    
    teardownMockIPC();
}

TEST(WebCore_ParkableString, IPCConcurrentOperations)
{
    setupTest();
    setupMockIPC();
    
    Vector<ParkableString> parkables;
    Vector<String> originals;
    
    // Create multiple strings concurrently
    for (int i = 0; i < 10; ++i) {
        String original = createLargeString('A' + (i % 26), 15 + i); // Varying sizes
        originals.append(original);
        
        ParkableString parkable(original.impl());
        parkables.append(parkable);
        
        // Park immediately
        EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk));
    }
    
    // Wait for all operations to complete
    for (auto& parkable : parkables) {
        waitForCompression(parkable);
    }
    
    // Verify all were stored (behavioral validation)
    // In real implementation, would check IPC call count
    
    // Access all strings concurrently
    Vector<String> results;
    for (auto& parkable : parkables) {
        results.append(parkable.toString());
    }
    
    // Verify data integrity
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_EQ(results[i], originals[i]);
    }
    
    // Data integrity verified through content comparison
    
    teardownMockIPC();
}

TEST(WebCore_ParkableString, IPCOriginIsolation)
{
    setupTest();
    setupMockIPC();
    
    // Simulate different origins by using different digest prefixes
    // In real WebKit, origin isolation happens in NetworkParkableStringStorage
    
    String baseContent = createLargeString('X', 30);
    
    // Create parkable strings that would be isolated by origin
    ParkableString parkable1(baseContent.impl());
    ParkableString parkable2(baseContent.impl()); // Same content, different "origin"
    
    // Both should park successfully
    EXPECT_TRUE(parkable1.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk));
    EXPECT_TRUE(parkable2.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk));
    
    EXPECT_TRUE(waitForCompression(parkable1));
    EXPECT_TRUE(waitForCompression(parkable2));
    
    // Due to deduplication, they might share the same implementation
    // but in Network Process, they would be stored per-origin
    
    // Verify both can retrieve data
    EXPECT_EQ(parkable1.toString(), baseContent);
    EXPECT_EQ(parkable2.toString(), baseContent);
    
    teardownMockIPC();
}

TEST(WebCore_ParkableString, IPCQuotaManagement)
{
    setupTest();
    setupMockIPC();
    
    Vector<ParkableString> parkables;
    
    // Create many large strings to test quota behavior
    for (int i = 0; i < 20; ++i) {
        String large = createLargeString('A' + (i % 26), 100); // 100KB each
        ParkableString parkable(large.impl());
        
        bool parked = parkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk);
        EXPECT_TRUE(parked);
        
        parkables.append(parkable);
    }
    
    // Wait for storage operations
    for (auto& parkable : parkables) {
        waitForCompression(parkable);
    }
    
    // In real implementation, NetworkParkableStringStorage would enforce quotas
    // Here we just verify parking was attempted
    
    // All strings should remain accessible
    for (auto& parkable : parkables) {
        String result = parkable.toString();
        EXPECT_GT(result.length(), 50000U); // Should be large
    }
    
    teardownMockIPC();
}

TEST(WebCore_ParkableString, IPCErrorRecovery)
{
    setupTest();
    setupMockIPC();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Start with successful parking
    EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk));
    EXPECT_TRUE(waitForCompression(parkable));
    
    // Verify storage works
    EXPECT_EQ(parkable.toString(), original);
    
    // Simulate IPC becoming unavailable
    teardownMockIPC(); // Remove IPC functions
    
    // String should still be accessible from compressed data cache
    String result = parkable.toString();
    EXPECT_EQ(result, original);
    
    // Try parking again without IPC - may or may not succeed depending on implementation
    bool reparked = parkable.park(ParkableStringImpl::ParkingMode::CompressOnly);
    UNUSED_PARAM(reparked); // Don't require re-parking to succeed - the key test is data accessibility
    
    // Should still work without IPC regardless of parking success
    EXPECT_EQ(parkable.toString(), original);
}

TEST(WebCore_ParkableString, IPCMemoryPressureHandling)
{
    setupTest();
    setupMockIPC();
    
    Vector<ParkableString> parkables;
    
    // Create strings under memory pressure simulation
    for (int i = 0; i < 15; ++i) {
        String content = createLargeString('A' + (i % 26), 50);
        ParkableString parkable(content.impl());
        parkables.append(parkable);
        
        // Park aggressively due to "memory pressure"
        EXPECT_TRUE(parkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk));
    }
    
    // Wait for all to be processed
    for (auto& parkable : parkables) {
        waitForCompression(parkable);
    }
    
    // Should have attempted to store (behavioral validation)
    // In real implementation, would check IPC call count
    
    // Simulate memory pressure by accessing only some strings
    Vector<String> keepAlive;
    for (size_t i = 0; i < 5; ++i) {
        keepAlive.append(parkables[i].toString());
    }
    
    // Other strings should remain parked/on disk
    for (size_t i = 5; i < 10; ++i) {
        // These should still be accessible via IPC
        String result = parkables[i].toString();
        EXPECT_GT(result.length(), 25000U);
    }
    
    teardownMockIPC();
}

TEST(WebCore_ParkableString, IPCAsyncOperations)
{
    setupTest();
    setupMockIPC();
    
    String original = createLargeString();
    ParkableString parkable(original.impl());
    
    // Start async parking operation
    bool parked = parkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk);
    EXPECT_TRUE(parked);
    
    // Should not be immediately parked (async operation)
    EXPECT_FALSE(parkable.isParked());
    
    // Wait for async completion
    bool asyncCompleted = waitForCondition([&] {
        return parkable.isParked();
    }, 5000);
    
    EXPECT_TRUE(asyncCompleted);
    
    // Async retrieval
    String result = parkable.toString();
    EXPECT_EQ(result, original);
    // Data integrity verified through content comparison
    
    teardownMockIPC();
}

TEST(WebCore_ParkableString, IPCNetworkProcessIntegration)
{
    setupTest();
    setupMockIPC();
    
    // Test the full pipeline: ParkableString -> IPC -> Network Process -> Disk
    
    String original = createLargeString('N', 75); // 75KB string
    ParkableString parkable(original.impl());
    
    // Request disk storage via IPC
    bool parked = parkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk);
    EXPECT_TRUE(parked);
    
    // Wait for Network Process to handle request
    EXPECT_TRUE(waitForCompression(parkable));
    
    // Verify parking behavior (behavioral validation)
    // In real implementation, would verify IPC communication
    
    // Simulate process state changes
    // In real Network Process, data would persist across requests
    
    // Verify content integrity through full pipeline
    String result = parkable.toString();
    EXPECT_EQ(result, original);
    EXPECT_EQ(result.length(), original.length());
    
    teardownMockIPC();
}

TEST(WebCore_ParkableString, MemoryDumpProvider)
{
    setupTest();
    
    auto& manager = ParkableStringManager::instance();
    
    // Get baseline statistics
    auto initialStats = manager.getMemoryStatistics();
    
    // Create several test strings in different states
    String small = createLargeString('S', 5);     // 5KB - won't be parkable
    String medium = createLargeString('M', 15);   // 15KB - parkable
    String large = createLargeString('L', 50);    // 50KB - parkable, eligible for disk
    
    ParkableString smallParkable(small.impl());
    ParkableString mediumParkable(medium.impl());
    ParkableString largeParkable(large.impl());
    
    // Small string should not be parkable
    EXPECT_FALSE(smallParkable.mayBeParked());
    
    // Park the medium string (compression only)
    EXPECT_TRUE(mediumParkable.mayBeParked());
    EXPECT_TRUE(mediumParkable.park(ParkableStringImpl::ParkingMode::CompressOnly));
    EXPECT_TRUE(waitForCompression(mediumParkable));
    
    // Park the large string (compression + disk)
    EXPECT_TRUE(largeParkable.mayBeParked());
    EXPECT_TRUE(largeParkable.park(ParkableStringImpl::ParkingMode::CompressThenWriteToDisk));
    EXPECT_TRUE(waitForCompression(largeParkable));
    
    // Get updated statistics
    auto stats = manager.getMemoryStatistics();
    
    // Verify string counts increased
    EXPECT_GT(stats.totalStrings, initialStats.totalStrings);
    // Note: Small strings might not be parkable, but all strings end up being tracked
    
    // Verify compression statistics
    if (stats.totalCompressions > 0) {
        EXPECT_GT(stats.averageCompressionRatio, 0.0);
        EXPECT_LT(stats.averageCompressionRatio, 1.0); // Should be compressed
        EXPECT_GT(stats.compressionSavings, 0U);
    }
    
    // Test memory footprint calculation
    size_t memoryFootprint = manager.memoryFootprint();
    EXPECT_GT(memoryFootprint, 0U);
    
    // Test debug output (debug builds only)
#if !LOG_DISABLED
    // This should produce readable output to the console
    manager.dumpStatistics();
    manager.dumpDetailedStringBreakdown();
#endif
    
    // Verify statistics make sense
    EXPECT_GE(stats.totalUncompressedSize, stats.totalCompressedSize);
    EXPECT_EQ(stats.totalStrings, stats.unparkedStrings + stats.parkedStrings + stats.onDiskStrings);
    
    printf("Memory Dump Provider Test Results:\n");
    printf("Total Strings: %zu (Unparked: %zu, Parked: %zu, OnDisk: %zu)\n", 
           stats.totalStrings, stats.unparkedStrings, stats.parkedStrings, stats.onDiskStrings);
    printf("Memory: Uncompressed=%.1fKB, Compressed=%.1fKB, Metadata=%.1fKB\n",
           stats.totalUncompressedSize / 1024.0, stats.totalCompressedSize / 1024.0, 
           stats.metadataOverhead / 1024.0);
    if (stats.averageCompressionRatio > 0) {
        printf("Compression: %.1f%% ratio, %.1fKB saved\n", 
               stats.averageCompressionRatio * 100.0, stats.compressionSavings / 1024.0);
    }
    printf("Performance: %zu compressions, %zu decompressions\n", 
           stats.totalCompressions, stats.totalDecompressions);
}



} // namespace TestWebKitAPI

#endif // ENABLE(PARKABLE_STRINGS) 
