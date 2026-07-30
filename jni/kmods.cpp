#include "kmods.h"
#include "Offsets.h"
#include "SDK.h"
#include "WatchAllObjects.h"
#include "WatchAllObjectRecords.h"
#include "WatchAllSDKModel.h"
#include <csignal>

class ProcessMemoryReader {
public:
    bool TryReadBuffer(void *address, void *buffer, size_t size) {
        return ::TryReadBuffer(address, buffer, size);
    }

    bool TryReadObjectName(kaddr object, std::string &out) {
        if (!UObject::isValid(object)) {
            return false;
        }
        out = UObject::getName(object);
        return !out.empty() && out != "None";
    }

    bool TryReadObjectOuter(kaddr object, kaddr &out) {
        if (!UObject::isValid(object)) {
            return false;
        }
        out = UObject::getOuter(object);
        return true;
    }

    bool TryReadObjectClass(kaddr object, kaddr &out) {
        if (!UObject::isValid(object)) {
            return false;
        }
        out = UObject::getClass(object);
        return out != 0;
    }
};

static WatchAllObjectSnapshotConfig MakeWatchAllObjectSnapshotConfig() {
    WatchAllObjectSnapshotConfig config{};
    config.guObjectArray = getRealOffset(Offsets::GUObjectArray);
    config.fuObjectArrayToTuObjectArray = Offsets::FUObjectArrayToTUObjectArray;
    config.tuObjectArrayToNumElements = Offsets::TUObjectArrayToNumElements;
    config.pointerSize = Offsets::PointerSize;
    config.fuObjectItemSize = Offsets::FUObjectItemSize;
    config.fuObjectItemPadd = Offsets::FUObjectItemPadd;
    config.chunkElements = WATCH_ALL_CHUNK_ELEMENTS;
    config.derefGuObjectArray = deRefGUObjectArray;
    return config;
}

static WatchAllSDKClass WatchAllParseSDKClassTask(const WatchAllSDKClassTask &task) {
    WatchAllSDKClass clazz;
    clazz.key = task.classKey;
    clazz.classPtr = task.classPtr;

    if (!sdkIsValidUStruct(task.classPtr)) {
        return clazz;
    }

    list<kaddr> recurrce;
    kaddr child = UStruct::getChildProperties(task.classPtr);
    int fieldGuard = 0;
    while (child) {
        if (sdkGuardExceeded("watch-fields", task.classPtr, fieldGuard++) || !sdkIsValidFField(child)) {
            break;
        }

        WatchAllSDKField field;
        field.name = FField::getName(child);
        field.type = resolveProp423(recurrce, child);
        field.nativeOffset = UProperty::getOffset(child);
        field.size = UProperty::getElementSize(child);
        field.arrayDim = UProperty::getArrayDim(child);
        field.propertyFlags = UProperty::getPropertyFlags(child);
        if (isEqual(FField::getClassName(child), "BoolProperty")) {
            field.byteOffset = UBoolProperty::getByteOffset(child);
            field.byteMask = UBoolProperty::getByteMask(child);
            field.fieldMask = UBoolProperty::getFieldMask(child);
        }
        clazz.fields.push_back(field);
        child = FField::getNext(child);
    }

    kaddr func = UStruct::getChildren(task.classPtr);
    int funcGuard = 0;
    while (func) {
        if (sdkGuardExceeded("watch-functions", task.classPtr, funcGuard++) || !sdkIsValidUField(func)) {
            break;
        }

        const string functionClass = UObject::getClassName(func);
        if (isStartWith(functionClass, "Function") || isEqual(functionClass, "DelegateFunction")) {
            WatchAllSDKFunction function;
            function.name = UObject::getName(func);
            function.returnType = "void";
            function.functionFlags = UFunction::getFunctionFlags(func);
            kaddr nativeFunc = UFunction::getFunc(func);
            function.rva = nativeFunc > libbase ? nativeFunc - libbase : 0;

            string params;
            kaddr param = UStruct::getChildProperties(func);
            int paramGuard = 0;
            while (param) {
                if (sdkGuardExceeded("watch-function-params", func, paramGuard++) || !sdkIsValidFField(param)) {
                    break;
                }
                uint64 flags = UProperty::getPropertyFlags(param);
                if ((flags & 0x0000000000000400) == 0x0000000000000400) {
                    function.returnType = resolveProp423(recurrce, param);
                } else {
                    if (!params.empty()) {
                        params += ", ";
                    }
                    if ((flags & 0x0000000000000100) == 0x0000000000000100) {
                        params += "out ";
                    }
                    if ((flags & 0x0000000000000002) == 0x0000000000000002) {
                        params += "const ";
                    }
                    params += resolveProp423(recurrce, param);
                    params += " ";
                    params += FField::getName(param);
                }
                param = FField::getNext(param);
            }
            function.parameterSignature = params;
            clazz.functions.push_back(function);
        }
        func = UField::getNext(func);
    }

    return clazz;
}

