# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 仓库定位

这是 AOSP `frameworks/base` 的 fork（源自 platform-tools 35.0.2），以 git submodule 形式被外层项目 `/Users/liufukang.11/Code/OpenApm/aapt2/` 引用，**唯一目的是承载 aapt2 的 Aura/Portal 定制补丁**。

- 当前分支 **`aapt-base-patch`**：所有定制提交都在这里
- 分支 **`main`**：保持纯净 AOSP 状态，用作生成 `patches/*.patch` 的基线
- 其余分支（`beryl`、`kitkat` 等）是原 fork 上游遗留，**忽略即可**

**除 `tools/aapt2/` 之外的所有目录（`services/`、`core/`、`packages/` ……）都不参与本项目构建，禁止修改**。它们只是原始 AOSP 代码残留，动它们既无编译验证手段，也会污染 patch。

## 构建与验证

本 submodule 本身不独立构建。编译、生成 patch、跑 aapt2 的完整流程见外层 `/Users/liufukang.11/Code/OpenApm/aapt2/CLAUDE.md`。快速命令：

```bash
# 在外层项目根目录（/Users/liufukang.11/Code/OpenApm/aapt2/）执行
./patch_local.sh                       # 会先 git checkout -- . 重置 submodule，再打 patch
ninja -C build-host-arm64 aapt2        # 增量编译（改完源码后最快的验证路径）
cp build-host-arm64/bin/aapt2 build/bin/aapt2

# 版本自检：定制版会打印一行以 [aura] 开头的定制标志清单
./build/bin/aapt2 version
```

⚠️ `patch_local.sh` 会先 `git checkout -- .` 重置本 submodule 工作区。**先 commit 或 stash，再运行 patch 脚本**，否则未 commit 的修改会被清空。

生成/更新 patch：

```bash
# 在本 submodule 内，改好代码并 commit 到 aapt-base-patch 后
git diff main -- tools/aapt2/ > ../../patches/search_all_include_packages.patch
```

## aapt 定制逻辑总览

所有定制服务于同一个总目标：**让 aapt2 能与 Aura/Portal bundle 体系正确协作** —— 宿主 apk 在 link 阶段直接引用未 merge 进 apk 的 bundle 0x7F 资源，同时保证 arsc 中 ID 位置和 R.txt 正确性。

围绕这个目标的六大能力：

| 能力 | 解决的问题 | 相关 CLI 标志 |
|------|-----------|--------------|
| **跨包资源引用** | 宿主写 `@drawable/xxx` 但资源实际来自 bundle | `--search-all-include-packages` |
| **可见性放行** | bundle 资源多数不是 PUBLIC，标准 aapt2 会拒绝引用 | `--disable-visibility-check`（默认 true） |
| **Shadow 资源** | bundle 资源应参与引用解析、但**不能写入宿主 arsc** | `--shadow-ids <rtxt>`（可多次） |
| **ID 位置固定** | 宿主与 bundle 的资源必须在 arsc entry position 上一致 | `--type-id-mapping`、`--entry-slot-config`、`--entry-slot-size` |
| **Legacy 0x7F 双 PackageChunk** | 保留一份 0x7F PackageChunk 存放 legacy 资源，与宿主主包共存 | `--legacy-public-xml`（+ 0x7F 自动推导） |
| **arsc 包名解耦** | arsc 里的 packageName 要与 manifest/R 类不同 | `--arsc-package-name` |
| **辅助能力** | AGP 通过文件传 no-compress 扩展名列表 | `-e <file>` |

Apktool 兼容层（放宽资源名/attr 引用/package_id 校验、绕过 `PrivateAttributeMover` CHECK 等）通过 `apktool_ibotpeaches.patch` 补丁提供，属于同一定制体系的基础层。

## 定制代码布局

### 1. `tools/aapt2/aura/` — 独立命名空间的定制 helper（首选新增位置）

新增定制逻辑**优先**放这里，避免污染 AOSP 原文件、简化后续 rebase。当前包含：

