#ifndef WATCH_ALL_OUTPUT_H
#define WATCH_ALL_OUTPUT_H

#include "WatchAllObjectRecords.h"
#include "WatchAllSDKModel.h"
#include "WatchAllStrings.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

struct WatchAllOutputBundle {
    std::string strings;
    std::string objects;
    std::string sdk;
    std::string sdkIndexJson;
};

struct WatchAllOutputResult {
    bool ok;
    bool aborted;
    std::string error;

    WatchAllOutputResult() : ok(false), aborted(false) {}
};

static inline std::chrono::milliseconds WatchAllComputeSleep(std::chrono::milliseconds interval,
                                                             std::chrono::milliseconds scanTime) {
    if (scanTime >= interval) {
        return std::chrono::milliseconds(0);
    }
    return interval - scanTime;
}

static inline bool WatchAllPathIsDirectory(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static inline std::string WatchAllJoinOutputPath(const std::string &dir, const std::string &name) {
    if (dir.empty() || dir == ".") {
        return std::string("./") + name;
    }
    if (dir[dir.size() - 1] == '/') {
        return dir + name;
    }
    return dir + "/" + name;
}

static inline bool WatchAllWriteTmpFile(const std::string &path,
                                        const std::string &content,
                                        std::string &error) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error = std::string("open ") + path + ": " + strerror(errno);
        return false;
    }

    const char *data = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        const ssize_t written = write(fd, data, remaining);
        if (written < 0) {
            error = std::string("write ") + path + ": " + strerror(errno);
            close(fd);
            return false;
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }

    if (fsync(fd) != 0) {
        error = std::string("fsync ") + path + ": " + strerror(errno);
        close(fd);
        return false;
    }
    if (close(fd) != 0) {
        error = std::string("close ") + path + ": " + strerror(errno);
        return false;
    }
    return true;
}

static inline bool WatchAllRenameFile(const std::string &tmpPath,
                                      const std::string &finalPath,
                                      std::string &error) {
    if (rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
        error = std::string("rename ") + tmpPath + " -> " + finalPath + ": " + strerror(errno);
        return false;
    }
    return true;
}

static inline void WatchAllCleanupTmps(const std::vector<std::string> &paths) {
    for (const auto &path : paths) {
        remove(path.c_str());
    }
}

template<typename AliveChecker>
static inline WatchAllOutputResult WatchAllWriteFinalOutputs(const std::string &outputDir,
                                                             const WatchAllOutputBundle &bundle,
                                                             AliveChecker isAlive);

static inline WatchAllOutputResult WatchAllWriteFinalOutputs(const std::string &outputDir,
                                                             const WatchAllOutputBundle &bundle) {
    return WatchAllWriteFinalOutputs(outputDir, bundle, []() { return true; });
}

