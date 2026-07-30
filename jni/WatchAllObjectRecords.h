#ifndef WATCH_ALL_OBJECT_RECORDS_H
#define WATCH_ALL_OBJECT_RECORDS_H

#include "WatchAllObjects.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <cstddef>
#include <string>
#include <vector>

static const size_t WATCH_ALL_OUTER_PATH_MAX_DEPTH = 64;

struct WatchAllObjectPath {
    std::string value;
    bool depthLimited;
    bool cycleDetected;
    bool readFailed;

    WatchAllObjectPath() : depthLimited(false), cycleDetected(false), readFailed(false) {}
};

struct WatchAllObjectRecord {
    uint32 index;
    kaddr objectPtr;
    kaddr classPtr;
    std::string objectName;
    std::string outerPath;
    std::string className;
    std::string classOuterPath;
    std::string classPath;
    std::string logicalKey;
};

static std::string WatchAllJoinPath(const std::string &outerPath, const std::string &name) {
    if (outerPath.empty()) {
        return name;
    }
    if (name.empty()) {
        return outerPath;
    }
    return outerPath + "." + name;
}

template<typename ObjectReader>
static WatchAllObjectPath WatchAllBuildOuterPath(ObjectReader &reader,
                                                 kaddr object,
                                                 size_t maxDepth = WATCH_ALL_OUTER_PATH_MAX_DEPTH) {
    WatchAllObjectPath result;
    std::vector<std::string> names;
    std::set<kaddr> visited;

    kaddr outer = 0;
    if (!reader.TryReadObjectOuter(object, outer)) {
        result.readFailed = true;
        return result;
    }

    size_t depth = 0;
    while (outer != 0) {
        if (visited.find(outer) != visited.end()) {
            result.cycleDetected = true;
            break;
        }
        visited.insert(outer);

        if (depth >= maxDepth) {
            result.depthLimited = true;
            break;
        }

        std::string name;
        if (!reader.TryReadObjectName(outer, name)) {
            result.readFailed = true;
            break;
        }
        names.push_back(name);
        ++depth;

        kaddr next = 0;
        if (!reader.TryReadObjectOuter(outer, next)) {
            result.readFailed = true;
            break;
        }
        outer = next;
    }

    std::reverse(names.begin(), names.end());
    for (size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            result.value += ".";
        }
        result.value += names[i];
    }
    return result;
}

static std::string WatchAllPathStateSuffix(const WatchAllObjectPath &path) {
    std::string suffix;
    if (path.depthLimited) {
        suffix += "#depth";
    }
    if (path.cycleDetected) {
        suffix += "#cycle";
    }
    if (path.readFailed) {
        suffix += "#readfail";
    }
    return suffix;
}

template<typename ObjectReader>
static bool WatchAllBuildObjectRecord(ObjectReader &reader,
                                      const WatchAllObjectDiff &diff,
                                      WatchAllObjectRecord &record,
                                      size_t maxDepth = WATCH_ALL_OUTER_PATH_MAX_DEPTH) {
    const kaddr object = static_cast<kaddr>(diff.current.object);
    if (object == 0) {
        return false;
    }

    std::string objectName;
    kaddr classPtr = 0;
    if (!reader.TryReadObjectName(object, objectName) ||
        !reader.TryReadObjectClass(object, classPtr) ||
        classPtr == 0) {
        return false;
    }

    std::string className;
    if (!reader.TryReadObjectName(classPtr, className)) {
        return false;
    }

    WatchAllObjectPath outerPath = WatchAllBuildOuterPath(reader, object, maxDepth);
    WatchAllObjectPath classOuterPath = WatchAllBuildOuterPath(reader, classPtr, maxDepth);
    const std::string classPath = WatchAllJoinPath(classOuterPath.value, className);

    record.index = diff.index;
    record.objectPtr = object;
    record.classPtr = classPtr;
    record.objectName = objectName;
    record.outerPath = outerPath.value;
    record.className = className;
    record.classOuterPath = classOuterPath.value;
    record.classPath = classPath;

    std::ostringstream key;
    key << outerPath.value << WatchAllPathStateSuffix(outerPath)
        << "|" << objectName
        << "|" << classPath << WatchAllPathStateSuffix(classOuterPath);
    record.logicalKey = key.str();
    return true;
}


static std::string WatchAllFormatObjectRecord(const WatchAllObjectRecord &record) {
    std::ostringstream out;
    out << std::hex;
    out << "[0x" << record.index << "]:" << std::endl;
    out << "Name: " << record.objectName << std::endl;
    out << "Class: " << record.className << std::endl;
    out << "ObjectPtr: 0x" << record.objectPtr << std::endl;
    out << "ClassPtr: 0x" << record.classPtr << std::endl;
    out << std::endl;
    return out.str();
}

class WatchAllObjectRecordStore {
public:
    template<typename ObjectReader>
    const WatchAllObjectRecord *AddDiff(ObjectReader &reader, const WatchAllObjectDiff &diff) {
        WatchAllObjectRecord record;
        if (!WatchAllBuildObjectRecord(reader, diff, record)) {
            return nullptr;
        }
        if (seenKeys_.find(record.logicalKey) != seenKeys_.end()) {
            return nullptr;
        }
        seenKeys_.insert(record.logicalKey);
        records_.push_back(record);
        const size_t index = records_.size() - 1;
        return &records_[index];
    }

    const std::vector<WatchAllObjectRecord> &Records() const { return records_; }

private:
    std::set<std::string> seenKeys_;
    std::vector<WatchAllObjectRecord> records_;
};

#endif
