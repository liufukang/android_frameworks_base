/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef AAPT_COMPILE_IDASSIGNER_H
#define AAPT_COMPILE_IDASSIGNER_H

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "android-base/macros.h"

#include "Resource.h"
#include "process/IResourceTableConsumer.h"

namespace aapt {

// Assigns IDs to each resource in the table, respecting existing IDs and
// filling in gaps in between fixed ID assignments.
class IdAssigner : public IResourceTableConsumer {
 public:
  IdAssigner() = default;
  explicit IdAssigner(const std::unordered_map<ResourceName, ResourceId>* map)
      : assigned_id_map_(map) {}
  // 支持自定义 type ID 映射和 entry slot 配置
  IdAssigner(const std::unordered_map<ResourceName, ResourceId>* map,
             const std::map<std::string, uint8_t>* type_id_map,
             const std::vector<int>* entry_slots,
             const std::set<std::pair<std::string, std::string>>* legacy_entry_names = nullptr,
             int entry_slot_size = 1024)
      : assigned_id_map_(map), type_id_mapping_(type_id_map), entry_slots_(entry_slots),
        legacy_entry_names_(legacy_entry_names), entry_slot_size_(entry_slot_size) {}

  bool Consume(IAaptContext* context, ResourceTable* table) override;

 private:
  const std::unordered_map<ResourceName, ResourceId>* assigned_id_map_ = nullptr;
  // 全局 Type ID 映射表：type_name -> type_id
  const std::map<std::string, uint8_t>* type_id_mapping_ = nullptr;
  // Entry ID slot 配置：合法的 slot 索引列表，每 slot entry_slot_size_ 个 entry
  const std::vector<int>* entry_slots_ = nullptr;
  // Entry slot 容量
  int entry_slot_size_ = 1024;
  // legacy entry 名称集合 {(type_name, entry_name)}，这些 entry 不受 entry slot 约束
  const std::set<std::pair<std::string, std::string>>* legacy_entry_names_ = nullptr;
};

}  // namespace aapt

#endif /* AAPT_COMPILE_IDASSIGNER_H */
