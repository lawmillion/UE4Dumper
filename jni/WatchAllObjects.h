#ifndef WATCH_ALL_OBJECTS_H
#define WATCH_ALL_OBJECTS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#ifndef MEMORY_H
typedef uint8_t uint8;
typedef uint32_t uint32;
typedef uintptr_t kaddr;
#endif

static const uint32 WATCH_ALL_CHUNK_ELEMENTS = 0x10000;
static const uint32 WATCH_ALL_FU_OBJECT_ITEM_SIZE_ARM64 = 0x18;

struct WatchAllObjectItem {
    kaddr object;
    uint32 flags;
    uint32 clusterRootIndex;
    uint32 serialNumber;
    uint32 pad;
};

static_assert(sizeof(WatchAllObjectItem) == WATCH_ALL_FU_OBJECT_ITEM_SIZE_ARM64,
              "watch-all ARM64 FUObjectItem snapshot layout must stay 0x18 bytes");

enum class WatchAllObjectDiffKind {
    Added,
    Changed
};

struct WatchAllObjectDiff {
    uint32 index;
    WatchAllObjectDiffKind kind;
    WatchAllObjectItem previous;
    WatchAllObjectItem current;
};

struct WatchAllObjectSnapshotConfig {
    kaddr guObjectArray;
    kaddr fuObjectArrayToTuObjectArray;
    kaddr tuObjectArrayToNumElements;
    kaddr pointerSize;
    kaddr fuObjectItemSize;
    kaddr fuObjectItemPadd;
    uint32 chunkElements;
    bool derefGuObjectArray;
};

struct WatchAllObjectSlot {
    bool valid;
    WatchAllObjectItem item;
};

struct WatchAllChunkReadResult {
    uint32 attempted;
    uint32 succeeded;

    bool Complete() const {
        return attempted == succeeded;
    }
};

class WatchAllObjectArraySnapshot {
public:
    WatchAllObjectArraySnapshot() : lastNumElements_(0) {}

    template<typename Reader>
    bool Capture(const WatchAllObjectSnapshotConfig &config,
                 Reader &reader,
                 std::vector<WatchAllObjectDiff> &diffs) {
        diffs.clear();
        if (config.pointerSize != sizeof(kaddr) ||
            config.fuObjectItemSize != sizeof(WatchAllObjectItem) ||
            config.chunkElements == 0) {
            return false;
        }

        kaddr fuObjectArray = config.guObjectArray;
        if (config.derefGuObjectArray && !ReadValue(reader, config.guObjectArray, fuObjectArray)) {
            return false;
        }

        kaddr chunkTable = 0;
        uint32 numElements = 0;
        if (!ReadValue(reader, fuObjectArray + config.fuObjectArrayToTuObjectArray, chunkTable) ||
            !ReadValue(reader, fuObjectArray + config.fuObjectArrayToTuObjectArray + config.tuObjectArrayToNumElements,
                       numElements)) {
            return false;
        }

        if (numElements == 0 || chunkTable == 0) {
            previous_.clear();
            chunkPointers_.clear();
            lastNumElements_ = 0;
            return true;
        }

        const uint32 chunkCount = (numElements + config.chunkElements - 1) / config.chunkElements;
        std::vector<kaddr> chunkPointers(chunkCount, 0);
        if (!ReadChunkPointers(config, reader, chunkTable, chunkPointers)) {
            return false;
        }

        previous_.resize(numElements);
        std::vector<bool> chunkReadComplete(chunkCount, false);

        for (uint32 chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
            const kaddr chunk = chunkPointers[chunkIndex];
            const uint32 firstIndex = chunkIndex * config.chunkElements;
            const uint32 elementsInChunk = std::min(config.chunkElements, numElements - firstIndex);
            if (chunk == 0 || elementsInChunk == 0) {
                continue;
            }

            const bool pointerChanged = chunkIndex >= chunkPointers_.size() || chunkPointers_[chunkIndex] != chunk;
            std::vector<WatchAllObjectSlot> current(elementsInChunk);
            WatchAllChunkReadResult readResult = ReadChunkItems(config, reader, chunk, current, 0, elementsInChunk);
            chunkReadComplete[chunkIndex] = readResult.Complete();

            for (uint32 offset = 0; offset < elementsInChunk; ++offset) {
                if (!current[offset].valid) {
                    continue;
                }

                const uint32 index = firstIndex + offset;
                const WatchAllObjectItem &sample = current[offset].item;
                if (!IsLiveItem(sample)) {
                    previous_[index].valid = false;
                    previous_[index].item = sample;
                    continue;
                }

                if (!previous_[index].valid) {
                    WatchAllObjectDiff diff{};
                    diff.index = index;
                    diff.kind = WatchAllObjectDiffKind::Added;
                    diff.current = sample;
                    diffs.push_back(diff);
                } else if (pointerChanged || !SameItem(previous_[index].item, sample)) {
                    WatchAllObjectDiff diff{};
                    diff.index = index;
                    diff.kind = WatchAllObjectDiffKind::Changed;
                    diff.previous = previous_[index].item;
                    diff.current = sample;
                    diffs.push_back(diff);
                }

                previous_[index].valid = true;
                previous_[index].item = sample;
            }
        }

        chunkPointers_.resize(chunkCount, 0);
        for (uint32 chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
            if (chunkReadComplete[chunkIndex]) {
                chunkPointers_[chunkIndex] = chunkPointers[chunkIndex];
            }
        }
        lastNumElements_ = numElements;
        return true;
    }

