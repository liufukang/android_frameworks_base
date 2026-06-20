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

#include "process/SymbolTable.h"

#include <iostream>
#include <set>

#include "android-base/logging.h"
#include "android-base/stringprintf.h"
#include "androidfw/Asset.h"
#include "androidfw/AssetManager2.h"
#include "androidfw/ConfigDescription.h"
#include "androidfw/ResourceTypes.h"
#include "androidfw/ResourceUtils.h"

#include "NameMangler.h"
#include "Resource.h"
#include "ResourceUtils.h"
#include "ValueVisitor.h"
#include "trace/TraceBuffer.h"
#include "util/Util.h"

using ::android::ApkAssets;
using ::android::ConfigDescription;
using ::android::StringPiece;
using ::android::StringPiece16;

namespace aapt {

SymbolTable::SymbolTable(NameMangler* mangler)
    : mangler_(mangler),
      delegate_(util::make_unique<DefaultSymbolTableDelegate>()),
      cache_(200),
      id_cache_(200) {
}

void SymbolTable::SetDelegate(std::unique_ptr<ISymbolTableDelegate> delegate) {
  CHECK(delegate != nullptr) << "can't set a nullptr delegate";
  delegate_ = std::move(delegate);

  // Clear the cache in case this delegate changes the order of lookup.
  cache_.clear();
}

void SymbolTable::AppendSource(std::unique_ptr<ISymbolSource> source) {
  sources_.push_back(std::move(source));

  // We do not clear the cache, because sources earlier in the list take
  // precedent.
}

void SymbolTable::PrependSource(std::unique_ptr<ISymbolSource> source) {
  sources_.insert(sources_.begin(), std::move(source));

  // We must clear the cache in case we did a lookup before adding this
  // resource.
  cache_.clear();
}

const SymbolTable::Symbol* SymbolTable::FindByName(const ResourceName& name) {
  const ResourceName* name_with_package = &name;

  // Fill in the package name if necessary.
  // If there is no package in `name`, we will need to copy the ResourceName
  // and store it somewhere; we use the std::optional<> class to reserve storage.
  std::optional<ResourceName> name_with_package_impl;
  if (name.package.empty()) {
    name_with_package_impl = ResourceName(mangler_->GetTargetPackageName(), name.type, name.entry);
    name_with_package = &name_with_package_impl.value();
  }

  // We store the name unmangled in the cache, so look it up as-is.
  if (const std::shared_ptr<Symbol>& s = cache_.get(*name_with_package)) {
    return s.get();
  }

  // The name was not found in the cache. Mangle it (if necessary) and find it in our sources.
  // Again, here we use a std::optional<> object to reserve storage if we need to mangle.
  const ResourceName* mangled_name = name_with_package;
  std::optional<ResourceName> mangled_name_impl;
  if (mangler_->ShouldMangle(name_with_package->package)) {
    mangled_name_impl = mangler_->MangleName(*name_with_package);
    mangled_name = &mangled_name_impl.value();
  }

  std::unique_ptr<Symbol> symbol = delegate_->FindByName(*mangled_name, sources_);
  if (symbol == nullptr) {
    return nullptr;
  }

  // Take ownership of the symbol into a shared_ptr. We do this because
  // LruCache doesn't support unique_ptr.
  std::shared_ptr<Symbol> shared_symbol(std::move(symbol));

  // Since we look in the cache with the unmangled, but package prefixed
  // name, we must put the same name into the cache.
  cache_.put(*name_with_package, shared_symbol);

  if (shared_symbol->id) {
    // The symbol has an ID, so we can also cache this!
    id_cache_.put(shared_symbol->id.value(), shared_symbol);
  }

  // Returns the raw pointer. Callers are not expected to hold on to this
  // between calls to Find*.
  return shared_symbol.get();
}

const SymbolTable::Symbol* SymbolTable::FindByNameNoMangle(const ResourceName& name) {
  // 直接用原始名称查找，跳过 mangling
  if (const std::shared_ptr<Symbol>& s = cache_.get(name)) {
    return s.get();
  }

  std::unique_ptr<Symbol> symbol = delegate_->FindByName(name, sources_);
  if (symbol == nullptr) {
    return nullptr;
  }

  std::shared_ptr<Symbol> shared_symbol(std::move(symbol));
  cache_.put(name, shared_symbol);

  if (shared_symbol->id) {
    id_cache_.put(shared_symbol->id.value(), shared_symbol);
  }

  return shared_symbol.get();
}

const SymbolTable::Symbol* SymbolTable::FindById(const ResourceId& id) {
  if (const std::shared_ptr<Symbol>& s = id_cache_.get(id)) {
    return s.get();
  }

  // We did not find it in the cache, so look through the sources.
  std::unique_ptr<Symbol> symbol = delegate_->FindById(id, sources_);
  if (symbol == nullptr) {
    return nullptr;
  }

  // Take ownership of the symbol into a shared_ptr. We do this because LruCache
  // doesn't support unique_ptr.
  std::shared_ptr<Symbol> shared_symbol(std::move(symbol));
  id_cache_.put(id, shared_symbol);

  // Returns the raw pointer. Callers are not expected to hold on to this
  // between calls to Find*.
  return shared_symbol.get();
}

const SymbolTable::Symbol* SymbolTable::FindByReference(const Reference& ref) {
  // First try the ID. This is because when we lookup by ID, we only fill in the ID cache.
  // Looking up by name fills in the name and ID cache. So a cache miss will cause a failed
  // ID lookup, then a successful name lookup. Subsequent look ups will hit immediately
  // because the ID is cached too.
  //
  // If we looked up by name first, a cache miss would mean we failed to lookup by name, then
  // succeeded to lookup by ID. Subsequent lookups will miss then hit.
  const SymbolTable::Symbol* symbol = nullptr;
  if (ref.id) {
    symbol = FindById(ref.id.value());
  }

  if (ref.name && !symbol) {
    symbol = FindByName(ref.name.value());
  }
  return symbol;
}

std::unique_ptr<SymbolTable::Symbol> DefaultSymbolTableDelegate::FindByName(
    const ResourceName& name, const std::vector<std::unique_ptr<ISymbolSource>>& sources) {
  for (auto& source : sources) {
    std::unique_ptr<SymbolTable::Symbol> symbol = source->FindByName(name);
    if (symbol) {
      return symbol;
    }
  }
  return {};
}

std::unique_ptr<SymbolTable::Symbol> DefaultSymbolTableDelegate::FindById(
    ResourceId id, const std::vector<std::unique_ptr<ISymbolSource>>& sources) {
  for (auto& source : sources) {
    std::unique_ptr<SymbolTable::Symbol> symbol = source->FindById(id);
    if (symbol) {
      return symbol;
    }
  }
  return {};
}

std::unique_ptr<SymbolTable::Symbol> ResourceTableSymbolSource::FindByName(
    const ResourceName& name) {
  std::optional<ResourceTable::SearchResult> result = table_->FindResource(name);
  if (!result) {
    if (name.type.type == ResourceType::kAttr) {
      // Recurse and try looking up a private attribute.
      return FindByName(ResourceName(name.package, ResourceType::kAttrPrivate, name.entry));
    }
    return {};
  }

  ResourceTable::SearchResult sr = result.value();

  std::unique_ptr<SymbolTable::Symbol> symbol = util::make_unique<SymbolTable::Symbol>();
  symbol->is_public = (sr.entry->visibility.level == Visibility::Level::kPublic);
  symbol->is_shadow = sr.entry->is_shadow;

  if (sr.entry->id) {
    symbol->id = sr.entry->id.value();
    symbol->is_dynamic =
        (sr.entry->id.value().package_id() == 0) || sr.entry->visibility.staged_api;
  }

  if (name.type.type == ResourceType::kAttr || name.type.type == ResourceType::kAttrPrivate) {
    const ConfigDescription kDefaultConfig;
    ResourceConfigValue* config_value = sr.entry->FindValue(kDefaultConfig);
    if (config_value) {
      // This resource has an Attribute.
      if (Attribute* attr = ValueCast<Attribute>(config_value->value.get())) {
        symbol->attribute = std::make_shared<Attribute>(*attr);
      } else {
        return {};
      }
    } else if (sr.entry->values.empty()) {
      // 预填充的空 attr 条目（只有 pinned ID，无 Attribute 值）：
      // 返回 nullptr 让 SymbolTable 回退到 AssetManagerSymbolSource 提供完整 Attribute 信息。
      // entry 的 pinned ID 仍由 IdAssigner 在 ResourceTable 层面正确处理。
      return {};
    }
  }
  return symbol;
}

bool AssetManagerSymbolSource::AddAssetPath(StringPiece path) {
  TRACE_CALL();
  if (auto apk = ApkAssets::Load(path.data())) {
    apk_assets_.push_back(std::move(apk));
    asset_manager_.SetApkAssets(apk_assets_);
    return true;
  }
  return false;
}

std::map<size_t, std::string> AssetManagerSymbolSource::GetAssignedPackageIds() const {
  TRACE_CALL();
  std::map<size_t, std::string> package_map;
  asset_manager_.ForEachPackage([&package_map](const std::string& name, uint8_t id) -> bool {
    package_map.insert(std::make_pair(id, name));
    return true;
  });

  return package_map;
}

std::vector<std::string> AssetManagerSymbolSource::GetAllPackageNames() const {
  // 收集所有 include 包的包名（包括 0x7F），用于 fallback 搜索。
  // bundle 的 0x7F 资源会在 host 编译时被引用，后续由 RemapHostResourceIdsTask 重映射 ID
  std::vector<std::string> names;
  std::set<std::string> seen;
  asset_manager_.ForEachPackage([&names, &seen](const std::string& name, uint8_t id) -> bool {
    if (seen.insert(name).second) {
      names.push_back(name);
    }
    return true;
  });
  return names;
}

bool AssetManagerSymbolSource::IsPackageDynamic(uint32_t packageId,
    const std::string& package_name) const {
  if (packageId == 0) {
    return true;
  }

  for (auto&& assets : apk_assets_) {
    for (const std::unique_ptr<const android::LoadedPackage>& loaded_package
         : assets->GetLoadedArsc()->GetPackages()) {
      if (package_name == loaded_package->GetPackageName() && loaded_package->IsDynamic()) {
        return true;
      }
    }
  }

  return false;
}

static std::unique_ptr<SymbolTable::Symbol> LookupAttributeInTable(
    android::AssetManager2& am, ResourceId id) {
  using namespace android;
  if (am.GetApkAssetsCount() == 0) {
    return {};
  }

  auto op = am.StartOperation();
  auto bag_result = am.GetBag(id.id);
  if (!bag_result.has_value()) {
    return nullptr;
  }

  // We found a resource.
  std::unique_ptr<SymbolTable::Symbol> s = util::make_unique<SymbolTable::Symbol>(id);
  const ResolvedBag* bag = *bag_result;
  const size_t count = bag->entry_count;
  for (uint32_t i = 0; i < count; i++) {
    if (bag->entries[i].key == ResTable_map::ATTR_TYPE) {
      s->attribute = std::make_shared<Attribute>(bag->entries[i].value.data);
      break;
    }
  }

  if (s->attribute) {
    for (size_t i = 0; i < count; i++) {
      const ResolvedBag::Entry& map_entry = bag->entries[i];
      if (Res_INTERNALID(map_entry.key)) {
        switch (map_entry.key) {
          case ResTable_map::ATTR_MIN:
            s->attribute->min_int = static_cast<int32_t>(map_entry.value.data);
            break;
          case ResTable_map::ATTR_MAX:
            s->attribute->max_int = static_cast<int32_t>(map_entry.value.data);
            break;
        }
        continue;
      }

      auto name = am.GetResourceName(map_entry.key);
      if (!name.has_value()) {
        // 当 -I 包含双包 bundle（0x7F + target）时，0x7F 包中 attr 的 enum/flag symbol
        // 可能引用 target 包中已被排除的 legacy entry（如 id/parent），
        // 此时 GetResourceName 会失败。跳过此 symbol 而非中止整个 attr 解析，
        // 因为 attr 的 type mask 已从 ATTR_TYPE 获取，缺失的 symbol 不影响编译
        continue;
      }

      std::optional<ResourceName> parsed_name = ResourceUtils::ToResourceName(*name);
      if (!parsed_name) {
        return nullptr;
      }

      Attribute::Symbol symbol;
      symbol.symbol.name = parsed_name.value();
      symbol.symbol.id = ResourceId(map_entry.key);
      symbol.value = map_entry.value.data;
      symbol.type = map_entry.value.dataType;
      s->attribute->symbols.push_back(std::move(symbol));
    }
  }

  return s;
}

std::unique_ptr<SymbolTable::Symbol> AssetManagerSymbolSource::FindByName(
    const ResourceName& name) {
  const std::string mangled_entry = NameMangler::MangleEntry(name.package, name.entry);

  bool found = false;
  ResourceId res_id = 0;
  uint32_t type_spec_flags = 0;
  ResourceName real_name;

  // There can be mangled resources embedded within other packages. Here we will
  // look into each package and look-up the mangled name until we find the resource.
  asset_manager_.ForEachPackage([&](const std::string& package_name, uint8_t id) -> bool {
    real_name = ResourceName(name.package, name.type, name.entry);
    if (package_name != name.package) {
      real_name.entry = mangled_entry;
      real_name.package = package_name;
    }

    auto real_res_id = asset_manager_.GetResourceId(real_name.to_string());
    if (!real_res_id.has_value()) {
      return true;
    }

    res_id.id = *real_res_id;
    if (!res_id.is_valid_static()) {
      return true;
    }

    auto flags = asset_manager_.GetResourceTypeSpecFlags(res_id.id);
    if (flags.has_value()) {
      type_spec_flags = *flags;
      found = true;
      return false;
    }

    return true;
  });

  if (!found) {
    return {};
  }

  std::unique_ptr<SymbolTable::Symbol> s;
  if (real_name.type.type == ResourceType::kAttr) {
    s = LookupAttributeInTable(asset_manager_, res_id);
  } else {
    s = util::make_unique<SymbolTable::Symbol>();
    s->id = res_id;
  }

  if (s) {
    s->is_public = (type_spec_flags & android::ResTable_typeSpec::SPEC_PUBLIC) != 0;
    s->is_dynamic = IsPackageDynamic(ResourceId(res_id).package_id(), real_name.package) ||
                    (type_spec_flags & android::ResTable_typeSpec::SPEC_STAGED_API) != 0;
    return s;
  }
  return {};
}

static std::optional<ResourceName> GetResourceName(android::AssetManager2& am, ResourceId id) {
  auto name = am.GetResourceName(id.id);
  if (!name.has_value()) {
    return {};
  }
  return ResourceUtils::ToResourceName(*name);
}

std::unique_ptr<SymbolTable::Symbol> AssetManagerSymbolSource::FindById(
    ResourceId id) {
  if (!id.is_valid_static()) {
    // Exit early and avoid the error logs from AssetManager.
    return {};
  }

  if (apk_assets_.empty()) {
    return {};
  }

  std::optional<ResourceName> maybe_name = GetResourceName(asset_manager_, id);
  if (!maybe_name) {
    return {};
  }

  auto flags = asset_manager_.GetResourceTypeSpecFlags(id.id);
  if (!flags.has_value()) {
    return {};
  }

  ResourceName& name = maybe_name.value();
  std::unique_ptr<SymbolTable::Symbol> s;
  if (name.type.type == ResourceType::kAttr) {
    s = LookupAttributeInTable(asset_manager_, id);
  } else {
    s = util::make_unique<SymbolTable::Symbol>();
    s->id = id;
  }

  if (s) {
    s->is_public = (*flags & android::ResTable_typeSpec::SPEC_PUBLIC) != 0;
    s->is_dynamic = IsPackageDynamic(ResourceId(id).package_id(), name.package) ||
                    (*flags & android::ResTable_typeSpec::SPEC_STAGED_API) != 0;
    return s;
  }
  return {};
}

std::unique_ptr<SymbolTable::Symbol> AssetManagerSymbolSource::FindByReference(
    const Reference& ref) {
  // AssetManager always prefers IDs.
  if (ref.id) {
    return FindById(ref.id.value());
  } else if (ref.name) {
    return FindByName(ref.name.value());
  }
  return {};
}

std::vector<std::pair<ResourceName, ResourceId>>
AssetManagerSymbolSource::GetAll7fResources() const {
  // 遍历所有 -I 加载的 include 包，收集 package_id == 0x7F 的全部资源
  // 这些资源需要预填充到 host ResourceTable，让 IdAssigner 分配 host 体系 ID
  //
  // 关键：直接走 LoadedArsc → LoadedPackage → TypeSpec → key string pool 路径，
  // **不要**经 AssetManager2::GetResourceName。因为 AssetManager2 的解析受 PackageGroup
  // 路由约束：当多个 .bundle 都使用 0x7F 时，AssetManager2 的 0x7F PackageGroup 只会
  // 路由到第一个加载的 0x7F 包，后续 0x7F 资源会因可见性/路由问题查不到名字（覆盖率
  // 实测仅约 2162/N），而 LoadedArsc 直接读 chunk 数据可拿到全部名字。
  constexpr uint8_t kAppPackageId = 0x7F;
  std::vector<std::pair<ResourceName, ResourceId>> result;

  for (const auto& assets : apk_assets_) {
    for (const auto& loaded_package : assets->GetLoadedArsc()->GetPackages()) {
      if (loaded_package->GetPackageId() != kAppPackageId) {
        continue;
      }

      const android::ResStringPool* type_pool = loaded_package->GetTypeStringPool();
      const android::ResStringPool* key_pool = loaded_package->GetKeyStringPool();
      if (type_pool == nullptr || key_pool == nullptr) continue;

      // 通过 ForEachTypeSpec 遍历所有 type，按 (type, entry_index) 输出真实存在的 entry。
      // type_id 是 1-based 内部索引（已减去 type_id_offset_），需要从 ResTable_type chunk
      // 的 header.id 字段读出 effective type id（含 offset）才能拼出正确 ResourceId。
      loaded_package->ForEachTypeSpec(
          [&](const android::TypeSpec& type_spec, uint8_t internal_type_id) {
            auto type_name_str16_result = type_pool->stringAt(
                static_cast<size_t>(internal_type_id - 1));
            if (!type_name_str16_result.ok()) return;
            std::string type_name = android::util::Utf16ToUtf8(*type_name_str16_result);

            // 跳过 attr-private（aapt2 内部类型，运行时不使用）
            if (type_name == "^attr-private") return;

            uint16_t entry_count = dtohs(type_spec.type_spec->entryCount);
            if (entry_count == 0) return;

            // effective type id 从 type_entries[0] 的 ResTable_type header.id 取
            uint8_t effective_type_id = 0;
            if (!type_spec.type_entries.empty() && type_spec.type_entries[0].type) {
              effective_type_id = type_spec.type_entries[0].type->id;
            }
            if (effective_type_id == 0) return;

            // 收集每个 entry：扫所有 type_entries（不同 config 的 ResTable_type chunks），
            // 取并集（任一 config 中存在 entry 就视为存在）
            std::vector<bool> entry_present(entry_count, false);
            std::vector<uint32_t> entry_key_indices(entry_count, 0xFFFFFFFFu);
            for (const auto& te : type_spec.type_entries) {
              const auto& tchunk = te.type;
              if (!tchunk) continue;
              uint32_t this_entry_count = dtohl(tchunk->entryCount);
              uint32_t entries_start = dtohl(tchunk->entriesStart);
              const uint8_t flags = tchunk->flags;
              const uint8_t* chunk_base =
                  reinterpret_cast<const uint8_t*>(tchunk.unsafe_ptr());
              for (uint32_t i = 0;
                   i < this_entry_count && i < entry_count;
                   ++i) {
                // entry offset 表：32-bit 或 16-bit（FLAG_OFFSET16）
                uint32_t offset;
                if (flags & android::ResTable_type::FLAG_OFFSET16) {
                  const uint16_t* off16 = reinterpret_cast<const uint16_t*>(
                      chunk_base + dtohs(tchunk->header.headerSize));
                  uint16_t off = dtohs(off16[i]);
                  offset = (off == 0xFFFFu) ? 0xFFFFFFFFu : (off * 4u);
                } else {
                  const uint32_t* off32 = reinterpret_cast<const uint32_t*>(
                      chunk_base + dtohs(tchunk->header.headerSize));
                  offset = dtohl(off32[i]);
                }
                if (offset == 0xFFFFFFFFu) continue;
                // 从 entries_start + offset 取 ResTable_entry，读 key 字段
                // ResTable_entry 是 union（Full/Compact），key() 方法自动处理两种格式
                const uint8_t* entry_ptr = chunk_base + entries_start + offset;
                const auto* entry =
                    reinterpret_cast<const android::ResTable_entry*>(entry_ptr);
                uint32_t key_index = entry->key();
                entry_present[i] = true;
                if (entry_key_indices[i] == 0xFFFFFFFFu) {
                  entry_key_indices[i] = key_index;
                }
              }
            }

            // 输出
            for (uint32_t i = 0; i < entry_count; ++i) {
              if (!entry_present[i]) continue;
              uint32_t key_index = entry_key_indices[i];
              if (key_index == 0xFFFFFFFFu) continue;
              auto entry_name_str16_result = key_pool->stringAt(key_index);
              if (!entry_name_str16_result.ok()) continue;
              std::string entry_name = android::util::Utf16ToUtf8(*entry_name_str16_result);
              if (entry_name.empty()) continue;

              // ParseResourceType 返回 const ResourceType*；某些 type 名（如 ^attr-private）
              // 不被识别，跳过。前面已经过滤过 ^attr-private，但这里再保护一层。
              const ResourceType* parsed_type = ParseResourceType(type_name);
              if (parsed_type == nullptr) continue;

              ResourceName name(loaded_package->GetPackageName(),
                                *parsed_type,
                                entry_name);
              uint32_t resid =
                  (uint32_t(kAppPackageId) << 24) |
                  (uint32_t(effective_type_id) << 16) |
                  uint32_t(i);
              result.emplace_back(std::move(name), ResourceId(resid));
            }
          });
    }
  }
  return result;
}

}  // namespace aapt
