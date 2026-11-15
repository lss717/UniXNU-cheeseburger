# UniXNU-cheeseburger 下一步迁移与适配审计计划

更新日期：2026-11-15

## 目标

在保留现有 OnePlus 5 实机测量值、bring-up 成果和诊断证据的前提下，将
`UniXNU-cheeseburger` 从依赖定制 UniXNU fork 的 format 1 设备配置迁移到官方
format 2 架构。迁移完成后，应能明确区分以下问题的归属：

- 官方 ARM64 和 OS profile 的公共行为；
- MSM8998 SoC 的资源、配置和通用补丁；
- cheeseburger 板级 wiring、固件和分区策略；
- UFS、USB、显示、输入等独立组件；
- 仅用于 bring-up 的诊断补丁和安全绕过。

本计划首先追求可解析、可追踪和可复现，不在迁移过程中同时重写驱动或改变已验证
的硬件常量。

## 架构决策：以官方为基线维护 cheeseburger 下游分支

官方 `UniXNU/UniXNU` 提供 format 2 框架、公共 profiles、component catalog、ADT、
payload 和 package 流程，但官方仓库本身不包含完整的 OnePlus 5 / MSM8998
cheeseburger 适配。项目不再把“原封不动使用官方 UniXNU”作为完成条件。

正式方案是在 `lss717/UniXNU` 中建立一个**从官方指定 commit 直接创建**的新分支，
承载 cheeseburger 当前需要、但尚未合入官方的 UniXNU 层扩展。建议分支名：

```text
upstream-format2-cheeseburger
```

依赖关系调整为：

```text
UniXNU/UniXNU 官方固定 commit
└── lss717/UniXNU: upstream-format2-cheeseburger
    ├── format 2 通用兼容提交
    ├── MSM8998 device-tree converter 扩展
    ├── external firmware release 支持
    ├── toolchain/EDK II 兼容修复
    └── 其他明确归属的 cheeseburger 下游提交
         ↓ 固定 40 字符 commit
UniXNU-msm8998-common + UniXNU-cheeseburger
```

现有 `lss717/UniXNU` 的旧 `main` 继续作为 format 1 历史实现、实机证据和迁移参考，
不得直接合并到新分支。新分支必须从官方基线创建，再逐个移植经过审查的提交。

### 分支建立要求

```sh
git remote add upstream https://github.com/UniXNU/UniXNU.git
git fetch upstream
git switch --create upstream-format2-cheeseburger \
  578ae75c8e7356d4d562bcfc294ab26dfe640a31
```

如果 remote 已存在，先确认其 URL，不重复添加。分支创建后记录：

- upstream URL 与基线 commit；
- `git merge-base`；
- 新分支第一个和最终验证 commit；
- 相对 upstream 的提交列表和 diffstat；
- 每个提交的 owner、适用范围和上游化状态。

### 设备仓库的使用规则

- `UniXNU-cheeseburger` 的 `[package].unixnu` 指向新分支的 checkout 布局；
- GitHub Actions checkout `lss717/UniXNU`，但 `ref` 必须是经过验证的完整 commit，
  不能直接写分支名；
- `scripts/package.sh` 不再运行时修改 UniXNU checkout；
- 当前 `patches/unixnu/*.patch` 在迁入新分支并验证后从 package 应用路径移除；
- `package.manifest.json` 记录 downstream commit、官方基线和 merge-base；
- 上游是否接受这些提交不是设备适配完成的前提，但每个下游提交必须可追溯、可审查、
  可重复构建。

### 当前 6 个兼容补丁的迁移顺序

将当前 `patches/unixnu/` 中的补丁转换为新分支上的独立提交，每个提交只解决一个
问题：

1. 导出 MSM8998 UFS/PHY profile 属性；
2. 从公共 boot-progress 中移除 sheng/cheeseburger PMGR ownership leak；
3. 兼容 Xcode 26.3 unknown warning option；
4. 导出 UniXNULib 所需 EmbeddedPkg GUID；
5. 提供 edk2-msm8998 所需 legacy `FdtLib.h` 兼容层；
6. 支持 external firmware release 与设备侧 boot-image assembly。

提交顺序必须与依赖顺序一致。禁止把 6 个补丁 squash 成无法独立审查的大提交。

### 新分支验收条件

- 分支 ancestry 能证明直接源自官方固定 commit；
- 相对官方基线只有明确列出的下游提交；
- checkout 后无需 `git apply` 即可完成 cheeseburger package；
- package 前后新分支 checkout 的 `git status --short` 均为空；
- 两次干净构建的 kernel 在枚举身份/调试/时间字段之外逐字节一致，其他随机化
  产物的差异得到解释；
- manifest 同时记录官方基线、downstream commit、MSM8998 common、device 和 edk2 commit；
- CI 与本地构建固定同一个 downstream commit；
- 旧 format 1 `main` 不再是 format 2 正式构建依赖。

## 执行状态（2026-11-15）

本轮已经完成 format 2 的静态迁移和主机侧解析验证，但没有把静态解析结果等同于
完整迁移或已通过实机启动。

| 阶段 | 状态 | 本轮结果 | 尚未完成 |
| --- | --- | --- | --- |
| 阶段 0：冻结基线 | 已完成 | 保存 format 1 board、迁移前 status、历史 H91 产物哈希和检查点来源 | 本地没有 H91 构建产物，未重新计算其原始哈希 |
| 阶段 1：MSM8998 common | 已完成最小骨架 | `UniXNU-msm8998-common` 已发布，当前固定 commit `8227828eb1706b51aea9762bd46006328d91db26` | platform operations 和 SoC patch 分类仍为空 |
| 阶段 2：format 2 设备配置 | 已完成主机侧构建 | schema 2、四层 profile、7 个组件、61 个 patch、完整 `fetch/tree/release` 已通过 | 仍需实机确认 profile 和组件行为 |
| 阶段 3：ADT 转 DTS | 已完成主机侧对照 | DTS 编译通过，format 2 ADT 通过 47 项结构检查，旧节点零丢失 | ADT 仍需随 format 2 镜像实机启动验证 |
| 阶段 4：UFS/root | 未完成 | 已明确记录 probe、runtime 和正式 component 的冲突 | A/B/C/D 单路径矩阵和实机读写/flush/重启测试均未执行 |
| 阶段 5：patch 所有权 | 部分完成 | 已解析 61 个唯一 patch；6 个兼容改动已转成独立下游提交，PMGR hunk 已移出公共 series | 旧 format 1 fork 的其余差异尚未逐个归类 |
| 阶段 6：安全链分离 | 未完成 | diagnostic 参数和绕过已在 README 中明确标记 | release-candidate 参数集和 VerifiedRoot/AMFI/SystemOS 验证尚未完成 |
| 阶段 7：文档、记录与 CI | 部分完成 | downstream commit、DarwinBoot 子模块、CI 和 manifest 已固定；macOS 完整 package 已通过 | GitHub workflow 尚未在 Actions 上实际运行 |
| downstream UniXNU | 已完成主机侧验收 | 分支从官方 `578ae75c...` 直接创建，当前固定 `d7fafc5d...`；package 不再执行 `git apply`，构建前后 checkout 均为 clean | 需要真实 seed 和实机验证 |
| kernel/payload/boot image | 主机侧完成 | macOS 已产出 kernel、hardware.dtb、payload、UniXNU EFI package、FD 和 boot image；全部结构检查通过 | 尚未进行实机启动确认 |
| 实机 bring-up | 未执行 | 保留 format 1 历史证据，未冒充 format 2 结果 | UEFI、DarwinBoot、XNU、UFS、APFS、launchd、dyld 均需重新确认 |

因此当前结论是：**format 2 正式 downstream 主机侧构建链完成，实机 bring-up、UFS
和安全链仍未完成。**

### 主机构建记录

- macOS 干净 checkout 的 `defconfig/status/tree/porting-record/package --check`
  已通过。
- 首次 kernel 构建发现公共 boot-progress patch 内含未声明的 PMGR 依赖；现已在
  device series 中显式排序 `sheng-platform-power` 和进度标记 patch。
- 第二次构建进入 XNU bootstrap 后触发 NativeAES guard：设备 defconfig 仍用
  `CONFIG_XNU_CRYPTO_BOOTSTRAP="source"` 覆盖 OS profile 的 `prelinked`。
  该覆盖属于 format 1 诊断遗留，现已对齐到 format 2 的 prelinked closure。
- 第三次构建发现设备仓库的同名 `build-arm-driver.py` 覆盖了官方
  canonical-aware builder。设备 probe 已改为
  `build-cheeseburger-driver.py`，官方组件继续使用 UniXNU 脚本。
- 第四次构建发现 Xcode 26.3 把旧 `-Wno-unnecessary-virtual-specifier`
  视为未知警告选项；已加入显式 `-Wno-unknown-warning-option` 兼容 patch。
- 非 catalog 的设备 probe 不会由 resolver 链接进 `tree/drivers`；本地
  builder 现通过 `UNIXNU_DEVICE` 直接读取设备仓库源码。
- `build-native-ufs-runtime.py` 需要尚未提供的 VerifiedRoot signed policy，
  且当前 `CONFIG_XNU_NATIVE_UFS` 未启用。初始 format 2 构建已移除该 builder，
  作为 UFS 负向控制；正式 runtime 路径留到 C 配置矩阵。
- `build-sheng-pmgr.py` 依赖已存在的 prelinked kernel，不能作为 pre-link
  archive 执行。初始构建改用现有 `build-sheng-pmgr-stub.py`；真实 PMGR
  在首个 kernel 候选生成后进行二次构建。
- UFS 负向控制还需要为 `NativeRamBlock` 提供显式拒绝型 store stub。
  `native_ufs_store_mode()=0`，因此 Data 保持纯内存，不会被误报为持久 UFS。
- `NativeRamBlock` 的 IOBlockStorageDevice 继承槽通过现有 190-slot
  binding 表绑定；`ml_static_mfree_prepare` 在 iPadOS 17 profile 下由 stub
  返回 false，保留种子映射。

### 主机构建产物（2026-11-15）

使用官方 UniXNU 基线 `578ae75c8e7356d4d562bcfc294ab26dfe640a31`、downstream
`d7fafc5d45a795a3b7fc9e9ae313e704c5a1938e`、DarwinBoot
`2ba9cad556f1794fb74ac8af164bc34b0e096338`、MSM8998 common
`8227828eb1706b51aea9762bd46006328d91db26` 和 edk2
`da889248ba63e3b76003e016ee8744b34aa91a01` 在 macOS 15.8.2 / Xcode 26.3
完成。第三次干净构建的代表产物为：

| 产物 | 大小 | SHA-256 |
| --- | ---: | --- |
| `kernel.macho` | 52,043,776 | `147eb7f4c9345c06f762f64f5c1a7d67eb1506f32c687287571669a3521d3cd1` |
| `hardware.dtb` | 3,507 | `3809aa23a82a409735b32af07a9cbe9118257a8b0ed03062994ac5b2931f755b` |
| `payload.img` | 79,691,776 | `cc7cff7cf9a12033219d14c7b18abc0f8700b07ec2ac4a12afc860d00d6cb228` |
| payload 内 `kernel.macho.adt` | 16,408 | `4a03da03e495e78f19f848be8ca66aeb0a8d07402bbcef1fc2cb056abf0acd40` |
| `MSM8998PKG_UEFI.fd` | 6,291,456 | `a68b961bf8e213f06560f7c6de024a06dfd513dbc85042ef099385f86f2bde36` |
| `boot.img` | 81,274,880 | `5e5ee46039f4ad47a4d7b4a636fd4dc47b071490a90510f81712e66ca5386b09` |
| `package.manifest.json` | 9,552 | `c90726503b115327d4139a40579d4d8665b64d3c473f1b5349882d66917c30e5` |

DTB 在全部干净构建中逐字节相同。kernel 的原始差异只落在 Mach-O UUID、960 个
`N_OSO` 调试时间戳、派生 `_PrelinkKCID` 和 kernel build date；FD 只差
`__TIME__` 字符串。枚举字段归一化后，两次独立 kernel 和 FD 重建均为 0 字节差异。
详细范围和三次原始哈希见 `docs/migration/reproducibility-2026-11-15.md`。
构建使用测试 platform seed `0x12345678`，产物未用于实机。

完整命令、输入哈希和 payload 内容见
`docs/migration/host-build-2026-11-15.md`。

最终 `scripts/package.sh --os 17 --uefi ../edk2-msm8998` 已完整通过，
并自动执行 payload ADT 的 47 项检查。
UniXNU release 使用外部 firmware 模式构建 kernel/package/payload，随后由
`lss717/edk2-msm8998` commit
`da889248ba63e3b76003e016ee8744b34aa91a01` 的 CLANG38 + LLD 流程生成 FD，
再由设备脚本写入 format 2 payload 并运行 FD、FV、boot header 和 payload
检查。该结果仍使用测试 seed，不能作为可安装或实机验证版本。

## 审计基线（迁移前）

审计时使用的参考版本：

| 仓库 | Commit |
| --- | --- |
| 官方 `UniXNU/UniXNU` | `578ae75c8e7356d4d562bcfc294ab26dfe640a31` |
| 官方 `UniXNU/UniXNU-sheng` | `ec384c17dd584d5b52d2788b27e55d9fe4ff157d` |
| 当前 `lss717/UniXNU` fork | `d04fad7c2ed4e8d35743142d3f89d37222a2c13b` |
| 当前 `UniXNU-cheeseburger` | `0c66d9998a91da526a2de1bad0e58d4f1847c88f` |

