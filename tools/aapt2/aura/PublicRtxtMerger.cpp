/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aura/PublicRtxtMerger.h"

#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "text/Printer.h"

namespace aapt {
namespace aura {

size_t PublicRtxtMerger::Append(const std::string& merged_rtxt_content,
                                const std::string& compilation_package,
                                ResourceTable* table,
                                android::OutputStream* fout_text,
                                android::IDiagnostics* diag) {
  if (merged_rtxt_content.empty()) return 0;

  // 收集已写入的 (type, name) 集合用于去重
  std::set<std::pair<std::string, std::string>> seen;
  for (const auto& package : table->packages) {
    if (package->name != compilation_package) continue;
    for (const auto& type : package->types) {
      std::string type_name = type->named_type.to_string();
      for (const auto& entry : type->entries) {
        seen.emplace(type_name, entry->name);
      }
    }
  }

  text::Printer rtxt_printer(fout_text);

  std::istringstream input(merged_rtxt_content);
  std::string line;
  // styleable 上下文：当前 styleable 是否已被去重决定
  bool skip_styleable_context = false;
  size_t appended = 0;

  while (std::getline(input, line)) {
    if (line.empty()) continue;

    bool skip_this = false;
    if (line.compare(0, 4, "int ") == 0) {
      std::istringstream ls(line);
      std::string kind, type, name;
      ls >> kind >> type >> name;
      if (type.empty() || name.empty()) {
        skip_this = true;
      } else if (type == "styleable") {
        // 子索引依附于上一条 int[] styleable 数组
        if (skip_styleable_context) skip_this = true;
      } else {
        if (seen.count(std::make_pair(type, name)) > 0) {
          skip_this = true;
        } else {
          seen.emplace(type, name);
        }
      }
    } else if (line.compare(0, 6, "int[] ") == 0) {
      // 顶层 styleable 数组：去重整组
      std::istringstream ls(line);
      std::string kind, type, name;
      ls >> kind >> type >> name;
      if (type == "styleable" && !name.empty()) {
        skip_styleable_context =
            seen.count(std::make_pair(std::string("styleable"), name)) > 0;
        if (skip_styleable_context) {
          skip_this = true;
        } else {
          seen.emplace(std::string("styleable"), name);
        }
      }
    }

    if (skip_this) continue;
    rtxt_printer.Println(line);
    ++appended;
  }

  if (appended > 0) {
    diag->Note(android::DiagMessage()
               << "merged " << appended << " R.txt lines from --public AAR(s) into host R.txt");
  }
  return appended;
}

}  // namespace aura
}  // namespace aapt
