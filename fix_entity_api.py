import sys
file_path = r'D:/Gryce-Engine/core/api/entity_api.cpp'
lines = open(file_path).readlines()
# Remove lines 183-187 (0-indexed: 182-186)
del lines[182:187]
open(file_path, 'w').writelines(lines)
print("Done")
