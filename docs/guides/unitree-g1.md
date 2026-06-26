---
layout: default
title: Unitree G1 Wrapper 分步指南（SDK2 + DDS）
---

# Unitree G1 Wrapper 分步指南

> **读者画像：** 您负责机器人底软，已经写过电机驱动，能读关节位置、发力矩指令，但从未接触过 robocraft wrapper。  
> **本指南基于：** [`sdk_wrapper/unitree_g1/`](https://github.com/BridgeDP-Robotics/robocraft-sdk/tree/main/sdk_wrapper/unitree_g1)  
> **预计用时：** 首次通读 + 本地编译约 1～2 小时（不含改自家机型）

---

## 第 0 步：用您熟悉的世界理解 wrapper

假设您现在的软件分层是这样的：

```text
您的上层运控  →  您的电机 API（read_q / write_tau）  →  CAN/EtherCAT/…  →  电机
```

接入 robocraft 之后，中间多一层 **wrapper**：

```text
robocraft-runtime  →  sdk_vtable（统一 C 接口）  →  您的厂商 SDK  →  硬件
```

**您不需要重写电机驱动。** 您要做的是：在 wrapper 的 9 个必填函数里，**调用您已有的 SDK** 完成同样的事。

| sdk_vtable 函数 | 对应您驱动里通常已有的能力 |
|-----------------|---------------------------|
| `create` | 初始化通信、订阅状态、创建发布者 |
| `update` | 把最新一帧状态拷到可读缓冲区 |
| `get_motor_q` / `get_motor_dq` | 读编码器/观测器输出的关节角、角速度 |
| `begin_cmd` / `set_motor_cmd` / `commit_cmd` | 组帧并下发 low-level 控制指令 |
| `destroy` | 断开连接、释放资源 |

G1 示例选择的是 **Unitree SDK2 + Cyclone DDS** 路径：不依赖 ROS 2，DDS 库打进交付包。

---

## 第 1 步：确认环境与硬件前提

### 1.1 您需要什么机器

| 项目 | 要求 |
|------|------|
| CPU 架构 | 与目标机器人一致，通常 `aarch64`（板载）或 `x86_64`（仿真/工控机） |
| 操作系统 | Linux，glibc 版本 **不能比机器人上更新的** |
| 编译器 | 推荐 **gcc-9**（与 Cyclone DDS 头文件兼容；示例 Dockerfile 固定 gcc-9） |
| 工具 | `cmake` ≥ 3.10、`make`、`patchelf`（打包时设置 RPATH） |

### 1.2 克隆仓库

```bash
git clone https://github.com/BridgeDP-Robotics/robocraft-sdk.git
cd robocraft-sdk
```

### 1.3 示例目录里有什么

```text
sdk_wrapper/unitree_g1/
├── unitree_g1_wrapper.cpp   # 核心：实现 sdk_vtable 各函数
├── unitree_g1_wrapper.h     # （本示例几乎为空，逻辑全在 .cpp）
├── CMakeLists.txt           # 链接 unitree_sdk2 静态库 + 导出 sdk_vtable
├── config.json              # runtime 可读的运行参数（话题名、网卡等）
├── manifest.yaml            # 交付包的「身份证」
├── ci.yaml                  # CI 打包用的库列表
└── Dockerfile               # 可选：复现 gcc-9 构建环境
```

厂商 SDK 在 `sdk/unitree_sdk2/`（本仓库已 vendoring）。

---

## 第 2 步：先编译跑通参考示例（不要改代码）

### 2.1 方式 A — 本地编译（推荐先试）

```bash
./scripts/build_sdk_wrappers.sh --local g1
```

成功后产物类似：

```text
build/sdk_wrappers_local/unitree_g1/libunitree_g1_wrapper.so
assets-g1/hardware/sdk_wrapper/libunitree_g1_wrapper.so   # 脚本也会复制一份
```

### 2.2 方式 B — Docker（与 CI 环境一致）

```bash
docker build -t unitree-g1-wrapper sdk_wrapper/unitree_g1/
docker run --rm -v "$PWD:/workspace" -w /workspace unitree-g1-wrapper \
  bash -c "./scripts/build_sdk_wrappers.sh --local g1"
```

### 2.3 编译时 CMake 在做什么（读懂 CMakeLists.txt）

1. 找到 `sdk/unitree_sdk2/lib/<arch>/libunitree_sdk2.a` 并 **静态链接**
2. 把 Cyclone DDS（`libddsc` / `libddscxx`）作为依赖链的一部分
3. 编译 `unitree_g1_wrapper.cpp` 为 **共享库**
4. 用 `export.map` **只导出** `sdk_vtable` 一个符号，其余全部隐藏
5. RPATH 由后续 `patchelf` 设为 `$ORIGIN/lib`（私有 `.so` 跟包走）

---

## 第 3 步：理解 config.json（runtime 会传给 wrapper）

`create(wrapper_dir, …)` 时，runtime 传入的 `wrapper_dir` 就是解压后的包目录。wrapper 读取其中的 `config.json`：

```json
{
  "domain_id": 0,
  "network_interface": "",
  "topic_low_state": "rt/lowstate",
  "topic_low_cmd": "rt/lowcmd",
  "max_release_retries": 5,
  "unready_mode_alias": "normal",
  "motion_switcher_timeout_s": 5.0,
  "retry_interval_s": 3
}
```

| 字段 | 含义 |
|------|------|
| `domain_id` | DDS 域 ID，多机同网时需区分 |
| `network_interface` | 绑定的网卡名（如 `eth0`），空则默认 |
| `topic_low_state` / `topic_low_cmd` | 订阅/发布的 DDS 话题 |
| `unready_mode_alias` | 释放控制权后切回的模式别名 |
| `motion_switcher_timeout_s` | MotionSwitcher RPC 超时 |

**要点：** 每个 key 在代码里都有默认值，文件可省略；但交付包内应带上 `config.json` 方便现场改参。

---

## 第 4 步：读代码 — 从 create 到 commit_cmd

打开 `unitree_g1_wrapper.cpp`，按 runtime 调用顺序理解。

### 4.1 `VCreate` — 连接机器人

```text
读 config.json
  → ChannelFactory::Init(domain_id, net_if)     // 初始化 DDS
  → 订阅 LowState（回调里解析电机/IMU/手柄）
  → 发布 LowCmd
  → 可选：MotionSwitcherClient、LocoClient（控制权切换）
```

**对您改自家机型的启示：** 把 `ChannelFactory` / `LowState` 换成您 SDK 的 init + subscribe；数据结构不同没关系，最终在回调里填 `StateSnapshot` 即可。

### 4.2 状态路径 — 回调 + 双缓冲

```text
DDS 回调 HandleLowState()
  → 解析 motor_q/dq/tau、IMU、手柄 → 写入 write_buf
  → 置 new_data 标志

每帧 VUpdate()
  → 若 new_data，把 write_buf 拷贝到 read_buf（加锁）

VGetMotorQ 等 getter
  → 只读 read_buf（不在 getter 里阻塞等硬件）
```

**为什么这样设计：** runtime 约 **2 kHz** 调 `update` 再调 getter；硬件回调可能在别的线程。双缓冲保证 getter **快且非阻塞**（目标 < 50 µs）。

### 4.3 指令路径 — begin → set → commit

```text
VBeginCmd()     → 清零 cmd 缓冲（防止上一帧残留）
VSetMotorCmd()  → 按 motor_index 写入 q,dq,tau,kp,kd
VCommitCmd()    → 组 LowCmd 帧、算 CRC、Publish
```

G1 低层控制是 **阻抗/力矩混合** 模式：`kp/kd` 为位置刚度，`tau` 为前馈力矩。这与 many 底软里的「位置环 + 力矩前馈」一致。

### 4.4 控制权 — `request_control` / `get_control_state`

robocraft 不会在没有 READY 时乱发 low-level 指令。wrapper 需实现：

| 状态 | 含义 |
|------|------|
| `SDK_CONTROL_UNREADY` | 高层运控占用中，或 damping/站立模式 |
| `SDK_CONTROL_READY` | 可收 low-level 电机指令 |
| `SDK_CONTROL_UNKNOWN` | 尚未收到状态或 RPC 失败 |

G1 示例通过 **MotionSwitcherClient** 查询/释放当前 motion service；`request_control(READY)` 会 release 占用者。

### 4.5 文件末尾 — 唯一导出符号

```cpp
SDK_API extern const sdk_vtable_t sdk_vtable = {
    sizeof(sdk_vtable_t), SDK_API_VERSION,
    VCreate, VDestroy, VGetNumMotors, /* ... 填齐所有函数指针 ... */
};
```

**硬性要求：** `.so` 对外 **只能** 有 `sdk_vtable` 这一个全局符号（`sdk_diag --check` 会验）。

---

## 第 5 步：打包交付物

```bash
ARCH=$(uname -m)

mkdir -p build/intermediates
cp build/sdk_wrappers_local/unitree_g1/libunitree_g1_wrapper.so build/intermediates/

# 打包脚本会按 manifest.yaml / ci.yaml 收集 libddsc 等到 lib/
ci/package_sdk.sh unitree_g1 "$ARCH"
# → sdk-unitree-g1-aarch64.tar.gz（架构名随机器而定）
```

包结构：

```text
sdk-unitree-g1-aarch64.tar.gz
├── libunitree_g1_wrapper.so    # 包根目录，entrypoint
├── config.json
├── manifest.yaml
├── checksums.sha256
└── lib/
    ├── libddsc.so.0
    └── libddscxx.so.0
```

---

## 第 6 步：用 sdk_diag 自检与上机

### 6.1 构建诊断工具（一次即可）

```bash
cmake -S tools/sdk_diag -B build-sdk_diag
cmake --build build-sdk_diag
```

### 6.2 静态检查（不接机器人）

```bash
build-sdk_diag/sdk_diag --check sdk-unitree-g1-aarch64.tar.gz
```

检查项包括：包结构、manifest、**仅导出 sdk_vtable**、RPATH、依赖白名单、glibc 版本等。

### 6.3 上真机联调（G1 + 同网）

```bash
build-sdk_diag/sdk_diag sdk-unitree-g1-aarch64.tar.gz
```

交互命令建议顺序：

```text
sdk_diag> wait          # 等首帧 LowState
sdk_diag> info          # 看 capability、comm 状态
sdk_diag> read 10       # 读 10 次关节角
sdk_diag> imu 5         # 读 IMU
sdk_diag> ready         # 申请 READY
sdk_diag> zero 50       # 安全零力矩
sdk_diag> unready       # 释放控制权
sdk_diag> quit
```

---

## 第 7 步：改成您自家机型（工作清单）

```bash
cp -r sdk_wrapper/unitree_g1 sdk_wrapper/my_robot
```

按顺序改：

| # | 文件 | 做什么 |
|---|------|--------|
| 1 | 目录/文件名 | `my_robot_wrapper.cpp`，CMake `project` 名 |
| 2 | `CMakeLists.txt` | 把 `unitree_sdk2` 换成您的 SDK 链接方式 |
| 3 | `unitree_*_wrapper.cpp` | 实现 9 个必填函数 + 您有的可选能力 |
| 4 | `config.json` | 改成您的话题/CAN 口/波特率等 |
| 5 | `manifest.yaml` | `sdk.name`、`api.entrypoint`、`capabilities`、`private_libs` |
| 6 | `ci.yaml` | 列出需要打进 `lib/` 的 `.so` |

**电机数量与索引：** `get_num_motors()` 必须与 URDF/运控约定一致；`motor_index` 顺序建议在代码注释里写清楚（G1 为 29 关节固定顺序）。

**能力诚实声明：** 没有电池就不要在 `get_capabilities` / manifest 里写 `SDK_CAP_BATTERY`；否则 runtime 或审核会失败。

---

## 常见问题

**Q：我已经有电机驱动 `.so`，还要重写吗？**  
A：不用重写驱动逻辑。wrapper 可以是薄薄一层，在 `set_motor_cmd` 里调您现有 API。若驱动已是 `.so`，也可链接它，但依赖须写进 `manifest.yaml` 的 `private_libs` 并打进包。

**Q：一定要用 Unitree SDK2 吗？**  
A：本目录是 **G1 参考实现**。您的机型应复制后替换通信层；接口规范以 [`sdk_wrapper_interface.h`](https://github.com/BridgeDP-Robotics/robocraft-sdk/blob/main/sdk_wrapper/interface/sdk_wrapper_interface.h) 为准。

**Q：和 `unitree_g1_rmw` 怎么选？**  
A：机器人 **没有 ROS 2** → 本指南（SDK2+DDS）。**已有 ROS 2** 且数据在 `/lowstate` 话题 → 看 [G1 RMW 指南](unitree-g1-rmw.html)。

**Q：编译报 Cyclone DDS / gcc 相关错误？**  
A：优先用示例 `Dockerfile` 的 gcc-9 环境，或 `docker build` 方式 B。

---

## 下一步

- [G1 RMW 分步指南](unitree-g1-rmw.html) — ROS 2 集成路径  
- [适配手册 v1.0](../vendor-api-manual-v1.0.html) — 交付与审核细节  
- [文档首页](../) — 返回索引
