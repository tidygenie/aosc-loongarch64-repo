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

0.1.30修复多顶点binding相互覆盖：Pipeline和执行器现保存16组独立VBO/offset/stride，每个attribute按自身binding上传与取址。双binding位置offset8/stride8、UV stride16的四种indexed/indirect组合安装后全为gradient且validation=0；旧32项功能及21项原生回归通过。普通场景小样本vertex/shading均值-1.85%/-0.38%，不归因回退。原生trace只见独立XDMA，两个VBO base VA均未字面出现在GFX中，不虚构地址字段。签名源31项、HTTPS/服务器字节一致。详见 [`vkgl-0.1.30.md`](../scripts/tests/vkgl/vkgl-0.1.30.md)。

0.1.31实现软件实例化下的instance-rate属性步进：每次重复draw仅对INSTANCE binding设置`voff+attr.offset+(firstInstance+instance)*stride`，vertex-rate保持不动，不调用未声明的divisor。0.1.30八项two/first均错误绿色，0.1.31十二项候选/安装后全过且validation=0；BGRA红色raw为0000ffff。旧36项功能、21项原生回归和双路径smoke全过。小样本vertex/shading均值+1.28%/+0.87%，不宣称提升。签名源32项、HTTPS/服务器字节一致。详见 [`vkgl-0.1.31.md`](../scripts/tests/vkgl/vkgl-0.1.31.md)。

0.1.31后续30秒cpu-clock剖析仅287/258样本，vkgl DSO占5.57%/5.43%，gsgpu_dri占23.00%/28.29%；未知样本不归因，perf_event_paranoid已在每次采样后恢复2，不据此再发CPU微优化版本。原生顶点控制以同VBO字节和同gradient隔离offset0→8、UV stride8→16，XDMA/GFX及两个辅助BO前64 dword均无差异；新增仅显式tracked VA的只读临时GEM映射，默认/无效VA保护通过，未写BO或命名未知字段。详见 [`lg110-vertex-state-bo.md`](../scripts/research/lg110/lg110-vertex-state-bo.md) 与 [`lg110-cpu-profile-0.1.31.md`](../scripts/research/lg110/lg110-cpu-profile-0.1.31.md)。

顶点研究进一步改为同进程same/offset/stride三帧A→B→A，九帧validation与raw均通过。跨进程同参数handle11哈希仍变化，排除该方向的进程噪声；第二pipeline初次fill也通过ABA识别为懒初始化。初始化后same/offset/stride的小BO在seq4→5均无变化，GFX差分分别12/12/14，stride仅多word338/362的状态BO地址切换；对应两组16K BO完整哈希与首64 dword分别相同，故不命名为stride字段。bo_list限域只读快照、旧count=3/2回归及机器审计通过，驱动保持0.1.31。详见 [`lg110-vertex-state-aba.md`](../scripts/research/lg110/lg110-vertex-state-aba.md)。

# 2026-11-07 Homebrew与系统维护

- APT元数据刷新后可更新包为0；第三方更新检查确认ruby 4.0.9、v2raya 2.5.13、zerotier-one 1.16.2均为当前最新，没有为本轮盲目升级图形组件。
- 双lane保持运行。dsvpn原测试只接受Linux gateway错误，但无CAP_NET_ADMIN环境实际先返回精确的`tun device creation: Operation not permitted`。自定义测试接受这两个确定结果，不改变安装/运行逻辑；重新构建后`brew test`、bottle回装及回装测试全部通过，状态为`test=1,pour=1`。四种单横线/双横线及rebuild别名的本地、服务器、HTTPS SHA-256均为`a66feb2835d0b87ce624c5f676fa8161e6d16c0f7cee50c3fbaf5d965ae3d921`。
- 五类定制巡检首次恢复22处已登记的旧包gnu17/GNU_SOURCE、告警兼容与gputils `-lfl`漂移；逐项diff审计后第二次巡检修复0处。dsvpn/lcdf等后续定制均走`state/custom`与`finalize-formula.sh`，未绕过stub/custom保护。
- lcdf-typetools原测试硬编码不存在的DejaVuSans.ttf。自定义测试保留真实`otfinfo -p`解析与family断言，在原文件缺失时选用已确认存在的FreeSans.otf；构建测试及bottle回装测试均通过，`test=1,pour=1`。四别名本地/服务器/HTTPS SHA-256均为`98428c51990fcbd02d09ed1a57c0bdb5a8d11f38491a1f733437ebbc70dd81ab`。最终定制巡检修复0处，tap本地/服务器HEAD均为`290a73cbb7d873aa976776a31828d46b8359170e`，瓶子别名计数两边均为3606。
- 后续lane审计发现`brew install --only-dependencies`并不安装test依赖，已在`brew-lane.sh`原子加入`--include-test`，运行中的lane保持原inode且未重启。endlessh dry-run及实际日志均确认开始解析/安装netcat；其0.7.1旧autoreconf另报`AM_INTL_SUBDIR`，不将该依赖问题伪称已修。针对endlessh仅需`nc -z`连通性检查的测试，custom formula改用Ruby标准库`TCPSocket`并删除netcat test依赖，构建测试、bottle回装及回装测试均通过，`test=1,pour=1`。最终四别名本地/服务器/HTTPS SHA-256均为`6d58e5a160bda8ace693fe1ecfeb0a47ebba152b37f1476e8aaf0c8e2eaaf9d1`，tap main本地/服务器HEAD=`2f96092a79f097f50520ac32a645458f3adf469b`。
- `make-cflags-formula.py`的CMake分支此前把C gnu17同时写入C/CXX；现仅将CXX替换为gnu++17，不回填覆盖现有formula。使用临时INFRA运行真实生成器，合成CMake formula的Ruby语法、C gnu17、CXX gnu++17、cmake参数展开及custom跳过全部通过，真实tap无cmaketest文件。APT检查仍为0项可更新，未执行autoremove或高风险图形升级。
