---
layout: default
title: 适配手册 v1.0
---

# 机器人厂商 SDK Wrapper 适配手册 v1.0

robocraft-runtime 部署在机器人本体上，通过加载**厂商提供的一份 SDK Wrapper（`.so`）**与硬件通信。厂商只需交付这一份 wrapper——把自家机器人 SDK 适配到 robocraft 的 `sdk_vtable` C ABI。

接口由单一头文件 `sdk_wrapper_interface.h` 定义（随手册附带），**以该头文件为准**。当前公开版本 **v1.0**（`SDK_API_VERSION = 0x00010000`）。

## 规范用词

- **必须 / 不允许**：强制。违反会导致加载失败、ABI 不兼容，或审核被拒。
- **应当 / 不应**：强烈建议。违反会触发审核告警或埋运行期隐患。
- **可**：自行决定。

本地冒烟与打包自检工具 `sdk_diag` 的用法见其自带 README（`tools/sdk_diag/README.md`）——构建完成后用它一键检查符号导出、RPATH、依赖、包结构。

## 交付物

一个包，固定含：

- `manifest.yaml`（必需，§5）
- `lib<name>.so`（必需，导出 `sdk_vtable`，放包根目录）
- `lib/`（可选，私有依赖库，被 `$ORIGIN/lib` 引用）
- `checksums.sha256`（必需）
- `LICENSE` / `CHANGELOG.md` / `README.md`（建议）

外层压缩格式（tar.gz / zip 等）不由手册约定，以平台上传通道收件时告知为准。


---

# 1 — 基本架构

## 1.1 定位

```
┌───────────────────────────────────┐
│  robocraft_runtime（机器人本体）    │
│   通过 sdk_vtable C ABI 加载并调用  │
│   ↓                               │
│  ┌──────────────────────────┐     │
│  │  SDK wrapper (.so)         │← 厂商提供，导出 sdk_vtable
│  │   ↓ 厂商 SDK ↔ 机器人硬件   │
│  └──────────────────────────┘     │
└───────────────────────────────────┘
```

runtime 内部的状态机、控制器、平台对接均由 robocraft 实现，与厂商无关。厂商只实现 wrapper。

术语：**wrapper** = 厂商交付的 `.so`；**vendor SDK** = 厂商自家底层 SDK；**handle** = wrapper 内部状态的不透明指针 `sdk_handle_t`，runtime 永不解引用；**capability** = wrapper 声明的可选能力位图。

## 1.2 加载模型

wrapper 在 runtime 的**独立子进程**中加载运行，与主进程**不共享地址空间**：

- 加载方式为 `dlopen(RTLD_NOW | RTLD_LOCAL)`，加载前 runtime **强制清空 `LD_LIBRARY_PATH`**。
- wrapper 与主进程的通信完全由 runtime 处理，对 wrapper 透明——你只需正确实现 vtable。

进程隔离对依赖库处理的影响（自带库不冲突、靠 `$ORIGIN/lib` 自包含）见 §3。

## 1.3 生命周期

```
[启动]   vt->create(&config, &handle)        连接机器人 SDK

[主循环 ~2kHz 每 tick]
   vt->update(handle)                         刷新内部状态
   一系列 getter（get_motor_q / get_imu_rpy / ...）
   vt->begin_cmd(handle)                       清零 cmd 缓冲
   vt->set_motor_cmd(handle, i, ...) × N
   vt->commit_cmd(handle)                      下发到机器人

[关机]   runtime 驱动 wrapper 回安全态
         vt->destroy(handle)                   释放资源
```

## 1.4 接入流程

```
1. 拿到手册 + sdk_wrapper_interface.h + 参考骨架（`sdk_wrapper/unitree_g1`、`sdk_wrapper/unitree_g1_rmw`）
2. 写 manifest（机型、版本、capabilities）          §5
3. 实现 vtable 必备字段                              见头文件
4. 用任意工具链 build（满足 §2/§3/§4 对外 ABI 边界即可）
5. 打包（结构见 §5）
6. 用 sdk_diag 一键检查 + 本地冒烟（见其 README）
7. 按平台告知格式上传
```


---

# 2 — 系统 baseline 声明

## 2.1 核心原则

wrapper 用什么 **OS / 编译器 / C++ 标准 / 构建系统**，**完全由厂商自决**。手册只约束 wrapper.so 的**对外 ABI 边界**。

