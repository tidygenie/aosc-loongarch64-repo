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

# 2026-11-08 LG110原始UAPI与同步全VA顶点审计

- 原始LoongGPU ioctl解码排除strace错误别名：same/stride仅第二pipeline懒初始化使CREATE/MMAP/VA各多2次，没有额外请求类型。14次METADATA与4次GEM_OP规范化后完全相同；GEM_OP只是GET_GEM_CREATE_INFO，其value为用户态输出指针，不是顶点状态更新。
- 新增属性敏感fragment与同进程same/stride/pipeline三帧控制。stride原始BGRA为`008040ff→009f20ff→008040ff`，same及两个不同pipeline但同stride16均为三次`009f20ff`。GALLIUM_TRACE确认position始终stride8，UV严格为16/16/16或8/16/8，排除vkgl漏传。
- 敏感pipeline/stride的seq3/4/5 GFX逐字相同，seq4→5的11词差分列表也完全相同；word204/228仅确认为shader程序VA选择器，配对shader完整16K FNV相同，不命名其他未知词。旧word338/362仍是状态BO选择器。
- 新只读哈希每次最多映射2MiB，覆盖完整16MiB BO及不在CS bo_list中的tracked VA。未等待时handle12在seq4整480块差异、seq5又一致，判定异步采样竞争；增加有界WAIT_CS后，三draw每模式13516/13524/13524条记录跨pipeline/stride全部完全相同。20次设备mmap全部匹配GEM_MMAP，DRM read/write/pread/pwrite调用为0。
- 默认same/offset/stride、敏感same/stride/pipeline和legacy被动count=3均通过；主动count=2仍为预期663300ff/word270=2，旧程序因保留count=3期望而预期退出1。安装/发布vkgl保持0.1.31，无新deb、原生Vulkan、零拷贝或性能提升宣称。详见 [`lg110-vertex-state-uapi-all-va.md`](../scripts/research/lg110/lg110-vertex-state-uapi-all-va.md)。

# 2026-11-08 Homebrew netcat、pnpm与系统健康闭环

