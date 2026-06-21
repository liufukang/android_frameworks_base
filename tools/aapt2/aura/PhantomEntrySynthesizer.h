/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AAPT2_AURA_PHANTOM_ENTRY_SYNTHESIZER_H
#define AAPT2_AURA_PHANTOM_ENTRY_SYNTHESIZER_H

#include <string>
#include <vector>

#include "ResourceTable.h"
#include "androidfw/IDiagnostics.h"
#include "format/binary/TableFlattener.h"

namespace aapt {
namespace aura {

// 为仍未在 ResourceTable 中出现的 legacy entries 合成占位条目。
// 合成空 entry（无 values，设 ID）使 ReferenceLinker 能找到它们。
class PhantomEntrySynthesizer {
 public:
  using LegacyEntry = TableFlattenerOptions::LegacyPublicEntry;

  // 扫描 legacy_entries，为 ResourceTable 中不存在的条目合成占位 entry。
  // 返回合成数量。
  static size_t Synthesize(const std::vector<LegacyEntry>& legacy_entries,
                           const std::string& compilation_package,
                           ResourceTable* table,
                           android::IDiagnostics* diag);
};

}  // namespace aura
}  // namespace aapt

#endif  // AAPT2_AURA_PHANTOM_ENTRY_SYNTHESIZER_H
