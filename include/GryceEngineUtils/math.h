#pragma once

// GryceEngineUtils::math.h — re-export core/math/ 到 GryceEngineUtils::math::
//
// 数学库本身不重写，只做命名空间别名转发：
//   GryceEngineUtils::math::Vector3f == gryce_engine::math::Vector3f

#include "math/math.h"

namespace GryceEngineUtils {
namespace math {
    using namespace gryce_engine::math;
}
} // namespace GryceEngineUtils