| 文件 | 职责 |
|------|------|
| `ArscPackageNameRewriter.{h,cpp}` | RAII：flatten 期间临时改写 arsc `PackageChunk` 的 package name（`--arsc-package-name`），析构自动恢复。不影响 manifest / R 类 |
| `LegacyPackageCollector.{h,cpp}` | `ParseLegacyPublicXml` 读 `--legacy-public-xml`；`AutoDeriveFromIncludes` 在 packageId==0x7F 时扫描 `-I` 中的 0x7F 资源自动生成 legacy 集合 |
| `PhantomEntrySynthesizer.{h,cpp}` | IdAssigner 后、Flatten 前调用，为 ResourceTable 中缺失的 legacy entry 合成空占位 entry，供 ReferenceLinker 解析 |
| `PublicAarReader.{h,cpp}` | 读 `--shadow-ids` R.txt（纯文本 `int <type> <name> 0x...`），产出 `ShadowIdsData{shadow_set, merged_rtxt_content}`。类主名 `ShadowIdsReader`，保留 `PublicAarReader` 作过渡别名 |
| `PublicRtxtMerger.{h,cpp}` | 把 shadow R.txt 内容去重后追加到宿主 R.txt 输出，让下游 R.jar 生成包含 bundle 资源符号 |
| `ShadowResourceMarker.{h,cpp}` | 把 `shadow_set` 应用到 `ResourceTable`，将匹配 entry 标记为 `is_shadow=true` |

规约：
- 所有文件都是 `namespace aapt::aura`
- 头文件加 header guard `AAPT2_AURA_*`
- License header 用 `Copyright (C) 2026 The OpenApm Authors / SPDX-License-Identifier: Apache-2.0`
- 所有 aura helper 必须在 `Android.bp` 的 `libaapt2` srcs 里注册（AOSP 构建路径），CMake 侧由外层 `cmake/aapt2.cmake` glob 收录

### 2. 侵入式修改（尽量避免新增，只在无法用 aura helper 隔离时做）

| 文件 | 关键改动 |
|------|---------|
| `tools/aapt2/Main.cpp` | `aapt2 version` 输出 `[aura]` 定制标志清单，用于确认二进制身份 |
| `tools/aapt2/cmd/Link.{h,cpp}` | 新增所有定制 CLI 标志和它们的挂载点（见下节） |
| `tools/aapt2/compile/IdAssigner.{h,cpp}` | 双分配器模型：`assigned_ids`（带 slot 约束）+ `legacy_assigned_ids`（无 slot 约束）；`is_shadow` / legacy entry 走 legacy 分配器；支持 `type_id_mapping` / `entry_slots` / `entry_slot_size` |
| `tools/aapt2/format/binary/TableFlattener.{h,cpp}` | `is_shadow` entry 不写 arsc；`legacy_entries` 驱动 legacy 0x7F PackageChunk 独立输出；legacy 过滤后为空的 PackageView 必须跳过（`HasNonLegacyEntries` 检查） |
| `tools/aapt2/process/SymbolTable.{h,cpp}` | `Symbol::is_shadow` 字段；`FindByNameNoMangle`、`GetAllPackageNames`、`GetAll7fResources` 方法；`search_all_include_packages_` + `include_package_names_` 状态 |
| `tools/aapt2/link/ReferenceLinker.cpp` | 非限定引用查找失败时 fallback 到所有 `-I` 包；shadow entry 跳过本地查找 |
| `tools/aapt2/link/PrivateAttributeMover.cpp` | 注释 `CHECK(entries.empty())`，兼容预填充 attr_private |
| `tools/aapt2/ResourceTable.cpp` / `ResourceUtils.cpp` | 放宽资源名和 `?attr` 引用校验（Apktool 兼容） |
| `tools/aapt2/java/JavaClassGenerator.cpp` | `IsValidSymbol` 始终 true；styleable 跳过的 attr 仍输出 R.txt child 行（保证 R.txt 长度对齐） |
| `tools/aapt2/format/binary/PackageFlattener.cpp` | 即使 entries 全空也保留 type，确保 Type StringPool 包含正确 type name |

## 定制 CLI 标志与挂载点