官方参考仓库位于：

- `references/UniXNU`
- `references/UniXNU-sheng`

当前官方工具对 cheeseburger 的解析结果为：

```text
schema_version: 1
components: []
patch_order: []
output: out/cheeseburger
```

这说明当前配置没有进入 format 2 的 ARM64、SoC、OS、device 四层解析流程。

迁移完成后，最新 MSM8998 common source 已发布到：

- <https://github.com/lss717/UniXNU-msm8998-common>
- commit `8227828eb1706b51aea9762bd46006328d91db26`

## 执行原则

1. 迁移前冻结并记录当前可启动基线，包括 commit、构建参数、产物哈希和最后一个已确认
   的启动检查点。
2. 不覆盖或清理 `UniXNU` 工作区当前未提交修改；迁移工作使用独立分支或工作树。
3. 第一轮迁移不改变已测量的 RAM、GIC、timer、UFS、GPT、framebuffer 和 CPU topology
   常量。
4. format 2 解析成功之前，不把新的运行时故障归因于驱动。
5. 每次只改变一个层级，并保存迁移前后的 `unixnu status --json`、构建日志和产物哈希。
6. 诊断参数、安全绕过和正式配置分开记录，不将“禁用检查后可启动”描述为完整支持。

## 阶段 0：冻结现有基线

### 工作项

- 保存三个工作区的 `git status --short`、remote、branch 和 commit。
- 记录当前实际使用的 UniXNU fork、edk2-msm8998、设备仓库和外部输入版本。
- 对当前可启动的 kernel、payload、boot image、ADT 和关键日志计算 SHA-256。
- 保存当前 `board.toml`、defconfig、有效 kernel arguments 和预链接根列表。
- 明确最近一次实机启动中以下检查点的原始证据：
  - UEFI 菜单；
  - DarwinBoot handoff；
  - XNU early boot；
  - BSD/IOKit；
  - UFS controller 与块设备；
  - APFS root mount；
  - launchd exec；
  - dyld shared cache。

### 验收条件

- 可以从记录中唯一还原当前构建所使用的所有源码 revision 和私有输入。
- 每项“已完成”状态至少对应一份日志、照片、哈希或结构化诊断结果。
- 当前未提交修改被完整列出且未被迁移操作覆盖。

## 阶段 1：建立 MSM8998 shared SoC 层

建议建立独立的 `UniXNU-msm8998-common` 仓库或迁移工作目录。该层负责多个 MSM8998
设备可共享的内容，不能混入 OnePlus 5 特有分区和板级 wiring。

### SoC 层应包含

- `soc.toml`；
- `feeds.toml` 或所需公共 source pin；
- MSM8998 DTSI；
- GICv3 distributor/redistributor、timer 和 CPU topology；
- UFS controller、USB controller 和其他 SoC 级寄存器资源；
- `configs/msm8998.config`；
- `patches/xnu/series` 及按依赖排序的 SoC 补丁；
- MSM8998 platform operations；
- SoC 级 README 和 `AGENTS.md`。

### 所有权边界

| 内容 | 归属 |
| --- | --- |
| GIC、timer、CPU topology、controller MMIO | MSM8998 common |
| OnePlus 5 RAM reservation、按键 wiring、panel、分区 | cheeseburger device |
| iPad7,1 / 21H461 ABI 和 OS 补丁 | UniXNU OS profile |
| 可复用 UFS、USB、AES、prelink 实现 | component catalog/组件仓库 |
| 临时日志探针和单次实验 patch | 诊断分支，不进入公共 SoC 基线 |

### 验收条件

- SoC DTSI 可被 `dtc` 单独纳入设备 DTS 编译。
- SoC patch series 不包含 cheeseburger 分区、产品身份或设备专用路径。
- 所有 source 使用完整 40 字符 commit 固定。

## 阶段 2：将设备仓库迁移到 format 2

### 必需文件

补齐或更新：

```text
AGENTS.md
board.toml
feeds.toml
porting.toml
proprietary-files.txt
local.toml.example
unixnu.lock
configs/cheeseburger_defconfig
dts/cheeseburger.dts
patches/xnu/series
.github/workflows/porting-record.yml
.github/workflows/package.yml
```

### `board.toml` 最小结构

```toml
schema_version = 2

[board]
name = "cheeseburger"
soc = "MSM8998"
soc_source = "msm8998"
os_profile = "ipados-17.7.11-21H461"
description = "OnePlus 5"
kit = "cheeseburger"

[components]
include = [
  "ArmDTPlatform",
  "ArmCpu",
  "SoftwareAES",
  "RamBlock",
  "XnuPrelink",
  "DarwinBoot",
]

[kernel]
defconfig = "cheeseburger_defconfig"
config_fragments = [
  "arch:configs/common.config",
  "soc:configs/msm8998.config",
  "os:configs/ipados.config",
]
patch_series = [
  "arch:patches/xnu/series",
  "soc:patches/xnu/series",
  "os:patches/xnu/series",
  "device:patches/xnu/series",
]

[device_tree]
source = "device:dts/cheeseburger.dts"
include_dirs = ["soc:dts"]
```

组件列表只是迁移起点，必须根据实际依赖和 catalog 结果补全，不能直接把示例视为最终配置。

### 旧组件名称映射

| 当前名称 | format 2 canonical ID |
| --- | --- |
| `NativeAES` | `SoftwareAES` |
| `NativeVirtioBlock` | `VirtioMmioBlock` |
| `NativeRamBlock` | `RamBlock` |
| `NativeKeyStoreEvents` | `AppleKeyStoreState` |
| `NativePrelink` | `XnuPrelink` |

运行时 class、bundle ID、符号和协议名不因 catalog ID 迁移而擅自改名。

### 验收条件

运行以下命令时不再回退到 schema 1：

```sh
./unixnu defconfig cheeseburger_defconfig -D ../UniXNU-cheeseburger
./unixnu fetch -D ../UniXNU-cheeseburger
./unixnu status --json -D ../UniXNU-cheeseburger
./unixnu tree -D ../UniXNU-cheeseburger
python3 scripts/porting-record.py check --device ../UniXNU-cheeseburger
```

`status --json` 必须满足：

- `schema_version` 为 `2`；
- profiles 包含 ARM64、MSM8998、iPadOS 17.7.11 和 device 四层；
- `components` 非空且依赖完整；
- `patch_order` 非空或明确证明设备无需额外 patch；
- output 使用 `out/cheeseburger/ipados-17.7.11-21H461/cheeseburger/` 结构；
- 每个 module 都能追溯到 pinned revision 或明确记录的 local override。

### 本轮执行结果

- `unixnu defconfig cheeseburger_defconfig`：通过。
- `unixnu status --json`：通过，报告 schema 2、ARM64/MSM8998/iPadOS/device
  四层 profile、7 个组件和 61 个 patch。
- 输出目录：
  `out/cheeseburger/ipados-17.7.11-21H461/cheeseburger/`。
- `porting-record.py check`：通过，13 个结构化 area 全部有效。
- `unixnu fetch`：首次执行卡在网络 `fetch-pack` 阶段并中止；验证时使用了
  已发布的 MSM8998 common 和明确记录的 local override。
- `unixnu tree`：尚未执行成功，因为完整 tree 需要先完成外部 source fetch。

status 结果保存在 `docs/migration/after-status.json`，迁移前结果保存在
`docs/migration/before-status.json`。

## 阶段 3：把自定义 ADT 转换为 DTS

### 工作项

- 逐项盘点 `scripts/make-cheeseburger-adt.py` 生成的节点和属性。
- 将 SoC 公共节点移动到 MSM8998 DTSI。
- 将板级 RAM、reserved-memory、启用 CPU、UFS、USB、按键和 framebuffer 信息写入
  `dts/cheeseburger.dts`。
- 将 Apple 产品身份和 typed OS 属性放入 format 2 的 `[adt]` 表。
- 将 root volume UUID、packed Data RAM disk 和 payload handoff 选项保留在
  `[payload.adt]`。
- 对驱动读取的每个节点核对 `compatible`、`unixnu,adt-name`、interrupt、reg 和 phandle。
- 同时保留旧 ADT 生成结果，进行节点、属性和二进制结构对照。

### 验收条件

- DTS 在 MSM8998 include path 下通过 `dtc` 编译。
- common ADT generator 生成的 ADT 包含当前 DarwinBoot sidecar 所需的所有
  `/chosen/memory-map` placeholder。
- CPU、GIC、timer、memory、UFS 和 root-match 属性与已验证旧 ADT 一致。
- 新旧 ADT 差异均有明确归属，不能出现未解释的关键节点丢失。

### 本轮执行结果

- `dts/cheeseburger.dts` 在 MSM8998 include path 下由 `dtc` 1.7.0 编译通过。
- format 2 ADT 为 16,460 字节，使用测试 seed 生成，SHA-256 为
  `a92cb2a36bb7babe3326887c7352122adde404ad42156b36122df1f5c1157a99`。
- `scripts/check-format2-adt.py` 的 47 项检查全部通过。
- UFS probe 依赖的 `p8,msm8998-direct-dma` 需要
  `patches/unixnu/0001-devicetree-export-msm8998-ufs-profile.patch`；
  `scripts/package.sh` 会幂等应用该补丁。
- 带补丁的检查确认 `p8,msm8998-direct-dma=1`、UFS 物理地址、SPI 265、
  GIC 物理范围和 sidecar placeholder 均被保留。
- 新旧 ADT 已完成全量逐节点、逐属性对照：format 2 没有丢失任何 format 1
  节点，新增节点和属性差异均已分类。
- 原始差异报告：`docs/migration/adt-diff-2026-11-15.md`。
- 归属审查：`docs/migration/adt-diff-review-2026-11-15.md`。
- 对照期间发现并修复了 CPU cluster 映射反向的问题；现在 `0..3` 为
  Cortex-A73/P，`0x100..0x103` 为 Cortex-A53/E。

## 阶段 4：专项核查 UFS 和 root 路径

这是当前最高优先级的功能审计。

### 当前冲突

- defconfig 启用 `CONFIG_XNU_NATIVE_UFS_PROBE=y`；
- defconfig 没有启用 `CONFIG_XNU_NATIVE_UFS`；
- kernel arguments 又设置 `native-ufs-probe=0`；
- archive plan 同时执行 `build-native-ufs-runtime.py`；
- README 声称 APFS root 已挂载并进入 launchd。

### 必须回答的问题

1. 实机上真正发布块设备服务的是哪个 driver/class？
2. `native-ufs-probe=0` 关闭的具体路径是什么？
3. `build-native-ufs-runtime.py` 生成的对象如何进入 kernel/prelink？
4. root device 最终通过哪个 registry entry 和 BSD name 匹配？
5. UFS 的 MMIO、IRQ、DMA、cache coherency 和 4096-byte block size 分别来自哪里？
6. 所谓“root mounted”是否来自真实 UFS，还是 RAM/映像窗口或其他中间层？

### 测试矩阵

至少构建以下可区分的配置，每个配置只启用一条候选路径：

| 配置 | UFS probe | UFS runtime | 正式 UFS component | 目的 |
| --- | --- | --- | --- | --- |
| A | 开 | 关 | 关 | 确认 probe 只观察、不提供 root |
| B | 关 | 开 | 关 | 确认当前 runtime 路径的实际职责 |
| C | 关 | 关 | 开 | 验证 format 2 正式组件路径 |
| D | 全关 | 全关 | 全关 | 负向控制，确认日志和 root 行为确实来自 UFS |

### 验收条件

- 能从日志中唯一识别 controller 初始化、LUN 枚举、块设备发布和 APFS mount 的提供者。
- format 2 配置只保留一条正式 root storage 路径。
- 记录真实设备读、写、flush、重启持久化和错误恢复结果。

### 当前状态

本阶段尚未开始实机矩阵。当前 defconfig、kernel arguments 和 archive plan 仍同时
包含 probe/runtime 多条路径，不能据此宣称 UFS root 已迁移完成。

## 阶段 5：核查 patch 所有权和 fork 差异

### 分类规则

将当前 UniXNU fork 的每个差异归入以下一类：

1. 应提交官方 ARM64 profile；
2. 应进入 iPadOS 17.7.11 OS profile；
3. 应进入 MSM8998 common；
4. 应进入 cheeseburger device；
5. 应进入独立 component；
6. 仅用于诊断，不能进入发布配置；
7. 已被官方实现取代，应删除。

### 工作项

- 对 fork 与官方当前 commit 建立文件级差异清单。
- 为每个 XNU patch 写明前置 patch、修改符号、适用 XNU build 和运行时消费者。
- 消除通过超长文件名前缀隐式控制顺序的做法，改用 `series` 文件显式排序。
- 检查同一行为是否同时存在于 fork、device repo 和 component 中。
- 对已经 upstream 的代码使用官方版本，避免保留第二份实现。

### 验收条件

- `patch_order` 中每个 patch 只出现一次。
- 每个 patch 都能说明所属层级、依赖、目标 ABI 和验证证据。
- 构建不再依赖目录枚举顺序或补丁文件名长度。

### 当前状态

