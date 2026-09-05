#include <gtest/gtest.h>
#include "reflection/reflection.h"
using namespace gryce_engine::reflection;

TEST(ReflectionDiag, CheckRegistry) {
    auto& reg = Registry::instance();
    auto* info = reg.find("ColorRect");
    if (info) {
        printf("ColorRect found, fields: %zu\n", info->fields.size());
        auto fields = reg.all_fields("ColorRect");
        printf("all_fields ColorRect: %zu\n", fields.size());
    } else {
        printf("ColorRect NOT found in registry\n");
    }
    info = reg.find("MeshRenderer");
    if (info) {
        printf("MeshRenderer found, fields: %zu\n", info->fields.size());
        auto fields = reg.all_fields("MeshRenderer");
        printf("all_fields MeshRenderer: %zu\n", fields.size());
    } else {
        printf("MeshRenderer NOT found in registry\n");
    }
    printf("Total types in registry: %zu\n", reg.type_count());
}
