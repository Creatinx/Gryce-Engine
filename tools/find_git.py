import shutil, os, glob

# Search common paths
paths = [
    r'C:/Program Files/Git/bin/git.exe',
    r'C:/Program Files/Git/cmd/git.exe',
    r'D:/Git/bin/git.exe',
    r'D:/Git/cmd/git.exe',
]
for p in paths:
    if os.path.exists(p):
        print('FOUND:', p)

# Also search in PATH directories
print('which:', shutil.which('git'))

# Search msys64
for p in glob.glob(r'D:/msys64/**/git.exe', recursive=True):
    print('GLOB:', p)