`LinkOptions`（`cmd/Link.h`）里的定制字段与它们在 `cmd/Link.cpp::LinkCommand::Action` 中的处理顺序（近似）：

1. **`-e <file>`** → `extensions_to_not_compress_path` — 从文件读 no-compress 扩展名列表
2. **`--type-id-mapping "attr=1,drawable=2,..."`** → 全局 Type ID 强制映射（覆盖 IdAssigner 默认分配）
3. **`--entry-slot-config "0,2,3"` + `--entry-slot-size N`**（默认 1024）→ Entry ID 分槽分配，`entry_id / N` 必须在 config 列表内
4. **`--legacy-public-xml <path>`** → `aura::LegacyPackageCollector::ParseLegacyPublicXml` 收集 legacy 0x7F entries
5. **`--search-all-include-packages`** → 打开 `SymbolTable::search_all_include_packages_`；packageId==0x7F 时触发 `LegacyPackageCollector::AutoDeriveFromIncludes` 自动推导
6. **`--arsc-package-name <name>`** → 通过 `aura::ArscPackageNameRewriter` 在 flatten 阶段 RAII 改写包名
7. **`--shadow-ids <rtxt>`（可多次）** → `aura::ShadowIdsReader::Read` 产出 `shadow_ids_data_`；`ShadowResourceMarker::Mark` 打标记；flatten 后 `PublicRtxtMerger::Append` 把内容并入宿主 R.txt
8. **`--disable-visibility-check`**（默认 true）→ 关闭 `ResolveSymbolCheckVisibility` 的 PRIVATE 拒绝路径

## 数据结构关键改造点

### `SymbolTable::Symbol::is_shadow` —— shadow 语义跨模块联动

同一个 flag 影响三个阶段的行为：

- **IdAssigner**：`is_shadow` entry 走 `legacy_assigned_ids`（无 slot 约束的分配器），避免占用 `--entry-slot-config` 槽位
- **TableFlattener**：`is_shadow` entry **不写入 arsc**（TypeSpec 和 Type 都会跳过），保证宿主 apk 里只有真正属于宿主的资源
- **ReferenceLinker**：`is_shadow` entry 跳过本地查找，走 `-I` 真实 ID（fallback 搜索）—— 生成的引用直接指向 bundle 中的 ID

### 双分配器模型（IdAssigner）

`IdAssigner::Consume` 里同时维护两个 `IdAssignerContext`：

```
assigned_ids          : 主分配器，遵守 entry_slots / entry_slot_size 约束
legacy_assigned_ids   : 副本，无 slot 约束（entry_slots=nullptr）
```

分派规则：
- `entry->id` 已设定 + 非 legacy：只在 `assigned_ids` 预留
- `entry->id` 已设定：都在 `legacy_assigned_ids` 预留（避免后续 legacy 冲突）
- 未设 ID + `is_shadow` → `legacy_assigned_ids.NextId`
- 未设 ID + legacy entry → `legacy_assigned_ids.NextId`
- 未设 ID + 普通 entry → `assigned_ids.NextId`

`SkipToNextAvailableId` 里的 `id_validator_` 由 slot 配置生成，跳过不满足 `entry_id / slot_size ∈ entry_slots` 的 ID。

### `TableFlattenerOptions::LegacyPublicEntry` —— legacy 0x7F 双 PackageChunk

驱动 `TableFlattener::Flatten` 里额外调用一次 `FlattenLegacyPackage`，把 legacy entries 单独打成一个 0x7F PackageChunk 输出，并把它们从其他 PackageView 中过滤掉。关键坑位：

- **过滤后为空的 PackageView 必须跳过**（`HasNonLegacyEntries` 检查），否则 arsc 里会残留空 0x7F PackageChunk（见 commit `d8f50299d6e9`）
- Legacy 0x7F 包中 entry value 引用**保留** target packageId（如 0x50），运行时由 Portal 重映射为 0x7F
- `legacy_package_name` 可用 `--arsc-package-name` 场景中的包名，缺省用源包名

### Type StringPool

