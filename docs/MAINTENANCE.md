# 操作系统更新维护

## 原则

- 先刷新索引并模拟升级，记录版本、依赖和服务影响；不做无审查的全系统变更。
- 编译器、Vulkan loader、Mesa、loonggpu、内核和 initramfs 属于高风险路径，更新后必须重跑硬件像素、呈现和回归测试。
- 不执行盲目的 `apt autoremove`。本机保留 GCC/LLVM、Rust、CMake、VTK 等开发依赖，即使 apt 当前将其中一些标为可自动移除。
- 单次烟雾 FPS 只用于发现异常，不作为升级提速/降速证明；性能结论需要同负载正反顺序配对。

## 2026-11-06：15项用户态更新

刷新 AOSC stable 索引后共有15项升级，模拟结果为无新包、无卸载。GCC16.2、Mesa 26.1.5、Vulkan loader 1.4.363.0 和目标内核均未变化。

| 软件包 | 旧版本 | 新版本 |
|---|---|---|
| apt | 2:3.2.0-1 | 2:3.2.1 |
| oma | 1.28.2 | 1.28.3 |
| openssh | 10.6p1 | 10.6p2 |
| lz4 | 1:1.10.0 | 1:1.11.0 |
| shadow | 4.19.4-1 | 4.19.4-2 |
| unbound | 1.26.1+icannbundle20260528 | 1.26.2+icannbundle20260528 |
| ostree | 2026.5 | 2026.6 |
| aom | 3.13.1-4 | 3.15.2 |
| dav1d | 1.5.2 | 1.5.4 |
| gstreamer | 1.28.7-1 | 1.28.7-2 |
| libass | 0.17.4 | 0.17.5 |
| libplacebo | 7.360.1-1 | 7.387.0 |
| mpv | 0.41.0-6 | 0.42.0 |
| prison | 1:6.30.0 | 1:6.30.0-1 |
| zxing-cpp | 1:2.3.0-2 | 1:3.1.1-1 |

没有执行 autoremove。升级后 apt 可升级列表为空，SSH 到发布服务器成功，SSH/SDDM、v2rayA、Homebrew 双 lane 均保持 active。

### initramfs 与 GRUB

更新触发器为三个7.3.1目标内核重建 initrd 并成功生成 GRUB 项。直接 `lsinitrd` 确认 gcc-O3 与 clang-LTO initrd 均包含各自 `updates/dkms/loonggpu.ko`；`dkms status` 也显示三个7.3.1目标均 installed。

旧7.0.12条目仍出现缺少 loonggpu 的 dracut 警告，不能将整个触发器输出描述为“无警告”。本轮没有为旧内核伪造模块或改动其条目。

GRUB `saved_entry` 保持 gcc-O3，当前运行内核仍为 clang-LTO；维护操作没有切换默认项或重启。

### 图形回归

在已安装 vkgl 0.1.25、LG110 硬件 Wayland 会话中：

- split-texture LINEAR 精确 raw `007f22ff` 通过；
- 64周期 dispatchable 生命周期测试通过；
- dmabuf 与 wl_shm 的 vkcube/vkmark 烟雾均通过，validation 保持28 events/4类；
- 没有 llvmpipe/swrast 替代，也没有 GPU 测试进程残留。

证据：[`os-maintenance-20261106.log`](../scripts/tests/vkgl/os-maintenance-20261106.log)、
[`os-post-pixel-linear.log`](../scripts/tests/vkgl/os-post-pixel-linear.log)、
[`smoke-os-updated-0.1.25-dmabuf.log`](../scripts/tests/vkgl/smoke-os-updated-0.1.25-dmabuf.log) 和
[`smoke-os-updated-0.1.25-shm.log`](../scripts/tests/vkgl/smoke-os-updated-0.1.25-shm.log)。

## 2026-11-07：6项修订与vkmark构建依赖

隔日检查发现6项升级，模拟无新装/卸载：aosc-aaa 14.0.1、glibc/glibc-dbg 2.44-1、libjpeg-turbo 3.2.1、screen 5.0.2-1、sip 6.17.2。另为 vkmark 同步修复构建新装 glm 1:1.0.2。GCC16.2、Mesa26.1.5、Vulkan loader1.4.363.0和vkgl0.1.26未变；仍不执行autoremove。

