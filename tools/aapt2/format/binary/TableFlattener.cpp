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

#include "format/binary/TableFlattener.h"

#include <algorithm>
#include <limits>
#include <set>
#include <sstream>
#include <type_traits>
#include <variant>

#include "ResourceTable.h"
#include "ResourceValues.h"
#include "SdkConstants.h"
#include "android-base/logging.h"
#include "android-base/macros.h"
#include "android-base/stringprintf.h"
#include "androidfw/BigBuffer.h"
#include "androidfw/ResourceUtils.h"
#include "format/binary/ChunkWriter.h"
#include "format/binary/ResEntryWriter.h"
#include "format/binary/ResourceTypeExtensions.h"
#include "optimize/Obfuscator.h"
#include "trace/TraceBuffer.h"

using namespace android;

namespace aapt {

namespace {

template <typename T>
static bool cmp_ids(const T* a, const T* b) {
  return a->id.value() < b->id.value();
}

static void strcpy16_htod(uint16_t* dst, size_t len, const StringPiece16& src) {
  if (len == 0) {
    return;
  }

  size_t i;
  const char16_t* src_data = src.data();
  for (i = 0; i < len - 1 && i < src.size(); i++) {
    dst[i] = android::util::HostToDevice16((uint16_t)src_data[i]);
  }
  dst[i] = 0;
}

struct OverlayableChunk {
  std::string actor;
  android::Source source;
  std::map<PolicyFlags, std::set<ResourceId>> policy_ids;
};

class PackageFlattener {
 public:
  PackageFlattener(IAaptContext* context, const ResourceTablePackageView& package,
                   const ResourceTable::ReferencedPackages* shared_libs,
                   SparseEntriesMode sparse_entries, bool compact_entries,
                   bool collapse_key_stringpool,
                   const std::set<ResourceName>& name_collapse_exemptions,
                   bool deduplicate_entry_values)
      : context_(context),
        diag_(context->GetDiagnostics()),
        package_(package),
        shared_libs_(shared_libs),
        sparse_entries_(sparse_entries),
        compact_entries_(compact_entries),
        collapse_key_stringpool_(collapse_key_stringpool),
        name_collapse_exemptions_(name_collapse_exemptions),
        deduplicate_entry_values_(deduplicate_entry_values) {
  }