官方 resolver 已生成 61 个唯一且有序的 patch。构建时发现公共
`zzz-boot-progress.patch` 内嵌了未声明依赖的 sheng PMGR hunk；现已通过
UniXNU 兼容 patch 移除该 hunk，并由设备 series 显式追加 PMGR source 和
progress 两个 patch。fork 与官方 commit 的其余文件级差异仍需分类。

## 阶段 6：分离 bring-up 参数与正式安全链

当前配置包含多项临时绕过：

```text
native-skip-root-auth=1
amfi_get_out_of_my_way=1
amfi_allow_any_signature=1
cs_enforcement_disable=1
native-skip-exec-signature=1
launchd_unsecure_cache=1
```

### 工作项

- 建立明确的 `diagnostic` 和 `release-candidate` 参数集合。
- 每删除一个绕过参数，记录最后完成的启动检查点和失败原因。
- 引入并验证 VerifiedRoot、trust cache、AMFI 初始化和 SystemOS policy。
- 对 owner trust cache、root policy 和 prelink 输入记录哈希。
- 禁止 README 使用单一“已启动”状态覆盖安全链是否启用的差异。

### 验收条件

- release-candidate 配置不包含无条件签名绕过。
- root、SystemOS、dyld cache 和 launchd 的验证路径有独立证据。
- diagnostic 配置被明确标记，不能被 CI 或发布流程误选。

### 当前状态

现阶段只完成了“明确标记”。`board.toml` 仍保留
`native-skip-root-auth=1`、AMFI 绕过、exec signature 绕过、
`launchd_unsecure_cache=1` 和 `debug-enabled=1`。这些参数仍是 diagnostic，
不是 release-candidate。

## 阶段 7：修正文档、记录与 CI

### 已知文档漂移

- README 声称 `panic_screen` 开启，`board.toml` 实际为 `false`。
- README 的构建路径仍描述旧 fork 和旧输出目录。
- “root mounted; launchd exec” 没有说明使用了哪些安全绕过和存储实现。

### 工作项

- 以 `board.toml` 的实际值为唯一配置事实来源。
- 在 `porting.toml` 中按 subsystem 分别记录 source、host 和 device 证据。
- CI 运行 format 2 resolver、record checker、DTS 编译和 package preflight。
- README 只保留用户需要的构建入口和当前验证状态，详细诊断链接到记录文件。
- 所有硬件完成度按 boot、storage、display、input、GPU、USB、安全链分别报告。

### 验收条件

- `porting-record` workflow 通过。
- README、board、porting record 和 CI 使用相同 device、SoC、OS profile 与 defconfig。
- 从干净 checkout 加私有本地输入即可生成代码等价的 kernel；payload 除明确记录
  的随机 handoff 数据外保持一致。

### 本轮执行结果

- README 已改为 format 2 构建入口和新输出路径。
- `panic_screen` 文档漂移已修正为实际值 `false`。
- `porting.toml` 已建立 13 个结构化 area，并通过本地 checker。
- 已添加 porting-record 和 package workflow。
- workflow 尚未在 GitHub 上运行；macOS 完整 package 尚未执行。

## 推荐实施顺序

严格按以下顺序推进：

1. 冻结基线并保存证据；
2. 建立 MSM8998 common 的最小可解析骨架；
3. 将 cheeseburger 配置迁移到 format 2；
4. 让 `status/tree/porting-record` 全部通过；
5. 转换 ADT 为 DTS，并进行新旧硬件描述对照；
6. 完成 UFS 单路径测试矩阵；
7. 分类和迁移 fork/XNU patches；
8. 恢复安全链并分离诊断配置；
9. 重建 kernel、payload、boot image；
10. 按 bring-up 检查点重新进行实机验证。

在第 4 步以前，不建议继续增加新的功能补丁；在第 6 步以前，不建议把存储或
launchd 后续故障归因于用户态。

## 下一步唯一入口

1. 使用真实设备 seed 和匹配的私有输入在 macOS 干净 checkout 中构建 boot image。
2. 把该 boot image 的 SHA-256 作为唯一标识，按 bring-up 检查点执行实机测试。
3. 按 UFS A/B/C/D 矩阵逐项上机，不同时改变其他驱动或安全参数。
4. 完成旧 format 1 fork 剩余差异的 owner 分类。
5. 建立独立 release-candidate 配置后再恢复安全链。

## 最终完成标准

迁移只有同时满足以下条件才算完成：

- 官方 UniXNU 将设备解析为 schema 2；
- SoC、OS、device 和 component source 均固定到明确 revision；
- DTS 和 common ADT generator 替代设备专用 ADT 主路径；
- component resolution 和 patch order 可检查且无重复；
- UFS root 路径只有一个正式实现并有实机证据；
- 安全绕过不出现在 release-candidate 配置；
- clean checkout 的构建流程可重复；
- porting record、CI、README 与实际配置一致；
- 重新确认 UEFI、DarwinBoot、XNU、UFS、APFS、launchd 和 dyld 各检查点。

截至 2026-11-15，静态配置、SoC/DTS、resolver、正式 downstream 和记录检查已经
完成；主机侧归一化构建复现已完成。UFS 单路径、真实 seed 构建、安全链以及全部
实机检查点仍未完成。

## 2026-11-15 适配审查后的下一步计划

### 审查结论

本次使用官方 UniXNU `578ae75c8e7356d4d562bcfc294ab26dfe640a31`
重新执行主机侧检查，结果如下：

```text
schema_version: 2
profiles: arch + soc + os + device
components: 7
patch_order: 61
output: out/cheeseburger/ipados-17.7.11-21H461/cheeseburger
porting record: 13 structured areas checked
```

因此 format 2 静态结构已经通过审查。以下项目仍是完整适配的阻断项，必须按顺序
完成。任何一项未完成时，只能使用“format 2 静态迁移完成”，不能使用“完整适配
完成”或“可发布”。

### P1：固定全部构建输入

#### 要求

1. 将 `.github/workflows/package.yml` 中 `lss717/edk2-msm8998` 的
   `ref: unixnu-cheeseburger` 替换为经过审查的完整 40 字符 commit。
2. 在 `docs/migration/README.md` 和 `porting.toml` 中记录：
   - repository URL；
   - commit；
   - submodule commits；
   - 构建所用 DSC/FDF；
   - 生成 FD 的 SHA-256。
3. package workflow 中的 UniXNU、MSM8998 common、edk2-msm8998 和组件输入均不得
   使用可移动 branch、tag 或隐式 `main`。
4. 保留 workflow run URL、run ID 和最终 artifact manifest。

#### 验收条件

- 两次独立的干净 checkout 使用相同输入生成相同 FD、kernel、payload 和 boot image
  哈希；若产物包含不可避免的时间戳，必须记录并验证去除时间戳后的可复现范围。
- package manifest 能从每个产物反查所有源码 commit 和补丁哈希。

### P1：完成无本地 override 的干净解析

当前检查在不包含 `local.toml` 的临时 downstream worktree 和全新输出目录中执行。

#### 要求

在新的临时目录或 CI runner 中执行，不复制 `local.toml`，也不使用
`--local-module`：

```sh
git clone https://github.com/lss717/UniXNU.git references/UniXNU
git -C references/UniXNU checkout d7fafc5d45a795a3b7fc9e9ae313e704c5a1938e
git clone <published-cheeseburger-repository> device

python3 references/UniXNU/unixnu defconfig cheeseburger_defconfig -D device
python3 references/UniXNU/unixnu fetch -D device
python3 references/UniXNU/unixnu status --json -D device \
  > device/docs/migration/clean-status.json
python3 references/UniXNU/unixnu tree -D device
python3 references/UniXNU/scripts/porting-record.py check --device device
```

#### 验收条件

- `fetch` 和 `tree` 完成，无 local source。
- `status` 中 MSM8998 revision 为
  `8227828eb1706b51aea9762bd46006328d91db26`。
- 所有 catalog component 的 repository 和 revision 与记录一致。
- `modules` 中不存在 `origin = "local (feeds)"` 或其他未声明本地路径。
- 保存 `clean-status.json` 并在 porting record 中添加 host evidence。

### P1：完成 UFS 单路径选择与实机矩阵

当前 storage 配置仍包含三种互相重叠的状态：

- defconfig 编译 `CONFIG_XNU_NATIVE_UFS_PROBE=y`；
- kernel arguments 设置 `native-ufs-probe=0`；
- archive plan 执行 `build-native-ufs-runtime.py`；
- 正式 `CONFIG_XNU_NATIVE_UFS` 未启用。

#### 要求

按本文件前述 A/B/C/D 矩阵分别构建。每个镜像只能改变 UFS 路径选择，禁止同时调整
AMFI、root policy、CPU、内存、显示或其他驱动。每个镜像记录：

- Git commits 和 dirty state；
- 有效 `.config`；
- payload 中的有效 kernel arguments；
- kernel、ADT、payload、boot image SHA-256；
- UFS controller 初始化日志；
- LUN 枚举、块大小和容量；
- IOMedia/IOBlockStorageDriver/BSD device 发布者；
- APFS root match 与 mount 日志；
- 读、写、flush、错误恢复和重启持久化结果。

#### 验收条件

- 负向控制 D 不能发布相同 root device，否则现有日志不能证明来自目标 UFS 路径。
- A、B、C 中只有一条路径被选为正式实现。
- 正式配置删除其他 UFS builder、probe 开关和冲突参数。
- `porting.toml` storage area 从 `planned` 升级时必须附实机 evidence，不能只引用源码。

### P1：生成并启动 format 2 产物

#### 要求

在干净 macOS checkout 中依次执行：

```sh
bash scripts/package.sh --check --os 17 --uefi ../uefi-msm8998
bash scripts/package.sh --os 17 --uefi ../uefi-msm8998
```

保存以下内容：

- package preflight 完整日志；
- resolved status 和 patch order；
- `.config`；
- `package.manifest.json`；
- kernel、ADT、payload、FD、boot image 哈希；
- workflow run URL 和 artifact 名称。

实机验证必须按以下顺序记录最后成功点：

1. stock bootloader 进入 BootShim；
2. UEFI 和 UniXNU 菜单；
3. DarwinBoot 加载 payload 并 handoff；
4. XNU early entry、pmap、timer 和中断；
5. BSD/IOKit；
6. 正式 UFS provider；
7. APFS root；
8. launchd；
9. dyld shared cache；
10. SpringBoard 或明确的第一用户态失败点。

#### 验收条件

- format 1 的历史照片、日志和 H91 哈希不得作为 format 2 镜像的实机证据。
- 每个检查点必须绑定本次 format 2 boot image 的 SHA-256。
- 失败时记录最后成功点，不通过同时增加多个补丁掩盖故障来源。

### P2 已完成：将运行时兼容补丁迁入正式 UniXNU 下游分支

运行时兼容补丁现已迁入正式架构
`lss717/UniXNU: upstream-format2-cheeseburger`。

#### 要求

1. 从官方 `578ae75c8e7356d4d562bcfc294ab26dfe640a31` 创建新分支；
2. 将 6 个 `patches/unixnu` 补丁转换为 6 个可审查提交；
3. 对每个提交运行对应 host test，并记录是否适合提交上游；
4. 更新设备仓库的 CI 和 package checkout，使其固定新分支上的完整 commit；
5. 删除 `scripts/package.sh` 中遍历 `patches/unixnu/*.patch` 并执行 `git apply` 的逻辑；
6. 验证不带 `patches/unixnu` 运行时应用也能生成相同 kernel、payload、FD 和 boot image；
7. 在迁移确认前保留 patch 文件作为审计材料，确认后再决定删除或移入历史目录。

#### 验收结果

- `git merge-base d7fafc5d... 578ae75c...` 返回官方基线；
- `git log 578ae75c..d7fafc5d` 只包含已登记的 8 个下游提交；
- `scripts/package.sh` 不创建 worktree、不执行 `git apply`；
- 连续构建前后 downstream checkout 均为 clean；
- manifest 记录 downstream、upstream base、merge-base、tree digest 和 edk2；
- workflow 固定完整 commit，不使用可移动分支名；
- 旧 `lss717/UniXNU main` 不参与 format 2 构建。

### P2 已完成主机侧：修正 MSM8998 CPU hardware compatible

`UniXNU-msm8998-common/dts/msm8998.dtsi` 当前把四个效率核描述为
`arm,cortex-a53`，四个性能核描述为 `arm,cortex-a73`。Linux MSM8998 DTS 对八个 CPU
使用 `qcom,kryo280`。E/P 分类可以继续由 `unixnu,cluster-type` 表达，但硬件
compatible 不应伪装成另一种实现。

#### 要求

- 对照 stock/Linux MSM8998 DTS，将八个 CPU 的 compatible 改为经过来源验证的值。
- 保留 MPIDR `0..3, 0x100..0x103` 和 E/P cluster tag。
- 检查 ArmCpu 是否依赖 compatible；如果不依赖，也要保证导出的 ADT 不产生匹配回归。
- 在 `porting.toml` CPU area 中记录输入文件、内核版本和对应行。

#### 验收结果

- 八个 CPU 的 compatible 已统一为 `qcom,kryo280`；
- MPIDR `0..3, 0x100..0x103`、P/E cluster tag 和 19.2 MHz timebase 保持不变；
- board DTS 通过 `dtc`，47 项 ADT 检查包含 CPU compatible、P/E 和 timebase；
- 实机单核、双簇启动仍未执行，不能把主机检查当作 CPU 运行证据。

