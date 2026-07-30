#ifndef WATCH_ALL_STRINGS_H
#define WATCH_ALL_STRINGS_H

#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef MEMORY_H
typedef unsigned int uint32;
#endif

struct WatchAllStringEntry {
    uint32 index;
    std::string value;
};

class WatchAllStringTable {
public:
    void Merge(uint32 index, const std::string &value) {
        strings_[index] = value;
    }

    size_t Size() const { return strings_.size(); }

    std::vector<WatchAllStringEntry> SortedEntries() const {
        std::vector<WatchAllStringEntry> entries;
        entries.reserve(strings_.size());
        for (const auto &item : strings_) {
            WatchAllStringEntry entry;
            entry.index = item.first;
            entry.value = item.second;
            entries.push_back(entry);
        }
        return entries;
    }

private:
    std::map<uint32, std::string> strings_;
};

static inline std::string WatchAllRenderStrings(const WatchAllStringTable &strings) {
    std::ostringstream out;
    for (const auto &entry : strings.SortedEntries()) {
        out << "[" << entry.index << "] " << entry.value << "\n";
    }
    return out.str();
}

template<typename NameReader>
static size_t WatchAllMergeStringRange(WatchAllStringTable &strings,
                                       NameReader &reader,
                                       uint32 begin,
                                       uint32 end) {
    size_t merged = 0;
    for (uint32 index = begin; index < end; ++index) {
        std::string value;
        if (reader.TryReadName(index, value) && !value.empty() && value != "None") {
            strings.Merge(index, value);
            ++merged;
        }
    }
    return merged;
}

#endif
