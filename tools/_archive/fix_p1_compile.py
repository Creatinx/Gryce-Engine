#!/usr/bin/env python3
"""Batch-fix P1 compilation errors."""

import os

CORE = r'D:/Gryce-Engine/core/api'

def fix_file(path, replacements):
    with open(path, 'r', encoding='utf-8') as f:
        s = f.read()
    for old, new in replacements:
        if old in s:
            s = s.replace(old, new)
            print(f'  [OK] Patched: {os.path.basename(path)}')
        else:
            print(f'  [SKIP] Pattern not found: {os.path.basename(path)}')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(s)

# 1. entity_handle_map.h — add #include <vector>
fix_file(os.path.join(CORE, 'entity_handle_map.h'), [
    ('#include <unordered_map>\n#include <atomic>',
     '#include <unordered_map>\n#include <vector>\n#include <atomic>'),
])

# 2. entity_handle_map.cpp — add #include <mutex>, fix const Scene* foreach
fix_file(os.path.join(CORE, 'entity_handle_map.cpp'), [
    ('#include "entity_handle_map.h"',
     '#include "entity_handle_map.h"\n\n#include <mutex>'),
    ('void EntityHandleMap::rebuild(const gryce_engine::scene::Scene* scene) {\n    if (!scene) return;\n    clear();\n    scene->foreach([this](gryce_engine::scene::Entity* e) {\n        if (e) alloc(e->uuid());\n    });\n}',
     'void EntityHandleMap::rebuild(gryce_engine::scene::Scene* scene) {\n    if (!scene) return;\n    clear();\n    scene->foreach([this](gryce_engine::scene::Entity* e) {\n        if (e) alloc(e->uuid());\n    });\n}'),
])

# 3. core_api.cpp — fix UUID namespace collision with Windows GUID
fix_file(os.path.join(CORE, 'core_api.cpp'), [
    ('using gryce_engine::scene::UUID;\nusing gryce_engine::scene::SceneSerializer;\nusing gryce_engine::ecs::World;\nusing gryce_engine::resources::Project;',
     'using gryce_engine::scene::SceneSerializer;\nusing gryce_engine::ecs::World;\nusing gryce_engine::resources::Project;'),
    ('    UUID* uuid = g_core_state.entity_map.resolve_uuid(h);',
     '    gryce_engine::scene::UUID* uuid = g_core_state.entity_map.resolve_uuid(h);'),
    ('    return s->find_entity_by_uuid(*uuid);',
     '    return s->find_entity_by_uuid(static_cast<const gryce_engine::scene::UUID&>(*uuid));'),
])

# 4. entity_api.cpp — add #include "ecs/world.h" for complete type
fix_file(os.path.join(CORE, 'entity_api.cpp'), [
    ('#include "GryceCore/entity_api.h"\n#include "internal_state.h"\n\n#include "scene/scene.h"',
     '#include "GryceCore/entity_api.h"\n#include "internal_state.h"\n\n#include "ecs/world.h"\n#include "scene/scene.h"'),
])

print('\nAll patches applied.')
