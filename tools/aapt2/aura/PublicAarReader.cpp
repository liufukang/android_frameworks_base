/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aura/PublicAarReader.h"

#include <fstream>
#include <sstream>
#include <string>

namespace aapt {
namespace aura {

bool ShadowIdsReader::Read(const std::vector<std::string>& rtxt_paths,
                           android::IDiagnostics* diag,
                           ShadowIdsData* out_data) {
  out_data->shadow_set.clear();
  out_data->merged_rtxt_content.clear();

  for (const std::string& rtxt_path : rtxt_paths) {
    std::ifstream ifs(rtxt_path);
    if (!ifs.is_open()) {
      diag->Error(android::DiagMessage()
                  << "failed to open --shadow-ids file: " << rtxt_path);
      return false;
    }

    // 读取整个文件内容
    std::string r_txt_content((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());

    // 保存原始 R.txt 内容用于后续合并到宿主 R.txt
    if (!r_txt_content.empty()) {
      if (!out_data->merged_rtxt_content.empty() &&
          out_data->merged_rtxt_content.back() != '\n') {
        out_data->merged_rtxt_content += '\n';
      }
      out_data->merged_rtxt_content += r_txt_content;
    }

    // 从 R.txt 提取 (type, name) 加入 shadow_set
    std::istringstream r_txt_stream(r_txt_content);
    std::string line;
    size_t entry_count = 0;
    while (std::getline(r_txt_stream, line)) {
      if (line.empty()) continue;
      // 跳过 styleable 数组与子索引（不是 ResourceTable 中的真实 entry）
      if (line.compare(0, 4, "int ") != 0) continue;
      // 格式："int <type> <name> 0x..."
      std::istringstream ls(line);
      std::string kind, type, name;
      ls >> kind >> type >> name;
      if (type.empty() || name.empty()) continue;
      if (type == "styleable") continue;
      out_data->shadow_set.emplace(type, name);
      ++entry_count;
    }
    diag->Note(android::DiagMessage()
               << "--shadow-ids: " << entry_count << " entries from " << rtxt_path);
  }
  return true;
}

}  // namespace aura
}  // namespace aapt