### P2：完成 format 1/format 2 ADT 全量差异报告

已有检查覆盖关键属性，但尚未完成逐节点、逐属性的全量对照。

#### 要求

生成机器可读和人工可读两份报告，至少分类：

- 预期新增：common generator、timer、PHY、manifest 和 sidecar placeholder；
- 预期移动：Apple identity 和 typed board properties；
- 仅编码变化：arm-io base 变化后重新计算的 offset；
- 必须保持：GIC、timer、CPU、RAM、UFS、PSCI、root UUID、payload sidecars；
- 未解释差异：视为阻断项。

#### 验收条件

- 每个新增、删除或值变化都有 owner 和原因。
- 不存在未解释的 driver-consumed 属性丢失。
- 报告绑定生成工具 commit、输入 DTS hash、seed 类型和输出 ADT hash。

### P2：完成 fork patch 所有权分类

当前 61 个 patch 是官方 profile、XnuPrelink 和设备 series 解析结果，不等于旧 fork 的所有改动都已
迁移。必须对 `lss717/UniXNU` 与官方基线做完整差异清单。

#### 要求

每个差异归入 ARM64、MSM8998、OS profile、device、component、diagnostic 或 obsolete
之一，并记录：

- 修改文件和符号；
- 适用 XNU/OS build；
- 前置 patch；
- runtime consumer；
- 是否已经被官方实现取代；
- host test 和 device evidence。

#### 验收条件

- 不再依赖旧 fork 目录中未列入 series 的 patch 或修改文件。
- 每个保留 patch 只在一个 owner 层出现一次。
- diagnostic patch 不进入 release-candidate series。

### P2：建立 release-candidate 安全配置

#### 要求

从 diagnostic 配置复制出明确的 release-candidate profile，逐项移除：

```text
native-skip-root-auth=1
amfi_get_out_of_my_way=1
amfi_allow_any_signature=1
cs_enforcement_disable=1
native-skip-exec-signature=1
launchd_unsecure_cache=1
debug-enabled=1
```

同时用真实输入替换全零 `boot-manifest-hash`，验证 VerifiedRoot、owner trust cache、AMFI、
SystemOS policy 和 dyld cache 签名路径。

#### 验收条件

- diagnostic 和 release-candidate 不能通过手工注释切换，必须是明确、可审查的配置选择。
- release-candidate 不包含无条件签名或 root-auth 绕过。
- 每移除一个绕过都记录对应镜像哈希和最后成功检查点。

### 执行顺序和停止条件

后续严格按以下顺序执行：

1. 固定 edk2-msm8998 commit 和所有构建输入；
2. 无 `local.toml` 完成 `fetch/status/tree/record`；
3. 消除 package 对共享 UniXNU checkout 的原地修改；
4. 修正 CPU compatible，完成 DTS 和 ADT 全量差异；
5. 在 macOS 干净环境完成 preflight 和 format 2 构建；
6. 完成 UFS A/B/C/D 实机矩阵并选择唯一正式路径；
7. 完成旧 fork patch 所有权分类；
8. 建立 release-candidate 并逐项恢复安全链；
9. 重新验证全部启动检查点；
10. 更新 porting record、README、CI 和最终产物 manifest。

出现以下任一情况时停止扩展功能，只修复当前阻断项：

- source revision 或输入哈希不明确；
- clean status 与本地 override status 不一致；
- DTS/ADT 出现未解释差异；
- UFS 负向控制仍能发布同一个 root device；
- package 后官方 UniXNU checkout 变脏且未记录；
- 同一个镜像同时改变 UFS、安全链、CPU 或内存等多个变量；
- 实机日志无法绑定到唯一 boot image SHA-256。

### 完成声明模板

只有满足本节全部 P1、P2 验收条件后，才能使用以下声明：

> UniXNU-cheeseburger 已完成 format 2 构建与实机适配；源码、固件、补丁和私有输入均
> 已固定并可追溯；正式 UFS root 路径、启动检查点和 release-candidate 安全链已在指定
> boot image 哈希上验证。

在此之前统一使用：

> UniXNU-cheeseburger 已完成 format 2 静态迁移；构建复现、UFS 单路径、fork patch
> 归属、安全链和 format 2 实机验证仍在进行。

## 2026-11-15 完成情况复核

### 复核结论

本轮迁移已经越过“仅能解析”的阶段，完成了 format 2 主机侧 kernel、payload、
UniXNU EFI、MSM8998 firmware volume 和 Android boot image 的实际构建。构建输入、
产物大小和 SHA-256 已记录在 `docs/migration/host-build-2026-11-15.md`。

但是，按照本文件定义的完整完成标准，当前仍不能声明“完整适配完成”。准确状态是：

> UniXNU-cheeseburger 已完成 format 2 主机构建迁移、正式 UniXNU downstream
> 固定和 boot image 打包；正式 UFS、format 2 实机启动及 release-candidate
> 安全链仍待完成。

### 已通过项目

| 项目 | 结果 | 证据 |
| --- | --- | --- |
| format 2 schema | 通过 | 官方 resolver 报告 `schema_version = 2` |
| 四层 profile | 通过 | ARM64、MSM8998、iPadOS 17.7.11、device 均解析 |
| canonical components | 通过 | 7 个组件被解析 |
| patch order | 通过 | 61 个显式、有序 patch |
| porting record 结构 | 通过 | 13 个 structured area checked |
| MSM8998 source pin | 通过 | `8227828eb1706b51aea9762bd46006328d91db26` |
| edk2-msm8998 source pin | 通过 | `da889248ba63e3b76003e016ee8744b34aa91a01` |
| downstream UniXNU pin | 通过 | `d7fafc5d45a795a3b7fc9e9ae313e704c5a1938e`，merge-base 为官方基线 |
| kernel build | 通过归一化复现 | 三次干净构建均通过；枚举 UUID/debug/KCID/build-date 后逐字节一致 |
| payload build | 通过 | payload 与提取 ADT 有记录哈希 |
| UniXNU EFI package | 通过 | `UniXNU.efi`、`UniXNUDxe.efi`、`UniXNURamDiskDxe.efi` 已生成 |
| MSM8998 FD | 通过归一化复现 | `MSM8998PKG_UEFI.fd` 已生成；差异仅为 `__TIME__` |
| boot image | 通过主机检查 | header、地址范围、长度和 `OP5ESP` label 检查通过 |
| ADT 结构迁移 | 通过主机检查 | format 1/2 全量比较无关键节点丢失 |
| 仓库工作区 | 通过 | package 前后 downstream checkout 均为 clean；common/submodule pin 已固定 |

主机构建记录中的测试 seed 为 `0x12345678`。相关产物不能作为目标手机的安装镜像或
实机适配证据。

### P1 已完成：package 直接使用固定 downstream commit

`scripts/package.sh` 现在直接运行
`lss717/UniXNU: upstream-format2-cheeseburger` 的固定 checkout。脚本在构建前
校验 HEAD、clean 状态和官方 merge-base，不创建 worktree，也不执行 `git apply`。
DarwinBoot 的两处 EDK II 兼容改动已作为独立 downstream 子模块提交
`2ba9cad556f1794fb74ac8af164bc34b0e096338` 固定，而不是留在子模块工作区。

正式 downstream 分支当前为：

```text
repository: https://github.com/lss717/UniXNU
branch: upstream-format2-cheeseburger
upstream base: 578ae75c8e7356d4d562bcfc294ab26dfe640a31
downstream commit: d7fafc5d45a795a3b7fc9e9ae313e704c5a1938e
merge-base: 578ae75c8e7356d4d562bcfc294ab26dfe640a31
source tree: 2f1ea33e2b842c2529fe0698955473b2178df918
```

8 个独立提交依次覆盖 UFS profile 导出、PMGR boot-progress 清理、Xcode warning
兼容、EmbeddedPkg GUID、legacy FdtLib、external firmware release、DarwinBoot
子模块固定和 prelink staging path。`package.manifest.json` 记录 downstream
commit、upstream base、merge-base、source-tree digest、MSM8998 common 和 edk2
revision，不再记录运行时 patch hash。

三次干净构建都产出 kernel、payload、FD 和 boot image，并自动通过 47 项 ADT
检查。DTB 原始哈希逐字节相同；kernel 和 FD 的时间/身份字段已逐项定位，归一化后
两次独立重建均为 0 字节差异。原始产物不能上机，因为仍使用测试 seed。

### P1 已完成：无 local override 的 clean status

已在不包含 `local.toml` 的全新 UniXNU worktree 和全新输出目录中完成
`fetch`、`status --json`、`tree` 和 porting record check。结果保存为：

- `docs/migration/clean-status.json`；
- `docs/migration/clean-fetch.log`；
- `docs/migration/clean-tree.log`；
- `docs/migration/clean-record.log`。

clean status 包含 23 个 modules、61 个 patches，所有 module 的 `local` 均为
`false`，不存在 `origin = "local (feeds)"`。

### P1 未完成：正式 UFS 路径未验证

当前 archive plan 已删除 `build-native-ufs-runtime.py`，这是有效改进。当前实际结构是：

- `RamBlock` 被构建并通过 `bind-native-ufs-storage.py` 绑定存储接口；
- `NativeUfsProbe` 被编译；
- kernel arguments 设置 `native-ufs-probe=0`；
- `CONFIG_XNU_NATIVE_UFS` 仍未启用；
- 没有 format 2 实机 UFS read/write/flush/reboot evidence。

因此当前构建成功不能证明真实 UFS root 已适配。必须继续执行 A/B/C/D 单路径矩阵，
并从日志中唯一确认 root block device 的 provider。

文档漂移已修正：`README.md` 和 `docs/migration/README.md` 现在都明确说明
`build-native-ufs-runtime.py` 不在 archive plan，当前只有 RamBlock 绑定和
`NativeUfsProbe` 负向控制。

`scripts/package.sh` 目前已接入 `UNIXNU_TRUST_CACHE`、`UNIXNU_DATA_RAMDISK`、
`UNIXNU_GPU_ADDON` 和 `UNIXNU_SKIP_SETUP`，并在打包后自动提取 payload ADT。
带 trust cache 和 Data seed 的构建已再次通过全部 47 项 ADT 检查。

### P1 未完成：format 2 实机启动尚未复核

`docs/migration/host-build-2026-11-15.md` 明确列出以下项目未验证：

- hardware boot；
- storage；
- display、input、GPU；
- release security path。

必须使用真实设备 platform seed 重新构建，并将实机日志绑定到唯一 boot image
SHA-256。依次验证 BootShim、UEFI、UniXNU、DarwinBoot、XNU、BSD/IOKit、正式 UFS、
APFS、launchd 和 dyld。测试 seed 产物不能用于这一验收。

### P2 已完成主机侧：CPU compatible 已改为 Kryo 280

MSM8998 common 已修正 MPIDR、P/E cluster mapping 和 hardware compatible：

```text
0..3             Cortex-A73 / P
0x100..0x103     Cortex-A53 / E
```

八个 CPU 现在统一使用 Linux MSM8998 DTS 的 `qcom,kryo280`，同时保留
`unixnu,cluster-type`。ArmCpu 源码读取 cluster tag，不依赖 CPU compatible；
47 项 ADT 检查已锁定 compatible、MPIDR、P/E 和 timebase。

当前完成范围和剩余边界是：

- 主机侧 DTS/ADT 已完成；
- `cpus=1` 仍是当前诊断启动参数；
- 单核、双核和双 cluster 的实机启动与 IPI/timer 验证尚未执行。

因此 `porting.toml` 的 CPU area 升级为 `implemented`，但 completion 证据仍明确
限定为 host-side，不能解释为 CPU 已在 format 2 实机运行。

### P2 未完成：release-candidate 安全链

当前配置仍包含：

```text
native-skip-root-auth=1
amfi_get_out_of_my_way=1
amfi_allow_any_signature=1
cs_enforcement_disable=1
native-skip-exec-signature=1
launchd_unsecure_cache=1
debug-enabled=1
```

`boot-manifest-hash` 仍为全零，`CONFIG_XNU_VERIFIED_ROOT_PROBE` 仍未启用。因此当前
产物只能标记为 diagnostic build，不能称为 release candidate。

### P2 缺证据：fork patch 所有权

当前 resolver 能产生 61 个显式 patch，且 PMGR patch 顺序已经修复。但尚未看到旧
`lss717/UniXNU` 与官方基线的完整文件级差异分类报告。必须继续证明：

- 旧 fork 中没有未迁移但运行时仍依赖的修改；
- 每个保留 patch 只有一个 owner；
- diagnostic patch 不进入 release-candidate；
- 6 个 UniXNU compatibility patches 已进入 downstream 提交；其余旧 fork 差异的
  长期 owner 仍待分类。

### 当前完成度

| 范围 | 状态 |
| --- | --- |
| format 2 静态配置 | 完成 |
| macOS kernel/payload 构建 | 完成 |
| UniXNU EFI package | 完成 |
| MSM8998 FD 与 boot image 打包 | 完成主机验证 |
| ADT 迁移 | 完成主机结构验证 |
| 固定 UniXNU/MSM8998/edk2 revision | 完成 |
| 无 local override 的 clean fetch/tree | 完成 |
| package 不污染输入 checkout | 完成 |
| 正式 UFS root | 未完成 |
| format 2 实机启动 | 未完成 |
| release-candidate 安全链 | 未完成 |
| fork patch 全量归属 | 缺证据 |

