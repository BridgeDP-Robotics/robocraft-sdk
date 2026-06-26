---
layout: default
title: 首页
---

# robocraft SDK Wrapper 文档

欢迎。本文档面向**第一次接触 robocraft 项目**的机器人底软工程师——您可能已经为自家机器人写好了电机驱动、CAN/EtherCAT 收发、状态机，但还不清楚「wrapper」是什么、为什么要做、怎么做。

## 您现在的位置 vs 我们要您做的事

| 您已经会做的 | wrapper 要您额外做的 |
|-------------|---------------------|
| 读电机编码器、发电流/力矩指令 | 把这些能力**翻译成一张固定函数表**（`sdk_vtable`） |
| 维护机器人通信协议（CAN、DDS、ROS 话题等） | 在 wrapper 内部继续用您的协议，**对外只暴露统一 C 接口** |
| 打包 `.so` 给自家上层用 | 按规范打包成 `sdk-<机型>-<架构>.tar.gz` 交给 robocraft |

**一句话：** wrapper 是「转接头」。robocraft-runtime 只认一种标准插头；您把自家 SDK/驱动包一层，露出标准插头即可。

## 分步指南（从示例学起）

仓库里有两个宇树 G1 参考实现，请选与您的集成方式最接近的一个跟着做：

| 指南 | 适用场景 | 示例目录 |
|------|----------|----------|
| **[Unitree G1 — 直连 SDK2 + 自带 DDS](guides/unitree-g1.html)** | 机器人上没有 ROS 2，或您直接链接厂商 C++ SDK / 静态库 | `sdk_wrapper/unitree_g1/` |
| **[Unitree G1 RMW — ROS 2 话题通信](guides/unitree-g1-rmw.html)** | 机器人上已跑 ROS 2，底层数据在 `/lowstate`、`/lowcmd` 等话题上 | `sdk_wrapper/unitree_g1_rmw/` |

读完任一分步指南后，您应能：

1. 在本地编译出参考 wrapper 的 `.so`
2. 理解 `create → update → get_* → begin_cmd → set_motor_cmd → commit_cmd` 每一帧在干什么
3. 复制示例目录，改成您自家机型的 wrapper 骨架

## 其他资料

| 文档 | 说明 |
|------|------|
| [仓库 README](https://github.com/BridgeDP-Robotics/robocraft-sdk/blob/main/README.md) | 项目总览、7 步接入流程、术语表 |
| [适配手册 v1.0（vendor-api-manual）](vendor-api-manual-v1.0.html) | 交付规范、依赖白名单、manifest schema、审核项 |
| [接口头文件](https://github.com/BridgeDP-Robotics/robocraft-sdk/blob/main/sdk_wrapper/interface/sdk_wrapper_interface.h) | `sdk_vtable` 唯一权威 ABI 定义 |
| [sdk_diag 用法](https://github.com/BridgeDP-Robotics/robocraft-sdk/blob/main/tools/sdk_diag/README.md) | 打包自检与上真机联调 |

## 建议阅读顺序

```text
① 本页（建立概念）
    ↓
② 选 G1 或 G1-RMW 分步指南（动手编译 + 读代码）
    ↓
③ 复制示例改成本机型 → sdk_diag --check → 上机 sdk_diag
    ↓
④ 需要查规范细节时再看适配手册
```
