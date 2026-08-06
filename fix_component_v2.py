import re

path = r'D:/Gryce-Engine/core/api/component_api.cpp'
content = open(path, 'r', encoding='utf-8').read()

# First, normalize line endings and remove excessive blank lines
content = content.replace('\r\n', '\n').replace('\r', '\n')
content = re.sub(r'\n{3,}', '\n\n', content)

# Remove the old static function from anonymous namespace
old_func = '''// Helper: get type name from a component via RTTI typeid
static std::string get_component_type_name(gryce_engine::components::Component* comp) {
    if (!comp) return "";
    const char* raw = typeid(*comp).name();
    // MSVC: prefix "class " or "struct " -- strip it
    if (std::strncmp(raw, "class ", 6) == 0) raw += 6;
    else if (std::strncmp(raw, "struct ", 7) == 0) raw += 7;
    return std::string(raw);
}

'''
content = content.replace(old_func, '')

# Replace the anonymous namespace close + extern "C" with:
# close anonymous namespace, then define function in gryce_core, then extern "C"
old_block = '''} // namespace

extern "C" {'''

new_block = '''} // namespace

namespace gryce_core {
std::string get_component_type_name(gryce_engine::components::Component* comp) {
    if (!comp) return "";
    const char* raw = typeid(*comp).name();
    // MSVC: prefix "class " or "struct " -- strip it
    if (std::strncmp(raw, "class ", 6) == 0) raw += 6;
    else if (std::strncmp(raw, "struct ", 7) == 0) raw += 7;
    return std::string(raw);
}
} // namespace gryce_core

extern "C" {'''

content = content.replace(old_block, new_block)

open(path, 'w', encoding='utf-8').write(content)
print("done")
