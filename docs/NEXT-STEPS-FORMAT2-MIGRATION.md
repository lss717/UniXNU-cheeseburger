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

## 当前基线

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
- 从干净 checkout 加私有本地输入即可生成相同 kernel 和 payload。

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

## 官方参考

- <https://unixnu.com/porting/>
- <https://unixnu.com/porting/steps>
- <https://unixnu.com/porting/migration>
- <https://unixnu.com/porting/device-repository>
- <https://unixnu.com/porting/device-tree>
- <https://unixnu.com/porting/drivers>