### 后续优先顺序

1. 使用真实设备 seed 和匹配的私有输入生成可上机 format 2 boot image；
2. 执行 UFS A/B/C/D 矩阵并选择唯一正式存储路径；
3. 将实机日志绑定 boot image SHA-256，重验完整启动检查点；
4. 完成旧 format 1 fork 剩余差异的 owner 结论；
5. 建立独立 release-candidate 配置并恢复安全链；
6. 在 GitHub Actions 上实际运行固定 downstream commit 的 package workflow；
7. 更新 porting record 的实机 evidence 和最终发布声明。

## 2026-11-15 TWRP 实机验证入口

当前 OnePlus 5 已通过 ADB 连接，主机识别结果为：

```text
state: recovery
product: omni_cheeseburger
model: ONEPLUS_A5000
device: cheeseburger
```

这表示 format 2 工作可以进入实机准备阶段，但不改变上文的验收边界：测试 seed
`0x12345678` 生成的产物仍然禁止上机，主机侧构建成功也不能代替启动证据。

### 本轮执行边界

1. 第一轮只允许只读采集：设备属性、当前槽位、分区表、boot 分区大小、挂载状态、
   TWRP/内核版本和可用日志路径。
2. 在任何写入前，必须确认设备代号为 `cheeseburger`，记录当前 active slot，并保存
   当前 boot 镜像或确认存在可恢复的已知良好备份。
3. 必须取得真实 `UNIXNU_UNIQUE_CHIP_ID`，并确认 trust cache、Data ramdisk/seed 与该
   设备和目标系统版本匹配；不得把测试 seed 替换成猜测值。
4. 使用真实输入重新构建后，记录 boot image SHA-256、所有固定 source revision、构建
   参数和 manifest；实机日志只能绑定到这一唯一镜像。
5. 首次启动优先采用可恢复、非持久化的启动方式；如果设备/bootloader 不支持，则在
   明确恢复路径和精确目标分区后再单独决定是否写入。
6. 未经单独确认，不得执行 `fastboot flash`、`adb shell dd`、分区擦除、格式化、修改
   GPT、切换槽位或覆盖 recovery/boot 等关键分区。
7. 首轮只验证最早检查点：BootShim、UEFI 串口/屏幕、DarwinBoot。只有前一检查点有
   明确证据后，才继续 XNU、BSD/IOKit 和 UFS 路径测试。

### 立即执行顺序

1. 采集并保存 TWRP 只读基线，脱敏记录设备标识，不把私有 seed 写入仓库或日志。
2. 核对本机是否已有该设备的 stock/current boot 备份及恢复工具。
3. 定位真实 platform seed 的可靠来源；若 TWRP 不能安全提供，则记录阻塞，不猜测。
4. 核对 trust cache 和 Data ramdisk 的来源、系统版本和哈希。
5. 在 macOS 构建机使用固定 downstream revision 生成新的 diagnostic boot image，并运行
   47 项 ADT、manifest、镜像结构和 SHA-256 检查。
6. 输出“候选镜像信息 + 恢复步骤 + 首次启动命令”供人工确认；在得到确认前不写设备。
7. 首次启动后按 BootShim -> UEFI -> DarwinBoot -> XNU 的顺序保存原始日志，并将最后
   成功点及失败签名更新回本文档和 porting evidence。

### 本轮完成条件

- TWRP 基线和恢复路径已记录；
- 真实 seed 与私有输入已核验；
- 候选 boot image 使用真实输入构建并拥有唯一 SHA-256；
- 给出不会混淆 boot/recovery/slot 的精确启动或写入方案；
- 在实际写设备前保留独立人工确认点。

### 2026-11-15 只读基线结果

机器可读记录保存在
`docs/migration/twrp-baseline-2026-11-15.json`。本轮没有写入设备，没有执行
`fastboot flash`、`adb shell dd` 写入、erase、format、GPT 修改或槽位切换。

| 项目 | 实测结果 |
| --- | --- |
| 设备 | `cheeseburger` / `omni_cheeseburger` / ONEPLUS A5000 |
| 状态 | TWRP recovery |
| TWRP | `3.4.0_9-10-mauronofrio` |
| Recovery kernel | Linux `4.4.153-blu_spark-CUSTOM #168` |
| A/B slot | 无；slot property 为空，分区表中没有 `_a`/`_b` |
| `boot` | `/dev/block/sde19`，64 MiB，SHA-256 `86d734d97b29bedda17659d3c7499fd8dfc24c5bb579f6c3c32318e689ba1e55` |
| `recovery` | `/dev/block/sde22`，64 MiB，SHA-256 `75d09b68be17d2da6072b932796d498bd24e6428f2adf26a63c146a23258ee03` |
| `system` | `/dev/block/sde21`，3 GiB，ext4 |
| `userdata` | `/dev/block/sda13`，64 GiB，ext4，当前挂载于 `/data` 和 `/sdcard` |
| `userdata_ios` | `/dev/block/sda14`，49.212 GiB，当前未挂载，`blkid` 未识别文件系统 |
| `cache` | `/dev/block/sda3`，256 MiB，ext4，挂载于 `/cache` |
| `firmware` | `/dev/block/sde10`，110 MiB，vfat，只读挂载于 `/firmware` |

这台手机的数据区已经拆成两个独立分区，不能继续把旧的 113 GiB 单一
`userdata` 几何当作当前事实：

| 分区 | GPT/LBA | 角色 |
| --- | --- | --- |
| `userdata` | `0x014F40..0x1014F3F` | Android ext4 `/data`，64 GiB |
| `userdata_ios` | `0x1014F40..0x1C627FA` | iOS SHNGROOT/Data 目标，49.212 GiB |

`scripts/NativeUfsProbe` 和修复后的 `scripts/RootLayout.h` 必须按 GPT 名称选择
`userdata_ios`，绝对窗口地址以它的 first LBA 为基准。官方 build schema 仍把
分区几何表命名为 `[userdata]`；`board.toml` 在此表中填写的是
`userdata_ios` 的 `first_lba = 0x1014F40`，避免生成的 iOS 镜像覆盖 Android
的 64 GiB `/data`。任何 `fastboot flash userdata` 都不是本项目的 iOS 安装命令。

TWRP 的 `/sdcard/TWRP/BACKUPS` 为空。为避免后续误操作，已经通过只读读取把当前
`boot` 和 `recovery` 保存到仓库外的
`E:\WindowsData\Project\ios-backups\op5-20261115-twrp`，两个文件均为 64 MiB、
只读属性，主机 SHA-256 与设备侧计算结果一致。

用户指出的 `192.168.10.127` 上存在完整备份：

```text
/home/ls/UniXNU/.cache/firmware/backups/20261024-125857
```

该备份的 `SHA256SUMS` 共 84 项，已全部校验成功；包含全部命名系统分区以及
sda–sdf 的主/备 GPT。它明确不包含 `userdata`、`userdata_ios` 和 `cache`。

该备份不是当前状态的快照：

| 分区 | 2026-10-24 备份 | 当前设备 |
| --- | --- | --- |
| `boot` | `b3f8c0b4...9325453` | `86d734d9...ba1e55` |
| `recovery` | `f6625f13...bd2a7` | `75d09b68...58ee03` |
| `system` | `acb4a621...3b892` | `c2ded155...c389fe` |
| `vendor` | `dbe1e60b...e360` | `f2e4e93f...e24e7a` |
| `persist` | `69c67d37...a8ca` | `fe5955e7...9e6448` |

因此它可作为“恢复 2026-10-24 Android 系统基线”的完整回滚包，但不能当作当前
`userdata`/`userdata_ios` 的备份，也不能在未确认的情况下用它覆盖当前系统。

可用日志与只读哈希：

| 路径 | 大小 | SHA-256 |
| --- | ---: | --- |
| `/tmp/recovery.log` | 26,504 | `e1aba5f0cedaf0c0bbf761b641db06062ce5c87c0d154eb88680a309464ed8c7` |
| `/cache/recovery/last_log` | 44,315 | `c2637de9a8ad4b0dea050ac42f782c5257a05fd7a50b9037e7a1f117ce672e08` |
| `/cache/recovery/last_kmsg` | 137,371 | `29b3b1f9bc001670d707f86137092de3af6e16f6171293d87024b2302eccb601` |

### 真实 platform seed 已安全生成

`scripts/make-cheeseburger-adt.py` 明确把 `unique-chip-id` 定义为 8 字节
**软件身份 seed**，用于生成 XNU `IOPlatformUUID`，不是 Apple/Qualcomm 硬件 ID。
用户已授权生成一个新的软件身份 seed。

该值由操作系统 CSPRNG 生成，恰好 8 字节。明文没有进入 Git、本文档、
`package.manifest.json`、构建日志或聊天输出。

| 项目 | 值 |
| --- | --- |
| 格式 | raw 8-byte binary |
| SHA-256 指纹 | `1e8a3854f9c16f2ab693c0a31793d80f07a9671efa53b22514d53f2284c2beae` |
| Windows 主副本 | `E:\WindowsData\Project\ios-backups\op5-20261115-twrp\secrets\unique-chip-id.seed` |
| 独立恢复副本 | `192.168.10.127:/home/ls/private/op5-platform-seed/unique-chip-id.seed` |
| Mac 构建副本 | `/Users/lss/private/op5-platform-seed/unique-chip-id.seed` |

Windows 文件/目录 ACL 仅允许当前用户、SYSTEM 和 Administrators；远程及 Mac 副本
权限为 `600`。

Trust cache 和 Data seed 已与固定构建记录核对：

```text
OS profile: ipados-17.7.11-21H461
trust cache SHA-256: 7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7
Data seed SHA-256: d474b36616dca3227beb09243276e20818e32b11f55ad0cc2c3f1d5c0a656d41
```

两者均匹配 `host-build-2026-11-15.md` 中记录的 21H461 构建输入。

### 候选 diagnostic boot image

Windows 仓库外候选目录：

```text
E:\WindowsData\Project\ios-backups\op5-20261115-candidate
```

`boot.img` SHA-256：

```text
935a54dfe1afa622e4bfad6cdd9ed4cd8a04190fa9021d7f2cae7e3e1eb36c06
```

构建固定输入：

```text
device:            85490ba682ab1e178a35f40fd80f42ea3aac08c9
UniXNU downstream: d7fafc5d45a795a3b7fc9e9ae313e704c5a1938e
UniXNU base:       578ae75c8e7356d4d562bcfc294ab26dfe640a31
DarwinBoot:        2ba9cad556f1794fb74ac8af164bc34b0e096338
MSM8998 common:    8227828eb1706b51aea9762bd46006328d91db26
edk2-msm8998:      da889248ba63e3b76003e016ee8744b34aa91a01
```

构建通过 47 项 ADT、FD/FV、boot header/load range、payload label/member、
manifest revision 和 SHA-256 检查。已确认 seed 明文不在构建日志和 manifest 中。
该镜像已进行一次非持久实机启动。候选产物表在
`docs/migration/device-candidate-2026-11-15.md`，实机结果在
`docs/migration/device-run-2026-11-15-01.md`。

### 非持久启动方案

设备是 A-only，历史 H380–H403 也使用过 `fastboot boot`，因此首选不写分区的启动：

```sh
# 仅在用户再次确认后执行
adb reboot bootloader
fastboot devices
fastboot boot E:\WindowsData\Project\ios-backups\op5-20261115-candidate\boot.img
```

该方案不执行 `fastboot flash`，不改变 `boot`、`recovery`、GPT 或槽位。失败时优先
重启回 Android/TWRP；若 bootloader 状态异常，再基于已校验的当前状态备份或
2026-10-24 整机备份制定单独恢复操作。任何 raw 分区写回仍必须再次确认目标分区和
SHA-256。

### 2026-11-15 首次非持久实机启动结果

实机启动严格绑定到 boot SHA-256：

```text
935a54dfe1afa622e4bfad6cdd9ed4cd8a04190fa9021d7f2cae7e3e1eb36c06
```

执行路径：

```text
adb reboot bootloader
fastboot product: msm8998
fastboot boot: Sending OKAY; Booting OKAY
```

现场先显示完整图形 UEFI 菜单。用户手动选择 `OnePlus 5 IOS` 后，DarwinBoot
短暂运行并很快返回同一个菜单；超过 30 秒主机没有 ADB/Fastboot 枚举。该轮没有
flash、dd、erase、format、GPT 或槽位操作。

| 检查点 | 状态 |
| --- | --- |
| BootShim / FD / UEFI | 已到达 |
| UniXNU EFI application 和图形菜单 | 已到达 |
| `OnePlus 5 IOS` 启动项派发 | 已到达 |
| DarwinBoot window allocation | 失败，`EFI_OUT_OF_RESOURCES` |
| DarwinBoot kernel handoff | 未到达 |
| XNU | 未到达 |

静态上，候选 `unixnu.toml` 为 `timeout = 3`、`default = "xnu"`，菜单项为
`OnePlus 5 IOS`、`Restart`、`Power off`。候选 ADT 请求 4 GiB DarwinBoot
window（`0x100000000`），但没有 `darwinboot-memory-excluded`，因此当前
DarwinBoot 会走要求单段连续空间的 `AllocateWindow` 路径。

