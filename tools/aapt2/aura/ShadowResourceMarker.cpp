/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aura/ShadowResourceMarker.h"

namespace aapt {
namespace aura {

size_t ShadowResourceMarker::Mark(
    const std::set<std::pair<std::string, std::string>>& shadow_set,
    const std::string& compilation_package,
    ResourceTable* table,
    android::IDiagnostics* diag) {
  if (shadow_set.empty()) return 0;

  size_t marked_count = 0;
  for (auto& package : table->packages) {
    if (package->name != compilation_package) continue;
    for (auto& type : package->types) {
      const std::string type_name = type->named_type.to_string();
      for (auto& entry : type->entries) {
        if (shadow_set.count(std::make_pair(type_name, entry->name))) {
          entry->is_shadow = true;
          ++marked_count;
        }
      }
    }
  }

  if (marked_count > 0) {
    diag->Note(android::DiagMessage()
               << "shadow resources: " << marked_count << " entries marked");
  }
  return marked_count;
}

}  // namespace aura
}  // namespace aapt