template<typename AliveChecker>
static inline WatchAllOutputResult WatchAllWriteFinalOutputs(const std::string &outputDir,
                                                             const WatchAllOutputBundle &bundle,
                                                             AliveChecker isAlive) {
    WatchAllOutputResult result;
    if (!WatchAllPathIsDirectory(outputDir)) {
        result.error = std::string("output directory not found: ") + outputDir;
        return result;
    }

    struct OutputFile {
        const char *name;
        const std::string *content;
    } files[] = {
            {"Strings.txt", &bundle.strings},
            {"Objects.txt", &bundle.objects},
            {"SDK.txt", &bundle.sdk},
            {"SDK_index.json", &bundle.sdkIndexJson},
    };

    std::vector<std::string> tmpPaths;
    tmpPaths.reserve(4);
    for (const auto &file : files) {
        const std::string tmpPath = WatchAllJoinOutputPath(outputDir, std::string(file.name) + ".watch-all.tmp");
        tmpPaths.push_back(tmpPath);
        if (!WatchAllWriteTmpFile(tmpPath, *file.content, result.error)) {
            WatchAllCleanupTmps(tmpPaths);
            return result;
        }
    }

    std::vector<std::string> backupPaths;
    std::vector<std::string> finalPaths;
    backupPaths.reserve(4);
    finalPaths.reserve(4);
    for (const auto &file : files) {
        finalPaths.push_back(WatchAllJoinOutputPath(outputDir, file.name));
        backupPaths.push_back(WatchAllJoinOutputPath(outputDir, std::string(file.name) + ".watch-all.bak"));
    }

    for (const auto &backupPath : backupPaths) {
        remove(backupPath.c_str());
    }

    for (size_t i = 0; i < finalPaths.size(); ++i) {
        if (!isAlive()) {
            result.aborted = true;
            result.error = "pid disappeared during output publish";
            WatchAllCleanupTmps(tmpPaths);
            for (size_t r = 0; r < i; ++r) {
                rename(backupPaths[r].c_str(), finalPaths[r].c_str());
            }
            return result;
        }
        if (rename(finalPaths[i].c_str(), backupPaths[i].c_str()) != 0 && errno != ENOENT) {
            result.error = std::string("backup ") + finalPaths[i] + " -> " + backupPaths[i] + ": " + strerror(errno);
            WatchAllCleanupTmps(tmpPaths);
            for (size_t r = 0; r < i; ++r) {
                rename(backupPaths[r].c_str(), finalPaths[r].c_str());
            }
            return result;
        }
    }

    size_t renamed = 0;
    for (; renamed < 4; ++renamed) {
        if (!isAlive()) {
            result.aborted = true;
            result.error = "pid disappeared during output publish";
            break;
        }
        const std::string tmpPath = WatchAllJoinOutputPath(outputDir, std::string(files[renamed].name) + ".watch-all.tmp");
        if (!WatchAllRenameFile(tmpPath, finalPaths[renamed], result.error)) {
            break;
        }
    }

    if (renamed != 4) {
        for (size_t i = 0; i < renamed; ++i) {
            remove(finalPaths[i].c_str());
        }
        for (size_t i = 0; i < finalPaths.size(); ++i) {
            rename(backupPaths[i].c_str(), finalPaths[i].c_str());
        }
        WatchAllCleanupTmps(tmpPaths);
        return result;
    }

    for (const auto &backupPath : backupPaths) {
        remove(backupPath.c_str());
    }

    const int dirFd = open(outputDir.c_str(), O_RDONLY);
    if (dirFd >= 0) {
        fsync(dirFd);
        close(dirFd);
    }
    result.ok = true;
    return result;
}

template<typename AliveChecker>
static inline WatchAllOutputResult WatchAllFinalizeOutputs(const std::string &outputDir,
                                                           const WatchAllOutputBundle &bundle,
                                                           AliveChecker isAlive) {
    WatchAllOutputResult result;
    if (!isAlive()) {
        result.aborted = true;
        result.error = "pid disappeared; output skipped";
        return result;
    }
    return WatchAllWriteFinalOutputs(outputDir, bundle, isAlive);
}

static inline WatchAllOutputResult WatchAllFinalizeOutputs(const std::string &outputDir,
                                                           const WatchAllOutputBundle &bundle,
                                                           bool pidAlive) {
    return WatchAllFinalizeOutputs(outputDir, bundle, [pidAlive]() { return pidAlive; });
}

static inline std::string WatchAllJsonEscape(const std::string &value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(ch);
                } else {
                    out << ch;
                }
                break;
        }
    }
    return out.str();
}

static inline std::vector<WatchAllObjectRecord> WatchAllSortedObjectRecords(const std::vector<WatchAllObjectRecord> &records) {
    std::vector<WatchAllObjectRecord> sorted = records;
    std::sort(sorted.begin(), sorted.end(), [](const WatchAllObjectRecord &lhs, const WatchAllObjectRecord &rhs) {
        if (lhs.index != rhs.index) return lhs.index < rhs.index;
        if (lhs.outerPath != rhs.outerPath) return lhs.outerPath < rhs.outerPath;
        if (lhs.objectName != rhs.objectName) return lhs.objectName < rhs.objectName;
        return lhs.classPath < rhs.classPath;
    });
    return sorted;
}

static inline std::string WatchAllRenderObjects(const std::vector<WatchAllObjectRecord> &records) {
    std::ostringstream out;
    for (const auto &record : WatchAllSortedObjectRecords(records)) {
        out << WatchAllFormatObjectRecord(record);
    }
    return out.str();
}

