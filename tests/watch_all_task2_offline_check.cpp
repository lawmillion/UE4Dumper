#include "WatchAllObjects.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

class FakeReader {
public:
    std::map<kaddr, std::vector<uint8_t> > memory;
    std::map<kaddr, int> failByAddress;
    std::vector<size_t> readSizes;

    bool TryReadBuffer(void *address, void *buffer, size_t size) {
        kaddr addr = reinterpret_cast<kaddr>(address);
        readSizes.push_back(size);
        auto fail = failByAddress.find(addr);
        if (fail != failByAddress.end() && fail->second > 0) {
            fail->second--;
            return false;
        }
        for (auto it = memory.rbegin(); it != memory.rend(); ++it) {
            kaddr base = it->first;
            const std::vector<uint8_t> &bytes = it->second;
            if (addr >= base && addr + size <= base + bytes.size()) {
                memcpy(buffer, bytes.data() + (addr - base), size);
                return true;
            }
        }
        return false;
    }

    void Put(kaddr address, const void *data, size_t size) {
        const uint8_t *first = static_cast<const uint8_t *>(data);
        memory[address] = std::vector<uint8_t>(first, first + size);
    }
};

static WatchAllObjectItem Item(kaddr object, uint32 flags, uint32 cluster, uint32 serial) {
    WatchAllObjectItem item{};
    item.object = object;
    item.flags = flags;
    item.clusterRootIndex = cluster;
    item.serialNumber = serial;
    return item;
}

static void PutItems(FakeReader &reader, kaddr address, const std::vector<WatchAllObjectItem> &items) {
    const size_t bytes = items.size() * sizeof(WatchAllObjectItem);
    std::vector<uint8_t> block(bytes);
    memcpy(block.data(), items.data(), bytes);
    reader.memory[address] = block;
}

static WatchAllObjectSnapshotConfig Config(kaddr objectArray) {
    WatchAllObjectSnapshotConfig cfg{};
    cfg.guObjectArray = objectArray;
    cfg.fuObjectArrayToTuObjectArray = 0x10;
    cfg.tuObjectArrayToNumElements = 0x14;
    cfg.pointerSize = 8;
    cfg.fuObjectItemSize = 0x18;
    cfg.fuObjectItemPadd = 0;
    cfg.chunkElements = 0x10000;
    cfg.derefGuObjectArray = false;
    return cfg;
}

static void PutHeader(FakeReader &reader, kaddr guObjectArray, kaddr chunkTable, uint32 count) {
    std::vector<uint8_t> header(0x18, 0);
    memcpy(header.data(), &chunkTable, sizeof(chunkTable));
    memcpy(header.data() + 0x14, &count, sizeof(count));
    reader.memory[guObjectArray + 0x10] = header;
}

static void test_detects_added_and_changed_slots() {
    FakeReader reader;
    WatchAllObjectArraySnapshot snapshot;
    const kaddr gu = 0x100000;
    const kaddr table = 0x200000;
    const kaddr chunk = 0x300000;
    kaddr chunks[] = {chunk};
    PutHeader(reader, gu, table, 3);
    reader.Put(table, chunks, sizeof(chunks));
    PutItems(reader, chunk, {Item(0x111, 1, 2, 3), Item(0x222, 4, 5, 6), Item(0, 0, 0, 0)});

    std::vector<WatchAllObjectDiff> first;
    assert(snapshot.Capture(Config(gu), reader, first));
    assert(first.size() == 2);
    assert(first[0].index == 0 && first[0].kind == WatchAllObjectDiffKind::Added);
    assert(first[1].index == 1 && first[1].kind == WatchAllObjectDiffKind::Added);

    PutItems(reader, chunk, {Item(0x111, 1, 2, 3), Item(0x333, 4, 5, 7), Item(0x444, 8, 9, 10)});
    std::vector<WatchAllObjectDiff> second;
    assert(snapshot.Capture(Config(gu), reader, second));
    assert(second.size() == 2);
    assert(second[0].index == 1 && second[0].kind == WatchAllObjectDiffKind::Changed);
    assert(second[1].index == 2 && second[1].kind == WatchAllObjectDiffKind::Added);
}

