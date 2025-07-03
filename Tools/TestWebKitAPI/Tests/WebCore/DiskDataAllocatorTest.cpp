/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if ENABLE(PARKABLE_STRINGS)

#include <WebCore/DiskDataAllocator.h>
#include "DiskDataAllocatorTestUtils.h"
#include <WebCore/DiskDataMetadata.h>

#include <wtf/Vector.h>
#include <wtf/FileSystem.h>
#include <wtf/CryptographicallyRandomNumber.h>

namespace TestWebKitAPI {
using namespace WebCore;

class DiskDataAllocatorTest : public testing::Test {
public:
    void SetUp() override
    {
        // Ensure clean state for each test
    }

protected:
    /**
     * Creates random test data of specified size.
     * Uses cryptographically random numbers for realistic test scenarios.
     * 
     * @param size Number of bytes to generate
     * @return Vector containing random test data
     */
    static WTF::Vector<uint8_t> makeRandomData(size_t size)
    {
        WTF::Vector<uint8_t> data;
        data.reserveInitialCapacity(size);
        for (size_t i = 0; i < size; ++i) {
            data.append(static_cast<uint8_t>(cryptographicallyRandomNumber<uint32_t>() % 256));
        }
        return data;
    }
    
    /**
     * Compares two Vector<uint8_t> using span-based comparison.
     * Provides efficient byte-level comparison for test validation.
     * 
     * @param a First vector to compare
     * @param b Second vector to compare
     * @return true if vectors contain identical data
     */
    static bool vectorsEqual(const WTF::Vector<uint8_t>& a, const WTF::Vector<uint8_t>& b)
    {
        if (a.size() != b.size())
            return false;
        auto spanA = a.span();
        auto spanB = b.span();
        return std::memcmp(spanA.data(), spanB.data(), a.size()) == 0;
    }
};

// ===== Basic Functionality Tests =====

TEST_F(DiskDataAllocatorTest, BasicReservation)
{
    InMemoryDataAllocator allocator;

    auto reservedChunk1 = allocator.tryReserveChunk(100);
    EXPECT_TRUE(reservedChunk1);
    auto metadata1 = reservedChunk1->take();
    EXPECT_EQ(0, metadata1->startOffset());

    auto reservedChunk2 = allocator.tryReserveChunk(100);
    EXPECT_TRUE(reservedChunk2);
    auto metadata2 = reservedChunk2->take();
    EXPECT_EQ(100, metadata2->startOffset());

    // Reserved chunk can be released via discard()
    allocator.discard(WTFMove(metadata2));
    
    // Second chunk is reused.
    auto reservedChunk3 = allocator.tryReserveChunk(100);
    EXPECT_TRUE(reservedChunk3);
    auto metadata3 = reservedChunk3->take();
    EXPECT_EQ(100, metadata3->startOffset());

    // If a ReservedChunk is destructed with DiskDataMetadata, the chunk is
    // released automatically.
    auto reservedChunk4 = allocator.tryReserveChunk(300);
    EXPECT_TRUE(reservedChunk4);
    reservedChunk4 = nullptr; // Auto-discard

    auto reservedChunk5 = allocator.tryReserveChunk(100);
    EXPECT_TRUE(reservedChunk5);
    auto metadata5 = reservedChunk5->take();
    EXPECT_EQ(200, metadata5->startOffset());
}

TEST_F(DiskDataAllocatorTest, ReadWrite)
{
    InMemoryDataAllocator allocator;

    constexpr size_t kSize = 1000;
    auto randomData = makeRandomData(kSize);
    auto reservedChunk = allocator.tryReserveChunk(kSize);
    ASSERT_TRUE(reservedChunk);
    
    auto metadata = allocator.write(WTFMove(reservedChunk), randomData);
    EXPECT_TRUE(metadata);
    EXPECT_EQ(kSize, metadata->size());

    auto readData = WTF::Vector<uint8_t>(kSize);
    allocator.read(*metadata, readData);

    EXPECT_TRUE(vectorsEqual(randomData, readData));
}

TEST_F(DiskDataAllocatorTest, ReadWriteDiscardMultiple)
{
    InMemoryDataAllocator allocator;

    WTF::Vector<std::pair<std::unique_ptr<DiskDataMetadata>, WTF::Vector<uint8_t>>> dataWritten;

    for (int i = 0; i < 10; i++) {
        size_t size = 100 + (cryptographicallyRandomNumber<uint32_t>() % 900); // Random size 100-1000
        auto data = makeRandomData(size);
        auto reservedChunk = allocator.tryReserveChunk(size);
        ASSERT_TRUE(reservedChunk);
        auto metadata = allocator.write(WTFMove(reservedChunk), data);
        EXPECT_TRUE(metadata);
        dataWritten.append(std::make_pair(WTFMove(metadata), WTFMove(data)));
    }

    // Verify all data can be read back correctly
    for (const auto& pair : dataWritten) {
        const auto& metadata = pair.first;
        const auto& originalData = pair.second;
        
        size_t size = metadata->size();
        auto readData = WTF::Vector<uint8_t>(size);
        allocator.read(*metadata, readData);

        EXPECT_TRUE(vectorsEqual(originalData, readData));
    }

    // Clean up
    for (auto& pair : dataWritten) {
        auto metadata = WTFMove(pair.first);
        allocator.discard(WTFMove(metadata));
    }
}

// ===== Advanced Algorithm Tests =====

TEST_F(DiskDataAllocatorTest, ExactThenWorstFit)
{
    InMemoryDataAllocator allocator;

    constexpr int count = 10;
    auto allMetadata = InMemoryDataAllocator::Allocate(&allocator, 10000, count);

    // Create holes of different sizes
    auto& holeMetadata = allMetadata[4];
    size_t holeSize = holeMetadata->size();
    int64_t holeOffset = holeMetadata->startOffset();
    allocator.discard(WTFMove(holeMetadata));

    auto& largerHoleMetadata = allMetadata[9];
    int64_t largerHoleOffset = largerHoleMetadata->startOffset();
    allocator.discard(WTFMove(largerHoleMetadata));

    // Test exact fit
    auto testData = makeRandomData(holeSize);
    auto reservedChunk = allocator.tryReserveChunk(holeSize);
    ASSERT_TRUE(reservedChunk);
    auto metadata = allocator.write(WTFMove(reservedChunk), testData);
    EXPECT_TRUE(metadata);
    EXPECT_EQ(metadata->startOffset(), holeOffset); // Exact fit
    allocator.discard(WTFMove(metadata));

    // Test worst fit (-1 to ensure it's not best fit)
    testData = makeRandomData(holeSize - 1);
    reservedChunk = allocator.tryReserveChunk(holeSize - 1);
    ASSERT_TRUE(reservedChunk);
    metadata = allocator.write(WTFMove(reservedChunk), testData);
    EXPECT_TRUE(metadata);
    EXPECT_EQ(metadata->startOffset(), largerHoleOffset); // Worst fit (larger hole)
}

TEST_F(DiskDataAllocatorTest, FreeChunksMerging)
{
    constexpr size_t kSize = 100;

    auto allocator = makeUnique<InMemoryDataAllocator>();
    auto chunks = InMemoryDataAllocator::Allocate(allocator.get(), kSize, 4);
    EXPECT_EQ(4 * kSize, allocator->diskFootprint()); // Fix type comparison
    EXPECT_EQ(0u, allocator->freeChunksSize());

    // Layout is (indices in chunks):
    // | 0 | 1 | 2 | 3 |
    // Discarding a higher index after a lower one triggers merging on the left.

    // Merge left.
    allocator->discard(WTFMove(chunks[0]));
    EXPECT_EQ(1u, allocator->FreeChunks().size());
    allocator->discard(WTFMove(chunks[1]));
    EXPECT_EQ(1u, allocator->FreeChunks().size());
    EXPECT_EQ(2 * kSize, allocator->FreeChunks().begin()->second);
    allocator->discard(WTFMove(chunks[2]));
    EXPECT_EQ(1u, allocator->FreeChunks().size());
    EXPECT_EQ(3 * kSize, allocator->FreeChunks().begin()->second);
    EXPECT_EQ(3 * kSize, allocator->freeChunksSize());
    allocator->discard(WTFMove(chunks[3]));
    EXPECT_EQ(1u, allocator->FreeChunks().size());
    EXPECT_EQ(4 * kSize, allocator->FreeChunks().begin()->second);
    EXPECT_EQ(4 * kSize, allocator->diskFootprint()); // Fix type comparison

    allocator = makeUnique<InMemoryDataAllocator>();
    chunks = InMemoryDataAllocator::Allocate(allocator.get(), kSize, 4);

    // Merge right.
    allocator->discard(WTFMove(chunks[3]));
    EXPECT_EQ(1u, allocator->FreeChunks().size());
    allocator->discard(WTFMove(chunks[2]));
    EXPECT_EQ(1u, allocator->FreeChunks().size());
    EXPECT_EQ(2 * kSize, allocator->FreeChunks().begin()->second);
    allocator->discard(WTFMove(chunks[0]));
    EXPECT_EQ(2u, allocator->FreeChunks().size());
    EXPECT_EQ(3 * kSize, allocator->freeChunksSize());
    // Multiple merges: left, then right.
    allocator->discard(WTFMove(chunks[1]));
    EXPECT_EQ(1u, allocator->FreeChunks().size());

    allocator = makeUnique<InMemoryDataAllocator>();
    chunks = InMemoryDataAllocator::Allocate(allocator.get(), kSize, 4);

    // Left then right merging.
    allocator->discard(WTFMove(chunks[0]));
    allocator->discard(WTFMove(chunks[2]));
    EXPECT_EQ(2u, allocator->FreeChunks().size());
    allocator->discard(WTFMove(chunks[1]));
    EXPECT_EQ(1u, allocator->FreeChunks().size());
}

TEST_F(DiskDataAllocatorTest, CanReuseFreedChunk)
{
    InMemoryDataAllocator allocator;

    constexpr size_t kSize = 1024; // 1KB chunks
    WTF::Vector<std::unique_ptr<DiskDataMetadata>> allMetadata;

    // Allocate 10 chunks sequentially
    for (int i = 0; i < 10; i++) {
        auto randomData = makeRandomData(kSize);
        auto reservedChunk = allocator.tryReserveChunk(kSize);
        ASSERT_TRUE(reservedChunk);
        auto metadata = allocator.write(WTFMove(reservedChunk), randomData);
        EXPECT_TRUE(metadata);
        allMetadata.append(WTFMove(metadata));
    }

    // Free chunk #4 (middle chunk)
    auto metadata = WTFMove(allMetadata[4]);
    ASSERT_TRUE(metadata);
    int64_t freedOffset = metadata->startOffset();
    allocator.discard(WTFMove(metadata));

    // Allocate same size - should reuse exact freed chunk
    auto randomData = makeRandomData(kSize);
    auto reservedChunk = allocator.tryReserveChunk(kSize);
    ASSERT_TRUE(reservedChunk);
    auto newMetadata = allocator.write(WTFMove(reservedChunk), randomData);
    EXPECT_TRUE(newMetadata);
    
    // Critical test: Must reuse exact same offset
    EXPECT_EQ(newMetadata->startOffset(), freedOffset);
}

TEST_F(DiskDataAllocatorTest, WriteWithLimitedCapacity)
{
    InMemoryDataAllocator allocator;
    allocator.setCapacityLimit(1); // 1MB limit to match Chrome's test

    constexpr size_t kMB = 1024 * 1024;

    {
        // Test: If we use max capacity, another reservation should not be possible
        auto reservedChunk = allocator.tryReserveChunk(kMB);
        ASSERT_TRUE(reservedChunk);
        auto reservedChunkFailed = allocator.tryReserveChunk(1);
        ASSERT_FALSE(reservedChunkFailed);
        // reservedChunk will be released (auto-discarded) when scope ends
    }

    // Create complex fragmented scenario:
    // | chunk1 (1MB-1000) | hole(500) | chunk3(100) | hole(400) |
    auto randomData1 = makeRandomData(kMB - 1000);
    auto reservedChunk = allocator.tryReserveChunk(randomData1.size());
    ASSERT_TRUE(reservedChunk);
    auto metadata1 = allocator.write(WTFMove(reservedChunk), randomData1);
    EXPECT_TRUE(metadata1);

    auto randomData2 = makeRandomData(500);
    reservedChunk = allocator.tryReserveChunk(randomData2.size());
    ASSERT_TRUE(reservedChunk);
    auto metadata2 = allocator.write(WTFMove(reservedChunk), randomData2);
    EXPECT_TRUE(metadata2);

    auto randomData3 = makeRandomData(100);
    reservedChunk = allocator.tryReserveChunk(randomData3.size());
    ASSERT_TRUE(reservedChunk);
    auto metadata3 = allocator.write(WTFMove(reservedChunk), randomData3);
    EXPECT_TRUE(metadata3);

    // Free middle chunk to create fragmentation
    allocator.discard(WTFMove(metadata2));

    // Now 500 bytes should be available in the hole
    reservedChunk = allocator.tryReserveChunk(450);
    ASSERT_TRUE(reservedChunk);

    // But since that chunk is now reserved, we should have no space left
    // even though there's still a 50-byte hole and 400 bytes at the end
    auto randomData4 = makeRandomData(450);
    auto reservedChunk2 = allocator.tryReserveChunk(randomData4.size());
    ASSERT_FALSE(reservedChunk2); // Should fail due to capacity limit
}

// ===== Error Handling Tests =====

TEST_F(DiskDataAllocatorTest, WriteEventuallyFail)
{
    InMemoryDataAllocator allocator;

    constexpr size_t kSize = 1 << 18; // 256KB
    auto randomData = makeRandomData(kSize);

    static_assert(4 * kSize == InMemoryDataAllocator::kMaxSize, "Size calculation check");
    
    // Fill up to capacity
    for (int i = 0; i < 4; i++) {
        auto reservedChunk = allocator.tryReserveChunk(kSize);
        ASSERT_TRUE(reservedChunk);
        auto metadata = allocator.write(WTFMove(reservedChunk), randomData);
        EXPECT_TRUE(metadata);
    }
    
    // This should exceed capacity and fail
    auto reservedChunk = allocator.tryReserveChunk(kSize);
    EXPECT_FALSE(reservedChunk);
}

// ===== File I/O Tests (using real file system) =====

TEST_F(DiskDataAllocatorTest, ProvideInvalidFile)
{
    // This test uses the actual DiskDataAllocator singleton
    auto& allocator = DiskDataAllocator::instance();
    
    EXPECT_FALSE(allocator.mayWrite());
    allocator.provideTemporaryFile(FileSystem::FileHandle());
    EXPECT_FALSE(allocator.mayWrite());
}

TEST_F(DiskDataAllocatorTest, ProvideValidFile)
{
    // This test uses the actual DiskDataAllocator singleton
    auto& allocator = DiskDataAllocator::instance();
    
    auto [tempFilePath, tempFileHandle] = FileSystem::openTemporaryFile("DiskDataAllocatorTest"_s, ".data"_s);
    
    if (!tempFileHandle.isValid()) {
        GTEST_SKIP() << "Cannot create temporary file.";
        return;
    }

    EXPECT_FALSE(allocator.mayWrite());
    allocator.provideTemporaryFile(WTFMove(tempFileHandle));
    EXPECT_TRUE(allocator.mayWrite());

    // Test read/write with a real file.
    constexpr size_t kSize = 1000;
    auto randomData = makeRandomData(kSize);
    auto reservedChunk = allocator.tryReserveChunk(kSize);
    ASSERT_TRUE(reservedChunk);
    
    auto metadata = allocator.write(WTFMove(reservedChunk), randomData);
    if (!metadata) {
        GTEST_SKIP() << "Disk full?";
        return;
    }

    EXPECT_EQ(kSize, metadata->size());

    auto readData = WTF::Vector<uint8_t>(kSize);
    allocator.read(*metadata, readData);

    EXPECT_TRUE(vectorsEqual(randomData, readData));
}

// ===== Performance Tests =====

TEST_F(DiskDataAllocatorTest, PositionedIOPerformance)
{
    // This test verifies our positioned I/O optimization works correctly
    auto& allocator = DiskDataAllocator::instance();
    
    auto [tempFilePath, tempFileHandle] = FileSystem::openTemporaryFile("PositionedIOTest"_s, ".data"_s);
    
    if (!tempFileHandle.isValid()) {
        GTEST_SKIP() << "Cannot create temporary file for positioned I/O test.";
        return;
    }

    allocator.provideTemporaryFile(WTFMove(tempFileHandle));
    EXPECT_TRUE(allocator.mayWrite());

    // Test multiple positioned writes and reads
    constexpr size_t kTestChunks = 10;
    constexpr size_t kChunkSize = 4096; // 4KB chunks
    
    WTF::Vector<std::unique_ptr<DiskDataMetadata>> metadataVector;
    WTF::Vector<WTF::Vector<uint8_t>> originalData;
    
    // Write multiple chunks at different offsets
    for (size_t i = 0; i < kTestChunks; i++) {
        auto testData = makeRandomData(kChunkSize);
        auto reservedChunk = allocator.tryReserveChunk(kChunkSize);
        ASSERT_TRUE(reservedChunk);
        
        auto metadata = allocator.write(WTFMove(reservedChunk), testData);
        EXPECT_TRUE(metadata);
        
        metadataVector.append(WTFMove(metadata));
        originalData.append(WTFMove(testData));
    }
    
    // Read back all chunks in random order to test positioned reads
    for (size_t i = 0; i < kTestChunks; i++) {
        size_t index = (i * 7) % kTestChunks; // Pseudo-random access pattern
        
        auto readData = WTF::Vector<uint8_t>(kChunkSize);
        allocator.read(*metadataVector[index], readData);
        
        EXPECT_TRUE(vectorsEqual(originalData[index], readData))
            << "Positioned I/O failed for chunk " << index;
    }
    
    WTFLogAlways("PositionedIOPerformance: Successfully tested positioned I/O with %zu chunks", kTestChunks);
}

} // namespace TestWebKitAPI

#endif // ENABLE(PARKABLE_STRINGS) 