using namespace std;

const char *short_options = "hlrfnsabcdevi:j:p:o:g:u:w:";

enum LongOnlyOption {
    OPT_WATCH_ALL = 1000,
    OPT_INTERVAL
};

static volatile sig_atomic_t gWatchStopRequested = 0;

void HandleWatchSigint(int) {
    gWatchStopRequested = 1;
}

int RunWatchAll(const string &outputpath, int intervalSeconds) {
    (void) outputpath;

    target_pid = find_pid(pkg.c_str());
    if (target_pid == -1) {
        cout << "watch-all: Can't find the process" << endl;
        return -1;
    }
    cout << "watch-all: Process name: " << pkg.c_str() << ", Pid: " << target_pid << endl;

    libbase = get_module_base(lib_name);
    if (libbase == 0) {
        cout << "watch-all: Can't find Library: " << lib_name << endl;
        return -1;
    }
    cout << "watch-all: Base Address of " << lib_name << " Found At "
         << setbase(16) << libbase << setbase(10) << endl;

    if (!isUE423) {
        cout << "watch-all: requires --newue chunked GUObjectArray mode" << endl;
        return -1;
    }
    if (Offsets::GUObjectArray < 1) {
        cout << "watch-all: Please Enter Correct GUObject Addresses!!" << endl;
        return -1;
    }

    signal(SIGINT, HandleWatchSigint);
    cout << "watch-all: bound to current PID/libUE4.so; interval=" << intervalSeconds
         << "s. Scanning ARM64 --newue chunked GUObjectArray." << endl;

    WatchAllObjectArraySnapshot snapshot;
    WatchAllObjectRecordStore objectRecords;
    WatchAllSDKWorker sdkWorker(WatchAllParseSDKClassTask);
    sdkWorker.Start();
    ProcessMemoryReader reader;
    WatchAllObjectSnapshotConfig snapshotConfig = MakeWatchAllObjectSnapshotConfig();

    while (!gWatchStopRequested) {
        if (!PidAlive(target_pid)) {
            cout << "watch-all: session aborted; game PID disappeared" << endl;
            return -1;
        }

        std::vector<WatchAllObjectDiff> diffs;
        if (!snapshot.Capture(snapshotConfig, reader, diffs)) {
            cout << "watch-all: GUObjectArray snapshot failed; keeping previous baseline" << endl;
        } else {
            size_t newRecords = 0;
            for (const auto &diff : diffs) {
                const WatchAllObjectRecord *record = objectRecords.AddDiff(reader, diff);
                if (record != nullptr) {
                    ++newRecords;
                    sdkWorker.Submit(*record);
                }
            }

            cout << "watch-all: objects=" << snapshot.LastNumElements()
                 << " diffs=" << diffs.size()
                 << " records=" << objectRecords.Records().size()
                 << " new-records=" << newRecords
                 << " sdk-classes=" << sdkWorker.ClassCount()
                 << " sdk-pending=" << sdkWorker.PendingCount() << endl;
            if (isVerbose) {
                for (const auto &diff : diffs) {
                    cout << "watch-all: "
                         << (diff.kind == WatchAllObjectDiffKind::Added ? "added" : "changed")
                         << " slot=" << diff.index
                         << " object=0x" << setbase(16) << diff.current.object << setbase(10)
                         << endl;
                }
            }
        }
        sleep(intervalSeconds);
    }

    cout << "watch-all: stopped by SIGINT" << endl;
    return 0;
}
const struct option long_options[] = {
        {"help",       no_argument,       nullptr, 'h'},
        {"lib",        no_argument,       nullptr, 'l'},
        {"raw",        no_argument,       nullptr, 'r'},
        {"fast",       no_argument,       nullptr, 'f'},
        {"objs",       no_argument,       nullptr, 'n'},
        {"strings",    no_argument,       nullptr, 's'},
        {"sdku",       no_argument,       nullptr, 'a'},
        {"sdkw",       no_argument,       nullptr, 'b'},
        {"newue",      no_argument,       nullptr, 'c'},
        {"actors",     no_argument,       nullptr, 'd'},
        {"ptrdec",     no_argument,       nullptr, 'e'},
        {"verbose",    no_argument,       nullptr, 'v'},
        {"derefgname", required_argument, nullptr, 'i'},
        {"derefguobj", required_argument, nullptr, 'j'},
        {"package",    required_argument, nullptr, 'p'},
        {"output",     required_argument, nullptr, 'o'},
        {"gname",      required_argument, nullptr, 'g'},
        {"guobj",      required_argument, nullptr, 'u'},
        {"gworld",     required_argument, nullptr, 'w'},
        {"watch-all",  no_argument,       nullptr, OPT_WATCH_ALL},
        {"interval",   required_argument, nullptr, OPT_INTERVAL},
        {nullptr, 0,                      nullptr, 0}
};

