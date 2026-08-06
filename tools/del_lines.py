lines = open(r'D:/Gryce-Engine/core/render/render_context.cpp', 'r', encoding='utf-8').readlines()
# Delete lines 570-575 (0-indexed: 569-574)
del lines[569:575]
open(r'D:/Gryce-Engine/core/render/render_context.cpp', 'w', encoding='utf-8').writelines(lines)
print('Deleted lines 570-575, new length:', len(lines))
