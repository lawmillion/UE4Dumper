#include "WatchAllObjectRecords.h"

#include <cassert>
#include <iostream>
#include <map>
#include <string>

struct FakeObject {
    std::string name;
    kaddr outer;
    kaddr clazz;
};

class FakeObjectReader {
public:
    std::map<kaddr, FakeObject> objects;

    bool TryReadObjectName(kaddr object, std::string &out) {
        auto it = objects.find(object);
        if (it == objects.end()) {
            return false;
        }
        out = it->second.name;
        return true;
    }

    bool TryReadObjectOuter(kaddr object, kaddr &out) {
        auto it = objects.find(object);
        if (it == objects.end()) {
            return false;
        }
        out = it->second.outer;
        return true;
    }

    bool TryReadObjectClass(kaddr object, kaddr &out) {
        auto it = objects.find(object);
        if (it == objects.end()) {
            return false;
        }
        out = it->second.clazz;
        return true;
    }
};

static WatchAllObjectDiff Diff(uint32 index, kaddr object) {
    WatchAllObjectDiff diff{};
    diff.index = index;
    diff.kind = WatchAllObjectDiffKind::Added;
    diff.current.object = object;
    return diff;
}

static void test_outer_path_has_depth_limit() {
    FakeObjectReader reader;
    reader.objects[0x1000] = {"Obj", 0x2000, 0x9000};
    reader.objects[0x2000] = {"Outer1", 0x3000, 0x9000};
    reader.objects[0x3000] = {"Outer2", 0x4000, 0x9000};
    reader.objects[0x4000] = {"Outer3", 0, 0x9000};

    WatchAllObjectPath path = WatchAllBuildOuterPath(reader, 0x1000, 2);

    assert(path.value == "Outer2.Outer1");
    assert(path.depthLimited);
    assert(!path.cycleDetected);
}

static void test_outer_path_detects_cycles() {
    FakeObjectReader reader;
    reader.objects[0x1000] = {"Obj", 0x2000, 0x9000};
    reader.objects[0x2000] = {"Outer1", 0x3000, 0x9000};
    reader.objects[0x3000] = {"Outer2", 0x2000, 0x9000};

    WatchAllObjectPath path = WatchAllBuildOuterPath(reader, 0x1000, 8);

    assert(path.value == "Outer2.Outer1");
    assert(path.cycleDetected);
}

static void test_logical_dedupe_ignores_pointer_and_index() {
    FakeObjectReader reader;
    reader.objects[0x1000] = {"Package", 0, 0x9000};
    reader.objects[0x2000] = {"Actor", 0x1000, 0x8000};
    reader.objects[0x3000] = {"Actor", 0x1000, 0x8000};
    reader.objects[0x8000] = {"BlueprintGeneratedClass", 0x4000, 0x9000};
    reader.objects[0x4000] = {"Engine", 0, 0x9000};
    reader.objects[0x9000] = {"Class", 0, 0x9000};

    WatchAllObjectRecordStore store;
    assert(store.AddDiff(reader, Diff(7, 0x2000)));
    assert(!store.AddDiff(reader, Diff(99, 0x3000)));
    assert(store.Records().size() == 1);
    assert(store.Records()[0].index == 7);
    assert(store.Records()[0].objectPtr == 0x2000);
}

static void test_formats_object_record_like_legacy_objects_txt() {
    WatchAllObjectRecord record{};
    record.index = 0x2a;
    record.objectPtr = 0x1234;
    record.classPtr = 0x5678;
    record.objectName = "Actor";
    record.className = "Class";

    const std::string text = WatchAllFormatObjectRecord(record);

    assert(text == "[0x2a]:\nName: Actor\nClass: Class\nObjectPtr: 0x1234\nClassPtr: 0x5678\n\n");
}

static void test_same_name_different_outer_does_not_merge() {
    FakeObjectReader reader;
    reader.objects[0x1000] = {"PackageA", 0, 0x9000};
    reader.objects[0x1100] = {"PackageB", 0, 0x9000};
    reader.objects[0x2000] = {"Actor", 0x1000, 0x8000};
    reader.objects[0x3000] = {"Actor", 0x1100, 0x8000};
    reader.objects[0x8000] = {"BlueprintGeneratedClass", 0x4000, 0x9000};
    reader.objects[0x4000] = {"Engine", 0, 0x9000};
    reader.objects[0x9000] = {"Class", 0, 0x9000};

    WatchAllObjectRecordStore store;
    assert(store.AddDiff(reader, Diff(1, 0x2000)));
    assert(store.AddDiff(reader, Diff(2, 0x3000)));
    assert(store.Records().size() == 2);
}

int main() {
    test_outer_path_has_depth_limit();
    test_outer_path_detects_cycles();
    test_logical_dedupe_ignores_pointer_and_index();
    test_same_name_different_outer_does_not_merge();
    test_formats_object_record_like_legacy_objects_txt();
    std::cout << "watch-all task 3 offline checks passed" << std::endl;
    return 0;
}