IdAssigner 生成的 Type 即使 entries 全空也必须保留，确保 Type StringPool 包含正确 type name（见 commit `3d3f8ca16fae`）。Portal 依赖 Type StringPool 顺序做 Type ID rewrite。

### 预填充 `-I` 中的 0x7F 资源到 `final_table_`

这是 `search_all_include_packages` 补丁的核心机制：在 IdAssigner 运行**之前**，把 `-I` 中所有 0x7F 资源通过 `AddResource + SetId` 预填到 host `final_table_`。区分两种情况：
- **宿主已有同名资源** → 只 pin ID（保持 entry position）
- **纯 bundle 资源** → 新增空条目占位（无 values）

`ResourceTableSymbolSource::FindByName` 里对 `values.empty()` 的 attr 条目返回 nullptr，使 SymbolTable 回退到 `AssetManagerSymbolSource` 获取完整 Attribute（type flags / enum symbols），保证 ReferenceLinker 能正确校验 style 值。这样 IdAssigner 看到的是"占位 ID"，ReferenceLinker 看到的是"完整语义"。

## 关键 link 流水线时序

`LinkCommand::Action` 内部大致顺序（略去无关步骤）：

```
1.  解析所有 CLI 标志 → LinkOptions
2.  加载 -I include packages → AssetManagerSymbolSource
    - 收集 GetAllPackageNames() → SymbolTable.include_package_names_
    - 如需：读 --legacy-public-xml + AutoDeriveFromIncludes → legacy_entries
3.  读 --shadow-ids R.txt → ShadowIdsReader::Read → shadow_ids_data_
4.  编译资源 → ResourceTable（final_table_）
5.  ShadowResourceMarker::Mark(shadow_set) → 打 is_shadow 标记
6.  预填充 0x7F 资源到 final_table_（pin ID / 空占位）
7.  IdAssigner.Consume（双分配器：assigned_ids + legacy_assigned_ids）
8.  PhantomEntrySynthesizer::Synthesize → 补齐仍缺失的 legacy entries
9.  ReferenceLinker：非限定引用 fallback 搜索所有 -I 包；shadow entry 走真实 ID
10. TableFlattener：
      - ArscPackageNameRewriter RAII 改写包名
      - is_shadow entry 不写 arsc
      - 过滤 legacy entries 后为空的 PackageView 跳过
      - 额外 FlattenLegacyPackage 输出 legacy 0x7F PackageChunk
11. R.txt 输出：PublicRtxtMerger::Append 追加 shadow R.txt 内容
```

## 修改代码时的注意事项

- **改动前先读原注释**：本项目的机制耦合较深（IdAssigner ↔ SymbolTable ↔ ReferenceLinker ↔ TableFlattener 四方联动），改一处常需同步另一处。仓库中的中文注释是关键上下文，动代码前务必看懂再改。
- **新增逻辑优先放 `tools/aapt2/aura/`**，仅在需要 hook AOSP 内部数据流时才修改原文件。侵入越多，rebase AOSP 越痛。
- **提交信息用中文**（现有 commit 全部中文，例：`feat: 支持 --public/--shadow-resources 与自动 0x7F legacy 推导`）。
- **不要碰 `tools/aapt2/` 之外的 AOSP 目录**，它们不参与构建、不进 patch。
- **不要修改 `tools/aapt2/**/*.pb.cc` 和 `*.pb.h`**——protoc 生成产物，外层 `.gitignore` 已忽略，编译时会重新生成。
- **改 CLI 标志时同步 `Main.cpp` 的 version 输出**，让 `aapt2 version` 反映所有 aura 定制。
- **改 shadow / legacy 相关逻辑时**：`is_shadow` 和 `legacy_entry_names_` 在 IdAssigner / TableFlattener / ReferenceLinker 三处都有分支，改一处务必检查其余两处的语义是否一致。
- **验证修改**：`ninja -C ../../build-host-arm64 aapt2` 能编过即基本可用；进一步验证需在真实 App 项目跑 `link`，用 `aapt2 dump resources` 检查 arsc entry 位置、`aapt2 dump chunks` 检查 PackageChunk 数量，及比对 R.txt 内容。