- wrapper baseline = 你对自己机器人的承诺，**不是** robocraft 对你的承诺。
- robocraft **不**提供 builder 镜像、**不**提供 Dockerfile、**不**推荐编译器版本。
- 你的最低标准：wrapper.so 能在目标机器人上 `dlopen` 并通过端到端冒烟测试。

## 2.2 架构要求

| 架构 | manifest `target.arch` | 推荐 `-march` |
|------|------------------------|---------------|
| x86_64 | `x86_64` | `-march=x86-64-v2`（或按目标 CPU 选 `v3`） |
| aarch64 | `aarch64` | `-march=armv8-a` |

- wrapper **只需**发布与目标机器人匹配的**一种**架构（不要求双架构）。
- `target.arch` **必须**与实际架构一致（审核用 `file` 校对）。
- **禁止** `-march=native`——会编入目标 CPU 不支持的指令导致运行期 `SIGILL`。

## 2.3 对外 ABI 边界（必须满足）

| 边界 | 详见 |
|------|------|
| 仅导出 `sdk_vtable` 一个符号 | §4 |
| 符号隐藏（version script + `--exclude-libs,ALL`） | §4 |
| 动态依赖在白名单内（系统库 + 自带私有库） | §3 |
| `RPATH/RUNPATH` 必须包含 `$ORIGIN/lib` | §3 |
| 不假定 `LD_LIBRARY_PATH` 存在 | §3 |
| 不重定义全局 `malloc`/`free`/`new`/`delete` | §4 |
| 不抛 C++ 异常穿越 vtable | 头文件 |
| manifest 必备字段齐全 | §5 |

满足这些后，wrapper 内部用什么工具链编都不受限。

## 2.4 必须的编译/链接选项

```cmake
target_compile_options(${W} PRIVATE -fPIC)
set_target_properties(${W} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    INSTALL_RPATH "$ORIGIN/lib"    # 最低要求；允许追加额外路径，如 "$ORIGIN/lib:/opt/ros/humble/lib"
    BUILD_WITH_INSTALL_RPATH ON)
target_link_options(${W} PRIVATE
    -Wl,--exclude-libs,ALL
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/wrapper.ldscript
    -Wl,--disable-new-dtags)
target_compile_definitions(${W} PRIVATE SDK_WRAPPER_BUILDING)
```

建议（非强制）：`-Wl,--no-undefined`、`-Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now`、`-fno-strict-aliasing`。

完整可编译的 CMake 模板见参考 example：`sdk_wrapper/unitree_g1/CMakeLists.txt`（厂商静态库 + 自带 Cyclone DDS）与 `sdk_wrapper/unitree_g1_rmw/CMakeLists.txt`（依赖宿主 ROS 2），构建与打包步骤见顶层 README。

## 2.5 glibc / libstdc++ 兼容

wrapper 用到的 `GLIBC_*` / `GLIBCXX_*` 符号版本若**高于**目标机器人，加载会报 `version 'GLIBC_x.yy' not found`。

- 解决：用**不高于**目标 glibc 的工具链编译，或 `-static-libstdc++`。
- 构建后用 `sdk_diag` 检查实际符号版本（见其 README）。

> manifest 的 `target.os` / `glibc_max` / `libstdcxx_max` 仅 informational。


---

# 3 — 系统库与第三方库

> 这一章讲 wrapper 该怎么处理它依赖的各种库：哪些可以直接用系统的、哪些必须自带、哪些绝对不能碰，以及它们怎么被找到。

## 3.1 前提：进程隔离 → 自包含

wrapper 在 runtime 的**独立子进程**中加载，与主进程**不共享地址空间**。两点直接后果：

- wrapper 自带的任何第三方库（DDS / OpenSSL / Protobuf 等**任意版本**）都**不会**与 runtime 的同名库冲突——不必规避版本，**自带即可**。
- 加载前 runtime **强制清空 `LD_LIBRARY_PATH`**——wrapper **不能**靠它找库，所有非系统依赖必须靠 `$ORIGIN/lib` RPATH 自包含（§3.4）。

因此总原则是：**除白名单系统库外，其余依赖一律自带进包，放 `lib/` 子目录。**

## 3.2 三类库

| 类别 | 处理方式 |
|------|----------|
| **系统库**（白名单内） | 可直接动态依赖，不必自带 |
| **第三方/私有库** | 自带到包内 `lib/`，manifest `private_libs` 声明 |
| **禁用库** | 不允许依赖 |