照片中实际出现：

```text
NO WINDOW 100000000 BIG RUN FCCD0000 @ 1800000
FREE ...
```

该字符串由 DarwinBoot `AllocateWindow` 在连续窗口分配失败后直接写 framebuffer：

```text
NO WINDOW %lx BIG RUN %lx @ %lx
```

数值换算为：

| 项目 | 值 |
| --- | --- |
| 请求 | `0x100000000` = 4 GiB |
| 最大连续 run | `0xFCCD0000` = 3.950012 GiB |
| run 起始 | `0x01800000` |
| run 结束（不含） | `0xFE4D0000` |
| 相对 4 GiB 缺口 | `0x03330000` = 51.188 MiB |

因此不是布局重叠，也不是 XNU panic：当前 UEFI memory map 没有任何单个
`EfiConventionalMemory` run 能满足 4 GiB 请求。`AllocateWindow` 返回
`EFI_OUT_OF_RESOURCES`，DarwinBoot 返回 UniXNU loader protocol，
`UniXNUBootEntry` 再把状态传回 `Main.c`，菜单因此以无倒计时状态重绘。
状态没有被吞掉；菜单还应有一条较小的 `'OnePlus 5 IOS' returned:
Out of Resources` 提示，但当前照片说明没有记录到它。

boot image 的加载范围本身没有重叠：

```text
kernel   0x10000000 + 0x181B63
ramdisk  0x12000000 + 0x04C00000
tags     0x18000000
```

这些范围本身不能解释 51.188 MiB 的缺口，决定性证据仍是 UEFI 最长连续 run。

历史 H380–H403 均通过非持久 `fastboot boot` 进入 XNU/IOKit；H403 日志持续到
约 687 秒，包含 UFS alive、GIC/timer 和用户态容器活动。这说明当前设备与 ABL
本身能够通过 BootShim、UEFI 和 DarwinBoot，但那些轮次没有保存 UEFI 屏幕文字，
不能直接解释本次 format 2 的最终画面。

需要优先匹配的最后一行：

| 最后一行 | 最小解释 |
| --- | --- |
| `Booting OnePlus 5 IOS in N s` 且不再变化 | 停在 UniXNU 菜单倒计时/定时器路径 |
| `Select an entry` 位于仍存活的菜单底部 | 启动项已返回，菜单关闭倒计时并等待下一次输入 |
| `'OnePlus 5 IOS' returned: Out of Resources` | 菜单仍存活，DarwinBoot 已返回并报告资源不足 |
| `NO WINDOW 100000000 BIG RUN ...` | 4 GiB 连续窗口分配失败，已确认根因 |
| `DARWINBOOT: unsupported inputs or exception level` | loader 收到不支持的请求或异常级 |
| `DARWINBOOT: ramdisk layout rejected` | Data RAMDisk 布局校验失败 |
| `DARWINBOOT: boot args rejected at ...` | command line 字符或长度校验失败 |
| `DARWINBOOT: boot refused: ...` | DarwinBoot 已知失败并返回固件 |
| `DARWINBOOT: inputs validated; exiting boot services` | DarwinBoot 已到 EBS 前，XNU 仍未证明 |
| `DARWINBOOT: hand-off to the kernel` | loader 已完成 handoff，XNU 入口仍需色带/日志证明 |
| `Shell>` 或 `UEFI Shell` | UniXNU application 未启动 |
| OnePlus logo / 黑屏 | 停在 UEFI 之前 |

完整证据和恢复按键方案见
`docs/migration/device-run-2026-11-15-01.md`。该方案只描述操作，本轮没有执行
恢复按键。

### 第二版窗口修复：128 MiB

历史 H236br–H236bu 已经在同型号、同固件上做过单次 `AllocatePages` 尺寸实验：

- 256 MiB 在多个地址、两种内存类型和两种分配方式下全部返回
  `EFI_OUT_OF_RESOURCES`；
- 128 MiB 固定分配成功。

因此不能只把 4 GiB 改成“略小于 `0xFCCD0000`”的 3.75 GiB；那仍会超过该
固件的单次分配上限。第二版选择 `owned_memory_size = 0x08000000`（128 MiB）：

| 项目 | 数值 |
| --- | ---: |
| 窗口 | 134,217,728 B（128 MiB） |
| kernel | 52,043,776 B |
| packed Data seed | 4,370,688 B |
| trust cache | 61,356 B |
| ADT | 16,408 B |
| 已知文件合计 | 56,492,228 B（53.875 MiB） |
| 扣除文件后的余量 | 77,725,500 B（74.125 MiB） |

余量用于 16 KiB 对齐、boot args、Mach-O 段空洞、sidecar 头部和 XNU 早期页表，
同时仍低于已经实测成功的 128 MiB 分配上限。

对应代码改动：

- DarwinBoot `d90c9113836d61af3e90285d873cae9edd528d09` 允许 128 MiB 下限；
- downstream UniXNU `7d3f04dacfbffb343db936e24cb9af69877e078a` 允许 ADT
  生成器接受 128 MiB 窗口；
- 设备 `board.toml` 固定新 downstream revision 和 `0x08000000`。

第二版将使用同一组固定输入重建 ADT、payload、FD 和 boot image，并重新执行
47 项 ADT、FD/FV、boot header、payload、manifest 和 SHA-256 检查。第一版
boot image 和失败证据保持不变。

### 第二版非持久实机结果

第二版 boot image：

```text
de86e1c2c773389bd1c3d23e0a5c275e2680607f8c712786f56f709aa2a1ea59
```

`fastboot product` 为 `msm8998`，仅执行 `fastboot boot`，返回
`Sending OKAY; Booting OKAY`。用户手动选择 IOS 后，屏幕看到：

```text
Synchronous Exception at 0x000000017ADBC378
UNIXNU: menu: enter: booting 0
UNIXNU: boot: entry 'xnu' (OnePlus 5 IOS)
UNIXNU: xnu: handing off 0 staged kext(s) to loader
```

随后菜单仍在，IOS 条目上保留同一异常文字。该轮没有自动重试，也没有执行
flash、erase、format、dd、GPT 或槽位操作。

检查点结果：

| 检查点 | 状态 |
| --- | --- |
| BootShim / FD / UEFI 菜单 | 已到达 |
| 手动 IOS 启动项派发 | 已到达 |
| `RunXnu -> Loader->Boot` | 已到达 |
| 旧 4 GiB `NO WINDOW` 分支 | 未再出现 |
| DarwinBoot 精确子阶段 | 未确定 |
| 完整 kernel handoff / XNU entry | 未证明 |

`Synchronous Exception at` 来自 EDK2 的
`ArmPkg/Library/DefaultExceptionHandlerLib/AArch64/DefaultExceptionHandler.c`。
该行打印的是 `SystemContextAArch64->ELR`，即出错指令地址，不是 FAR，也不是
LR。上下文中的 `ESR/FAR/SPSR/LR/SP/FP` 存在，但 RELEASE 构建把详细 dump
编译掉了，因此当前只有 ELR。

第二版 `DarwinBoot.efi` 是可重定位 PE，`ImageBase=0`、`SizeOfImage=0xC000`、
`SectionAlignment=0x1000`。没有运行时 `LoadedImage.ImageBase`，就不能把
`0x17ADBC378` 合法映射到某个 DarwinBoot 函数。按不同对齐 base 反推会得到
`Boot`、`RamdiskLayout`、`ReadBootVideo` 或 `GetMap` 等多个互相竞争的候选，
因此当前不归因。

`RunXnu` 在打印 `handing off 0 staged kext(s)` 后立即调用
`Loader->Boot`。在 DarwinBoot 第一次打印正常 `DARWINBOOT: ARM64 ...` 之前，
仍可能失败于 ramdisk layout、kernel read/parse、ADT、sidecar、video、
window allocation 或 image copy 任一步。旧 `NO WINDOW` 消失只能说明没有走
旧的 4 GiB 失败分支，不能单独证明 128 MiB window 已完成分配。

`0 staged kext(s)` 是预期结果：配置为 `[kits] autoload = false`，且没有
`[[entry.kext]]`，因此不把 kext 数量当作根因。

下一步只做诊断，不盲改内存/UFS/USB：

1. 在 EDK2 AArch64 默认异常处理器中无条件输出
   `ELR/SPSR/ESR/FAR/LR/SP/FP`。
2. 调用已有 `GetImageName(ELR, ...)`，输出 image name、runtime image base 和
   `ELR - ImageBase`，解决本次无法精确符号化的问题。
3. 在 DarwinBoot 的关键调用前输出短阶段码，至少覆盖 request、
   ramdisk、read-kernel、parse-kernel、read-ADT、ADT-parse、video、
   allocate-window、place-image 和 copy-kernel。
4. 保留手动选择 IOS 和禁止自动重试的策略；诊断镜像通过检查和单独授权后才
   能再次上机。

暂不启用 `darwinboot-memory-excluded`。当前 iPadOS 17.7.11 profile 没有
`window-memory-excluded-ranges.patch`，直接发布 excluded 属性会声明一个
目标内核尚未证明支持的能力。

若必须在改窗口前再次截取明确 EFI_STATUS，最小诊断补丁位于 DarwinBoot 子模块：
在 allocation error 后用 `DiagDraw` 显示
`NO KERNEL WINDOW %r size=0x...`，并 `Stall` 15 秒再返回。当前照片和
`AllocateWindow` 源码路径已经足以确认本次失败，因此该补丁不是继续修复的前置条件。

### 第三轮完整异常上下文与 buffer-overlap 修复

第三版诊断镜像只做非持久 `fastboot boot`，使用 SHA-256
`7edcefbdcd80093b8ae1c376646b01fe692a9750489d2f5b66671801f742c1ef`。
手动选择 IOS 后，完整异常上下文和全部 DarwinBoot 阶段标记均被屏幕记录：

```text
EXC ELR=0x000000017ADBC378 ESR=0x0000000096000061 FAR=0x00000000A1A10000 SPSR=0x0000000020000005
EXC LR=0x000000017ADBEAE4 SP=0x000000016FFFCE90 FP=0x000000016FFFDF60
DARWINBOOT: stage=copy-kernel used=0x3070000
```

`ESR=0x96000061` 解码为当前 EL 的 Data Abort、写/缓存维护、DFSC
`0x21`（alignment fault）。运行时镜像基址为 `0x17ADBB000`：

- `ELR - base = 0x1378`，精确落在 `InternalMemSetMem` 的
  `dc zva, x3`；
- `LR - base = 0x3AE4`，精确是 `Boot` 调用 `ZeroMem(Address, Used)` 的
  返回地址。

根因不是 kernel segment 的 `CopyMem`，而是平台内存图重叠：
`A1A10000 + 0x200000` 是 `InMemorySerialPortLib` 的调试缓冲，并被
`msm8998Dxe` 映射为 `EFI_MEMORY_UC | EFI_MEMORY_XP`；但 8 GiB RAM 描述仍把
它列为 `EfiConventionalMemory`。DarwinBoot 因而把这段设备属性内存包含进
128 MiB window，`DC ZVA` 在 `0xA1A10000` 触发对齐异常。

EDK2 修复提交：

```text
f3ca800eaf67a0450ee3445560da463cc0eab05c
fix(memory): reserve in-memory serial buffer
```

修复把 8 GiB RAM 首段拆为：

```text
0xA0000000 + 0x01A10000  conventional
0xA1A10000 + 0x00200000  reserved and uncached
0xA1C10000 + 0xDE3F0000  conventional
```

新的 buffer-fix 候选已经通过 47 项 ADT、FD/FV、模块、payload、boot
header/load range、manifest revision 和 SHA-256 检查：

```text
boot.img   2f1761df0784061c15e7efb2f4bf3f74baa1144116553619cdcc36c01f0605d7
payload    e8bf8afb439ea7f89b3dc9a7cae685521dd1eb508155ef68007974fda1d06719
kernel     e66bd2737a1662039c497355df4a09110c0967d9c55241408f8787aaa14cc743
FD         2eda3f89b013bde34e9b7ee2902acf88811bce963f1f7817380ba89d236419ee
manifest   66634342b3d85db152bdeb35910f953ae010ea60fb1c2ad8171d04aeb055722a
```

候选目录：

```text
E:\WindowsData\Project\ios-backups\op5-20261115-diag-bufferfix
```

完整记录和原图见
`docs/migration/device-run-2026-11-15-03.md`。下一次实机测试只允许使用上面的
buffer-fix SHA-256，保持非持久启动、手动选择 IOS 和不连续自动重试。

### 第四轮 buffer-fix 实机结果与入口探针

buffer-fix 候选已完成一次非持久启动：

```text
boot.img 2f1761df0784061c15e7efb2f4bf3f74baa1144116553619cdcc36c01f0605d7
```

此前 `copy-kernel` 的 alignment fault 没有再次出现。DarwinBoot 完整通过
kernel、ADT、video 和命令行验证，并打印：

```text
stage=place-image pa=0xA1E04000 bytes=0x08000000
stage=copy-kernel used=0x6070000
ARM64 va=0xFFFFFFF007004000 pa=0xA1E04000 args=0xA7A30000 entry=0xA21F1488 owned=0x08000000
video base=0x9D400000 display=1 row=4320 width=1080 height=1920 depth=32
inputs validated; exiting boot services
```

