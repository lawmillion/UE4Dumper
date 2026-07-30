#include "WatchAllOutput.h"
#include "WatchAllStrings.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static std::string ReadFile(const std::string &path) {
    std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

static void WriteFile(const std::string &path, const std::string &content) {
    std::ofstream out(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    out << content;
}

static std::string TempDir(const std::string &name) {
    std::string base = std::string("/tmp/ue4dumper_task5_") + name + "_XXXXXX";
    std::vector<char> buffer(base.begin(), base.end());
    buffer.push_back('\0');
    char *created = mkdtemp(buffer.data());
    assert(created != nullptr);
    return std::string(created);
}

static void test_strings_merge_by_index_and_sort_stable() {
    WatchAllStringTable strings;
    strings.Merge(5, "Five");
    strings.Merge(2, "Two");
    strings.Merge(5, "FiveUpdated");
    strings.Merge(7, "Seven");

    const std::vector<WatchAllStringEntry> entries = strings.SortedEntries();
    assert(entries.size() == 3);
    assert(entries[0].index == 2 && entries[0].value == "Two");
    assert(entries[1].index == 5 && entries[1].value == "FiveUpdated");
    assert(entries[2].index == 7 && entries[2].value == "Seven");

    const std::string text = WatchAllRenderStrings(strings);
    assert(text.find("[2] Two\n") < text.find("[5] FiveUpdated\n"));
    assert(text.find("[5] FiveUpdated\n") < text.find("[7] Seven\n"));
}

static void test_interval_sleep_never_negative() {
    using namespace std::chrono;
    assert(WatchAllComputeSleep(milliseconds(1000), milliseconds(250)) == milliseconds(750));
    assert(WatchAllComputeSleep(milliseconds(1000), milliseconds(1000)) == milliseconds(0));
    assert(WatchAllComputeSleep(milliseconds(1000), milliseconds(1500)) == milliseconds(0));
    assert(WatchAllComputeSleep(milliseconds(0), milliseconds(1500)) == milliseconds(0));
}

static void test_atomic_publish_requires_all_tmp_success() {
    const std::string dir = TempDir("atomic_success");
    WriteFile(dir + "/Strings.txt", "old strings");
    WriteFile(dir + "/Objects.txt", "old objects");
    WriteFile(dir + "/SDK.txt", "old sdk");
    WriteFile(dir + "/SDK_index.json", "old index");

    WatchAllOutputBundle bundle;
    bundle.strings = "new strings";
    bundle.objects = "new objects";
    bundle.sdk = "new sdk";
    bundle.sdkIndexJson = "new index";

    WatchAllOutputResult result = WatchAllWriteFinalOutputs(dir, bundle);
    assert(result.ok);
    assert(ReadFile(dir + "/Strings.txt") == "new strings");
    assert(ReadFile(dir + "/Objects.txt") == "new objects");
    assert(ReadFile(dir + "/SDK.txt") == "new sdk");
    assert(ReadFile(dir + "/SDK_index.json") == "new index");
}

static void test_atomic_publish_failure_preserves_old_files() {
    const std::string dir = TempDir("atomic_failure");
    WriteFile(dir + "/Strings.txt", "old strings");
    WriteFile(dir + "/Objects.txt", "old objects");
    WriteFile(dir + "/SDK.txt", "old sdk");
    WriteFile(dir + "/SDK_index.json", "old index");

    WatchAllOutputBundle bundle;
    bundle.strings = "new strings";
    bundle.objects = "new objects";
    bundle.sdk = "new sdk";
    bundle.sdkIndexJson = "new index";

    WatchAllOutputResult result = WatchAllWriteFinalOutputs(dir + "/missing", bundle);
    assert(!result.ok);
    assert(ReadFile(dir + "/Strings.txt") == "old strings");
    assert(ReadFile(dir + "/Objects.txt") == "old objects");
    assert(ReadFile(dir + "/SDK.txt") == "old sdk");
    assert(ReadFile(dir + "/SDK_index.json") == "old index");
}

static void test_pid_abort_skips_output() {
    const std::string dir = TempDir("pid_abort");
    WriteFile(dir + "/Strings.txt", "old strings");
    WriteFile(dir + "/Objects.txt", "old objects");
    WriteFile(dir + "/SDK.txt", "old sdk");
    WriteFile(dir + "/SDK_index.json", "old index");

    WatchAllOutputBundle bundle;
    bundle.strings = "new strings";
    bundle.objects = "new objects";
    bundle.sdk = "new sdk";
    bundle.sdkIndexJson = "new index";

    WatchAllOutputResult result = WatchAllFinalizeOutputs(dir, bundle, false);
    assert(!result.ok);
    assert(result.aborted);
    assert(ReadFile(dir + "/Strings.txt") == "old strings");
    assert(ReadFile(dir + "/Objects.txt") == "old objects");
    assert(ReadFile(dir + "/SDK.txt") == "old sdk");
    assert(ReadFile(dir + "/SDK_index.json") == "old index");
}

int main() {
    test_strings_merge_by_index_and_sort_stable();
    test_interval_sleep_never_negative();
    test_atomic_publish_requires_all_tmp_success();
    test_atomic_publish_failure_preserves_old_files();
    test_pid_abort_skips_output();
    std::cout << "watch-all task 5 offline checks passed" << std::endl;
    return 0;
}
