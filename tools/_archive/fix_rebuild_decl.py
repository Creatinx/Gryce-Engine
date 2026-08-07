import re
p = r'D:/Gryce-Engine/core/api/entity_handle_map.h'
with open(p, 'r', encoding='utf-8') as f:
    s = f.read()
s = s.replace('void rebuild(const gryce_engine::scene::Scene* scene);', 'void rebuild(gryce_engine::scene::Scene* scene);')
with open(p, 'w', encoding='utf-8') as f:
    f.write(s)
print('Fixed entity_handle_map.h declaration')
