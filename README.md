# robocraft SDK wrapper 适配套件

robocraft-runtime 部署在机器人本体上，通过加载厂商提供的 SDK wrapper（`.so`）与硬件
通信。适配一个新机型，就是把自家机器人 SDK 封装成一份导出 `sdk_vtable` 的 `.so`，
按规范打包交付——wrapper 包是双方唯一的交付边界，runtime 侧接入由 robocraft 完成。

## 目录

- `sdk_wrapper/interface/sdk_wrapper_interface.h` — 接口头文件，唯一权威 ABI，以它为准
- `docs/vendor-api-manual-v1.0.md` — 适配手册（交付物、manifest、符号导出、RPATH、网络）
- `sdk_wrapper/unitree_g1/`、`sdk_wrapper/unitree_g1_rmw/` — 可编译参考 example，
  厂商 SDK 已 vendored 在 `sdk/` 下
- `examples/` — 上述两个 example 的成品包（CI 构建，aarch64），交付物长什么样解开即见
- `tools/sdk_diag/` — 打包自检与上机诊断工具（零运行时依赖，可独立构建）
- `ci/`、`scripts/` — 打包与构建脚本

## 适配流程

整体路径：复制 example → 实现 vtable → 构建 → 打包自检 → 上机冒烟 → 交付。

### 1. 复制 example 作起点

`sdk_wrapper/unitree_g1` 链接厂商静态 SDK、自带 Cyclone DDS；
`sdk_wrapper/unitree_g1_rmw` 走宿主 ROS 2。哪个接近你的集成方式就抄哪个，
目录改成自己的机型名（下文以 `my_robot` 为例）。

### 2. 实现 sdk_vtable

wrapper 内部状态装进不透明的 `sdk_handle_t`，逐个实现函数后填进 `sdk_vtable`
导出——这是 `.so` 唯一对外符号，没有工厂函数：

```cpp
SDK_API extern const sdk_vtable_t sdk_vtable = {
    sizeof(sdk_vtable_t), SDK_API_VERSION,
    VCreate, VDestroy, VGetNumMotors, /* ... */
};
```

- `create` / `destroy` / `get_num_motors` / `update` / `get_motor_q` /
  `get_motor_dq` / `begin_cmd` / `set_motor_cmd` / `commit_cmd`
  这九个是 runtime 加载时校验的必填项，缺了直接加载失败
- IMU、电池、摇杆、`request_control` 等按实际硬件实现，并在 capabilities 里如实声明
- `create` 收到的 `wrapper_dir` 是 `.so` 所在目录，运行参数从该目录下的
  `config.json` 读

### 3. 构建

照 example 的 `CMakeLists.txt`：定义 `SDK_WRAPPER_BUILDING`、
`CXX_VISIBILITY_PRESET hidden`、用 `sdk_wrapper/export.map` 限制只导出
`sdk_vtable`。产物命名 `lib<目录名>_wrapper.so`。工具链不限（手册 §2.1）。

`manifest.yaml` 和 `ci.yaml` 照 `sdk_wrapper/unitree_g1/` 改：`api.entrypoint`
填 `.so` 文件名，`api.capabilities` 与实现一致，`private_libs` 列出要随包带走的
依赖；`ci.yaml` 告诉打包脚本去哪收集这些库。

### 4. 打包与自检

```bash
ARCH=$(uname -m)

cmake -S sdk_wrapper/my_robot -B build/my_robot
cmake --build build/my_robot

# 放到打包脚本约定的位置再打包（读取 sdk_wrapper/my_robot/{manifest,ci}.yaml，
# 自动 patchelf RPATH=$ORIGIN/lib、收集私有库、生成 checksums.sha256）
mkdir -p build/intermediates
cp build/my_robot/libmy_robot_wrapper.so build/intermediates/
ci/package_sdk.sh my_robot $ARCH       # → sdk-my-robot-$ARCH.tar.gz

# 静态自检：符号导出 / RPATH / 依赖 / 包结构 / 校验和
cmake -S tools/sdk_diag -B build-sdk_diag && cmake --build build-sdk_diag
build-sdk_diag/sdk_diag --check sdk-my-robot-$ARCH.tar.gz
```

### 5. 上机冒烟与交付

`sdk_diag <包>` 进入交互模式，依次过 info → read → imu → ready → unready，
确认读数和控制权切换正常（用法见 `tools/sdk_diag/README.md`）。

把 `sdk-<name>-<arch>.tar.gz` 交给 robocraft 平台，适配到此为止。

## 构建参考 examples

`examples/` 下已放好两个 example 的成品包，可以直接解开对照
（包根是 wrapper `.so` + `manifest.yaml` + `checksums.sha256`，私有库在 `lib/`），
也可以用 `sdk_diag --check examples/sdk-unitree-g1-*.tar.gz` 体验自检。
下面是从源码构建出它们的完整命令：

```bash
ARCH=$(uname -m)

# --- Unitree G1（厂商静态库 + 自带 Cyclone DDS）---
./scripts/build_sdk_wrappers.sh --local g1
mkdir -p build/intermediates
cp build/sdk_wrappers_local/unitree_g1/libunitree_g1_wrapper.so build/intermediates/
ci/package_sdk.sh unitree_g1 $ARCH                # → sdk-unitree-g1-<arch>.tar.gz
build-sdk_diag/sdk_diag --check sdk-unitree-g1-$ARCH.tar.gz

# --- Unitree G1 RMW（依赖宿主 ROS 2）---
source /opt/ros/${ROS_DISTRO:-humble}/setup.bash
./scripts/build_sdk_wrappers.sh --local g1-rmw
cp build/sdk_wrappers_local/unitree_g1_rmw/libunitree_g1_rmw_wrapper.so build/intermediates/
SDK_RPATH="\$ORIGIN/lib:/opt/ros/${ROS_DISTRO:-humble}/lib" \
  ci/package_sdk.sh unitree_g1_rmw $ARCH
build-sdk_diag/sdk_diag --check sdk-unitree-g1-rmw-$ARCH.tar.gz
```

> g1 的 `Dockerfile` 是该 example 自选的构建环境（Ubuntu 20.04 + gcc-9，匹配
> unitree_sdk2 官方要求），并非 robocraft 强制——手册 §2.1 允许厂商自定工具链。
> g1_rmw 的前置（colcon 构建 unitree 消息）见其目录内 `README.md`。
