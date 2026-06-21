/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aura/PublicAarReader.h"

#include <sstream>
#include <string>

#include "io/ZipArchive.h"

namespace aapt {
namespace aura {

bool PublicAarReader::Read(const std::vector<std::string>& aar_paths,
                           android::IDiagnostics* diag,
                           PublicAarData* out_data) {
  out_data->shadow_set.clear();
  out_data->merged_rtxt_content.clear();

  for (const std::string& aar_path : aar_paths) {
    std::string err;
    auto aar = io::ZipFileCollection::Create(aar_path, &err);
    if (!aar) {
      diag->Error(android::DiagMessage()
                  << "failed to open --public AAR " << aar_path << ": " << err);
      return false;
    }

    io::IFile* r_txt_file = aar->FindFile("R.txt");
    if (!r_txt_file) {
      diag->Warn(android::DiagMessage()
                 << "--public AAR " << aar_path << " does not contain R.txt, skipping");
      continue;
    }

    auto r_txt_data = r_txt_file->OpenAsData();
    if (!r_txt_data) {
      diag->Error(android::DiagMessage()
                  << "failed to read R.txt from --public AAR " << aar_path);
      return false;
    }

    std::string r_txt_content(static_cast<const char*>(r_txt_data->data()),
                              r_txt_data->size());

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
    size_t aar_count = 0;
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
      ++aar_count;
    }
    diag->Note(android::DiagMessage()
               << "--public: " << aar_count << " entries from " << aar_path);
  }
  return true;
}

}  // namespace aura
}  // namespace aapt
