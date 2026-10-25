// ===========================================================================
// editor/ext/asset/asset_index.h — 资产浏览
//
// 目录列举本身是引擎能力（GAsset_ScanDirectory，按 README 规则一补进引擎的）：
// 打包清单、热更校验、CI 资源检查同样需要它。ext 在这里只做三件事：
//
//   1. 缓冲扩容重试。引擎接口按"返回所需长度"约定，一次给不够就按需重来。
//   2. 结果缓存。面板每帧都会问一次目录内容，不缓存就是每帧扫盘；
//      缓存只省 IO，不改内容，缓存命中与直接调用返回的字节完全一致。
//   3. 侧车信息转成前端好用的形状（GAssetMetaInfo 是定长 C 结构体）。
// ===========================================================================

#ifndef GRYCE_EDITOR_ASSET_INDEX_H
#define GRYCE_EDITOR_ASSET_INDEX_H

#include "api/asset_api.h"

#include <string>

namespace gryce::editor::asset {

void reset();

// 目录列举，返回引擎 JSON 原样。
const char* scan(const char* dir, int recursive);

// 导入并实例化（建实体 + MeshRenderer），返回资产句柄。
GAssetHandle import(const char* path);

// 侧车信息。不存在 / 损坏时 ok=0。
std::string meta_json(const char* path);

// 失效缓存（导入、改导入设置后必须调，否则面板显示旧列表）。
void invalidate();

} // namespace gryce::editor::asset

#endif // GRYCE_EDITOR_ASSET_INDEX_H
