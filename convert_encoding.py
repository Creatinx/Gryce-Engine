import codecs
content = codecs.open(r'D:/Gryce-Engine/core/api/component_api.cpp', 'r', 'iso-8859-1').read()
open(r'D:/Gryce-Engine/core/api/component_api.cpp', 'w', encoding='utf-8').write(content)
print("converted")
