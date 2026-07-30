#ifndef KMODS_H
#define KMODS_H

#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <dirent.h>
#include <unistd.h>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <string>
#include <list>
#include <vector>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <getopt.h>
#include <codecvt>
#include <cerrno>
#include <cstring>

#include "Log.h"
#include "Process.h"
#include "Mem.h"

#if defined(__LP64__)

#include "ELF64/fix.h"

#else
#include "ELF/ElfReader.h"
#include "ELF/ElfRebuilder.h"
#endif

bool isUE423 = false;
bool isPUBGNS = false;
bool isPUBGCN = false;
bool isPGLite = false;
bool isPtrDec = false;
bool isVerbose = false;
bool deRefGNames = true;
bool deRefGUObjectArray = false;
string pkg("com.tencent.ig");
static const char *lib_name = "libUE4.so";

bool PidAlive(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    char procPath[32];
    snprintf(procPath, sizeof(procPath), "/proc/%d", pid);
    return access(procPath, F_OK) == 0;
}

struct ModuleMapSegment {
    kaddr start;
    kaddr end;
    kaddr fileOffset;
    string perms;
};

vector<ModuleMapSegment> get_module_segments(const char *module_name) {
    vector<ModuleMapSegment> segments;
    FILE *fp;
    char filename[32], buffer[1024];
    snprintf(filename, sizeof(filename), "/proc/%d/maps", target_pid);
    fp = fopen(filename, "rt");
    if (fp == nullptr) {
        return segments;
    }

    while (fgets(buffer, sizeof(buffer), fp)) {
        if (!strstr(buffer, module_name)) {
            continue;
        }

        ModuleMapSegment seg{};
        char perms[8] = {0};
#if defined(__LP64__)
        if (sscanf(buffer, "%lx-%lx %7s %lx", &seg.start, &seg.end, perms, &seg.fileOffset) != 4) {
#else
        if (sscanf(buffer, "%x-%x %7s %x", &seg.start, &seg.end, perms, &seg.fileOffset) != 4) {
#endif
            continue;
        }
        seg.perms = perms;
        segments.push_back(seg);
    }
    fclose(fp);
    sort(segments.begin(), segments.end(), [](const ModuleMapSegment &a, const ModuleMapSegment &b) {
        return a.start < b.start;
    });
    return segments;
}

bool dump_module_by_maps(const char *module_name, const string &outPath, kaddr &baseOut, kaddr &endOut) {
    auto segments = get_module_segments(module_name);
    if (segments.empty()) {
        cout << "Can't find maps for " << module_name << endl;
        return false;
    }

    baseOut = segments.front().start;
    endOut = segments.back().end;
    size_t imageSize = endOut - baseOut;
    cout << "Module maps segments: " << segments.size() << endl;
    cout << "Module span: " << setbase(16) << baseOut << "-" << endOut << setbase(10)
         << " Size: " << imageSize << endl;

    vector<uint8_t> image(imageSize, 0);
    size_t totalRead = 0;
    int failedSegments = 0;

    for (const auto &seg : segments) {
        size_t segSize = seg.end - seg.start;
        size_t dstOff = seg.start - baseOut;
        errno = 0;
        ssize_t bytes = pvm_partial((void *) seg.start, image.data() + dstOff, segSize, false);
        if (bytes < 0) {
            failedSegments++;
            cout << "Read failed " << setbase(16) << seg.start << "-" << seg.end
                 << " off=" << seg.fileOffset << setbase(10)
                 << " perms=" << seg.perms << " errno=" << errno << " (" << strerror(errno) << ")" << endl;
            continue;
        }
        totalRead += (size_t) bytes;
        if ((size_t) bytes != segSize) {
            failedSegments++;
            cout << "Partial read " << setbase(16) << seg.start << "-" << seg.end
                 << " off=" << seg.fileOffset << setbase(10)
                 << " perms=" << seg.perms << " got=" << bytes << " need=" << segSize << endl;
        } else if (isVerbose) {
            cout << "Read segment " << setbase(16) << seg.start << "-" << seg.end
                 << " off=" << seg.fileOffset << setbase(10)
                 << " perms=" << seg.perms << " bytes=" << bytes << endl;
        }
    }

    ofstream out(outPath, ofstream::out | ofstream::binary);
    if (!out.is_open()) {
        cout << "Can't Output File" << endl;
        return false;
    }
    out.write((char *) image.data(), image.size());
    out.close();

    cout << "Dumped bytes: " << totalRead << " / " << imageSize
         << ", failed/partial segments: " << failedSegments << endl;
    return failedSegments == 0;
}

bool isStartWith(const string& str, const char *check) {
    return (str.rfind(check, 0) == 0);
}

bool isEqual(char *s1, const char *s2) {
    return (strcmp(s1, s2) == 0);
}

bool isEqual(const string& s1, const char *check) {
    string s2(check);
    return (s1 == s2);
}

bool isEqual(const string& s1, const string& s2) {
    return (s1 == s2);
}

bool isContain(const string& str, const string& check) {
    size_t found = str.find(check);
    return (found != string::npos);
}

void trimStr(string &str) {
    str.erase(std::remove(str.begin(), str.end(), ' '), str.end());
}

bool isASCII(const string &s) {
    return !any_of(s.begin(), s.end(), [](char c) {
        return static_cast<unsigned char>(c) > 127;
    });
}

bool isApexLegends() {
    return isEqual(pkg, "com.ea.gp.apexlegendsmobilefps");
}

bool isFarlight84() {
    return isEqual(pkg, "com.miraclegames.farlight84");
}

bool isFortnite() {
    return isEqual(pkg, "com.epicgames.fortnite");
}

bool isARKSurvival() {
    return isEqual(pkg, "com.studiowildcard.wardrumstudios.ark");
}

bool isPUBGNewState() {
    return isEqual(pkg, "com.pubg.newstate") || isEqual(pkg, "com.pubg.newstate.beta");
}

bool isGameOfPeace() {
    return isEqual(pkg, "com.tencent.tmgp.pubgmhd");
}

bool isPUBGLite() {
    return isEqual(pkg, "com.tencent.iglite");
}

bool isBGMIndia() {
    return isEqual(pkg, "com.pubg.imobile");
}

bool isPUBGSeries() {
    return isEqual(pkg, "com.tencent.ig") ||
           isEqual(pkg, "com.tencent.igce") ||
           isEqual(pkg, "com.pubg.krmobile") ||
           isEqual(pkg, "com.vng.pubgmobile") ||
           isEqual(pkg, "com.rekoo.pubgm") || isPUBGLite() || isBGMIndia();
}

#endif
