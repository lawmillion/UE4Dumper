#ifndef WATCH_ALL_SDK_MODEL_H
#define WATCH_ALL_SDK_MODEL_H

#include "WatchAllObjectRecords.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct WatchAllSDKClassKey {
    std::string outerPath;
    std::string className;

    std::string Signature() const {
        return outerPath + "|" + className;
    }

    bool operator<(const WatchAllSDKClassKey &other) const {
        if (outerPath != other.outerPath) {
            return outerPath < other.outerPath;
        }
        return className < other.className;
    }
};

struct WatchAllSDKField {
    std::string name;
    std::string type;
    uint32_t nativeOffset;
    uint32_t size;
    uint32_t arrayDim;
    uint64_t propertyFlags;
    uint8_t byteOffset;
    uint8_t byteMask;
    uint8_t fieldMask;

    WatchAllSDKField()
            : nativeOffset(0), size(0), arrayDim(0), propertyFlags(0),
              byteOffset(0), byteMask(0), fieldMask(0) {}

    std::string Signature() const {
        std::ostringstream out;
        out << name << "|" << type
            << "|off=" << nativeOffset
            << "|size=" << size
            << "|dim=" << arrayDim
            << "|flags=" << propertyFlags
            << "|byteOff=" << static_cast<unsigned int>(byteOffset)
            << "|byteMask=" << static_cast<unsigned int>(byteMask)
            << "|fieldMask=" << static_cast<unsigned int>(fieldMask);
        return out.str();
    }
};

struct WatchAllSDKFunction {
    std::string name;
    std::string returnType;
    std::string parameterSignature;
    uint64_t rva;
    uint32_t functionFlags;

    WatchAllSDKFunction() : rva(0), functionFlags(0) {}

    std::string Signature() const {
        std::ostringstream out;
        out << name << "|" << returnType << "|" << parameterSignature
            << "|rva=" << rva
            << "|flags=" << functionFlags;
        return out.str();
    }
};

struct WatchAllSDKClass {
    WatchAllSDKClassKey key;
    kaddr classPtr;
    std::vector<kaddr> classPointers;
    std::vector<WatchAllSDKField> fields;
    std::vector<WatchAllSDKFunction> functions;

    WatchAllSDKClass() : classPtr(0) {}
};

class WatchAllSDKModel {
public:
    bool MergeClass(const WatchAllSDKClass &incoming) {
        const std::string classKey = incoming.key.Signature();
        WatchAllSDKClass *target = FindClass(classKey);
        if (target == nullptr) {
            WatchAllSDKClass empty;
            empty.key = incoming.key;
            empty.classPtr = incoming.classPtr;
            classes_.push_back(empty);
            fieldSignatures_.push_back(std::set<std::string>());
            functionSignatures_.push_back(std::set<std::string>());
            target = &classes_.back();
        }

        const size_t index = static_cast<size_t>(target - classes_.data());
        bool changed = AddClassPointer(*target, incoming.classPtr);
        for (const auto &field : incoming.fields) {
            const std::string signature = field.Signature();
            if (fieldSignatures_[index].insert(signature).second) {
                target->fields.push_back(field);
                changed = true;
            }
        }
        for (const auto &function : incoming.functions) {
            const std::string signature = function.Signature();
            if (functionSignatures_[index].insert(signature).second) {
                target->functions.push_back(function);
                changed = true;
            }
        }
        return changed;
    }

    const std::vector<WatchAllSDKClass> &Classes() const { return classes_; }

private:
    WatchAllSDKClass *FindClass(const std::string &signature) {
        for (auto &clazz : classes_) {
            if (clazz.key.Signature() == signature) {
                return &clazz;
            }
        }
        return nullptr;
    }

    static bool AddClassPointer(WatchAllSDKClass &clazz, kaddr classPtr) {
        if (classPtr == 0) {
            return false;
        }
        if (std::find(clazz.classPointers.begin(), clazz.classPointers.end(), classPtr) != clazz.classPointers.end()) {
            return false;
        }
        clazz.classPointers.push_back(classPtr);
        return true;
    }

    std::vector<WatchAllSDKClass> classes_;
    std::vector<std::set<std::string>> fieldSignatures_;
    std::vector<std::set<std::string>> functionSignatures_;
};

struct WatchAllSDKClassTask {
    kaddr classPtr;
    WatchAllSDKClassKey classKey;

    WatchAllSDKClassTask() : classPtr(0) {}
};

class WatchAllSDKWorkerQueue {
public:
    WatchAllSDKWorkerQueue() : stopped_(false) {}

    bool Submit(const WatchAllObjectRecord &record) {
        WatchAllSDKClassTask task;
        task.classPtr = record.classPtr;
        task.classKey.outerPath = record.classOuterPath;
        task.classKey.className = record.className;
        return Submit(task);
    }

    bool Submit(const WatchAllSDKClassTask &task) {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(task);
        condition_.notify_one();
        return true;
    }

    bool TryPop(WatchAllSDKClassTask &out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tasks_.empty()) {
            return false;
        }
        PopFront(out);
        return true;
    }

    bool WaitPop(WatchAllSDKClassTask &out) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() { return stopped_ || !tasks_.empty(); });
        if (tasks_.empty()) {
            return false;
        }
        PopFront(out);
        return true;
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        condition_.notify_all();
    }

    size_t PendingCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

private:
    void PopFront(WatchAllSDKClassTask &out) {
        out = tasks_.front();
        tasks_.pop_front();
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<WatchAllSDKClassTask> tasks_;
    bool stopped_;
};

class WatchAllSDKWorker {
public:
    typedef std::function<WatchAllSDKClass(const WatchAllSDKClassTask &)> Parser;

    WatchAllSDKWorker() : parser_(DefaultParseClass), running_(false) {}
    explicit WatchAllSDKWorker(Parser parser) : parser_(parser), running_(false) {}
    ~WatchAllSDKWorker() { Stop(); }

    void Start() {
        if (running_) {
            return;
        }
        running_ = true;
        worker_ = std::thread(&WatchAllSDKWorker::Run, this);
    }

    void Stop() {
        queue_.Stop();
        if (worker_.joinable()) {
            worker_.join();
        }
        running_ = false;
    }

    bool Submit(const WatchAllObjectRecord &record) {
        return queue_.Submit(record);
    }

    size_t PendingCount() const {
        return queue_.PendingCount();
    }

    size_t ClassCount() const {
        std::lock_guard<std::mutex> lock(modelMutex_);
        return model_.Classes().size();
    }

    WatchAllSDKModel ModelSnapshot() const {
        std::lock_guard<std::mutex> lock(modelMutex_);
        return model_;
    }

private:
    static WatchAllSDKClass DefaultParseClass(const WatchAllSDKClassTask &task) {
        WatchAllSDKClass clazz;
        clazz.key = task.classKey;
        clazz.classPtr = task.classPtr;
        return clazz;
    }

    void Run() {
        WatchAllSDKClassTask task;
        while (queue_.WaitPop(task)) {
            WatchAllSDKClass clazz = parser_(task);
            std::lock_guard<std::mutex> lock(modelMutex_);
            model_.MergeClass(clazz);
        }
    }

    WatchAllSDKWorkerQueue queue_;
    WatchAllSDKModel model_;
    mutable std::mutex modelMutex_;
    Parser parser_;
    std::thread worker_;
    bool running_;
};

#endif
