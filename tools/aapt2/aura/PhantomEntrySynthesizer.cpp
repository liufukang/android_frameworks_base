/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aura/PhantomEntrySynthesizer.h"

#include "Resource.h"
#include "ResourceUtils.h"

namespace aapt {
namespace aura {

static constexpr uint8_t kAppPackageId = 0x7F;

size_t PhantomEntrySynthesizer::Synthesize(
    const std::vector<LegacyEntry>& legacy_entries,
    const std::string& compilation_package,
    ResourceTable* table,
    android::IDiagnostics* diag) {
  if (legacy_entries.empty()) return 0;

  auto* host_package = table->FindOrCreatePackage(compilation_package);
  size_t synthesized = 0;

  for (const auto& le : legacy_entries) {
    auto parsed_type = ParseResourceNamedType(le.type_name);
    if (!parsed_type) continue;

    auto* type = host_package->FindOrCreateType(parsed_type.value());
    ResourceEntry* existing = type->FindEntry(le.entry_name);
    if (existing != nullptr) {
      // 已存在（由 stub 或恢复流程创建），跳过
      continue;
    }

    // 合成空占位 entry（visibility 保持 kUndefined）
    ResourceEntry* entry = type->FindOrCreateEntry(le.entry_name);
    entry->id = ResourceId(kAppPackageId, le.type_id, le.entry_id);
    synthesized++;
  }

  if (synthesized > 0) {
    diag->Note(android::DiagMessage()
               << "synthesized " << synthesized
               << " phantom legacy entries for ReferenceLinker");
  }
  return synthesized;
}

}  // namespace aura
}  // namespace aapt
