import json

data = json.load(open(r'D:/Gryce-Engine/editor/obj/project.assets.json'))
print(json.dumps(data.get('project', {}).get('restore', {}), indent=2))