更新后 apt 可升级列表为空。三个7.3.1目标initrd均直接确认含 `updates/dkms/loonggpu.ko`，DKMS五目标installed，运行内核仍为clang-LTO，GRUB saved_entry仍为gcc-O3。旧7.0.12继续有缺loonggpu警告；aosc-aaa触发器有 `/var/tmp` 非空提示、ldconfig有libseat非符号链接提示，均记录而不伪称无警告。

vkmark2025.01-4双路径验证事件为0；vkgl库SHA保持f4479026，host format/sampler/fence/dispatch生命周期、LINEAR raw007f22ff全过。证据为 `os-upgrade-2026-11-07.log`、`post-os-2026-11-07-{dmabuf,shm}.log`、`post-os-*-2026-11-07.log`。

### 账号/组完整性与既有旧世界模块失败

健康检查发现每日shadow服务失败：21个系统账号缺shadow项、60个组缺gshadow项，另有http/locate/named三个home目录不存在。备份四文件后执行标准 `pwconv`/`grpconv`，逐条验证所有既有密码和组字段完全不变，仅新增锁定项；创建三个root-owned 0755空home目录后 `pwck -r`/`grpck -r`均为0，oneshot服务 `Result=success, ExecMainStatus=0`。

`systemd-modules-load` 仍保留既有失败：liblol0.1.12的 `la_ow_syscall` 在7.3.1 clang-LTO上EINVAL。该模块会解析kallsyms并替换旧世界syscall表，不能在未定位兼容原因时屏蔽配置或强行打补丁。本轮未改liblol/模块，也不把failed列表描述为空。Homebrew lane1/lane2、queue timer、SSH/SDDM/v2rayA/nginx均active；当前索引为2392个bottle formula，tap有3841个配方（两者不是同一计数口径）。证据：`system-integrity-2026-11-07.log`、`homebrew-health-2026-11-07.log`。

升级前后单次烟雾 FPS 有正有负且负载不同，只作为健康检查，不作性能升降宣称。

### vkgl 0.1.27 软件实例化

后续差分发现0.1.26忽略instanceCount且不支持gl_InstanceIndex shader。0.1.27以内部instance/firstInstance uniforms和重复GL draw实现软件实例化，不调用未声明的硬件入口。direct/indexed/indirect/indexed-indirect共16项安装后raw像素全对、validation为0，双路径vkcube/vkmark也为0；既有8项原生命令流回归不变。普通vertex/shading配对均值仅-0.43%/-0.79%，不宣称性能提升。deb已发布，签名源28项且HTTPS/服务器字节一致。详见 [`vkgl-0.1.27.md`](../scripts/tests/vkgl/vkgl-0.1.27.md)。

0.1.28继续修复索引位宽：0.1.27将UINT32硬当UINT16，直接与间接像素均失败；新版在录制时保存每个draw的VkIndexType。UINT16/UINT32×直接/间接×四实例控制共16项安装后全过且validation=0。原生GFX仅268在402/404间变化，主动反向单字修改精确复现旧错误，281仍不命名。普通性能仍只有波动；签名源29项、HTTPS/服务器字节一致。详见 [`vkgl-0.1.28.md`](../scripts/tests/vkgl/vkgl-0.1.28.md)。

0.1.29修复signed vertexOffset被忽略：正偏移与负偏移抵消在0.1.28直接/间接均失败，firstIndex控制原本正确。新版仅在非零偏移时调用已确认存在且硬件声明的glDrawElementsBaseVertex。UINT16/32×直接/间接×base/positive/negative/firstIndex共16项安装后全过、validation=0，旧16项实例/位宽矩阵也过。原生269获firstIndex因果，274获正/负base vertex因果，错误输入保护零修改。两组性能绝对值与方向不一致，不归因提升或回退；签名源30项、HTTPS/服务器字节一致。详见 [`vkgl-0.1.29.md`](../scripts/tests/vkgl/vkgl-0.1.29.md)。