- netcat 0.7.1的旧`AM_GNU_GETTEXT([external])`之外还冗余调用已被现代gettext删除的`AM_INTL_SUBDIR`，autoreconf因此在编译前失败。custom formula将该行替换为显式`AC_CHECK_HEADERS([locale.h])`，保留external gettext与NLS；临时源码autoreconf/configure/make及运行验证通过。原google.com测试改为本地Ruby TCPServer，排除外网和HTTP 301噪声。
- 现有lane完成netcat构建、测试、bottle回装和回装测试，`test=1,pour=1`。最终custom/tap保留本地root_url及瓶子块，lane1再次独立HTTPS `--force-bottle`灌注和本地测试通过。四种单横线/双横线及rebuild别名的本地、服务器、HTTPS SHA-256均为`df1ec6b11d4b9c6cd5a8057fbcc1bea93c00fb285b6dc2bd1951909315833f6c`；tap提交`25c1ddab6df609c09709fe2ed9daa3d124bcee91`为服务器main祖先。全量custom校验首次修复23处，第二、第三次均修复0处。
- 审查[pnpm 12.18.0发布说明](https://pnpm.io/blog/releases/12.18.0)后，因工作区重复安装/`verifyDepsBeforeRun`性能修复及watcher临时文件修复，单独将pnpm 12.15.0升至12.18.0；模拟和实际均只升级1项、无删除，既有esbuild进程和DSH GUI保持。fcitx5-qt/pyqt5/qt-5三项目前可联动升级，但Qt图形栈需要独立回归窗口，本轮未盲升，未执行autoremove。
- 运行内核仍为7.3.1 clang-full-LTO，三个7.3.1目标DKMS和现有initramfs均直接包含loonggpu.ko，GRUB saved_entry仍为gcc-o3。SDDM/KDE Wayland继续使用gsgpu和loonggpu-dc，无llvmpipe回退；v2rayA 2.5.13-2 active且当前日志无错误，历史DNS closed-pipe未复现。AOSC签名源仍为32项，站点Homebrew索引2403。
- 系统仍如实记为degraded：已知liblol `la_ow_syscall` EINVAL、六个历史DrKonqi五分钟超时、用户aa-notify缺少/var/log/syslog，以及KWin RT-priority EPERM/gamma ENOSYS警告未修；未通过清理failed状态或屏蔽模块伪装健康。

# 2026-11-08 仅保留双目标内核与liblol安全边界

- 按用户要求，只保留`7.3.1-mainline-16k-rt-gcc-o3`和`7.3.1-mainline-16k-rt-clang-lto`。精确purge `linux-kernel-7.0.12`、`linux-kernel-7.1.13`、`linux+kernel`、gcc非O3内核/headers共5包，删除旧调试initramfs/日志和3类旧DKMS记录，未执行autoremove。/boot与/lib/modules仅余两个目标；通用linux+api、kernel-tools和本地deb回滚归档不属于启动内核，未误删。
- /etc/grub.d/40_custom中的earlylog/watchdog旧菜单已清除并恢复为标准空模板，update-grub只生成两个目标Linux项；saved_entry保持gcc-o3，next_entry为空。两个新initramfs均直接包含updates/dkms/loonggpu.ko，SHA-256分别为clang `83cfcc5a76dae087e561254b1d973988207fb35e83013cfaa477545288fd82cb`、gcc-o3 `ed0258311629b4c14461adab1391c815ed4ca114e89a65d908bbc4f2641be06b`。
- liblol EINVAL已静态定位：Clang full-LTO把kallsyms_lookup_name内联进kprobe_lookup_name，旧双kprobe方法不会触发首探针，地址保持0并在la_ow_syscall_main.c:112返回-EINVAL；GCC保留尾调用。源码候选在unregister清空前使用register_kprobe已解析的kp0.addr作fallback，并保留符号校验。
- 候选实际加载命令与上一boot在18:02:42异常终止时间重合，随后18:04以7.1.13启动；没有持久panic日志，不能宣称修复有效或断言具体崩溃原因。当前7.1.13自带la_ow_syscall正常加载，本次清理后不主动重启，不再直接加载该候选；下一次验证必须先做不写syscall表的discovery-only测试。
- 随后外部重启已进入保留的7.3.1 gcc-o3：systemd-modules-load返回0，la_ow_syscall与loonggpu正常加载；通过old-world loader实际执行LibLoL glibc 2.44返回0，KDE Wayland、loonggpu-dc和vkgl LG110均正常。这建立可用对照，但不代表clang已修复。
- 公共内核配方新增[`0005-loongarch-ow-syscall-lto-kprobe-addr.patch`](../packages/kernel/linux-kernel-mainline-16k-rt/autobuild/patches/0005-loongarch-ow-syscall-lto-kprobe-addr.patch)：除kp0.addr fallback外加入默认关闭的discovery_only参数，init和exit均在任何syscall表写入前检查。补丁精确重放一致，并用构建树同一Clang full-LTO参数编译为LLVM IR成功；目前只完成编译验证，未加载、安装或发布新deb，clang运行验证仍待进行。新增[`test-liblol-discovery.sh`](../scripts/test-liblol-discovery.sh)，核对clang内核/vermagic/参数、拒绝替换已加载模块、使用EXIT trap卸载并保存持久证据；在gcc-o3上的拒绝保护已实测。

# 2026-11-08 Homebrew b2sum可移植实现闭环

- b2sum 20190724默认使用SSE2，原公式只对ARM切换NEON，LoongArch因emmintrin.h缺失失败。custom对非Intel且非ARM改用上游ref源文件，不伪造SIMD头；临时构建通过abc标准BLAKE2向量，lane构建/测试/bottle回装/回装测试全部通过，`test=1,pour=1`。
- 四种单横线/双横线及rebuild别名的本地、服务器、HTTPS SHA-256均为`ae83bb5e103c1e197f0352742160caeeebf00bfe3dfd2c9fb789cebcc653ce93`，tap提交`826e6d07e4a7283cd419b3b9a8c273ccca796220`已到服务器main。最终本地bottle块已回填custom（infra `1c1fe4ae4`）；全量custom校验随后第二次修复0处。

# 2026-11-08 LG110双swapchain输出归属

- 持久复测pipeline/stride/same并覆盖全部5个CS，IB首32字逐字一致（完整IB及descriptor见下节），输入handle15/16各10条哈希跨模式完全一致。新增只读单dword BO采样，直接证明stride frame0/2对应handle11 seq3/5、frame1对应handle12 seq4；两个BO均为0x1e0000，中心偏移0xf0500与CPU readback逐字一致。此前大表面差异因此归为双swapchain实际图像内容，不再当作隐藏stride状态；其余小BO仍未命名。严格构建、被动draw3、主动word270 count2及敏感三模式全过，库保持0.1.31，无性能或原生Vulkan宣称。详见[`lg110-swapchain-output-attribution.md`](../scripts/research/lg110/lg110-swapchain-output-attribution.md)。

# 2026-11-08 clang构建不阻塞启动目标

- 自有`kernel-build-clang-lto.service`原为oneshot，导致每次重启multi-user/graphical等待完整`make -j1 LLVM=1 all`。持久unit及/etc实例已改为`Type=simple`并移除RemainAfterExit，保留User=houge、Nice=10和IO idle；`systemd-analyze verify`通过，daemon-reload前后当前make PID均为738，未中断构建。当前旧启动job仍在等待，不能宣称本轮已解除；下一次重启需检查critical-chain和multi-user不再等make结束。infra提交`0d5170d37`。

# 2026-11-08 LG110顶点descriptor实证与IB范围更正

- 对SHA=`aff7101a`的gsgpu_dri恢复真实set_vertex_buffers=0x17ae08、draw_vbo=0x197d18和drawcall/VBO构造=0x17bb40。VBO缓存位于ctx+0xe38+16×slot；`17bc28`读取16位stride，`17bc38`将低14位插入descriptor word1 bits[29:16]。GDB只读捕获position/UV地址精确对应handle15/16，UV stride8/16时word1=`00080001`/`00100001`，其余地址、大小、格式字一致，CPU A-B-A正确。脚本和报告已公开。
- 更正：旧敏感trace每CS只有首32 dword，不得写成完整IB全等。新2048上限抓取实际16/16/544/528/528字，pipeline/stride完整数组仍全等且不含descriptor字，故descriptor不内联IB。进一步由resource分配器ctx+3896/3912及/proc maps定位descriptor BO为handle8（GPU base=0x1010c0000），每帧首个pair偏移2b00/8f80/f380；UV word1在pipeline为16/16/16、stride为8/16/8，其余字全等，6个定向程序全PASS，断言SHA=`50bde8d1`。撤回旧“handle8相同”负结论：当时单条VADUMP被256字截为1KiB，未覆盖2b00；第四构造偏移也尚未在seq3/4生成，失败断言未强行通过。无驱动修改、无性能宣称。详见[`lg110-vendor-vertex-descriptor.md`](../scripts/research/lg110/lg110-vendor-vertex-descriptor.md)。

# 2026-11-08 LG110 descriptor因果突变

- 新增`gsgpu-mutate-vertex-stride.gdb`，要求精确gsgpu SHA，仅对pipeline三帧首个UV entry（2b10/8f90/f390）在旧值00100001时改stride16→8。实际恰好3次写、无意外descriptor，原始像素由BBB变为AAA，外部逐字节断言SHA=`d3806dd12a570d2d5024232df6d14856c7fde101d2ddf6c76e62339c537bf087`；不依赖程序pattern_valid。初次误用pair基址导致0次写、BBB及断言失败，修正为pair+0x10后才成立。安装驱动未改，无性能宣称。

# 2026-11-08 LG110 descriptor IB间接引用闭环

- 完整IB在seq3 word220、seq4/5 word218分别含descriptor VA低字010c2b00/010c8f80/010cf380，高字均1。先捕获UV descriptor再监视实际drawcall+336，硬件写watchpoint命中`182074 st.d`（报告PC为下一指令182078）。函数181fe8精确计算`ctx[3912]->gpu_va + ctx[3908]`并写drawcall+336，首帧1010c0000+2b00与IB一致。公开`gsgpu-descriptor-pointer-writer.gdb`；API stride→VBO缓存→handle8 word1→IB间接VA→像素因果链闭合，仍无原生Vulkan/性能宣称。

# 2026-11-10 Homebrew 7.0.13、GUI与liblol候选

- clang full-LTO全量构建于11月9日00:41完成。发现构建树只有kp0.addr修正而缺discovery_only四个hunk，补齐后重编`la_ow_syscall.v2.ko`，vermagic匹配clang-lto且参数出现，SHA=`04d17dfcacef0345584f97bdc06e443f37dd7d591b2524f7157dc713c4d99add`；gcc-o3拒绝保护通过，未加载，运行验证留待受控clang启动。
- Homebrew 7.0.13浅标签候选保留原4个LoongArch补丁，并修复CPU.type/Linux family导致的`packages.dunno_linux`404。候选及lane1的`--version`、`--config`、b2sum配方、安装JSON均通过，lane1已升级；lane2仍在scheme48构建，拒绝中途覆盖，保留下一轮边界升级。完整patch已纳入Git，旧7.0.11 diff和stash保留回滚。所有测试使用NO_INSTALL_FROM_API，官方无LoongArch API。
- 新增非官方Qt6 `brew-ui-linux`，支持已安装JSON清单、精确formula安装、选中卸载/升级、全部升级、显式update、串行argv、完整输出和进程组取消；不调用autoremove。后端JSON与offscreen测试、安装后offscreen/Wayland smoke通过。修复轻量构建器多依赖未转逗号及GUI启动失败不复位问题。最终0.1.0-3 deb SHA=`f122a3768fc86a3b1e5e4a1f88a0e57ce9a4472543f6052fc40067532369f5e8`，dry-run/deploy/签名/HTTPS/隔离apt全部通过。依赖使用官方qt-6/glibc/gcc-runtime，无需第三方补包。详见[`HOMEBREW.md`](HOMEBREW.md)。

# 2026-11-10 双lane 7.0.13与GUI真实清单闭环

- 新增`upgrade-homebrew-lane.sh`：持有现有共享brew-queue.lock，使当前formula完成而下一条不能出队；/proc精确argv确认目标prefix无brew.rb后保存diff/stash、浅fetch、应用patch并跑config/b2sum/installed JSON，任何失败自动回滚，不杀进程或重启lane。首次`brew --version | head`在pipefail下SIGPIPE=141，已确认旧HEAD和4补丁完整恢复；改为全量捕获后重试成功。
- lane2现HEAD=bc63375ac3a4d6debb1ceb441ae6e22854f0909a，5文件与candidate逐字一致，b2sum及486项installed JSON通过；lane1同样7.0.13，双服务active。lane1升级后bwfmetaedit/chadwick/microsocks/miniupnpc/pstree/passt/symlinks均真实构建成功，无新增dunno错误。
- GUI新增`--check-inventory`，通过真实QProcess→brew info→JSON→表格路径后按结果退出，带60秒超时且启动失败立即非零；0.1.0-4分别对两prefix返回PASS。offscreen/Wayland smoke继续通过。deb SHA=`7c4ed634592c6abb1b4a91cf2d6e82b16593e7920efbc907aaf046484f8fa271`，dry-run/deploy及远端精确0.1.0-4可见性全过。

# 2026-11-10 选择性OS更新与present fence负结果

- apt模拟原有10项更新，仅选择spirv-llvm-translator22.1.8、wayland-protocols1.50和vim9.2.1310；单独模拟证明不牵拉Mesa。更新后安装vkgl SHA仍afac548f，dmabuf/wl_shm各vulkaninfo+vkcube60帧+vkmark五场景全过、validation=0、无llvmpipe。Mesa26.2.6仅下载审查，文件清单确认会替换vkgl直接依赖的libgbm及EGL/Vulkan层，未安装。autopilot/hourly-review/brew-update/site-publish timers均active。
- 独立候选把dmabuf QueuePresent的glFinish改为真实EGL fence wait，任何错误仍glFinish；candidate SHA=9ca09966，两路径严格烟雾0 validation。6轮AB/BA、640×480 FIFO、三场景各4秒，12进程36结果全成功。vertex/texture/shading同轮变化中位−0.61/−0.81/−1.29%，正对2/6、2/6、1/6；无可复现收益，不合并、不打deb、不换安装库。补丁、基准脚本和完整JSON公开，见[`vkgl-present-egl-fence.md`](../scripts/research/lg110/vkgl-present-egl-fence.md)。

# 2026-11-10 LG110顶点format descriptor因果闭环

- `vktest-vertex-pair`新增format A-B-A，仅改变UV R32G32_SFLOAT/R32_SFLOAT，严格-Werror构建；像素007010ff→000010ff→007010ff，position和UV word0/1/2不变，UV word3=0005f22c/00027204。原测试仍有swapchain semaphore复用validation警告，不称clean。
- 在17bc50首次64位load后观察raw meta：R32G32=0x000005f22c00000000、R32=0x000002720400000000。反汇编6组bit insert证明所有观测项word3[18:0]=raw[50:32]，bits31:19未由该段清除；末点t1/t2仅是截位结果，早期0x7/0xb/4输出已废弃。
- 新`gsgpu-mutate-vertex-format.gdb`在全R32G32 pipeline下仅写三帧UV word3低19位为R32，恰好3次且外部像素三帧000010ff，因果成立。首次0写因16MiB CPU mapping未对齐而mask偏移错误；改为首UV=2b10求base后成功，同修复stride脚本并复现3次/004040ff。旧offset误用attribute参数被usage拒绝，按原模式重跑后6/6控制全过。详见[`lg110-vendor-vertex-format.md`](../scripts/research/lg110/lg110-vendor-vertex-format.md)。

# 2026-11-10 LG110四种float vertex格式表

- format测试新增`LG110_UV_FORMAT=r32|rgb32|rgba32`，默认r32不变；rgb32/rgba32要求A-A-A同像素，r32要求A-B-A，三者严格构建和普通运行全过。最新test binary SHA=b1109ff1。
- R32G32/R32/RGB32/RGBA32 raw→word3依次为5f22c00000000→0005f22c、2720400000000→00027204、6f3ac00000000→0006f3ac、77fac00000000→00077fac。每run 11个UV条目全满足低19位映射，六组子字段以bit区间记录，不强猜语义。
- 最新二进制重跑直接word3突变仍MUTATIONS=3、三帧000010ff，log SHA=fcbb3beeb61e110db6e6f211099467b756096449b32f75f8bdce2414b5a6ac93。新增机器可读`lg110-vertex-format-summary.json`及日志哈希；仍无原生Vulkan或性能宣称。

# 2026-11-10 Homebrew Cuba/aalib失败修复与闭环

- system scope的autopilot/hourly-review/brew-update/site-publish timers及双lane均active；首次custom校验修复23处，二次0，新custom同步后再0。未重启健康lane或杀ponyc。OS模拟剩Mesa、Qt5、PipeWire/libcamera/WirePlumber等7项，高风险图形栈继续不盲从升级；v2rayA active/NRestarts=0。
- Cuba通用cflags命令行覆盖丢失`@DEFS@ -DREALSIZE -I$(common)`，导致stddecl.h缺失。custom改为保留configure生成CFLAGS并修复test局部flags作用域；4.2.2 source build、brew test、bottle回装+测试全过。瓶SHA=`f7367c07988c8b9270157be2728b5b722cd2a367008539036708187148b62905`，四别名本地/远端一致，tap远端formula SHA=`3b2f72ec8538045b5888369dbe9a5ee3cf17bf52af6fc873b87f6c938abb27d8`。
- aalib增加`-Wno-error=return-mismatch`后构建成功；首个test/pour失败实际为无TERM能力，隔离bottle实测NO_TERM_RC=1、XTERM_RC=0。custom test显式TERM=xterm且保留版本/宽高/aatest断言，第二次test=1/pour=1。瓶SHA=`b89244ed89768ec536417d02dafb8835a873fd3197ae1035b5f6627481ee8ed6`，四别名本地/远端一致，tap main=`c71bdd9f14dabd6f7c9511b6c2016654b852b84a`且远端formula含正确瓶SHA/TERM。
