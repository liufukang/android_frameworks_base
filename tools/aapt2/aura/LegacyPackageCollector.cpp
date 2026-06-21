/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aura/LegacyPackageCollector.h"

#include <set>
#include <string>
#include <utility>

#include "ResourceUtils.h"
#include "android-base/stringprintf.h"
#include "androidfw/FileStream.h"
#include "xml/XmlDom.h"
#include "xml/XmlUtil.h"

namespace aapt {
namespace aura {

bool LegacyPackageCollector::ParseLegacyPublicXml(
    const std::string& path,
    android::IDiagnostics* diag,
    std::vector<LegacyEntry>* out_entries) {
  // 加载 XML
  android::FileInputStream fin(path);
  if (fin.HadError()) {
    diag->Error(android::DiagMessage(path)
                << "failed to open --legacy-public-xml: " << fin.GetError());
    return false;
  }
  auto xml = xml::Inflate(&fin, diag, android::Source(path));
  if (!xml) {
    diag->Error(android::DiagMessage()
                << "failed to parse --legacy-public-xml: " << path);
    return false;
  }

  xml::Element* root_el = xml::FindRootElement(xml->root.get());
  if (!root_el || root_el->name != "resources") {
    diag->Error(android::DiagMessage()
                << "--legacy-public-xml root element must be <resources>, got: "
                << (root_el ? root_el->name : "(null)"));
    return false;
  }

  for (const xml::Element* child_el : root_el->GetChildElements()) {
    if (child_el->name != "public") continue;
    const xml::Attribute* type_attr = child_el->FindAttribute({}, "type");
    const xml::Attribute* name_attr = child_el->FindAttribute({}, "name");
    const xml::Attribute* id_attr = child_el->FindAttribute({}, "id");
    if (!type_attr || !name_attr || !id_attr) {
      diag->Error(android::DiagMessage(android::Source(path).WithLine(child_el->line_number))
                  << "<public> element requires type, name, and id attributes");
      return false;
    }

    auto maybe_id = ResourceUtils::ParseInt(id_attr->value);
    if (!maybe_id) {
      diag->Error(android::DiagMessage(android::Source(path).WithLine(child_el->line_number))
                  << "invalid resource ID: " << id_attr->value);
      return false;
    }

    uint32_t full_id = static_cast<uint32_t>(maybe_id.value());
    uint8_t pkg_id = (full_id >> 24) & 0xFF;
    if (pkg_id != 0x7F) {
      diag->Error(android::DiagMessage(android::Source(path).WithLine(child_el->line_number))
                  << "legacy public entry must have package ID 0x7F, got: "
                  << android::base::StringPrintf("0x%02x", pkg_id));
      return false;
    }

    uint8_t type_id = (full_id >> 16) & 0xFF;
    uint16_t entry_id = full_id & 0xFFFF;
    LegacyEntry entry;
    entry.type_name = type_attr->value;
    entry.entry_name = name_attr->value;
    entry.type_id = type_id;
    entry.entry_id = entry_id;
    out_entries->push_back(std::move(entry));
  }

  diag->Note(android::DiagMessage()
             << "--legacy-public-xml: " << out_entries->size() << " entries loaded from " << path);
  return true;
}

size_t LegacyPackageCollector::AutoDeriveFromIncludes(
    AssetManagerSymbolSource* asset_source,
    std::vector<LegacyEntry>* out_entries,
    android::IDiagnostics* diag) {
  auto entries_7f = asset_source->GetAll7fResources();

  // 用 (type, name) 去重，避免与已有的 --legacy-public-xml 条目重复
  std::set<std::pair<std::string, std::string>> seen;
  for (const auto& le : *out_entries) {
    seen.emplace(le.type_name, le.entry_name);
  }

  size_t added = 0;
  for (const auto& [name, id] : entries_7f) {
    std::string type_name = name.type.to_string();
    if (!seen.emplace(type_name, name.entry).second) continue;
    LegacyEntry entry;
    entry.type_name = type_name;
    entry.entry_name = name.entry;
    entry.type_id = (id.id >> 16) & 0xFF;
    entry.entry_id = id.id & 0xFFFF;
    out_entries->push_back(std::move(entry));
    ++added;
  }

  if (added > 0) {
    diag->Note(android::DiagMessage()
               << "auto-legacy-from-include: " << added
               << " 0x7F entries derived from -I (packageId=0x7F)");
  }
  return added;
}

}  // namespace aura
}  // namespace aapt