void Usage() {
    printf("UE4Dumper v0.21 <==> Made By KMODs(kp7742)\n");
    printf("Usage: ./ue4dumper <option(s)>\n");
    printf("Dump Lib libUE4.so from Memory of Game Process and Generate structure SDK for UE4 Engine\n");
    printf("Tested on PUBG Mobile Series and Other UE4 Based Games\n");
    printf(" Options:\n");
    printf("--SDK Dump With GObjectArray Args--------------------------------------------------------\n");
    printf("  --sdku                              Dump SDK with GUObject\n");
    printf("  --gname <address>                   GNames Pointer Address\n");
    printf("  --guobj <address>                   GUObject Pointer Address\n");
    printf("--SDK Dump With GWorld Args--------------------------------------------------------------\n");
    printf("  --sdkw                              Dump SDK with GWorld\n");
    printf("  --gname <address>                   GNames Pointer Address\n");
    printf("  --gworld <address>                  GWorld Pointer Address\n");
    printf("--Dump Strings Args----------------------------------------------------------------------\n");
    printf("  --strings                           Dump Strings\n");
    printf("  --gname <address>                   GNames Pointer Address\n");
    printf("--Dump Objects Args----------------------------------------------------------------------\n");
    printf("  --objs                              Dumping Object List\n");
    printf("  --gname <address>                   GNames Pointer Address\n");
    printf("  --guobj <address>                   GUObject Pointer Address\n");
    printf("--Lib Dump Args--------------------------------------------------------------------------\n");
    printf("  --lib                               Dump libUE4.so from Memory\n");
    printf("  --raw(Optional)                     Output Raw Lib and Not Rebuild It\n");
    printf("  --fast(Optional)                    Enable Fast Dumping(May Miss Some Bytes in Dump)\n");
    printf("--Show ActorList With GWorld Args--------------------------------------------------------\n");
    printf("  --actors                            Show Actors with GWorld\n");
    printf("  --gname <address>                   GNames Pointer Address\n");
    printf("  --gworld <address>                  GWorld Pointer Address\n");
    printf("--Other Args-----------------------------------------------------------------------------\n");
    printf("  --newue(Optional)                   Run in UE 4.23+ Mode\n");
    printf("  --ptrdec(Optional)                  Use Pointer Decryption Mode\n");
    printf("  --verbose(Optional)                 Show Verbose Output of Dumping\n");
    printf("  --derefgname(Optional) <true/false> De-Reference GNames Address(Default: true)\n");
    printf("  --derefguobj(Optional) <true/false> De-Reference GUObject Address(Default: false)\n");
    printf("  --package <packageName>             Package Name of App(Default: com.tencent.ig)\n");
    printf("  --output <outputPath>               File Output path\n");
    printf("  --watch-all                         Run independent watch-all mode\n");
    printf("  --interval <seconds>                watch-all polling interval(Default: 1)\n");
    printf("  --help                              Display this information\n");
}

