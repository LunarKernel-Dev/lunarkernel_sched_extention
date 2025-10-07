# Lunar Scheduling Extension (LSE)

**一个使用 Oplus hmbird_gki 风驰内核的 Slim WALT 负载跟踪驱动的调度器**

---

## 写在前面

起因是我想给 Xiaomi 设备移植 Oplus 的风驰游戏内核，也就是 OP 用户常说的 "scx"  

与你们印象中的不同, 风驰游戏内核并不是只有调频功能, 它是一套非常完整的, 集成了多种功能的调度机制, 包括:
 - 负载跟踪 (hmbird_gki/scx_util_track.c)
 - 任务调度 (hmbird_gki/scx_sched_gki.c)
 - sysctl 接口 (hmbird_gki/scx_main.c)
 - 处理器调频 (cpufreq_scx_main.c)
 - 影子计时器 (scx_shadow_tick.c)  

风驰游戏内核有两个版本 ( hmbird / hmbird_gki ), 它们的实现方式并不相同
 - **hmbird:** 基于一个魔改版的 sched_ext 调度类实现任务调度功能, sysctl 节点在 **/proc/hmbird_sched**, 其余功能的回调插入在这个调度类里, 在 task_struct 结构体中添加了一个指向必要结构体的指针成员
 - **hmbird_gki:** 模块本身包含了几乎所有功能, sysctl 节点在 /proc/sys/oplus_sched_ext, 通过 sched_assist 的 oplus_task_struct 将必要结构体挂载到原 task_struct 中的 android_oem_data1 数组, 其核心功能通过在高通 sched-walt 模块中插入的钩子 (SCX_CALL_OP) 实现  

目前看来, 6.6 内核版本的高通设备均使用 hmbird_gki 版本, 而 6.1 与 MTK 设备均使用 hmbird  
(所以网上那些所谓的保风驰补丁其实是不必要的, hmbird_gki 不需要这些)  

移植 hmbird 的工作量十分巨大困难, 并且大量修改了内核本体, 可以认为破坏了 KMI, 我不推荐  
相对来说, hmbird_gki 可以说是相当简单了, 只需要参考 Oplus 内核模块仓库中的 **sa_oem_data.c** 搓一套最小化的 ots, 并内建高通 sched-walt模块, 参考一加 13 的 msm-kernel 在其中插入风驰的函数回调即可 (特别说明一点, OP 开源的 SCX_CALL_OP 有问题, 你需要自己写 hook 框架)  

不过问题就在这里, Xiaomi 的 sched-walt 模块并没有完整开源, 内建 MiCode 的开源版本很大概率开不了机, 即便是 OP 这边, 其 sched-walt 模块也大量依赖了其他特性模块, 全部嵌入也非常费时费力  

于是我根据 TheVoyager0777 提供的思路, 拆出 hmbird_gki 中必要的部分
 - Slim WALT 负载跟踪, 用于为调频器提供负载信息
 - update_task_ravg 的最小化回调, 更新负载
 - scx 调频器, 字面意思
 - sched cluster 集群拓补, 为调度器提供集群信息  

跑了一套"最小化风驰", 也就是本项目 **Lunar Scheduling Extension**, 简称 **LSE**

---

## 源码结构

```
/ (模块根目录)
├─ Kconfig (定义内核配置)
├─ Makefile (模块构建 Makefi;e)
├─ README.md (本文件)
├─ include/ (一堆头文件)
│  ├─ cpufreq_lse.h
│  ├─ lse_cfs.h
│  ├─ lse_main.h
│  ├─ lse_sched_cluster.h
│  ├─ lse_sysctl.h
│  ├─ lse_task_struct_ext.h
│  └─ lse_util_track.h
├─ cpufreq_lse.c (调频器)
├─ lse_cfs.c (CFS 回调)
├─ lse_main.c (模块初始化)
├─ lse_sched_cluster.c (建立集群)
├─ lse_sysctl.c (sysctl 接口, 在 /proc/sys/lunar_sched_ext)
├─ lse_task_struct_ext.c (类似 ots, 将自定义结构体挂载到 task_struct 中 android_vendor_data1 数组的最后一位)
├─ lse_util_track.c (Slim WALT 负载跟踪)
└─ trace_lse.h
```

---

## 如何使用

无需对内核本体进行任何修改, 只需要将本仓库克隆到你的源码树中, 添加 Kconfig 引用和 Makefile 规则即可喵~  
默认编译为模块 (.ko), 我不确定 Built-in 是否会出问题, 编译完成后 insmod lunar_bsp_ext_sched.ko 就可以啦

## 配置接口

sysctl 节点中 (/proc/sys/lunar_sched_ext) 有 4 个参数
 - slim_walt_ctrl: 启用 Slim WALT 负载跟踪, 别随便关不然没法调频 ((((
 - slim_walt_policy: 额, 我也不知道 ((((
 - sched_ravg_window_frame_per_sec: 一秒的帧数, 用于调整窗口大小, 默认 120
 - lse_gov_debug: 调频器调试开关, 没啥用  

调频器 tunables (/sys//devices/system/cpu/cpufreq/policy*/lse)
 - apply_freq_immediately: ???
 - soft_freq_cur: ??????
 - soft_freq_max: 限制最大频率
 - soft_freq_min: 限制最小频率
 - target_loads: 目标负载, 与其他经典调频器的作用相同

---

## 贡献与协议

这是我空余时间随便写的项目, 代码质量不高, 包括但不限于
 - 乱七八糟的头文件引用
 - 随意的函数/变量命名
 - 糟糕的源码结构
 - 少得可怜的注释 (大部分都是 OP 的注释没删完)
 - 混乱的 README
 - ......  

因此欢迎各位提交贡献, 帮忙整理源码或者完善文档及翻译, 云云感激不尽 (> <)  

本项目使用 [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html) 协议开源, 若公开分发也请保持开源, 并保留原作者信息

---

## 鸣谢列表

 - [realme-kernel-opensource](https://github.com/realme-kernel-opensource/realme_15pro5G-AndroidV-vendor-source/tree/99b13443e39dc9c15f150b14134298443b406cf4/vendor/oplus/kernel/cpu/sched_ext): 风驰源码
 - [Huawei Open Source Release Center](https://consumer.huawei.com/en/opensource/detail/?siteCode=worldwide&productCode=Smartphones&fileType=openSourceSoftware&pageSize=10&curPage=1): 独立 sched cluster 实现参考
 - [TheVoyager0777](https://github.com/TheVoyager0777): 提供移植思路, 感谢 TV 佬!  

---