`inputs validated; exiting boot services` 是 EBS 调用前的日志。顶部绿色横条由
`DarwinEnter` 在 `ExitBootServices` 成功返回后绘制，因此本轮已经证明：

- EBS 成功；
- `DarwinEnter` 已开始；
- 尚未证明 XNU `_start`、`SCTLR_EL1` 写入或 MMU enable 后执行。

绿条出现后紧接黑屏，不能单独解释为 CPU 未进入 XNU，也不能排除 framebuffer
在 handoff 后失显示。当前窗口 `0xA1E04000` 已具有 H236cj 要求的 2 MiB 内
`0x4000` 偏移。

下一版最小入口探针已经构建：

```text
probe kernel  e90ee4837c68d55f595a8ea37b1d375c3c4437351bf63a16ba3f94e0dff9fcfa
probe payload 26ceba612810a6f15d531832bcd916c121c8b09c7c01a928fd848c9cd54764b0
probe boot    01facb8e87233efa4e7426f6dff873167e2569f89cf1319c77c02931de2b9258
```

探针含义：

- 绿条：EBS 返回且 `DarwinEnter` 开始；
- 蓝带：XNU `_start` 执行；
- 品红带：已到 `msr SCTLR_EL1, x0` 前；
- 6 秒后 PSCI 重启：MMU enable 点之后仍在执行。

完整记录见 `docs/migration/device-run-2026-11-16-01.md`。Windows 候选位于
`E:\WindowsData\Project\ios-backups\op5-20261116-entry-probe`。设备当前黑屏且
没有 ADB/Fastboot 枚举；下一次测试需要用户先手动回到 TWRP，随后只做一次
非持久启动。

### 第五轮入口探针结果与 reset 分类

入口探针 boot image
`01facb8e87233efa4e7426f6dff873167e2569f89cf1319c77c02931de2b9258`
已完成一次非持久实机启动。屏幕同时出现三条按源码常量绘制的色带：

| 颜色 | 常量 | 位置 | 结论 |
| --- | --- | --- | --- |
| 绿 | `0xFF00FF00` | 顶部 1/16 | EBS 成功、`DarwinEnter` 开始 |
| 品红 | `0xFFFF00FF` | stage 2 band | 已到 `msr SCTLR_EL1, x0` 前 |
| 蓝 | `0xFF0000FF` | stage 1 band 8 | XNU `_start` 已执行 |

随后约 6 秒设备自动重启并进入 Android。该重启对应 stage 3 的内建
`cntpct_el0` 等待和 PSCI `0x84000009 SYSTEM_RESET`，因此已经证明：

- XNU entry branch 成功；
- `boot_args` 指针可读；
- `SCTLR_EL1` 写入前的执行路径到达；
- MMU enable 点之后继续运行到 stage 3；
- PSCI reset 路径有效。

这不再是未知 watchdog 的首选解释。`fastboot boot` 本身不写 boot 分区，所以
reset 后进入已安装的 Android 是预期结果。

Android 侧只读采集受 user build/SELinux 限制：

```text
ro.build.type=user
ro.debuggable=0
ro.boot.bootreason=bootloader
boot_reason=0
system_boot_reason=55
ro.shutdown.record=192
```

`/sys/fs/pstore` 和 `/proc/cmdline` 被拒绝，`/proc/last_kmsg` 不存在。没有
尝试提权；最强证据仍是三色 marker 与 6 秒 reset 的对应关系。

下一轮停止修改 DarwinBoot 高层逻辑，只运行
`patch-xnu-entry-probe.py --reset-at _arm_vm_init` 单点探针，判断最早 XNU VM
初始化入口是否到达。完整记录见
`docs/migration/device-run-2026-11-16-02.md`。

单点候选已经构建并通过 47 项 ADT 与结构检查：

```text
probe kernel  32e9da61249471867c615d94ff936a917fd353640c91598541187ed6adad3174
probe payload 497216f6158e7d9cda02ece432e582fded0e224bf2696cce44fe3626ec6cd6f7
probe boot    1594b9ce5e5f898f4cc769ad64f582eb4b198ff0010e36486d70cd3e996d9ff5
```

Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-vm-init-probe`。该探针把
`_arm_vm_init` 入口 `0xFFFFFFF0073EA0F4` 替换为 PSCI reset，不执行函数本身。

### 第六轮 vm-init 探针与 arm-init 二分

`_arm_vm_init` 单点探针 boot
`1594b9ce5e5f898f4cc769ad64f582eb4b198ff0010e36486d70cd3e996d9ff5`
已完成一次非持久启动。画面只有绿色顶部横条，随后黑屏；没有新的同步异常，
也没有观察到目标点 PSCI reset 到 Android。

该候选本来就不绘制蓝/洋红 marker。逐层验证确认 patch 已进入最终产物：

```text
_arm_vm_init           0xFFFFFFF0073EA0F4
file offset            0x3E60F4
source word            0xD10583FF
patch word             0x1419107F
branch target          0xFFFFFFF007A2E2F0
payload kernel SHA-256 32E9DA61249471867C615D94FF936A917FD353640C91598541187ED6ADAD3174
```

payload 内嵌 kernel 与候选 `kernel.macho` 字节一致，patch 没有丢失。

结合上一轮已经证明 `_start`、`SCTLR_EL1` 前和 post-MMU stage 3，当前保守边界为：

```text
已到达 common_start+0xE0
未证明 common_start 返回进入 _arm_init
未到达 _arm_vm_init
```

当前源码路径为：

```text
common_start return target  _arm_init       0xFFFFFFF0079BF074
_arm_init -> _arm_vm_init   BL at +0x770    0xFFFFFFF0079BF7E4
```

下一版只把 `_arm_init` 入口替换成 PSCI reset：

```text
source word       0xD10343FF
probe kernel      71C3AF4268791D83E8F68BC004BD5092D05E4C08530B568CC82EB8DE9BE06AB4
probe payload     38B3C2402BAA57FFC62310717ACDDA96AF68F2A1A92CD105F947186841541584
probe boot        4EB2DDD6C4BA747D13AD1B9758A3500F98D122C60CF1136C361E01EC805BE00C
```

立即 reset 证明 common_start 已返回 `_arm_init`；不 reset 则继续在
`_arm_init` 内二分。完整记录见
`docs/migration/device-run-2026-11-16-03.md`。Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-arm-init-probe`。

### 第七轮 arm-init 结果与 thread-bootstrap 中点

`_arm_init` 单点探针 boot
`4eb2ddd6c4ba747d13ad1b9758a3500f98d122c60cf1136c361e01ec805be00c`
在手动选择 IOS 后立即重启。这就是 `_arm_init` 入口的 PSCI reset，不是未知
watchdog。

已验证最终 payload 中：

```text
_arm_init VA  0xFFFFFFF0079BF074
原指令        0xD10343FF
patch         0x1401BC9F
kernel SHA    71C3AF4268791D83E8F68BC004BD5092D05E4C08530B568CC82EB8DE9BE06AB4
```

因此 `common_start` 已返回并进入 `_arm_init`。故障区间严格缩小为
`_arm_init+4` 到 `0xFFFFFFF0079BF7E4` 的 `BL _arm_vm_init` 之间。

当前 128 MiB 窗口路径依次经过：

```text
保存 boot_args
cpu_data_init
PE_init_platform
Apple timer/misc 配置
Secure DT lookup
platform stall
CPU topology
thread_bootstrap
machine_set_current_thread
cpu_bootstrap
rtclock_early_init
kernel_startup_bootstrap
timer_call_init
cpu_init
processor_bootstrap
boot-arg PMEM/default 处理
nanoseconds_to_absolutetime
BL _arm_vm_init
```

该区间没有新的 MR/MSR；MMU/cache 已在 `common_start` 配置。主要栈对象是
`0xD0` 初始帧、callee-saved 寄存器区和 `0x480` boot_args 副本。

旧 H236m 成功 kernel 的 `_arm_init` 与当前均为 691 条指令；归一化链接地址后
调用序列和控制流骨架一致，H236m 已实际进入并返回 `_arm_vm_init`。

下一版取调用链中点，只替换 `_thread_bootstrap` 入口为 PSCI reset：

```text
_thread_bootstrap VA  0xFFFFFFF007991150
原指令                0xA9BE4FF4
patch                 0x14027468
probe kernel          76517002324C002E7A4895264317ED7C8D800D6A8EB3ACD3F66B08C5D0BAD6A0
probe payload         3F708760EE6AD5C606742E8B1EE31F2A094680F8D739F2FD6C1587A42C9FE3BB
probe boot            52759A6F3EF49FB893E157159E809D5E17BBB496C6E5D64ABDD5DA04A105F82C
```

无任何 stage 色带。立即 reset 表示已通过前半段到 `_thread_bootstrap`；不 reset
则继续在 `_arm_init` 入口到该中点之间二分。完整记录见
`docs/migration/device-run-2026-11-16-04.md`。Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-thread-bootstrap-probe`。

### 第八轮 thread-bootstrap 负结果与 configure-misc 正结果

`_thread_bootstrap` 入口探针 boot
`52759a6f3ef49fb893e157159e809d5e17bbb496c6e5d64abdd5da04a105f82c`
未触发立即 reset；只有绿色 `DarwinEnter`，随后黑屏。故
`_thread_bootstrap` 未到达。

随后在 `_configure_misc_apple_boot_args` 入口做中点探针：

```text
boot        739c7f17b2854d6fa6305e5941392f2ee2b8d421e6d23ab3d5a1744b64555439
kernel      1c7540a57baf2b056777359cee9a7df55d5b36901610c20dfe024ee3f1bc89db
payload     a784178358b5c2f2c7fb78d47fd12f49c293e511797f8bc554df03c7b05dc1d0
entry       0xFFFFFFF0073F7AF4
source RET  0xD65F03C0
```

选择 IOS 后立即重启，证明以下调用均已返回：

```text
memmove boot_args
cpu_data_init
PE_init_platform
configure_timer_apple_regs
wfe_timeout_configure
wfe_timeout_init
```

故障区间现为 configure-misc 后到 `_thread_bootstrap` 前。该范围包含 DT
lookup/property、`platform_stall_panic_or_spin` 和 CPU topology。

下一单点选 `_platform_stall_panic_or_spin` 入口：

```text
entry       0xFFFFFFF007279D1C
source      0xD10083FF
patch       0x141ED175
kernel      8F728B7B51A0CB02602269FB5A2E7363BE2080420A43AD0266BD980D4D4CB880
payload     E310A7C4DA630D76B0A638B31BFC68055DC9F4EE2BDD80D8B364CD5844509726
boot        4E070C38FE1E5CB7335B9B812EA01FBF65CFA0DE8EFE35EBD05DCA021C90C031
```

该函数若 `_reattach_wait` bit 0 异常置位，会进入永久 WFE 黑屏，因此是当前
最可疑的单点。立即 reset 表示 DT 路径已到 stall 入口；不 reset 则故障位于
DT/configure-misc 到 stall 之前。完整记录见
`docs/migration/device-run-2026-11-16-05.md`。Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-platform-stall-probe`。

### 第九轮 platform-stall 正结果与 CPU-topology 边界

`_platform_stall_panic_or_spin` 入口探针 boot
`4e070c38fe1e5cb7335b9b812ea01fbf65cfa0de8efe35ebd05dca021c90c031`
在手动选择 IOS 后立即重启，证明：

```text
configure misc stubs 返回
Secure DT lookup/property 完成
到达 _platform_stall_panic_or_spin 0xFFFFFFF007279D1C
```

此时尚未证明 stall 本身返回。下一单点放在紧随其后的
`_ml_get_boot_cpu_number` 入口，用于区分 stall/topology 是否正常返回：

```text
entry       0xFFFFFFF0073F5BE0
source      0xB0FFEC68  ADRP
patch       0x1418E1C4
kernel      D84147BBC6A06518A0F51272FD95D68589C7870BC205B88525741AFA788ABA05
payload     5F43320BC5A86A455F03F4433D2980AA3777AA12601C79FFC2EE9F6BA71AFC44
boot        36B30577ADBFFF5E941822BD17037D8CE6BCF9F27F5720759D4FF28F576FD37C
```

立即 reset 表示 platform stall 和 `_ml_parse_cpu_topology` 均已返回；不 reset
表示停点在这两者内，`_reattach_wait`/WFE 仍是最高优先级。完整记录见
`docs/migration/device-run-2026-11-16-06.md`。Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-boot-cpu-probe`。

### 第十轮 CPU-topology property 返回点负结果

`_ml_get_boot_cpu_number` 入口探针的结果为黑屏且不重启，因此
`_ml_parse_cpu_topology` 尚未返回。随后在 topology 的最终 property 路径做两次
单点探针：

| 探针 VA | 原指令 | probe boot | 结果 |
| --- | --- | --- | --- |
| `0xFFFFFFF0073F5830` | `0x7100041F` | `93cb960b...5ed5cc2` | 黑屏不重启 |
| `0xFFFFFFF0073F580C` | `0xF90016C8` | `8ec189e6...c7d9082` | 黑屏不重启 |

第二个最终 direct property 返回点和第一个最终 direct property 返回点都没有
到达，故停点已严格缩小到：

```text
0xFFFFFFF0073F5704 .. 0xFFFFFFF0073F580C
```

下一单点选择第一个 direct property 调用本身：

```text
entry       0xFFFFFFF0073F57E4
file offset 0x3F17E4
source      0x9415331E  BL
patch       0x1418E2C3
kernel      27CE5260512BFDC2B4D4EB61E33560FA51580336D8D4A67D5D51B313C208F241
payload     191DDD364089811A8CD841A7947A49B4EAED483C78F22A3017197496A5A01931
boot        EB126091BEF37E3F51299A1D55B93813CA8D5AE281E1A3C455A15FFF454E4819
```

立即 reset 表示已到达第一个 direct property 调用；不 reset 则继续缩小
`0x5704..0x57E4`。候选已通过 47/47 ADT、FD/FV、boot header、manifest 和
payload 内嵌内核逐字节检查。完整记录见
`docs/migration/device-run-2026-11-16-07.md`。Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-topology-first-property-call-probe`。