    uint32 LastNumElements() const { return lastNumElements_; }

private:
    template<typename Reader, typename T>
    static bool ReadValue(Reader &reader, kaddr address, T &out) {
        T tmp{};
        if (!reader.TryReadBuffer(reinterpret_cast<void *>(address), &tmp, sizeof(T))) {
            return false;
        }
        out = tmp;
        return true;
    }

    template<typename Reader>
    static bool ReadChunkPointers(const WatchAllObjectSnapshotConfig &config,
                                  Reader &reader,
                                  kaddr chunkTable,
                                  std::vector<kaddr> &chunkPointers) {
        const size_t bytes = chunkPointers.size() * config.pointerSize;
        return bytes == 0 || reader.TryReadBuffer(reinterpret_cast<void *>(chunkTable), chunkPointers.data(), bytes);
    }

    template<typename Reader>
    static WatchAllChunkReadResult ReadChunkItems(const WatchAllObjectSnapshotConfig &config,
                                                  Reader &reader,
                                                  kaddr chunk,
                                                  std::vector<WatchAllObjectSlot> &out,
                                                  uint32 begin,
                                                  uint32 end) {
        WatchAllChunkReadResult result{};
        if (begin >= end) {
            return result;
        }

        const uint32 itemCount = end - begin;
        result.attempted = itemCount;
        const kaddr address = chunk + config.fuObjectItemPadd + (begin * config.fuObjectItemSize);
        std::vector<WatchAllObjectItem> items(itemCount);
        if (reader.TryReadBuffer(reinterpret_cast<void *>(address), items.data(), itemCount * sizeof(WatchAllObjectItem))) {
            for (uint32 i = 0; i < itemCount; ++i) {
                out[begin + i].valid = true;
                out[begin + i].item = items[i];
            }
            result.succeeded = itemCount;
            return result;
        }

        if (itemCount == 1) {
            return result;
        }

        const uint32 mid = begin + (itemCount / 2);
        WatchAllChunkReadResult left = ReadChunkItems(config, reader, chunk, out, begin, mid);
        WatchAllChunkReadResult right = ReadChunkItems(config, reader, chunk, out, mid, end);
        result.attempted = left.attempted + right.attempted;
        result.succeeded = left.succeeded + right.succeeded;
        return result;
    }

    static bool IsLiveItem(const WatchAllObjectItem &item) {
        return item.object != 0;
    }

    static bool SameItem(const WatchAllObjectItem &lhs, const WatchAllObjectItem &rhs) {
        return lhs.object == rhs.object &&
               lhs.flags == rhs.flags &&
               lhs.clusterRootIndex == rhs.clusterRootIndex &&
               lhs.serialNumber == rhs.serialNumber;
    }

    std::vector<WatchAllObjectSlot> previous_;
    std::vector<kaddr> chunkPointers_;
    uint32 lastNumElements_;
};

#endif
