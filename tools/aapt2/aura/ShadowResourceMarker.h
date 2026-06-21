/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AAPT2_AURA_SHADOW_RESOURCE_MARKER_H
#define AAPT2_AURA_SHADOW_RESOURCE_MARKER_H

#include <set>
#include <string>
#include <utility>

#include "ResourceTable.h"
#include "androidfw/IDiagnostics.h"

namespace aapt {
namespace aura {

// 接收 shadow_set（来自 PublicAarReader），标记 ResourceTable 中当前 compilation package
// 匹配的 entry 为 is_shadow=true。shadow entry 在 IdAssigner 中走无 slot 约束分配，
// TableFlattener 不写入 arsc，ReferenceLinker 跳过本地查找走 -I 真实 ID。
class ShadowResourceMarker {
 public:
  // 将 shadow_set 中的 (type, name) 应用到 table 的 compilation_package 中。
  // 返回标记数量。
  static size_t Mark(const std::set<std::pair<std::string, std::string>>& shadow_set,
                     const std::string& compilation_package,
                     ResourceTable* table,
                     android::IDiagnostics* diag);
};

}  // namespace aura
}  // namespace aapt

#endif  // AAPT2_AURA_SHADOW_RESOURCE_MARKER_H
