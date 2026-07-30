#include "WatchAllSDKModel.h"

#include <cassert>
#include <iostream>

static WatchAllSDKClass Class(const std::string &outer, const std::string &name, kaddr ptr) {
    WatchAllSDKClass clazz;
    clazz.key.outerPath = outer;
    clazz.key.className = name;
    clazz.classPtr = ptr;
    return clazz;
}

static WatchAllSDKField Field(const std::string &name,
                              const std::string &type,
                              uint32_t offset,
                              uint32_t size,
                              uint8_t byteMask = 0,
                              uint8_t fieldMask = 0) {
    WatchAllSDKField field;
    field.name = name;
    field.type = type;
    field.nativeOffset = offset;
    field.size = size;
    field.byteMask = byteMask;
    field.fieldMask = fieldMask;
    return field;
}

static WatchAllSDKFunction Function(const std::string &name,
                                    const std::string &returnType,
                                    const std::string &params,
                                    uint64_t rva) {
    WatchAllSDKFunction function;
    function.name = name;
    function.returnType = returnType;
    function.parameterSignature = params;
    function.rva = rva;
    return function;
}

static void test_same_name_different_outer_not_merged() {
    WatchAllSDKModel model;
    assert(model.MergeClass(Class("/Game/A", "BP_Monster_C", 0x1000)));
    assert(model.MergeClass(Class("/Game/B", "BP_Monster_C", 0x2000)));
    assert(model.Classes().size() == 2);
}

static void test_same_class_new_pointer_reparsed_and_kept() {
    WatchAllSDKModel model;
    WatchAllSDKClass first = Class("/Game/A", "BP_Monster_C", 0x1000);
    first.fields.push_back(Field("Damage", "int", 0x120, 4));
    WatchAllSDKClass second = Class("/Game/A", "BP_Monster_C", 0x2000);
    second.fields.push_back(Field("HP", "float", 0x124, 4));

    assert(model.MergeClass(first));
    assert(model.MergeClass(second));
    assert(model.Classes().size() == 1);
    assert(model.Classes()[0].classPointers.size() == 2);
    assert(model.Classes()[0].fields.size() == 2);
}

static void test_duplicate_field_signature_dedupes() {
    WatchAllSDKModel model;
    WatchAllSDKClass first = Class("/Game/A", "BP_Monster_C", 0x1000);
    first.fields.push_back(Field("Damage", "int", 0x120, 4));
    WatchAllSDKClass second = Class("/Game/A", "BP_Monster_C", 0x1000);
    second.fields.push_back(Field("Damage", "int", 0x120, 4));

    assert(model.MergeClass(first));
    assert(!model.MergeClass(second));
    assert(model.Classes()[0].fields.size() == 1);
}

static void test_field_conflicts_all_preserved() {
    WatchAllSDKModel model;
    WatchAllSDKClass clazz = Class("/Game/A", "BP_Monster_C", 0x1000);
    clazz.fields.push_back(Field("Damage", "int", 0x120, 4));
    clazz.fields.push_back(Field("Damage", "float", 0x120, 4));
    clazz.fields.push_back(Field("Damage", "int", 0x124, 4));
    clazz.fields.push_back(Field("Damage", "int", 0x120, 1, 0x1, 0x1));

    assert(model.MergeClass(clazz));
    assert(model.Classes()[0].fields.size() == 4);
}

static void test_function_conflicts_all_preserved_and_duplicates_deduped() {
    WatchAllSDKModel model;
    WatchAllSDKClass first = Class("/Game/A", "BP_Monster_C", 0x1000);
    first.functions.push_back(Function("Hit", "void", "int Damage", 0x100));
    first.functions.push_back(Function("Hit", "bool", "int Damage", 0x100));
    first.functions.push_back(Function("Hit", "void", "float Damage", 0x100));
    first.functions.push_back(Function("Hit", "void", "int Damage", 0x104));
    WatchAllSDKClass second = Class("/Game/A", "BP_Monster_C", 0x1000);
    second.functions.push_back(Function("Hit", "void", "int Damage", 0x100));

    assert(model.MergeClass(first));
    assert(!model.MergeClass(second));
    assert(model.Classes()[0].functions.size() == 4);
}

static void test_first_merge_dedupes_duplicate_field_and_function_signatures() {
    WatchAllSDKModel model;
    WatchAllSDKClass clazz = Class("/Game/A", "BP_Monster_C", 0x1000);
    clazz.fields.push_back(Field("Damage", "int", 0x120, 4));
    clazz.fields.push_back(Field("Damage", "int", 0x120, 4));
    clazz.functions.push_back(Function("Hit", "void", "int Damage", 0x100));
    clazz.functions.push_back(Function("Hit", "void", "int Damage", 0x100));

    assert(model.MergeClass(clazz));
    assert(model.Classes()[0].fields.size() == 1);
    assert(model.Classes()[0].functions.size() == 1);
}

static WatchAllSDKClass ParseClassTask(const WatchAllSDKClassTask &task) {
    WatchAllSDKClass clazz = Class(task.classKey.outerPath, task.classKey.className, task.classPtr);
    clazz.fields.push_back(Field("Damage", "int", 0x120, 4));
    clazz.functions.push_back(Function("Hit", "void", "int Damage", 0x100));
    return clazz;
}

static void test_worker_parser_populates_structured_model() {
    WatchAllObjectRecord record{};
    record.classPtr = 0x1000;
    record.classOuterPath = "/Game/A";
    record.className = "BP_Monster_C";

    WatchAllSDKWorker worker(ParseClassTask);
    worker.Start();
    assert(worker.Submit(record));
    worker.Stop();
    WatchAllSDKModel model = worker.ModelSnapshot();
    assert(model.Classes().size() == 1);
    assert(model.Classes()[0].fields.size() == 1);
    assert(model.Classes()[0].functions.size() == 1);
    assert(worker.PendingCount() == 0);
}

int main() {
    test_same_name_different_outer_not_merged();
    test_same_class_new_pointer_reparsed_and_kept();
    test_duplicate_field_signature_dedupes();
    test_first_merge_dedupes_duplicate_field_and_function_signatures();
    test_field_conflicts_all_preserved();
    test_function_conflicts_all_preserved_and_duplicates_deduped();
    test_worker_parser_populates_structured_model();
    std::cout << "watch-all task 4 offline checks passed" << std::endl;
    return 0;
}