  bool FlattenPackage(BigBuffer* buffer) {
    TRACE_CALL();
    ChunkWriter pkg_writer(buffer);
    ResTable_package* pkg_header = pkg_writer.StartChunk<ResTable_package>(RES_TABLE_PACKAGE_TYPE);
    pkg_header->id = android::util::HostToDevice32(package_.id.value());

    // AAPT truncated the package name, so do the same.
    // Shared libraries require full package names, so don't truncate theirs.
    if (context_->GetPackageType() != PackageType::kApp &&
        package_.name.size() >= arraysize(pkg_header->name)) {
      diag_->Error(android::DiagMessage()
                   << "package name '" << package_.name
                   << "' is too long. "
                      "Shared libraries cannot have truncated package names");
      return false;
    }

    // Copy the package name in device endianness.
    strcpy16_htod(pkg_header->name, arraysize(pkg_header->name),
                  android::util::Utf8ToUtf16(package_.name));

    // Serialize the types. We do this now so that our type and key strings
    // are populated. We write those first.
    android::BigBuffer type_buffer(1024);
    FlattenTypes(&type_buffer);

    pkg_header->typeStrings = android::util::HostToDevice32(pkg_writer.size());
    android::StringPool::FlattenUtf16(pkg_writer.buffer(), type_pool_, diag_);

    pkg_header->keyStrings = android::util::HostToDevice32(pkg_writer.size());
    android::StringPool::FlattenUtf8(pkg_writer.buffer(), key_pool_, diag_);

    // Append the types.
    buffer->AppendBuffer(std::move(type_buffer));

    // If there are libraries (or if the package ID is 0x00), encode a library chunk.
    // 扩展：非标准 packageId（非 0x01/0x7F）也注入 LibraryChunk 用于 DynamicRefTable 解析
    if (package_.id.value() == 0x00 || !shared_libs_->empty() ||
        (package_.id.value() != kFrameworkPackageId && package_.id.value() != kAppPackageId)) {
      FlattenLibrarySpec(buffer);
    }

    if (!FlattenOverlayable(buffer)) {
      return false;
    }

    if (!FlattenAliases(buffer)) {
      return false;
    }

    pkg_writer.Finish();
    return true;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(PackageFlattener);

  // Use compact entries only if
  // 1) it is enabled, and that
  // 2) the entries will be accessed on platforms U+, and
  // 3) all entry keys can be encoded in 16 bits
  bool UseCompactEntries(const ConfigDescription& config, std::vector<FlatEntry>* entries) const {
    return compact_entries_ && context_->GetMinSdkVersion() > SDK_TIRAMISU &&
      std::none_of(entries->cbegin(), entries->cend(),
        [](const auto& e) { return e.entry_key >= std::numeric_limits<uint16_t>::max(); });
  }

  std::unique_ptr<ResEntryWriter> GetResEntryWriter(bool dedup, bool compact, BigBuffer* buffer) {
    if (dedup) {
      if (compact) {
        return std::make_unique<DeduplicateItemsResEntryWriter<true>>(buffer);
      } else {
        return std::make_unique<DeduplicateItemsResEntryWriter<false>>(buffer);
      }
    } else {
      if (compact) {
        return std::make_unique<SequentialResEntryWriter<true>>(buffer);
      } else {
        return std::make_unique<SequentialResEntryWriter<false>>(buffer);
      }
    }
  }

  bool FlattenConfig(const ResourceTableTypeView& type, const ConfigDescription& config,
                     const size_t num_total_entries, std::vector<FlatEntry>* entries,
                     BigBuffer* buffer) {
    CHECK(num_total_entries != 0);
    CHECK(num_total_entries <= std::numeric_limits<uint16_t>::max());

    ChunkWriter type_writer(buffer);
    ResTable_type* type_header = type_writer.StartChunk<ResTable_type>(RES_TABLE_TYPE_TYPE);
    type_header->id = type.id.value();
    type_header->config = config;
    type_header->config.swapHtoD();

    std::vector<uint32_t> offsets;
    offsets.resize(num_total_entries, 0xffffffffu);

    bool compact_entry = UseCompactEntries(config, entries);

    android::BigBuffer values_buffer(512);
    auto res_entry_writer = GetResEntryWriter(deduplicate_entry_values_,
                                              compact_entry, &values_buffer);

    for (FlatEntry& flat_entry : *entries) {
      CHECK(static_cast<size_t>(flat_entry.entry->id.value()) < num_total_entries);
      offsets[flat_entry.entry->id.value()] = res_entry_writer->Write(&flat_entry);
    }

    // whether the offsets can be represented in 2 bytes
    bool short_offsets = (values_buffer.size() / 4u) < std::numeric_limits<uint16_t>::max();

    bool sparse_encode = sparse_entries_ == SparseEntriesMode::Enabled ||
                         sparse_entries_ == SparseEntriesMode::Forced;

    if (sparse_entries_ == SparseEntriesMode::Forced ||
        (context_->GetMinSdkVersion() == 0 && config.sdkVersion == 0)) {
      // Sparse encode if forced or sdk version is not set in context and config.
    } else {
      // Otherwise, only sparse encode if the entries will be read on platforms S_V2+.
      sparse_encode = sparse_encode && (context_->GetMinSdkVersion() >= SDK_S_V2);
    }

    // Only sparse encode if the offsets are representable in 2 bytes.
    sparse_encode = sparse_encode && short_offsets;

    // Only sparse encode if the ratio of populated entries to total entries is below some
    // threshold.
    sparse_encode =
        sparse_encode && ((100 * entries->size()) / num_total_entries) < kSparseEncodingThreshold;

    if (sparse_encode) {
      type_header->entryCount = android::util::HostToDevice32(entries->size());
      type_header->flags |= ResTable_type::FLAG_SPARSE;
      ResTable_sparseTypeEntry* indices =
          type_writer.NextBlock<ResTable_sparseTypeEntry>(entries->size());
      for (size_t i = 0; i < num_total_entries; i++) {
        if (offsets[i] != ResTable_type::NO_ENTRY) {
          CHECK((offsets[i] & 0x03) == 0);
          indices->idx = android::util::HostToDevice16(i);
          indices->offset = android::util::HostToDevice16(offsets[i] / 4u);
          indices++;
        }
      }
    } else {
      type_header->entryCount = android::util::HostToDevice32(num_total_entries);
      if (compact_entry && short_offsets) {
        // use 16-bit offset only when compact_entry is true
        type_header->flags |= ResTable_type::FLAG_OFFSET16;
        uint16_t* indices = type_writer.NextBlock<uint16_t>(num_total_entries);
        for (size_t i = 0; i < num_total_entries; i++) {
          indices[i] = android::util::HostToDevice16(offsets[i] / 4u);
        }
      } else {
        uint32_t* indices = type_writer.NextBlock<uint32_t>(num_total_entries);
        for (size_t i = 0; i < num_total_entries; i++) {
          indices[i] = android::util::HostToDevice32(offsets[i]);
        }
      }
    }

    type_writer.buffer()->Align4();
    type_header->entriesStart = android::util::HostToDevice32(type_writer.size());
    type_writer.buffer()->AppendBuffer(std::move(values_buffer));
    type_writer.Finish();
    return true;
  }

  bool FlattenAliases(BigBuffer* buffer) {
    if (aliases_.empty()) {
      return true;
    }

    ChunkWriter alias_writer(buffer);
    auto header =
        alias_writer.StartChunk<ResTable_staged_alias_header>(RES_TABLE_STAGED_ALIAS_TYPE);
    header->count = android::util::HostToDevice32(aliases_.size());

    auto mapping = alias_writer.NextBlock<ResTable_staged_alias_entry>(aliases_.size());
    for (auto& p : aliases_) {
      mapping->stagedResId = android::util::HostToDevice32(p.first);
      mapping->finalizedResId = android::util::HostToDevice32(p.second);
      ++mapping;
    }
    alias_writer.Finish();
    return true;
  }

  bool FlattenOverlayable(BigBuffer* buffer) {
    std::set<ResourceId> seen_ids;
    std::map<std::string, OverlayableChunk> overlayable_chunks;

    CHECK(bool(package_.id)) << "package must have an ID set when flattening <overlayable>";
    for (auto& type : package_.types) {
      CHECK(bool(type.id)) << "type must have an ID set when flattening <overlayable>";
      for (auto& entry : type.entries) {
        CHECK(bool(type.id)) << "entry must have an ID set when flattening <overlayable>";
        if (!entry.overlayable_item) {
          continue;
        }

        const OverlayableItem& item = entry.overlayable_item.value();

        // Resource ids should only appear once in the resource table
        ResourceId id = android::make_resid(package_.id.value(), type.id.value(), entry.id.value());
        CHECK(seen_ids.find(id) == seen_ids.end())
            << "multiple overlayable definitions found for resource "
            << ResourceName(package_.name, type.named_type, entry.name).to_string();
        seen_ids.insert(id);

        // Find the overlayable chunk with the specified name
        OverlayableChunk* overlayable_chunk = nullptr;
        auto iter = overlayable_chunks.find(item.overlayable->name);
        if (iter == overlayable_chunks.end()) {
          OverlayableChunk chunk{item.overlayable->actor, item.overlayable->source};
          overlayable_chunk =
              &overlayable_chunks.insert({item.overlayable->name, chunk}).first->second;
        } else {
          OverlayableChunk& chunk = iter->second;
          if (!(chunk.source == item.overlayable->source)) {
            // The name of an overlayable set of resources must be unique
            context_->GetDiagnostics()->Error(android::DiagMessage(item.overlayable->source)
                                              << "duplicate overlayable name"
                                              << item.overlayable->name << "'");
            context_->GetDiagnostics()->Error(android::DiagMessage(chunk.source)
                                              << "previous declaration here");
            return false;
          }

          CHECK(chunk.actor == item.overlayable->actor);
          overlayable_chunk = &chunk;
        }

        if (item.policies == 0) {
          context_->GetDiagnostics()->Error(android::DiagMessage(item.overlayable->source)
                                            << "overlayable " << entry.name
                                            << " does not specify policy");
          return false;
        }

        auto policy = overlayable_chunk->policy_ids.find(item.policies);
        if (policy != overlayable_chunk->policy_ids.end()) {
          policy->second.insert(id);
        } else {
          overlayable_chunk->policy_ids.insert(
              std::make_pair(item.policies, std::set<ResourceId>{id}));
        }
      }
    }

    for (auto& overlayable_pair : overlayable_chunks) {
      std::string name = overlayable_pair.first;
      OverlayableChunk& overlayable = overlayable_pair.second;

      // Write the header of the overlayable chunk
      ChunkWriter overlayable_writer(buffer);
      auto* overlayable_type =
          overlayable_writer.StartChunk<ResTable_overlayable_header>(RES_TABLE_OVERLAYABLE_TYPE);
      if (name.size() >= arraysize(overlayable_type->name)) {
        diag_->Error(android::DiagMessage()
                     << "overlayable name '" << name << "' exceeds maximum length ("
                     << arraysize(overlayable_type->name) << " utf16 characters)");
        return false;
      }
      strcpy16_htod(overlayable_type->name, arraysize(overlayable_type->name),
                    android::util::Utf8ToUtf16(name));

      if (overlayable.actor.size() >= arraysize(overlayable_type->actor)) {
        diag_->Error(android::DiagMessage()
                     << "overlayable name '" << overlayable.actor << "' exceeds maximum length ("
                     << arraysize(overlayable_type->actor) << " utf16 characters)");
        return false;
      }
      strcpy16_htod(overlayable_type->actor, arraysize(overlayable_type->actor),
                    android::util::Utf8ToUtf16(overlayable.actor));

      // Write each policy block for the overlayable
      for (auto& policy_ids : overlayable.policy_ids) {
        ChunkWriter policy_writer(buffer);
        auto* policy_type = policy_writer.StartChunk<ResTable_overlayable_policy_header>(
            RES_TABLE_OVERLAYABLE_POLICY_TYPE);
        policy_type->policy_flags = static_cast<PolicyFlags>(
            android::util::HostToDevice32(static_cast<uint32_t>(policy_ids.first)));
        policy_type->entry_count =
            android::util::HostToDevice32(static_cast<uint32_t>(policy_ids.second.size()));
        // Write the ids after the policy header
        auto* id_block = policy_writer.NextBlock<ResTable_ref>(policy_ids.second.size());
        for (const ResourceId& id : policy_ids.second) {
          id_block->ident = android::util::HostToDevice32(id.id);
          id_block++;
        }
        policy_writer.Finish();
      }
      overlayable_writer.Finish();
    }

    return true;
  }

  ResTable_typeSpec* FlattenTypeSpec(const ResourceTableTypeView& type,
                                     const std::vector<ResourceTableEntryView>& sorted_entries,
                                     BigBuffer* buffer) {
    ChunkWriter type_spec_writer(buffer);
    ResTable_typeSpec* spec_header =
        type_spec_writer.StartChunk<ResTable_typeSpec>(RES_TABLE_TYPE_SPEC_TYPE);
    spec_header->id = type.id.value();

    if (sorted_entries.empty()) {
      type_spec_writer.Finish();
      return spec_header;
    }

    // We can't just take the size of the vector. There may be holes in the
    // entry ID space.
    // Since the entries are sorted by ID, the last one will be the biggest.
    const size_t num_entries = sorted_entries.back().id.value() + 1;

    spec_header->entryCount = android::util::HostToDevice32(num_entries);

    // Reserve space for the masks of each resource in this type. These
    // show for which configuration axis the resource changes.
    uint32_t* config_masks = type_spec_writer.NextBlock<uint32_t>(num_entries);

    for (const ResourceTableEntryView& entry : sorted_entries) {
      // shadow entry 不写入 type_spec config_masks（不占 spec 槽位）
      if (entry.is_shadow) {
        continue;
      }
      const uint16_t entry_id = entry.id.value();

      // Populate the config masks for this entry.
      uint32_t& entry_config_masks = config_masks[entry_id];
      if (entry.visibility.level == Visibility::Level::kPublic) {
        entry_config_masks |= android::util::HostToDevice32(ResTable_typeSpec::SPEC_PUBLIC);
      }
      if (entry.visibility.staged_api) {
        entry_config_masks |= android::util::HostToDevice32(ResTable_typeSpec::SPEC_STAGED_API);
      }

      const size_t config_count = entry.values.size();
      for (size_t i = 0; i < config_count; i++) {
        const ConfigDescription& config = entry.values[i]->config;
        for (size_t j = i + 1; j < config_count; j++) {
          config_masks[entry_id] |=
              android::util::HostToDevice32(config.diff(entry.values[j]->config));
        }
      }
    }
    type_spec_writer.Finish();
    return spec_header;
  }

  bool FlattenTypes(BigBuffer* buffer) {
    size_t expected_type_id = 1;
    for (const ResourceTableTypeView& type : package_.types) {
      if (type.named_type.type == ResourceType::kStyleable ||
          type.named_type.type == ResourceType::kMacro) {
        // Styleables and macros are not real resource types.
        continue;
      }

      // If there is a gap in the type IDs, fill in the StringPool
      // with empty values until we reach the ID we expect.
      while (type.id.value() > expected_type_id) {
        std::stringstream type_name;
        type_name << "?" << expected_type_id;
        type_pool_.MakeRef(type_name.str());
        expected_type_id++;
      }
      expected_type_id++;
      type_pool_.MakeRef(type.named_type.to_string());

      // entries 为空时（如 legacy 过滤后），只写入 type name 到 pool 和空 TypeSpec
      if (type.entries.empty()) {
        FlattenTypeSpec(type, type.entries, buffer);
        continue;
      }

      const auto type_spec_header = FlattenTypeSpec(type, type.entries, buffer);
      if (!type_spec_header) {
        return false;
      }

      // Since the entries are sorted by ID, the last ID will be the largest.
      const size_t num_entries = type.entries.back().id.value() + 1;

      // The binary resource table lists resource entries for each
      // configuration.
      // We store them inverted, where a resource entry lists the values for
      // each
      // configuration available. Here we reverse this to match the binary
      // table.
      std::map<ConfigDescription, std::vector<FlatEntry>> config_to_entry_list_map;

      for (const ResourceTableEntryView& entry : type.entries) {
        // shadow entry：仅供 IDE 索引，不写入 arsc。
        // 跳过该 entry 不占用 entry ID（保持其他 entry 的 ID 不变，依赖 entry.id 已分配）。
        if (entry.is_shadow) {
          continue;
        }
        if (entry.staged_id) {
          aliases_.insert(std::make_pair(
              entry.staged_id.value().id.id,
              ResourceId(package_.id.value(), type.id.value(), entry.id.value()).id));
        }

        uint32_t local_key_index;
        auto onObfuscate = [this, &local_key_index, &entry](Obfuscator::Result obfuscatedResult,
                                                            const ResourceName& resource_name) {
          if (obfuscatedResult == Obfuscator::Result::Keep_ExemptionList) {
            local_key_index = (uint32_t)key_pool_.MakeRef(entry.name).index();
          } else if (obfuscatedResult == Obfuscator::Result::Keep_Overlayable) {
            // if the resource name of the specific entry is obfuscated and this
            // entry is in the overlayable list, the overlay can't work on this
            // overlayable at runtime because the name has been obfuscated in
            // resources.arsc during flatten operation.
            const OverlayableItem& item = entry.overlayable_item.value();
            context_->GetDiagnostics()->Warn(android::DiagMessage(item.overlayable->source)
                                             << "The resource name of overlayable entry '"
                                             << resource_name.to_string()
                                             << "' shouldn't be obfuscated in resources.arsc");

            local_key_index = (uint32_t)key_pool_.MakeRef(entry.name).index();
          } else {
            local_key_index =
                (uint32_t)key_pool_.MakeRef(Obfuscator::kObfuscatedResourceName).index();
          }
        };

        Obfuscator::ObfuscateResourceName(collapse_key_stringpool_, name_collapse_exemptions_,
                                          type.named_type, entry, onObfuscate);

        // Group values by configuration.
        for (auto& config_value : entry.values) {
          config_to_entry_list_map[config_value->config].push_back(
              FlatEntry{&entry, config_value->value.get(), local_key_index});
        }
      }

      // Flatten a configuration value.
      for (auto& entry : config_to_entry_list_map) {
        if (!FlattenConfig(type, entry.first, num_entries, &entry.second, buffer)) {
          return false;
        }
      }

      // And now we can update the type entries count in the typeSpec header.
      type_spec_header->typesCount = android::util::HostToDevice16(uint16_t(std::min<uint32_t>(
          config_to_entry_list_map.size(), std::numeric_limits<uint16_t>::max())));
    }
    return true;
  }

  void FlattenLibrarySpec(BigBuffer* buffer) {
    ChunkWriter lib_writer(buffer);
    ResTable_lib_header* lib_header =
        lib_writer.StartChunk<ResTable_lib_header>(RES_TABLE_LIBRARY_TYPE);

   // 计算条目数量：动态包自身 + 非标准 packageId 自身映射 + 共享库引用
    const bool is_dynamic = (package_.id.value() == 0x00);
    const bool is_non_standard = (!is_dynamic &&
        package_.id.value() != kFrameworkPackageId &&
        package_.id.value() != kAppPackageId);
    const size_t num_entries = (is_dynamic ? 1 : 0) + (is_non_standard ? 1 : 0) + shared_libs_->size();
    CHECK(num_entries > 0);

    lib_header->count = android::util::HostToDevice32(num_entries);

    ResTable_lib_entry* lib_entry = buffer->NextBlock<ResTable_lib_entry>(num_entries);
    if (is_dynamic) {
      // Add this package
      lib_entry->packageId = android::util::HostToDevice32(0x00);
      strcpy16_htod(lib_entry->packageName, arraysize(lib_entry->packageName),
                    android::util::Utf8ToUtf16(package_.name));
      ++lib_entry;
    }

    // 非标准 packageId 的自身映射（如 bundle 的 0x50）
    if (is_non_standard) {
      lib_entry->packageId = android::util::HostToDevice32(package_.id.value());
      strcpy16_htod(lib_entry->packageName, arraysize(lib_entry->packageName),
                    android::util::Utf8ToUtf16(package_.name));
      ++lib_entry;
    }

    for (auto& map_entry : *shared_libs_) {
      lib_entry->packageId = android::util::HostToDevice32(map_entry.first);
      strcpy16_htod(lib_entry->packageName, arraysize(lib_entry->packageName),
                    android::util::Utf8ToUtf16(map_entry.second));
      ++lib_entry;
    }
    lib_writer.Finish();
  }

  IAaptContext* context_;
  android::IDiagnostics* diag_;
  const ResourceTablePackageView package_;
  const ResourceTable::ReferencedPackages* shared_libs_;
  SparseEntriesMode sparse_entries_;
  bool compact_entries_;
  android::StringPool type_pool_;
  android::StringPool key_pool_;
  bool collapse_key_stringpool_;
  const std::set<ResourceName>& name_collapse_exemptions_;
  std::map<uint32_t, uint32_t> aliases_;
  bool deduplicate_entry_values_;
};

}  // namespace

bool TableFlattener::Consume(IAaptContext* context, ResourceTable* table) {
  TRACE_CALL();
  // We must do this before writing the resources, since the string pool IDs may change.
  table->string_pool.Prune();
  table->string_pool.Sort(
      [](const android::StringPool::Context& a, const android::StringPool::Context& b) -> int {
        int diff = util::compare(a.priority, b.priority);
        if (diff == 0) {
          diff = a.config.compare(b.config);
        }
        return diff;
      });

  // Write the ResTable header.
  auto table_view =
      table->GetPartitionedView(ResourceTableViewOptions{.create_alias_entries = true});
  ChunkWriter table_writer(buffer_);
  ResTable_header* table_header = table_writer.StartChunk<ResTable_header>(RES_TABLE_TYPE);

  // 判定 PackageView 在 legacy_entries 过滤后是否仍有实际 entry。
  // 用于同时校准 packageCount 与后续 flatten 循环的跳过条件。
  // 场景：bundle packageId != 0x7F + --legacy-public-xml 时，
  //   PhantomEntrySynthesizer 会为未在 final_table_ 中出现的 legacy entry
  //   合成 id=0x7F.. 的占位条目，GetPartitionedView 把它们拆到独立的 0x7F
  //   PackageView；这些占位条目全部在 legacy_entries 过滤名单中，过滤后
  //   该 PackageView 的所有 type 都变成空 entries（仅剩 17 个空 TypeSpec 骨架）。
  //   此类空壳不承载任何资源信息，且真实数据已由 FlattenLegacyPackage 输出，
  //   故直接跳过；对普通场景（无 legacy_entries、或过滤后仍有 entry）无副作用。
  auto package_has_entries = [&](const ResourceTablePackageView& pkg) -> bool {
    if (options_.legacy_entries.empty()) return true;
    std::set<std::pair<std::string, std::string>> legacy_names;
    for (const auto& le : options_.legacy_entries) {
      legacy_names.insert({le.type_name, le.entry_name});
    }
    for (const auto& type : pkg.types) {
      std::string type_name = type.named_type.to_string();
      for (const auto& entry : type.entries) {
        if (legacy_names.count({type_name, std::string(entry.name)}) == 0) {
          return true;
        }
      }
    }
    return false;
  };

  // 计算 packageCount：非空 PackageView + 可选的 legacy PackageChunk。
  uint32_t non_empty_view_count = 0;
  for (const auto& package : table_view.packages) {
    if (package_has_entries(package)) non_empty_view_count++;
  }
  uint32_t total_packages = non_empty_view_count;
  if (!options_.legacy_entries.empty()) {
    total_packages += 1;  // FlattenLegacyPackage 额外输出的 legacy PackageChunk
  }
  table_header->packageCount = android::util::HostToDevice32(total_packages);

  // Flatten the values string pool.
  android::StringPool::FlattenUtf8(table_writer.buffer(), table->string_pool,
                                   context->GetDiagnostics());

  android::BigBuffer package_buffer(1024);

  // 先 flatten legacy 0x7F PackageChunk（在过滤 target 包之前，因为 legacy 包需要从 target 查找 entry 值）
  if (!options_.legacy_entries.empty() && !table_view.packages.empty()) {
    if (!FlattenLegacyPackage(context, table, table_view, &package_buffer)) {
      return false;
    }
  }

  // 从所有包中排除 legacy entry：
  // Link.cpp 在 IdAssigner 前移除了 legacy entry（避免 ID 碰撞），之后恢复了 legacy entry
  // 并设置了 legacy 声明的 ID（供 ReferenceLinker 解析引用）。
  // GetPartitionedView 按 entry->id 的 package_id 分配到不同 PackageView：
  //   - legacy entry（id=0x7F...）→ 0x7F PackageView
  //   - non-legacy entry（id=0x50...）→ 0x50 PackageView
  // 但 Portal 宿主本身也是 0x7F，所以宿主自身 entries 和 legacy entries 在同一 PackageView。
  // 此处从所有 PackageView 中过滤掉 legacy entries，保留宿主自身资源。
  if (!options_.legacy_entries.empty()) {
    std::set<std::pair<std::string, std::string>> legacy_names;
    for (const auto& le : options_.legacy_entries) {
      legacy_names.insert({le.type_name, le.entry_name});
    }
    for (auto& package : table_view.packages) {
      for (auto it = package.types.begin(); it != package.types.end(); ) {
        std::string type_name = it->named_type.to_string();
        auto& entries = it->entries;
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                [&](const ResourceTableEntryView& e) {
                  return legacy_names.count({type_name, std::string(e.name)}) > 0;
                }),
            entries.end());
        // 即使 entries 为空也保留 type，确保 Type StringPool 包含正确的 type name
        ++it;
      }
    }
  }

