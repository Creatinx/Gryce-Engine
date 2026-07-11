#include "async_loader.h"

namespace gryce_engine::assets {

AsyncLoader& AsyncLoader::instance() {
    static AsyncLoader loader;
    return loader;
}

} // namespace gryce_engine::assets