static inline std::vector<WatchAllSDKClass> WatchAllSortedSDKClasses(const WatchAllSDKModel &model) {
    std::vector<WatchAllSDKClass> sorted = model.Classes();
    std::sort(sorted.begin(), sorted.end(), [](const WatchAllSDKClass &lhs, const WatchAllSDKClass &rhs) {
        if (lhs.key.outerPath != rhs.key.outerPath) return lhs.key.outerPath < rhs.key.outerPath;
        return lhs.key.className < rhs.key.className;
    });
    for (auto &clazz : sorted) {
        std::sort(clazz.classPointers.begin(), clazz.classPointers.end());
        std::sort(clazz.fields.begin(), clazz.fields.end(), [](const WatchAllSDKField &lhs, const WatchAllSDKField &rhs) {
            return lhs.Signature() < rhs.Signature();
        });
        std::sort(clazz.functions.begin(), clazz.functions.end(), [](const WatchAllSDKFunction &lhs, const WatchAllSDKFunction &rhs) {
            return lhs.Signature() < rhs.Signature();
        });
    }
    return sorted;
}

static inline std::string WatchAllRenderSDK(const WatchAllSDKModel &model) {
    std::ostringstream out;
    for (const auto &clazz : WatchAllSortedSDKClasses(model)) {
        out << "Class: " << WatchAllJoinPath(clazz.key.outerPath, clazz.key.className) << "\n";
        for (const auto &field : clazz.fields) {
            out << "\t" << field.type << " " << field.name
                << ";//[Offset: 0x" << std::hex << field.nativeOffset
                << ", Size: 0x" << field.size << std::dec;
            if (field.byteMask != 0 || field.fieldMask != 0 || field.byteOffset != 0) {
                out << ", ByteOffset: " << static_cast<unsigned int>(field.byteOffset)
                    << ", ByteMask: " << static_cast<unsigned int>(field.byteMask)
                    << ", FieldMask: " << static_cast<unsigned int>(field.fieldMask);
            }
            out << "]\n";
        }
        for (const auto &function : clazz.functions) {
            out << "\t" << function.returnType << " " << function.name
                << "(" << function.parameterSignature << ");"
                << "// 0x" << std::hex << function.rva << std::dec << "\n";
        }
        out << "\n--------------------------------\n";
    }
    return out.str();
}

static inline std::string WatchAllRenderSDKIndexJson(const WatchAllSDKModel &model) {
    std::ostringstream out;
    const std::vector<WatchAllSDKClass> classes = WatchAllSortedSDKClasses(model);
    out << "{\n  \"classes\": [\n";
    for (size_t i = 0; i < classes.size(); ++i) {
        const auto &clazz = classes[i];
        out << "    {\n";
        out << "      \"outer_path\": \"" << WatchAllJsonEscape(clazz.key.outerPath) << "\",\n";
        out << "      \"class_name\": \"" << WatchAllJsonEscape(clazz.key.className) << "\",\n";
        out << "      \"class_path\": \"" << WatchAllJsonEscape(WatchAllJoinPath(clazz.key.outerPath, clazz.key.className)) << "\",\n";
        out << "      \"class_ptrs\": [";
        for (size_t p = 0; p < clazz.classPointers.size(); ++p) {
            if (p != 0) out << ", ";
            out << "\"0x" << std::hex << clazz.classPointers[p] << std::dec << "\"";
        }
        out << "],\n";
        out << "      \"fields\": " << clazz.fields.size() << ",\n";
        out << "      \"functions\": " << clazz.functions.size() << "\n";
        out << "    }" << (i + 1 == classes.size() ? "" : ",") << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

static inline WatchAllOutputBundle WatchAllBuildOutputBundle(const WatchAllStringTable &strings,
                                                             const std::vector<WatchAllObjectRecord> &objects,
                                                             const WatchAllSDKModel &sdkModel) {
    WatchAllOutputBundle bundle;
    bundle.strings = WatchAllRenderStrings(strings);
    bundle.objects = WatchAllRenderObjects(objects);
    bundle.sdk = WatchAllRenderSDK(sdkModel);
    bundle.sdkIndexJson = WatchAllRenderSDKIndexJson(sdkModel);
    return bundle;
}

#endif
