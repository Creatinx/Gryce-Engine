import re
from pathlib import Path

project_root = Path('D:/Gryce-Engine')
build_dir = project_root / 'build'
build_slnx = build_dir / 'GryceEngine.slnx'
root_slnx = project_root / 'GryceEngine.slnx'

if not build_slnx.exists():
    print('build/GryceEngine.slnx not found')
    exit(1)

content = build_slnx.read_text(encoding='utf-8')

# Update relative project paths so they remain valid from the repo root.
# In build/GryceEngine.slnx paths are relative to build/;
# in root/GryceEngine.slnx they must be relative to project_root.
def fix_path(match):
    full = match.group(0)
    path = match.group(1)
    # Skip absolute paths (editor csproj is absolute), make relative to root
    if ':' in path:
        rel = Path(path).relative_to(project_root)
        return f'Path="{rel.as_posix()}"'
    # In build/GryceEngine.slnx, dependency paths like "build/deps/..." are
    # actually relative to build/ (i.e. build/build/deps/... on disk).
    # When moving the solution to the repo root, preserve that meaning.
    if path.startswith('build/'):
        return f'Path="build/{path}"'
    # Other paths are relative to build/; prepend build/
    return f'Path="build/{path}"'

content = re.sub(r'Path="([^"]+)"', fix_path, content)

root_slnx.write_text(content, encoding='utf-8')
print(f'Synced root solution: {root_slnx}')