  // Flatten each package (legacy entries 已从各 PackageView 中过滤).
  for (auto& package : table_view.packages) {
    // 跳过过滤后无实际 entry 的 PackageView（PhantomEntrySynthesizer 造成的空 0x7F 残壳）。
    // packageCount 已在 header 阶段同步扣减，此处仅需省略 flatten。
    bool has_entries = std::any_of(
        package.types.begin(), package.types.end(),
        [](const ResourceTableTypeView& t) { return !t.entries.empty(); });
    if (!has_entries) continue;

    if (context->GetPackageType() == PackageType::kApp) {
      // Write a self mapping entry for this package if the ID is non-standard (0x7f).
      CHECK((bool)package.id) << "Resource ids have not been assigned before flattening the table";
      const uint8_t package_id = package.id.value();
      if (package_id != kFrameworkPackageId && package_id != kAppPackageId) {
        auto result = table->included_packages_.insert({package_id, package.name});
        if (!result.second && result.first->second != package.name) {
          // A mapping for this package ID already exists, and is a different package. Error!
          context->GetDiagnostics()->Error(
              android::DiagMessage() << android::base::StringPrintf(
                  "can't map package ID %02x to '%s'. Already mapped to '%s'", package_id,
                  package.name.c_str(), result.first->second.c_str()));
          return false;
        }
      }
    }

    PackageFlattener flattener(context, package, &table->included_packages_,
                               options_.sparse_entries,
                               options_.use_compact_entries,
                               options_.collapse_key_stringpool,
                               options_.name_collapse_exemptions,
                               options_.deduplicate_entry_values);
    if (!flattener.FlattenPackage(&package_buffer)) {
      return false;
    }
  }

