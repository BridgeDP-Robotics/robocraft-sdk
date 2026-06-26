---
layout: default
title: Unitree G1 RMW Wrapper 分步指南（ROS 2）
---

# Unitree G1 RMW Wrapper 分步指南

> **读者画像：** 您负责机器人底软，电机驱动已经跑在 **ROS 2** 上（例如能 `ros2 topic echo /lowstate`），但不了解 robocraft wrapper。  
> **本指南基于：** [`sdk_wrapper/unitree_g1_rmw/`](https://github.com/BridgeDP-Robotics/robocraft-sdk/tree/main/sdk_wrapper/unitree_g1_rmw)  
> **与 `unitree_g1` 的区别：** 本示例 **不链接** Unitree SDK2 静态库，而是用 **ROS 2 C API（rcl/rmw）** 直接订阅/发布话题。

---

## 第 0 步：先搞清楚「RMW 版 wrapper」是什么

您可能已经这样用 ROS 2 控制 G1：

```text
ros2 topic echo /lowstate          # 读关节、IMU
ros2 topic pub /lowcmd ...         # 写 low-level 指令
```

robocraft-runtime **不会** 直接跑您的 ROS 节点，它只认 `sdk_vtable`。RMW wrapper 的工作就是：

```text
robocraft-runtime
    ↓ sdk_vtable
unitree_g1_rmw_wrapper.so   ← 内部用 rcl/rmw，无 rclcpp executor
    ↓ ROS 2 话题
/lowstate  /lowcmd  /api/motion_switcher/*
    ↓
您现有的 ROS 驱动 / 宇树官方 ROS 栈
```

**关键设计选择（读本示例时请记住）：**

- 使用 **rcl C API**，不用 `rclcpp`，避免和 runtime 抢 executor 线程模型
- **不打包** ROS 系统库（`librcl.so` 等），运行时依赖机器人上的 `/opt/ros/<distro>/lib`
- **会打包** `unitree_hg` / `unitree_api` 的 rosidl typesupport 到 `lib/`

---

## 第 1 步：确认环境与前提

### 1.1 软件依赖

| 项目 | 要求 |
|------|------|
| ROS 2 | **Humble** 或 **Foxy**（与目标机器人一致） |
| RMW | 默认 `rmw_cyclonedds_cpp`（与宇树 G1 常见配置一致） |
| 构建工具 | `colcon`、`cmake` ≥ 3.15、`patchelf` |
| 消息包 | 需先编译 `unitree_hg`、`unitree_api`（仓库在 `sdk/unitree_ros2/`） |

### 1.2 机器人侧前提

- G1 上 ROS 2 栈正常，能收到 `/lowstate`
- 网络 **不能** 只绑 loopback（wrapper 会把 `ROS_LOCALHOST_ONLY` 设为 `0`）
- 若有多网卡，通过 `config.json` 的 `network_interface` 或 runtime 注入绑定 CycloneDDS

### 1.3 示例目录结构

```text
sdk_wrapper/unitree_g1_rmw/
├── unitree_g1_rmw_wrapper.cpp   # 核心实现（rcl 初始化、订阅、发布、API 调用）
├── CMakeLists.txt               # find_package(rcl, unitree_hg, …)
├── config.json                  # 话题名、API id、FSM id 等
├── manifest.yaml                # 含 private_libs + system_libs 声明
├── ci.yaml
└── README.md                    # 英文简要说明
```

---

## 第 2 步：构建 unitree ROS 2 消息包

RMW wrapper 依赖 `unitree_hg/msg/LowState`、`LowCmd` 等类型，需先用 colcon 编出来：

```bash
source /opt/ros/humble/setup.bash   # 或 foxy

cd sdk/unitree_ros2/cyclonedds_ws
colcon build --packages-select unitree_hg unitree_api --cmake-args -DBUILD_TESTING=OFF
```

确认生成：

```text
sdk/unitree_ros2/cyclonedds_ws/install/unitree_hg/share/unitree_hg/cmake/unitree_hgConfig.cmake
sdk/unitree_ros2/cyclonedds_ws/install/unitree_api/share/unitree_api/cmake/unitree_apiConfig.cmake
```

> `./scripts/build_sdk_wrappers.sh --local g1-rmw` 会在缺失时 **自动** 执行上述 colcon 步骤。

---

## 第 3 步：编译 RMW wrapper

```bash
source /opt/ros/humble/setup.bash
./scripts/build_sdk_wrappers.sh --local g1-rmw
```

产物：

```text
build/sdk_wrappers_local/unitree_g1_rmw/libunitree_g1_rmw_wrapper.so
assets-g1/hardware/sdk_wrapper/libunitree_g1_rmw_wrapper.so
assets-g1/hardware/sdk_wrapper/lib/libunitree_hg__rosidl_*.so
assets-g1/hardware/sdk_wrapper/lib/libunitree_api__rosidl_*.so
```

### CMakeLists.txt 要点

1. `CMAKE_PREFIX_PATH` 依次加入 `/opt/ros/$ROS_DISTRO` 与 colcon install 路径
2. `find_package(rcl rmw unitree_hg unitree_api …)`
3. 链接 rosidl typesupport（Foxy 上 fastrtps typesupport 可能需直接链 `.so` 路径）
4. `export.map` 只导出 `sdk_vtable`
5. `BUILD_RPATH` 含 colcon install 的 lib，便于本地链接；交付时 `patchelf` 为 `$ORIGIN/lib:/opt/ros/humble/lib`

---

## 第 4 步：理解 config.json

```json
{
  "domain_id": 0,
  "network_interface": "",
  "default_rmw_implementation": "rmw_cyclonedds_cpp",
  "node_name": "robocraft_unitree_g1_rmw_wrapper",
  "topic_low_state": "/lowstate",
  "topic_low_cmd": "/lowcmd",
  "topic_motion_switcher_request": "/api/motion_switcher/request",
  "topic_motion_switcher_response": "/api/motion_switcher/response",
  "topic_sport_request": "/api/sport/request",
  "topic_sport_response": "/api/sport/response",
  "api_check_mode": 1001,
  "api_select_mode": 1002,
  "api_release_mode": 1003,
  "api_timeout_s": 5,
  "max_release_retries": 5,
  "retry_interval_s": 3,
  "loco_set_fsm_id": 7101,
  "loco_damp_fsm_id": 1,
  "unready_mode_alias": "ai"
}
```

| 字段组 | 作用 |
|--------|------|
| `topic_low_*` | 与宇树 ROS 低层话题对齐；改机型时通常最先改这里 |
| `topic_motion_switcher_*` + `api_*` | 通过 `unitree_api/Request` 调 motion switcher（查模式、释放占用） |
| `topic_sport_*` + `loco_*` | UNREADY 时切 damping FSM |
| `default_rmw_implementation` | 未设置 `RMW_IMPLEMENTATION` 时 wrapper 自动 export |
| `network_interface` | 非空且未设 `CYCLONEDDS_URI` 时，wrapper 生成 Cyclone 网卡绑定配置 |

---

## 第 5 步：读代码 — ROS 2 版数据流

打开 `unitree_g1_rmw_wrapper.cpp`。

### 5.1 `VCreate` — rcl 生命周期

```text
设置 RMW_IMPLEMENTATION / ROS_LOCALHOST_ONLY / CYCLONEDDS_URI（按需）
  → rcl_init / rcl_node_init
  → 创建 /lowstate 订阅（回调解析 LowState → StateSnapshot）
  → 创建 /lowcmd 发布
  → 创建 motion_switcher / sport 的 request 发布 + response 订阅
  → 初始化 wait_set（API 同步调用用）
```

与 SDK2 版相同：**回调写 write_buf，`update` 拷到 read_buf，getter 只读 read_buf**。

### 5.2 订阅回调 — 从 ROS 消息到统一状态

回调里把 `unitree_hg/msg/LowState` 字段映射到：

- `motor_q[d]` / `motor_dq[d]` / `motor_tau[d]`（29 关节）
- IMU 四元数、gyro、acc、rpy
- 手柄 `wireless_remote` 字节流 → 解析为 `joy_axes` / `joy_buttons`

**对您改机型的启示：** 若您的 `/joint_states` 或自定义 msg 不同，只改这一层解析；**vtable 对外形状不变**。

### 5.3 指令下发 — LowCmd + CRC

`VCommitCmd` 将内部缓冲填进 `unitree_hg/msg/LowCmd`，计算 CRC（与 SDK2 示例相同算法），再 `rcl_publish`。

### 5.4 控制权 — ROS API 而非 MotionSwitcherClient

| 操作 | SDK2 版 | RMW 版 |
|------|---------|--------|
| 查当前模式 | `MotionSwitcherClient::CheckMode` | `CallMotionSwitcher(api_check_mode)` |
| 释放占用 | `ReleaseMode` | `CallMotionSwitcher(api_release_mode)` |
| UNREADY damping | `LocoClient::Damp` | `CallSport(loco_set_fsm_id, damp_fsm_id)` |

`request_control(SDK_CONTROL_READY)`：**非阻塞**提交 release 请求；物理切换异步完成，用 `get_control_state` 轮询。

### 5.5 环境变量（wrapper 自动处理）

| 变量 | wrapper 行为 |
|------|-------------|
| `RMW_IMPLEMENTATION` | 未设置 → 设为 `config.default_rmw_implementation` |
| `ROS_LOCALHOST_ONLY` | 强制设为 `0`（否则发现不了真机 DDS） |
| `CYCLONEDDS_URI` | 若配置了 `network_interface` 且未预设，则生成 `<NetworkInterface>` |

---

## 第 6 步：打包

RMW 包的 RPATH **必须** 包含宿主 ROS 路径：

```bash
ARCH=$(uname -m)
source /opt/ros/humble/setup.bash

mkdir -p build/intermediates
cp build/sdk_wrappers_local/unitree_g1_rmw/libunitree_g1_rmw_wrapper.so build/intermediates/

SDK_RPATH='$ORIGIN/lib:/opt/ros/humble/lib' ci/package_sdk.sh unitree_g1_rmw "$ARCH"
# → sdk-unitree-g1-rmw-aarch64.tar.gz
```

包结构：

```text
sdk-unitree-g1-rmw-aarch64.tar.gz
├── libunitree_g1_rmw_wrapper.so
├── config.json
├── manifest.yaml
├── checksums.sha256
└── lib/
    ├── libunitree_hg__rosidl_generator_c.so
    ├── libunitree_hg__rosidl_typesupport_c.so
    ├── …（unitree_api 同类库）
```

`manifest.yaml` 中 **`system_libs`** 列出由 `/opt/ros` 提供的库（不打包）：

```yaml
system_libs:
  - librcl.so
  - librcutils.so
  - librosidl_runtime_c.so
  - librmw.so
  - librmw_implementation.so
  - librosidl_typesupport_c.so
```

`sdk_diag --check` 会验证：这些库 **不在包内**，但 ELF `DT_NEEDED` 里若出现则必须在 manifest 声明。

---

## 第 7 步：sdk_diag 验证

### 7.1 静态检查

```bash
cmake -S tools/sdk_diag -B build-sdk_diag && cmake --build build-sdk_diag

build-sdk_diag/sdk_diag --check sdk-unitree-g1-rmw-aarch64.tar.gz
```

### 7.2 上机（机器人上需已 source ROS）

```bash
source /opt/ros/humble/setup.bash
export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp   # 若 wrapper 未自动设置

build-sdk_diag/sdk_diag sdk-unitree-g1-rmw-aarch64.tar.gz
```

建议流程：

```text
sdk_diag> wait          # 等 /lowstate 首帧；若无数据检查 DDS 网卡与 domain
sdk_diag> info
sdk_diag> read 10
sdk_diag> imu 5
sdk_diag> ready         # 释放 motion service
sdk_diag> zero 50
sdk_diag> unready
sdk_diag> quit
```

---

## 第 8 步：改成您自家 ROS 2 机器人

```bash
cp -r sdk_wrapper/unitree_g1_rmw sdk_wrapper/my_robot_rmw
```

| # | 改什么 | 说明 |
|---|--------|------|
| 1 | 消息类型 | 若不用 `unitree_hg`，换成您的 `my_msgs`；CMake `find_package` + manifest `private_libs` 同步更新 |
| 2 | 话题名 | `config.json` 全部 topic 字段 |
| 3 | 关节数 / 索引 | 改 `kNumMotors` 与 msg 字段映射 |
| 4 | 控制权逻辑 | 若您的栈无 motion_switcher，实现等价的「释放高层控制」RPC 或 service |
| 5 | manifest | `system_libs` 与目标 ROS distro 一致；Foxy/Humble 各打一包 |
| 6 | RPATH | 打包时 `SDK_RPATH` 指向目标机 `/opt/ros/<distro>/lib` |

**若您只有裸电机驱动、没有 ROS 消息定义：** 更简单的路径可能是 [SDK2 + DDS 指南](unitree-g1.html)，在 wrapper 里直接调您的 C API，而不强行上 ROS 2。

---

## 常见问题

**Q：wrapper 会启动 roscore / ros2 daemon 吗？**  
A：不会。它只是一个被 `dlopen` 的 `.so`，内部 `rcl_init` 创建一个 **匿名节点**（默认名见 `config.node_name`），与现有 ROS graph 共存。

**Q：能和 rclcpp 节点同时跑吗？**  
A：可以，同一进程外通常由 runtime 子进程加载 wrapper；机器人上其他 rclcpp 节点不受影响。注意 DDS domain 与话题名不要冲突。

**Q：Foxy 和 Humble 要分别编译吗？**  
A：是。rosidl / rmw ABI 不同，CI 会对两个 distro 各打一版 tar.gz。

**Q：为什么不把 librcl 打进包？**  
A：规范要求 ROS 核心库由 **机器人系统镜像** 提供，避免与 onboard ROS 版本冲突。manifest 的 `system_libs` 就是告知审核方「这些依赖由宿主提供」。

**Q：和 `unitree_g1`（SDK2）选哪个？**  
A：已有稳定 ROS 2 low-level 话题 → **本指南**。希望单包自包含、不依赖 `/opt/ros` → [SDK2 指南](unitree-g1.html)。

---

## 下一步

- [Unitree G1 SDK2 分步指南](unitree-g1.html)  
- [适配手册 v1.0](../vendor-api-manual-v1.0.html)  
- [文档首页](../)
