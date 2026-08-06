import re

p = r'D:/Gryce-Engine/core/api/core_api.cpp'
with open(p, 'r', encoding='utf-8') as f:
    content = f.read()

# Find the first occurrence of "} // extern \"C\"" after GCore_GetInternalWorldPtr
# and truncate everything after it
marker = 'void* GCore_GetInternalWorldPtr(void) {\n    return gryce_core::g_core_state.world.get();\n}\n\n} // extern "C"'
idx = content.find(marker)
if idx >= 0:
    content = content[:idx + len(marker)] + '\n'
    with open(p, 'w', encoding='utf-8') as f:
        f.write(content)
    print('Fixed core_api.cpp')
else:
    print('Marker not found')
