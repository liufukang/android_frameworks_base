/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AAPT2_AURA_PUBLIC_RTXT_MERGER_H
#define AAPT2_AURA_PUBLIC_RTXT_MERGER_H

#include <string>

#include "ResourceTable.h"
#include "androidfw/IDiagnostics.h"
#include "androidfw/Streams.h"

namespace aapt {
namespace aura {

// 将 --public AAR 中 R.txt 的原始内容追加到宿主 R.txt 输出，
// 让 AGP 后续 R.jar 生成步骤包含上游 public 资源字段。
// 去重策略：以当前 ResourceTable 已有的 (type, name) 为基准，同名跳过。
class PublicRtxtMerger {
 public:
  // 将 merged_rtxt_content 按行去重追加到 fout_text。
  // compilation_package 用于筛选 table 中的 seen 集合。
  // 返回实际追加行数。
  static size_t Append(const std::string& merged_rtxt_content,
                       const std::string& compilation_package,
                       ResourceTable* table,
                       android::OutputStream* fout_text,
                       android::IDiagnostics* diag);
};

}  // namespace aura
}  // namespace aapt

#endif  // AAPT2_AURA_PUBLIC_RTXT_MERGER_H
