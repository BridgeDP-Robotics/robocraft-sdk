# sdk_diag — SDK Wrapper 自检与诊断工具

`sdk_diag` 面向 wrapper 厂商，有两种用途：

- **检查模式（`--check`）**：对打好的包做全套静态检查（包结构、manifest、符号导出、RPATH、依赖白名单、禁用符号、glibc/ABI 版本）。手册里的各项要求都可由它一键核验，适合接入 CI。
- **交互模式**：接受 `<package_dir|archive>`，连接**真实机器人**，通过你的 wrapper 读取硬件状态（电机、IMU、电池、手柄等）并下发安全的电机命令，用于上机联调与验收。

> 交互模式下所有电机命令都被钳制在安全上限内：`max_tau = 5.0 Nm`、`max_kp = 30.0`、`max_kd = 5.0`。

## 检查模式（打包自检）

```
sdk_diag --check [--lang zh|en] <package_dir | package_archive>
```

报告语言可切换，默认中文。`[PASS]/[WARN]/[FAIL]` 后的英文检查 id 在两种语言下都保留（便于 CI grep）：

- `--lang zh`（默认）/ `--lang en`，简写 `--zh` / `--en`
- 或设置环境变量 `SDK_DIAG_LANG=en`（CLI 参数优先级更高）
- 输出统一为 UTF-8 编码

逐项核验（静态项任一不过即非零退出，可接入 CI）：

| 检查 | 内容 |
|------|------|
| 包结构 | wrapper.so 在包根、私有库在 `lib/`、`manifest.yaml` / `checksums.sha256` 齐全 |
| manifest | 字段校验（对照 `wrapper-manifest-v1.schema.json`）、`files[]` 的 SHA256 + size 与实际一致、`target.arch` 与 ELF 一致 |
| 符号导出 | 仅导出 `sdk_vtable`，无其它泄露符号 |
| RPATH | `RUNPATH` 包含 `$ORIGIN/lib` |
| 依赖 | 每个 `DT_NEEDED` 在系统库白名单内、或 `private_libs`（自带）/ `system_libs`（宿主 ROS 提供）已声明 |
| 禁用符号 | 无 `signal`/`fork`/`exec*`/`ptrace` 等禁用符号引用 |
| glibc/ABI | `GLIBC_*` / `GLIBCXX_*` 符号版本不超过声明上限 |
| capability | `get_capabilities()` 与 manifest `api.capabilities` 一致 |

```bash
sdk_diag --check ./acme-x100-1.2.3-aarch64/        # 解包后的目录
sdk_diag --check acme-x100-1.2.3-aarch64.tar.gz     # 或直接查压缩包（需系统 tar/unzip）
```

## 交互模式（上机联调）

```
sdk_diag <package_dir|archive>
```

与检查模式接受相同的输入类型（目录、`.tar`、`.tar.gz`、`.tgz`、`.zip`）。

示例：

```bash
sdk_diag ./acme-x100-1.2.3-aarch64/
sdk_diag acme-x100-1.2.3-aarch64.tar.gz
```

启动时工具会：`dlopen` → 校验 `sdk_vtable` 与 `api_version` → `create()` → 跑一次 `update()` 并打印 `info`，随后进入交互式提示符 `sdk_diag>`。退出时自动下发 damping 帧并 `destroy()`。

### 交互命令

| 命令 | 作用 |
|------|------|
| `info` (`i`) | wrapper 信息、API 版本、struct_size、capability、comm 状态 |
| `read [N]` (`r`) | 读电机状态 N 次（默认 10，间隔 100ms） |
| `imu [N]` | 读 IMU N 次（rpy / gyro / acc / quat） |
| `battery` (`bat`) | 读电池电压 / 电流 / 百分比 |
| `joystick` (`joy`) | 读手柄轴 / 按键 |
| `damping` (`damp`) | 下发 damping 帧（kd=0.1，50 帧） |
| `zero [N]` | 下发 N 帧零力矩（默认 50） |
| `hold [kp] [kd] [ms]` | 保持当前位置（默认 kp=10 kd=0.5 ms=3000），结束后 damping |
| `ready` | `request_control(READY)` 并轮询确认 |
| `unready` (`release`) | `request_control(UNREADY)` 并轮询确认 |
| `state` | 显示当前控制态 |
| `wait [sec]` | 等待首帧数据（默认 30s），含常见故障提示 |
| `diag` (`d`) | 显示 DDS 诊断统计（需 wrapper 提供 `get_debug_stats`，可选） |
| `help` (`h`/`?`) | 帮助 |
| `quit` / `exit` (`q`) | 退出 |

## 推荐用法

- **打包自检**：`sdk_diag --check <包>`，覆盖全部静态要求，建议作为上传前的 gate 接入 CI（非零退出即失败）。
- **上机联调**：在能接通机器人的环境用 `sdk_diag <你的包目录>`，确认 `info` 正常、`read` / `imu` 等返回合理、`ready`/`unready` 双向可切。

## 构建

`sdk_diag` 是最小依赖工具：检查逻辑（ELF 解析、SHA256、YAML、manifest 校验、编排）全部自带，唯一外部输入是公开头文件 `sdk_wrapper/interface/sdk_wrapper_interface.h`。

单独构建（仅需 C++17 编译器 + 标准库）：

```bash
cmake -S tools/sdk_diag -B build-sdk_diag
cmake --build build-sdk_diag        # 产出 build-sdk_diag/sdk_diag
# 头文件不在默认位置时：-DSDK_WRAPPER_INTERFACE_DIR=<含 sdk_wrapper/ 的目录>
```

源码组成：`sha256` / `yaml_lite` / `elf_inspect` / `manifest` / `package_check`（检查库 `sdk_diag_check`）+ `sdk_diag.cpp`（CLI）。manifest 字段参考见 `wrapper-manifest-v1.schema.json`。