`0x57E4` 实机结果仍为黑屏不重启，故第一个 direct property 调用也未到达，停点
进一步缩小到：

```text
0xFFFFFFF0073F5704 .. 0xFFFFFFF0073F57E4
```

反汇编确认已知正点 `0x5704` 后的第一个分叉为：

```text
0xFFFFFFF0073F5728  B.EQ 0xFFFFFFF0073F5920
0xFFFFFFF0073F572C  ADRP x8, ...
```

下一探针选择 `0x572C` 的 fall-through：

```text
entry       0xFFFFFFF0073F572C
file offset 0x3F172C
source      0xB0FFEC68  ADRP
patch       0x1418E2F1
kernel      64EE1C17351A754117F98AD5DD5BD5A415BD956713FEACD281B3C32D5694FC7D
payload     16A3151E4B385A6C647DE87D3046619C53EA28ADEE9016172CAE82BBC49C777B
boot        A75721DBFA8A489C8D8425DDA2590416720CC5F3B190B4D5D6CB3149838473A9
```

立即 reset 表示 `w8 == 2` 的 `0x5728` 分支没有走；不 reset 表示已走
`0x5920` 冷分支，或停在该分叉之前。候选已通过 47/47 ADT、FD/FV、boot header、
manifest 和 payload 内嵌内核逐字节检查。Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-topology-type2-branch-probe`。

### 第十一轮 CPU cluster 容量修复

`0x572C` 实机结果仍为黑屏不重启。结合已知正点 `0x5704`，控制流位于
`0x5728 B.EQ 0x5920`，其目标调用：

```text
Assert("phys_cluster_id <= MAX_CPU_CLUSTER_PHY_ID")
machine_routines.c:1099
```

根因是诊断内核沿用 `BCM2837` 目标配置：

```text
MAX_CPUS          4
MAX_CPU_CLUSTERS  1
MAX_CPU_CLUSTER_PHY_ID = 0
```

但 MSM8998 的 `P` 核需要 `phys_cluster_id = 1`，因此必然断言失败。新增
`patches/xnu/msm8998-cpu-topology.patch`，只把该目标的容量改为：

```text
MAX_CPUS          8
MAX_CPU_CLUSTERS  2
```

补丁通过 `patches/xnu/series` 在完整 package 构建中应用。新汇编已不再在
`cluster_type == 'P'` 时跳向 `0x5920`，而是用 `CSET/ORR` 选择两个有效的
cluster 槽位。

重建候选：

```text
kernel      A0A11AAF765973D054F77407D308CA32FEFC70E13491061FDF9A876ED1BA0F99
payload     2EFADB7C3B49F165025092574BB5F84B518914DB792904BB05A9C69AC103A44F
boot        4AED1AF3FA506B8C843D00431D9FAF10213CB235E5398D72CE0548E4622748A2
FD          4F298C30C600CE85497F6A720979936643BFBD1681C377514750A408266A7725
```

候选已通过 47/47 ADT、FD/FV、boot header、manifest 和 payload 内嵌内核逐字节
检查。完整记录见 `docs/migration/device-run-2026-11-16-08.md`。Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-msm8998-cluster2`。

### 第十二轮 two-cluster 实机结果与 topology 返回探针

two-cluster 修复镜像 boot
`4AED1AF3FA506B8C843D00431D9FAF10213CB235E5398D72CE0548E4622748A2`
在手动选择 IOS 后没有重启，也没有黑屏，而是冻结在最后的 UniXNU framebuffer
画面：

```text
DARWINBOOT: inputs validated; exiting boot services
Booting OnePlus 5 IOS in 1 s
```

这证明旧的单-cluster 断言路径已被改变，但尚未证明
`_ml_parse_cpu_topology` 已返回。照片和完整记录见
`docs/migration/device-run-2026-11-16-08.md`。

下一探针在新的 fixed kernel 上把 `_ml_get_boot_cpu_number` 入口替换为 PSCI
reset：

```text
reset VA        0xFFFFFFF0073F5C28
source kernel   A0A11AAF765973D054F77407D308CA32FEFC70E13491061FDF9A876ED1BA0F99
probe kernel    7B601E61BBEA68B6B68326F8EF9A04095FCD61C0547E53EDA42D4BB321D72E19
probe payload   A1A787D15FCE9AAA9BE08A5C2B544FF25B274675347338DABDE3F5A84C38A8D9
probe boot      06ECA08198842EA14EDACE3957910DB8F4BE36130988CA227996995EB279A0B4
```

立即 reset 表示 `_ml_parse_cpu_topology` 已返回；不 reset 表示仍停在 CPU
topology 解析内部。Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-cluster2-boot-cpu-probe`。

`_ml_get_boot_cpu_number` 入口探针立即重启，证明 two-cluster 修复后
`_ml_parse_cpu_topology` 已完整返回。下一探针在同一 fixed kernel 上把
`_thread_bootstrap` 入口替换为 PSCI reset：

```text
reset VA       0xFFFFFFF007991190
source kernel  A0A11AAF765973D054F77407D308CA32FEFC70E13491061FDF9A876ED1BA0F99
probe kernel   49B02747A521B800619CA14E9A48BC22885EFEB20309BBF63314DDCC16471CD5
probe payload  947AC054E4136C5900A61163BDBA6373774688DC51DAFF114C230B88BE84A750
probe boot     E22C09C0FE8CE73FBCE3FCF930874D3F2B2D24ACA348084F949E87BE8D3959BC
```

立即 reset 表示 `_arm_init` 已到达 `_thread_bootstrap`；不 reset 表示仍停在
topology 返回到 `_thread_bootstrap` 之间。Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-cluster2-thread-bootstrap-probe`。

`_thread_bootstrap` 入口探针立即重启，证明 two-cluster 修复后该函数已到达。
新 kernel 的下一条调用是 `_machine_set_current_thread`，下一探针选择其入口：

```text
reset VA       0xFFFFFFF0073F3730
source kernel  A0A11AAF765973D054F77407D308CA32FEFC70E13491061FDF9A876ED1BA0F99
probe kernel   1F75078F41829E4953ACC108DEB1FFF5882E742BCC6CF48651E2D1DFB175A881
probe payload  F723514ABD17D4DBC52C39771E12F821098A2DE32DB8739DDE97FE1A36FF0832
probe boot     A71C632689258B0E30C1FA4A7700499889F56E991BA95AF7FE7AF01A2C30413C
```

立即 reset 表示 `_thread_bootstrap` 已返回；不 reset 表示仍停在该函数内部。
Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-cluster2-machine-thread-probe`。

`_machine_set_current_thread` 入口探针立即重启。由于 reset 安装在该函数入口，
这个结果本身就证明 `_thread_bootstrap` 已返回并到达下一调用；不需要读取一闪而过
的屏幕文字。

下一探针选择紧随其后的 `_cpu_bootstrap` 入口：

```text
reset VA       0xFFFFFFF0073E5984
source kernel  A0A11AAF765973D054F77407D308CA32FEFC70E13491061FDF9A876ED1BA0F99
probe kernel   F3BB24B5FB0739347FA53D282D878BF0D1AE3F8752E397C92E0A558D076E569C
probe payload  07A3DB80449E6A434721D7E59E52B0C302F21A8377F7AAEF5207B219CFFDE6AA
probe boot     D064F3DD8F620A23E9DE0E05B0491737C02E53F091FCC1A014C677D826F5CD3F
```

立即 reset 表示 `_machine_set_current_thread` 已返回；不 reset 表示仍停在该函数
内部。Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-cluster2-cpu-bootstrap-probe`。

`_cpu_bootstrap` 入口探针立即重启，证明 `_machine_set_current_thread` 已返回。
下一探针选择紧随其后的 `_rtclock_early_init` 入口：

```text
reset VA       0xFFFFFFF0073FACCC
source kernel  A0A11AAF765973D054F77407D308CA32FEFC70E13491061FDF9A876ED1BA0F99
probe kernel   14FF3FE4ED3C24437E444F3FBCB2E7F0778AB6261F757FE8077F11090D764577
probe payload  DF8B3027BDA008A80F4721FF8464BA2C08F32F69B7C0ED87940ED2B6FA5F6B8D
probe boot     0EE6F4B6B99D424AD0D323730D289B68A7CAC48E69042DBE2E8BEBB50E07B13B
```

立即 reset 表示 `_cpu_bootstrap` 已返回；不 reset 表示仍停在该函数内部。
Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-cluster2-rtclock-probe`。

`_rtclock_early_init` 入口探针立即重启，证明 `_cpu_bootstrap` 已返回。下一探针
选择紧随其后的 `_kernel_debug_string_early` 入口：

```text
reset VA       0xFFFFFFF0076B1268
source kernel  A0A11AAF765973D054F77407D308CA32FEFC70E13491061FDF9A876ED1BA0F99
probe kernel   4D0C5B98A1874B8CAA72C20968F05455964340ED7F8A5CAC1E1A24DAE7B4CA9C
probe payload  B3FE0D0371E6F6402A87E6A702D39627AF28D25ACC5F13BA34ED9B8585A89205
probe boot     940B67893214314ECCDAE4D9CA8435728A024F6E12F886A7CB94757FA9F73686
```

立即 reset 表示 `_rtclock_early_init` 已返回；不 reset 表示仍停在该函数内部。
Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-cluster2-kdebug-string-probe`。

`_kernel_debug_string_early` 入口探针立即重启，证明 `_rtclock_early_init` 已返回。
下一探针选择紧随其后的 `_kernel_startup_bootstrap` 入口：

```text
reset VA       0xFFFFFFF00798ED24
source kernel  A0A11AAF765973D054F77407D308CA32FEFC70E13491061FDF9A876ED1BA0F99
probe kernel   1209A8F880D66A80D1AB35914A9C913FCB028812AB89F780D1027782445B4E79
probe payload  EAE008DE36241397830B9820C3B35D10AAF082D771839B4B96321066EEE67739
probe boot     3FA4238BC8C11AF534BBDD1D90A8EAAAA69F358E9BDB3B5F2D11B906FD7F3C56
```

立即 reset 表示 early debug-string 调用已返回；不 reset 表示仍停在该路径。
Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-cluster2-kernel-startup-probe`。

`_kernel_startup_bootstrap` 入口探针立即重启。为减少手动恢复次数，下一张直接取
更大的边界 `_arm_vm_init` 入口：

```text
reset VA       0xFFFFFFF0073EA174
source kernel  A0A11AAF765973D054F77407D308CA32FEFC70E13491061FDF9A876ED1BA0F99
probe kernel   97AC2CBF90A91364EF8A36A933A4295FB047B802DBF25E4E27E14E18A1EA9486
probe payload  171FBA20E2BA763B7FB47BC09085EB2CDA538301C3DA49D6F13947D79C92D816
probe boot     59AAEE74B5B5DE4DEA4E74C7F1C4F4DA539CEFD95FC71D2468F978EA74987ED8
```

立即 reset 表示此前整段 `_arm_init` bootstrap 调用均已返回；不 reset 表示停点位于
`_kernel_startup_bootstrap` 到 `_arm_vm_init` 之间。Windows 目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-cluster2-arm-vm-entry-probe`。

`_arm_vm_init` 入口探针立即重启，证明此前整段预 VM 初始化已完成。下一探针选择
`_arm_vm_init` 返回后的首个调用 `_PE_consistent_debug_inherit` 入口：

```text
reset VA       0xFFFFFFF007944258
source kernel  A0A11AAF765973D054F77407D308CA32FEFC70E13491061FDF9A876ED1BA0F99
probe kernel   0B5332B14BDF72706E5A970FC18C1C977D2CF75B897730D9571BF8F9703AE413
probe payload  830B668F9742A1F0A72CD3C27E5CEDA1962AFF04C7B60FA1A11EA7290C666A9F
probe boot     8B3EF6DECDA47EE4286F32E77DBC94FAEE8C8BC9B058635C1EE42552536BBC6A
```

立即 reset 表示 `_arm_vm_init` 已返回；不 reset 表示仍停在该函数内部。Windows
目录为
`E:\WindowsData\Project\ios-backups\op5-20261116-cluster2-pe-debug-inherit-probe`。

## 官方参考

- <https://unixnu.com/porting/>
- <https://unixnu.com/porting/steps>
- <https://unixnu.com/porting/migration>
- <https://unixnu.com/porting/device-repository>
- <https://unixnu.com/porting/device-tree>
- <https://unixnu.com/porting/drivers>