  // Finally merge all the packages into the main buffer.
  table_writer.buffer()->AppendBuffer(std::move(package_buffer));
  table_writer.Finish();
  return true;
}

bool TableFlattener::FlattenLegacyPackage(IAaptContext* context, ResourceTable* table,
                                           const ResourceTableView& table_view,
                                           android::BigBuffer* package_buffer) {
  // 按 type_name 分组 legacy entries
  struct LegacyTypeInfo {
    uint8_t type_id;
    std::vector<const TableFlattenerOptions::LegacyPublicEntry*> entries;
  };
  std::map<std::string, LegacyTypeInfo> legacy_types;
  for (const auto& le : options_.legacy_entries) {
    auto& ti = legacy_types[le.type_name];
    ti.type_id = le.type_id;
    ti.entries.push_back(&le);
  }

  // 找到源 package（目标包）
  const ResourceTablePackageView* src_pkg = nullptr;
  for (const auto& pkg : table_view.packages) {
    if (pkg.id.has_value() && pkg.id.value() != kFrameworkPackageId &&
        pkg.id.value() != kAppPackageId) {
      src_pkg = &pkg;
      break;
    }
  }
  if (!src_pkg && !table_view.packages.empty()) {
    src_pkg = &table_view.packages[0];
  }
  if (!src_pkg) {
    context->GetDiagnostics()->Error(android::DiagMessage()
        << "no source package found for legacy 0x7F package construction");
    return false;
  }

  // 构建从 (type_name, entry_name) 到 source entry view 的索引。
  // 由于 legacy entry 的 id 以 0x7F 为 package_id，GetPartitionedView 会将它们
  // 放到一个独立的 0x7F PackageView 中（而非 target 包的 view），
  // 所以需要从所有 packages 中收集 entry，确保能找到 legacy entry 的 values。
  std::map<std::pair<std::string, std::string>, const ResourceTableEntryView*> src_entry_index;
  for (const auto& pkg : table_view.packages) {
    for (const auto& type : pkg.types) {
      std::string type_name = type.named_type.to_string();
      for (const auto& entry : type.entries) {
        src_entry_index[{type_name, entry.name}] = &entry;
      }
    }
  }

  // 构建 legacy 0x7F PackageView
  ResourceTablePackageView legacy_pkg;
  legacy_pkg.name = options_.legacy_package_name.empty()
      ? src_pkg->name : options_.legacy_package_name;
  legacy_pkg.id = kAppPackageId;  // 0x7F

  // 按 type_id 排序构建 types
  std::map<uint8_t, ResourceTableTypeView> type_map;
  for (const auto& [type_name, type_info] : legacy_types) {
    auto& type_view = type_map[type_info.type_id];
    if (!type_view.id.has_value()) {
      type_view.id = type_info.type_id;
      type_view.named_type = ResourceNamedTypeWithDefaultName(ResourceType::kRaw).ToResourceNamedType();
      // 从所有 packages 中查找正确的 named_type（legacy entry 可能在 0x7F PackageView 中）
      bool found_type = false;
      for (const auto& pkg : table_view.packages) {
        for (const auto& src_type : pkg.types) {
          if (src_type.named_type.to_string() == type_name) {
            type_view.named_type = src_type.named_type;
            found_type = true;
            break;
          }
        }
        if (found_type) break;
      }
      // 所有 legacy 资源标记为 public
      type_view.visibility_level = Visibility::Level::kPublic;
    }

    // 添加 entries
    for (const auto* le : type_info.entries) {
      auto src_it = src_entry_index.find({le->type_name, le->entry_name});
      // 复制 entry view，使用 legacy 的 entry_id，标记为 public。
      // 若源包中找不到 entry 值（Portal 宿主场景：legacy 资源定义在 bundle 中，
      // 宿主自身 ResourceTable 无此资源），输出空 entry（values 为空），
      // PackageFlattener 会将其写为 NO_ENTRY（offset=-1），
      // Portal 合并阶段在空位注入 bundle 真实数据。
      ResourceTableEntryView ev;
      ev.name = le->entry_name;
      ev.id = le->entry_id;
      ev.visibility.level = Visibility::Level::kPublic;
      if (src_it != src_entry_index.end()) {
        ev.values = src_it->second->values;
      }
      type_view.entries.push_back(std::move(ev));
    }
  }

  // 按 entry_id 排序每个 type 的 entries，跳过空 type
  for (auto& [tid, type_view] : type_map) {
    if (type_view.entries.empty()) continue;
    std::sort(type_view.entries.begin(), type_view.entries.end(),
        [](const ResourceTableEntryView& a, const ResourceTableEntryView& b) {
          return a.id.value() < b.id.value();
        });
    legacy_pkg.types.push_back(std::move(type_view));
  }

  // legacy 0x7F 包中的 entry value 引用仍使用 target packageId（如 0x50），
  // 通过 LibraryChunk 注册 target→package_name 映射，
  // 使 DynamicRefTable 能在 -I 场景下正确解析这些引用。
  // 运行时 Portal 会将 bundle binary XML 和 arsc 中的 legacy 引用从 0x50 重映射到 0x7F，
  // 确保 legacy entry 只通过 0x7F PackageGroup 解析。
  ResourceTable::ReferencedPackages legacy_libs;
  if (src_pkg->id.has_value()) {
    legacy_libs[src_pkg->id.value()] = src_pkg->name;
  }

  // Flatten legacy 0x7F 包
  PackageFlattener legacy_flattener(context, legacy_pkg, &legacy_libs,
                                     SparseEntriesMode::Disabled,
                                     false,  // compact_entries
                                     false,  // collapse_key_stringpool
                                     options_.name_collapse_exemptions,
                                     false);  // deduplicate_entry_values
  if (!legacy_flattener.FlattenPackage(package_buffer)) {
    context->GetDiagnostics()->Error(android::DiagMessage()
        << "failed to flatten legacy 0x7F package");
    return false;
  }

  context->GetDiagnostics()->Note(android::DiagMessage()
      << "legacy 0x7F package flattened with " << options_.legacy_entries.size()
      << " entries, " << legacy_types.size() << " types");
  return true;
}

}  // namespace aapt