## 3.3 系统库白名单

可直接动态依赖（无需自带）：

```
libc.so.6   libm.so.6   libdl.so.2   librt.so.1   libpthread.so.0
libstdc++.so.6   libgcc_s.so.1   linux-vdso.so.*   ld-linux-*.so.*
```

**白名单之外的任何 `DT_NEEDED` 库都必须自带**。注意 `libz`、`libssl`、`libcrypto`、各类 DDS / EtherCAT / ZMQ / LCM 等**都不在**白名单内——不要假定目标机器人上存在，请打进包。

## 3.4 第三方/私有库

> wrapper 的 vendor SDK 及其依赖几乎都属于这一类。

规则：

- 全部放在包内 `lib/` 子目录（结构见 §5）。
- wrapper 的 `RPATH/RUNPATH` **必须以 `$ORIGIN/lib` 为其中一项**（wrapper.so 在包根、私有库在同级 `lib/`），让 `dlopen` 优先找到私有副本。
  - 允许在 `$ORIGIN/lib` 后追加额外路径（如 `/opt/ros/humble/lib`），以 `:` 分隔。
  - 仍**不**用绝对路径替代 `$ORIGIN/lib`、不用 `..` 跳出。
  - `LD_LIBRARY_PATH` 已被清空，故 RPATH 与 RUNPATH 等价，二者皆可。
- 在 manifest `private_libs` 中逐一声明。
- **应当**也用 `-fvisibility=hidden` 编译（如能拿到源码）；若是预编译库，靠 wrapper 的 version script + `--exclude-libs,ALL` 拦截其符号不外泄（§4）。

## 3.5 宿主提供的库（RMW / ROS wrapper 专用）

> 仅适用于依赖机器人**宿主 ROS** 运行时的 RMW wrapper（如 `unitree_g1_rmw`、`lx2501_3_rmw`）。

部分 wrapper 通过宿主已安装的 ROS（`/opt/ros/<distro>/lib`）接入机器人原生 DDS 栈，ROS 核心库（`librcl`、`librcutils`、`librosidl_runtime_c`、`librmw`、`librmw_implementation`、`librosidl_typesupport_c` 等）由宿主提供、**不打进包**。

此时：

- RPATH/RUNPATH 在 `$ORIGIN/lib` 之后追加 `/opt/ros/<distro>/lib`（§3.4）。
- 在 manifest 的 `system_libs` 中逐一声明这些宿主库的 **basename**（不带 `lib/` 前缀，因为它们不在包内）。`sdk_diag` 会把它们视为允许的 `DT_NEEDED`，无需自带。

```yaml
system_libs:
  - librcl.so
  - librcutils.so
  - librosidl_runtime_c.so
  - librmw.so
  - librmw_implementation.so
  - librosidl_typesupport_c.so
```

> 注意：`system_libs` **只**用于宿主必然存在的 ROS 运行时库。第三方/私有库（DDS / vendor SDK / 各类消息包等）仍按 §3.4 自带并用 `private_libs` 声明，不得放进 `system_libs` 规避打包。

## 3.6 禁用库

**不允许**依赖：

| 禁用库 | 原因 |
|--------|------|
| `libcuda` / `libcudart` / `libcudnn` / `libtensorrt` / `libnvinfer` | v1.0 无 GPU capability 框架 |
| `libsystemd` / `libdbus-1` | 不允许与系统服务直接交互 |
| 任何平台内部 IPC 库 | wrapper 经 vtable 与 runtime 交互，接触不到 |
| GPL（非 LGPL）许可证的库 | 许可证问题（§7） |

构建后用 `sdk_diag` 检查依赖（`DT_NEEDED` 对照白名单 + `private_libs`）与 `RUNPATH`，见其 README。


---

# 4 — 导出说明

> 本章讲 wrapper 的符号导出与内存边界。依赖库与 RPATH 见 §3。

## 4.1 唯一对外符号

wrapper **必须**且仅导出 `sdk_vtable`，其余符号（C++ 类、helper、vendor SDK 内部符号）**必须**隐藏。

最常见的审核失败：链接了 vendor SDK 的 `.a` 静态库，其符号默认 default visibility，**不被** `CXX_VISIBILITY_PRESET hidden` 覆盖，泄露成千上万个符号。

**修法（必须）**——version script + `--exclude-libs,ALL`：

`wrapper.ldscript`：
```
VERS_1.0 { global: sdk_vtable; local: *; };
```

