#!/usr/bin/env python3
# Fix internal_state.h: add #include <mutex> and init_mutex field

path = r'D:/Gryce-Engine/core/api/internal_state.h'

with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

# Step 1: Add #include <mutex>
if '#include <mutex>' not in content:
    content = content.replace(
        '#include <atomic>',
        '#include <atomic>\n#include <mutex>'
    )

# Step 2: Add init_mutex field inside GlobalState
if 'init_mutex' not in content:
    content = content.replace(
        '    bool initialized = false;',
        '    bool initialized = false;\n    std::mutex init_mutex;'
    )

with open(path, 'w', encoding='utf-8') as f:
    f.write(content)

print('Fixed internal_state.h')

# Verify
with open(path, 'r', encoding='utf-8') as f:
    verify = f.read()
assert '#include <mutex>' in verify
assert 'init_mutex' in verify
print('Verified OK')
