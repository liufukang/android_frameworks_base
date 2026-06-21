/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AAPT2_AURA_ARSC_PACKAGE_NAME_REWRITER_H
#define AAPT2_AURA_ARSC_PACKAGE_NAME_REWRITER_H

#include <optional>
#include <string>

#include "ResourceTable.h"
#include "androidfw/IDiagnostics.h"

namespace aapt {
namespace aura {

// RAII 工具：在构造时将 ResourceTable 中 compilation package 的名字临时改写为
// --arsc-package-name 指定的值，析构时自动恢复。
// 仅影响 arsc 中 PackageChunk 的 package name，不影响 manifest 和 R 类。
class ArscPackageNameRewriter {
 public:
  // 构造时执行改写。如果 arsc_package_name 为空或找不到匹配的 package，不做任何操作。
  ArscPackageNameRewriter(const std::optional<std::string>& arsc_package_name,
                          const std::string& compilation_package,
                          ResourceTable* table,
                          android::IDiagnostics* diag);

  // 析构时恢复 package name
  ~ArscPackageNameRewriter();

  // 禁止拷贝
  ArscPackageNameRewriter(const ArscPackageNameRewriter&) = delete;
  ArscPackageNameRewriter& operator=(const ArscPackageNameRewriter&) = delete;

 private:
  ResourceTablePackage* rewritten_pkg_ = nullptr;
  std::string original_name_;
};

}  // namespace aura
}  // namespace aapt

#endif  // AAPT2_AURA_ARSC_PACKAGE_NAME_REWRITER_H