链接：
```cmake
target_link_options(${W} PRIVATE
    -Wl,--exclude-libs,ALL
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/wrapper.ldscript)
```

`sdk_vtable` **应当**是只读数据符号。构建后用 `sdk_diag` 检查导出符号是否只剩 `sdk_vtable`、有无禁用符号引用（见其 README）。

## 4.2 内存边界

- **不允许**重定义全局 `malloc`/`free`/`operator new`/`operator delete`。
- **不允许** `LD_PRELOAD` 加载分配器（且加载 wrapper 时 `LD_PRELOAD` 已无效）。
- 内部可用局部自定义分配器，只要不污染全局符号。
- 公开 vtable 中**不存在** wrapper-malloc / runtime-free 型接口——避免跨 `.so` 边界 `free`。config 字符串由 wrapper 深拷贝，getter 数组由调用方分配（详见头文件注释）。

## 4.3 TLS

- **应当**避免 `thread_local`/`__thread`。
- 如必须，编译用 `-ftls-model=global-dynamic`（GCC 默认），**不**用 `initial-exec`——`dlopen` 后会报 `cannot allocate memory in static TLS block`。


---

# 5 — 包和 manifest

## 5.1 包结构

外层压缩格式不约定（平台收件时告知），**包内结构固定**：

```
<package_archive>
├── manifest.yaml             # 必需
├── lib<wrapper>.so           # 必需，导出 sdk_vtable，放在包根目录
├── lib/                      # 可选，私有库（被 $ORIGIN/lib 引用）
│   └── libxxx.so
├── LICENSE                   # 建议
├── CHANGELOG.md              # 建议
├── README.md                 # 建议
└── checksums.sha256          # 必需，所有文件 SHA256
```

> wrapper.so 在**包根**，私有库在 `lib/`；wrapper 的 `RPATH=$ORIGIN/lib` 正好指向同级 `lib/`。

## 5.2 manifest schema

manifest.yaml **必须**通过随手册附带的 JSON Schema 校验。完整示例：

```yaml
schema_version: 1

sdk:
  name: "acme_x100_wrapper"       # 必需，建议 <vendor>_<robot>_wrapper
  version: "1.2.3"                # 必需，SemVer
  description: "ACME X100 wrapper" # 可选

vendor:
  name: "ACME Robotics Co., Ltd." # 必需
  contact: "support@acme.com"     # 必需
  license: "Proprietary"          # 必需，取值见 §5.2 末

robot:
  model: "X100"                   # 必需，单一机型
  firmware_version_min: "2.0.0"   # 必需

target:
  arch: "aarch64"                 # 必需，x86_64 或 aarch64
  os: "ubuntu"                    # 以下均可选，informational
  glibc_max: "2.35"
  libstdcxx_max: "GLIBCXX_3.4.30"

api:
  version: "1.0"                  # 必需
  entrypoint: "libacme_x100_wrapper.so"   # 必需，包根目录下的 wrapper.so
  capabilities:                   # 必需，与 get_capabilities() 一致
    - SDK_CAP_LOW_LEVEL
    - SDK_CAP_IMU

network:                          # 必需，见 §5.3
  interface: "eth0"
  protocol: "dds"

files:                            # 必需，与实际文件一一对应
  - path: "libacme_x100_wrapper.so"
    sha256: "abc123..."
    size: 3145728

private_libs:                     # 可选，自带库清单
  - "lib/libacme_low_level.so.3"
```

- `target.arch` **必须**为 `x86_64` 或 `aarch64`；`files[]` 的 SHA256 + size **必须**与实际一致。
- `get_capabilities()` 返回值 **必须**与 `api.capabilities` 一致，审核会校核。

**`vendor.license` 取值**（择一）：`Proprietary`、`Apache-2.0`、`MIT`、`BSD-3-Clause`、`LGPL-2.1`、`LGPL-3.0`，或其它 SPDX 标识符。自带私有库**不允许** GPL（非 LGPL）。

## 5.3 网络声明

`network` 块声明 wrapper 与**机器人本体**通信使用的网络，供平台审核与部署时核对网卡 / 端口占用。

```yaml
network:
  interface: "eth0"               # 必需，与机器人通信的网卡名
  protocol: "dds"                 # 必需：dds / ethercat / serial / can / ...
  ports:                          # 可选，使用的端口或范围
    - "udp:7400-7500"
```