static void test_failed_batch_does_not_overwrite_previous_slot() {
    FakeReader reader;
    WatchAllObjectArraySnapshot snapshot;
    const kaddr gu = 0x110000;
    const kaddr table = 0x220000;
    const kaddr chunk = 0x330000;
    kaddr chunks[] = {chunk};
    PutHeader(reader, gu, table, 2);
    reader.Put(table, chunks, sizeof(chunks));
    PutItems(reader, chunk, {Item(0xaaa, 1, 0, 1), Item(0xbbb, 1, 0, 2)});
    std::vector<WatchAllObjectDiff> diffs;
    assert(snapshot.Capture(Config(gu), reader, diffs));
    assert(diffs.size() == 2);

    reader.failByAddress[chunk] = 8;
    reader.failByAddress[chunk + sizeof(WatchAllObjectItem)] = 8;
    PutItems(reader, chunk, {Item(0, 0, 0, 0), Item(0xccc, 1, 0, 3)});
    diffs.clear();
    assert(snapshot.Capture(Config(gu), reader, diffs));
    assert(diffs.empty());

    reader.failByAddress.clear();
    diffs.clear();
    assert(snapshot.Capture(Config(gu), reader, diffs));
    assert(diffs.size() == 1);
    assert(diffs[0].index == 1 && diffs[0].kind == WatchAllObjectDiffKind::Changed);
}


static void test_chunk_pointer_change_survives_failed_rebuild_attempt() {
    FakeReader reader;
    WatchAllObjectArraySnapshot snapshot;
    const kaddr gu = 0x130000;
    const kaddr table = 0x240000;
    const kaddr chunkA = 0x350000;
    const kaddr chunkB = 0x460000;
    kaddr chunks[] = {chunkA};
    PutHeader(reader, gu, table, 1);
    reader.Put(table, chunks, sizeof(chunks));
    PutItems(reader, chunkA, {Item(0x999, 1, 0, 9)});

    std::vector<WatchAllObjectDiff> diffs;
    assert(snapshot.Capture(Config(gu), reader, diffs));
    assert(diffs.size() == 1 && diffs[0].kind == WatchAllObjectDiffKind::Added);

    chunks[0] = chunkB;
    reader.Put(table, chunks, sizeof(chunks));
    PutItems(reader, chunkB, {Item(0x999, 1, 0, 9)});
    reader.failByAddress[chunkB] = 1;
    diffs.clear();
    assert(snapshot.Capture(Config(gu), reader, diffs));
    assert(diffs.empty());

    diffs.clear();
    assert(snapshot.Capture(Config(gu), reader, diffs));
    assert(diffs.size() == 1);
    assert(diffs[0].index == 0 && diffs[0].kind == WatchAllObjectDiffKind::Changed);
}

static void test_last_chunk_reads_only_valid_elements() {
    FakeReader reader;
    WatchAllObjectArraySnapshot snapshot;
    const kaddr gu = 0x120000;
    const kaddr table = 0x230000;
    const kaddr chunk0 = 0x340000;
    const kaddr chunk1 = 0x450000;
    const uint32 count = 0x10000 + 2;
    kaddr chunks[] = {chunk0, chunk1};
    PutHeader(reader, gu, table, count);
    reader.Put(table, chunks, sizeof(chunks));
    reader.memory[chunk0] = std::vector<uint8_t>(0x10000 * sizeof(WatchAllObjectItem), 0);
    WatchAllObjectItem first = Item(0x101, 1, 0, 1);
    memcpy(reader.memory[chunk0].data(), &first, sizeof(first));
    PutItems(reader, chunk1, {Item(0x201, 1, 0, 1), Item(0x202, 1, 0, 2), Item(0xdead, 1, 0, 3)});

    std::vector<WatchAllObjectDiff> diffs;
    assert(snapshot.Capture(Config(gu), reader, diffs));
    assert(diffs.size() == 3);
    bool sawTrimmedRead = false;
    for (size_t size : reader.readSizes) {
        if (size == 2 * sizeof(WatchAllObjectItem)) {
            sawTrimmedRead = true;
        }
        assert(size != 3 * sizeof(WatchAllObjectItem));
    }
    assert(sawTrimmedRead);
}

int main() {
    test_detects_added_and_changed_slots();
    test_failed_batch_does_not_overwrite_previous_slot();
    test_chunk_pointer_change_survives_failed_rebuild_attempt();
    test_last_chunk_reads_only_valid_elements();
    std::cout << "watch-all task 2 offline checks passed" << std::endl;
    return 0;
}