kaddr getHexAddr(const char *addr) {
#ifndef __SO64__
    return (kaddr) strtoul(addr, nullptr, 16);
#else
    return (kaddr) strtoull(addr, nullptr, 16);
#endif
}

int main(int argc, char *argv[]) {
    int c;
    string outputpath(".");
    bool isValidArg = true,
            isLibDump = false,
            isFastDump = false,
            isRawDump = false,
            isObjsDump = false,
            isStrDump = false,
            isSdkDump = false,
            isSdkDump2 = false,
            isActorDump = false,
            isWatchAll = false;
    int watchIntervalSeconds = 1;

    while ((c = getopt_long(argc, argv, short_options, long_options, nullptr)) != -1) {
        switch (c) {
            case 'l':
                isLibDump = true;
                break;
            case 'r':
                isRawDump = true;
                break;
            case 'f':
                isFastDump = true;
                break;
            case 'p':
                pkg = optarg;
                break;
            case 'o':
                outputpath = optarg;
                break;
            case 'g':
                Offsets::GNames = getHexAddr(optarg);
                break;
            case 'u':
                Offsets::GUObjectArray = getHexAddr(optarg);
                break;
            case 'w':
                Offsets::GWorld = getHexAddr(optarg);
                break;
            case 'n':
                isObjsDump = true;
                break;
            case 's':
                isStrDump = true;
                break;
            case 'a':
                isSdkDump = true;
                break;
            case 'b':
                isSdkDump2 = true;
                break;
            case 'c':
                isUE423 = true;
                break;
            case 'd':
                isActorDump = true;
                break;
            case 'e':
                isPtrDec = true;
                break;
            case 'v':
                isVerbose = true;
                break;
            case 'i':
                deRefGNames = isEqual(optarg, "true");
                break;
            case 'j':
                deRefGUObjectArray = isEqual(optarg, "true");
                break;
            case OPT_WATCH_ALL:
                isWatchAll = true;
                break;
            case OPT_INTERVAL:
                watchIntervalSeconds = atoi(optarg);
                if (watchIntervalSeconds < 1) {
                    isValidArg = false;
                }
                break;
            default:
                isValidArg = false;
                break;
        }
    }

#if defined(__LP64__)
    Offsets::initOffsets_64();
    if (isUE423) {
        Offsets::patchUE423_64();
    }
    Offsets::patchCustom_64();
#else
    Offsets::initOffsets_32();
    if (isUE423) {
        Offsets::patchUE423_32();
    }
    Offsets::patchCustom_32();
#endif

    isPGLite = isPUBGLite();
    isPUBGCN = isGameOfPeace();
    isPUBGNS = isPUBGNewState();

    if (isWatchAll) {
        if (!isValidArg) {
            printf("Wrong Arguments, Please Check!!\n");
            Usage();
            return -1;
        }
        return RunWatchAll(outputpath, watchIntervalSeconds);
    }

    if (!isValidArg ||
        (!isLibDump && !isObjsDump && !isStrDump && !isSdkDump && !isSdkDump2 && !isActorDump)) {
        printf("Wrong Arguments, Please Check!!\n");
        Usage();
        return -1;
    }

    //Find PID
    target_pid = find_pid(pkg.c_str());
    if (target_pid == -1) {
        cout << "Can't find the process" << endl;
        return -1;
    }
    cout << "Process name: " << pkg.c_str() << ", Pid: " << target_pid << endl;

    //Lib Base Address
    libbase = get_module_base(lib_name);
    if (libbase == 0) {
        cout << "Can't find Library: " << lib_name << endl;
        return -1;
    }
    cout << "Base Address of " << lib_name << " Found At " << setbase(16) << libbase << setbase(10)
         << endl;

    if (isLibDump) {
        kaddr dumpBase = 0;
        kaddr dumpEnd = 0;

        if (isRawDump) {
            string rawPath = outputpath + "/" + lib_name;
            if (!dump_module_by_maps(lib_name, rawPath, dumpBase, dumpEnd)) {
                cout << "Raw dump completed with read gaps; output keeps zero-filled gaps" << endl;
            }
        } else {
            string tempPath = outputpath + "/KTemp.dat";
            if (!dump_module_by_maps(lib_name, tempPath, dumpBase, dumpEnd)) {
                cout << "Dump completed with read gaps; rebuilding may still be incomplete" << endl;
            }

            //SoFixer Code//
            cout << "Rebuilding Elf(So)" << endl;

#if defined(__LP64__)
            string outPath = outputpath + "/" + lib_name;

            fix_so(tempPath.c_str(), outPath.c_str(), dumpBase);
#else
            ElfReader elf_reader;

            elf_reader.setDumpSoFile(true);
            elf_reader.setDumpSoBaseAddr(dumpBase);

            auto file = fopen(tempPath.c_str(), "rb");
            if (nullptr == file) {
                printf("source so file cannot found!!!\n");
                return -1;
            }
            auto fd = fileno(file);

            elf_reader.setSource(tempPath.c_str(), fd);

            if (!elf_reader.Load()) {
                printf("source so file is invalid\n");
                return -1;
            }

            ElfRebuilder elf_rebuilder(&elf_reader);
            if (!elf_rebuilder.Rebuild()) {
                printf("error occured in rebuilding elf file\n");
                return -1;
            }
            fclose(file);
            //SoFixer Code//

            ofstream redump(outputpath + "/" + lib_name, ofstream::out | ofstream::binary);
            if (redump.is_open()) {
                redump.write((char*) elf_rebuilder.getRebuildData(), elf_rebuilder.getRebuildSize());
            } else {
                cout << "Can't Output File" << endl;
                return -1;
            }
            redump.close();
#endif

            cout << "Rebuilding Complete" << endl;
            remove(tempPath.c_str());
        }
    }

    if (isStrDump) {
        if (Offsets::GNames < 1) {
            printf("Please Enter Correct GName Addresses!!\n");
            Usage();
            return -1;
        }
        DumpStrings(outputpath);
        cout << endl;
    }
    if (isObjsDump) {
        if (Offsets::GUObjectArray < 1) {
            printf("Please Enter Correct GUObject Addresses!!\n");
            Usage();
            return -1;
        }
        DumpObjects(outputpath);
        cout << endl;
    }
    if (isSdkDump) {
        if (Offsets::GNames < 1 || Offsets::GUObjectArray < 1) {
            printf("Please Enter Correct GName and GUObject Addresses!!\n");
            Usage();
            return -1;
        }
        DumpSDK(outputpath);
        cout << endl;
    }
    if (isSdkDump2) {
        if (Offsets::GNames < 1 || Offsets::GWorld < 1) {
            printf("Please Enter Correct GName and GWorld Addresses!!\n");
            Usage();
            return -1;
        }
        DumpSDKW(outputpath);
        cout << endl;
    }
    if (isActorDump) {
        if (Offsets::GNames < 1 || Offsets::GWorld < 1) {
            printf("Please Enter Correct GName and GWorld Addresses!!\n");
            Usage();
            return -1;
        }
        DumpActors();
        cout << endl;
    }
    return 0;
}
