import os, sys

for root, dirs, files in os.walk(r'D:/Gryce-Engine/editor'):
    for f in files:
        if f.endswith('.xaml'):
            path = os.path.join(root, f)
            content = open(path, 'r', encoding='utf-8').read()
            content = content.replace('https://schemas.inkore.net/lib/ui/wpf/modern', 'https://schemas.animasterstudios.com/lib/ui/wpf/modern')
            open(path, 'w', encoding='utf-8').write(content)
            print(f'Fixed: {path}')
