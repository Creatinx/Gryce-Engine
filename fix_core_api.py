path = r'D:/Gryce-Engine/core/api/core_api.cpp'
content = open(path).read()

# 1. Add reflection include after line 8 (#include "scene/uuid.h")
content = content.replace('#include "scene/uuid.h"', '#include "scene/uuid.h"\n#include "reflection/reflection.h"')

# 2. Fix the duplicated switch code
old_switch = '''        default:
            break;
            g_core_state.paused = !g_core_state.paused;
            fire_callback_play_mode_changed();
            break;
        }
        default:
            break;
    }'''

new_switch = '''        default:
            break;
    }'''

content = content.replace(old_switch, new_switch)

open(path, 'w').write(content)
print("core_api.cpp fixed")
