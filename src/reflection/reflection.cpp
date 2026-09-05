#include "reflection/reflection.h"

namespace gryce_engine::reflection {

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

} // namespace gryce_engine::reflection