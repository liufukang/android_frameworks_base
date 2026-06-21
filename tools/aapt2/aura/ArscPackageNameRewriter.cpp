/*
 * Copyright (C) 2026 The OpenApm Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aura/ArscPackageNameRewriter.h"

namespace aapt {
namespace aura {

ArscPackageNameRewriter::ArscPackageNameRewriter(
    const std::optional<std::string>& arsc_package_name,
    const std::string& compilation_package,
    ResourceTable* table,
    android::IDiagnostics* diag) {
  if (!arsc_package_name) return;

  for (auto& pkg : table->packages) {
    if (pkg->name == compilation_package) {
      rewritten_pkg_ = pkg.get();
      original_name_ = pkg->name;
      pkg->name = arsc_package_name.value();
      diag->Note(android::DiagMessage()
                 << "overriding arsc package name to '" << arsc_package_name.value() << "'");
      break;
    }
  }
}

ArscPackageNameRewriter::~ArscPackageNameRewriter() {
  if (rewritten_pkg_ != nullptr) {
    rewritten_pkg_->name = original_name_;
  }
}

}  // namespace aura
}  // namespace aapt
