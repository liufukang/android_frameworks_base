/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AAPT2_AURA_LEGACY_PACKAGE_COLLECTOR_H
#define AAPT2_AURA_LEGACY_PACKAGE_COLLECTOR_H

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "androidfw/IDiagnostics.h"
#include "format/binary/TableFlattener.h"
#include "process/SymbolTable.h"

namespace aapt {
namespace aura {

// 收集 legacy 0x7F 资源条目：
// 1. 从 --legacy-public-xml 文件解析声明的 legacy entries
// 2. 自动推导：当 packageId==0x7F 时，扫描 -I 中所有 0x7F entries
class LegacyPackageCollector {
 public:
  using LegacyEntry = TableFlattenerOptions::LegacyPublicEntry;

  // 解析 --legacy-public-xml 文件，追加到 out_entries。
  // 失败返回 false。
  static bool ParseLegacyPublicXml(const std::string& path,
                                   android::IDiagnostics* diag,
                                   std::vector<LegacyEntry>* out_entries);

  // 自动 legacy 推导：扫描 AssetManagerSymbolSource 中 packageId==0x7F 的资源，
  // 与 existing_entries 去重后追加到 out_entries。
  // 仅在当前 packageId==0x7F（Portal 宿主）时应调用。
  static size_t AutoDeriveFromIncludes(AssetManagerSymbolSource* asset_source,
                                       std::vector<LegacyEntry>* out_entries,
                                       android::IDiagnostics* diag);
};

}  // namespace aura
}  // namespace aapt

#endif  // AAPT2_AURA_LEGACY_PACKAGE_COLLECTOR_H