- 这里声明的是 wrapper 与机器人之间的链路，**不是**与 runtime 的通信（后者经 vtable，对 wrapper 透明）。

## 5.4 命名（建议）

```
<vendor>-<robot>-<version>-<arch>.<ext>     例：acme-x100-1.2.3-aarch64.tar.gz
```

具体以平台上传通道要求为准。

## 5.5 提交与校验

- 上传前用 `sdk_diag` 本地一键检查（包结构、manifest、符号导出、RPATH、依赖、create/update/destroy 冒烟，见其 README）。
- 上传走 HTTPS，成功后得 `submission_id`。
- 平台审核校验：manifest schema、文件校验和、`target.arch` 与 `file` 一致、仅导出 `sdk_vtable`、`RUNPATH` 包含 `$ORIGIN/lib`、依赖白名单、动态加载 + `api_version` 匹配、`get_capabilities()` 与声明一致、create/update/destroy 冒烟。任一不过即拒。
- 同一 `sdk.name` 同一 `version` 不允许重复上传——修复后**必须** bump version。
- 厂商**无需**对包签名；版本管理（生效 / 弃用 / 回滚）由平台维护。


---

# 6 — 版本说明

## 6.1 版本号

接口用语义化版本，编码进 vtable：

```c
#define SDK_API_VERSION_MAJOR  1
#define SDK_API_VERSION_MINOR  0
#define SDK_API_VERSION  ((SDK_API_VERSION_MAJOR << 16) | SDK_API_VERSION_MINOR)
```

wrapper **必须**填 `api_version = SDK_API_VERSION`、`struct_size = sizeof(sdk_vtable_t)`（用当时编译的头文件大小）。

## 6.2 兼容规则

loader 判定：**major 必须相同，minor 允许差异，`struct_size` 须满足 runtime 已知最小值**。

| wrapper | runtime | 结果 |
|---------|---------|------|
| 1.0 | 1.0 | ✓ |
| 1.0 | 1.3 | ✓ runtime 调新字段时自动检测 wrapper 未实现 → 返回 NOT_SUPPORTED，旧功能不受影响 |
| 1.3 | 1.0 | ✓ 旧 runtime 只用它认识的字段，忽略多出的新字段 |
| 2.0 ↔ 1.x | | ✗ major 不同，拒绝 |

**结论：同一 major（v1.x）内，无论谁先升 minor，wrapper 都不需要重编。**

## 6.3 什么改动算兼容

**Minor 允许（向后兼容）**：在 vtable / `sdk_config_t` / 状态 struct **末尾追加**新字段；新增 `SDK_CAP_*` 位；新增 error code 常量；新增 enum 值。

**需 major bump（破坏）**：删除 / 改名 / 重排 / 改签名任何 vtable 字段；改已有 struct 字段顺序或大小；改 capability bit 含义；改 error code 数值。

## 6.4 capability 与弃用

- 新 capability 只用未占用 bit，一旦发布**永久绑定**，不允许复用或改含义。
- v1.x 内字段只增不删；可标 `@deprecated`（文档警告），删除需 major bump。

## 6.5 major 升级

v2.0 发布时，runtime 会在一段过渡期内同时支持 v1 与 v2 wrapper 加载，之后才移除 v1 支持。wrapper 文档**应当**注明兼容的 runtime 版本范围（如 "Compatible with runtime v1.x"）。


---

# 7 — 禁用清单

> 以下是手册条款，违反 = 审核失败。运行期 `LD_LIBRARY_PATH` 被清空（§3），wrapper 不得依赖它。

**信号**：**不允许**注册 `SIGINT/SIGTERM/SIGSEGV/SIGABRT/SIGFPE/SIGILL` handler（会破坏 runtime 优雅退出与崩溃捕获）。例外：`signal(SIGPIPE, SIG_IGN)` 允许。

**环境变量**：可读 `HOME/USER/LANG/TZ` 与声明项；**不允许**读 `LD_*` / `ROBOCRAFT_*` / 云凭证；**不允许** `setenv`/`unsetenv` 改进程环境。

**系统调用**：**不允许** `ptrace`/`reboot`/`mount`/`chroot`/`setns`/`unshare`/`iopl`/eBPF / `mlock`(>16MB)。

**全局状态**：**不允许**改全局 locale、改进程优先级（自创建线程可自调）、改 `stdin/stdout/stderr` fd。

构建后用 `sdk_diag` 检查 wrapper 是否引用了禁用符号（见其 README）。
