/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AAPT2_AURA_PUBLIC_AAR_READER_H
#define AAPT2_AURA_PUBLIC_AAR_READER_H

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "androidfw/IDiagnostics.h"

namespace aapt {
namespace aura {

// --public AAR 读取结果
struct PublicAarData {
  // shadow 资源集合：(type, name)，用于标记 ResourceTable 中的 shadow entry
  std::set<std::pair<std::string, std::string>> shadow_set;
  // 所有 AAR 中 R.txt 的原始内容（拼接，含 styleable 行），用于追加到宿主 R.txt
  std::string merged_rtxt_content;
};

// 解压 --public AAR 文件列表，读取每个 AAR 内的 R.txt，
// 产出 shadow_set（供 ShadowResourceMarker 标记）和 merged_rtxt_content（供 PublicRtxtMerger 追加）。
class PublicAarReader {
 public:
  // 读取所有 --public AAR 文件，返回聚合后的数据。
  // 失败时返回 false（已输出错误诊断），data 内容未定义。
  static bool Read(const std::vector<std::string>& aar_paths,
                   android::IDiagnostics* diag,
                   PublicAarData* out_data);
};

}  // namespace aura
}  // namespace aapt

#endif  // AAPT2_AURA_PUBLIC_AAR_READER_H
