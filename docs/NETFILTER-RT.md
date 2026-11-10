# PREEMPT_RT 内核下的 iptables / 透明代理（v2raya、tproxy）

## 现象

在 RT 内核（`7.3.1-mainline-16k-rt-*`）上启动 v2raya 的透明代理会失败：

```
writeTransparentProxyRules: could not set up transparent proxy in tproxy mode:
iptables v1.8.11 (legacy): can't initialize iptables table `mangle':
Table does not exist (do you need to insmod?)
failed to start v2ray-core: could not set up transparent proxy in tproxy mode
```

## 根因

上游 Kconfig 明确禁止 legacy xtables 与 PREEMPT_RT 共存：

```
# net/netfilter/Kconfig
config NETFILTER_XTABLES_LEGACY
	bool "Netfilter legacy tables support"
	depends on !PREEMPT_RT
```

因此 RT 内核里 `IP_NF_IPTABLES_LEGACY / IP_NF_MANGLE / IP_NF_FILTER / IP_NF_NAT`
等符号**根本不会出现**（`make olddefconfig` 会直接丢掉），
`iptable_mangle.ko` 不存在，`iptables`（legacy 后端）自然报 “Table does not exist”。

这不是我们内核配置漏项，而是上游的硬性限制；AOSC 的 7.1.13 内核没有该限制，
是因为它早于这条 Kconfig 变更。

## 解决（v2raya 2.5.14-1）

内核已启用原生 nftables（`CONFIG_NF_TABLES=m` 和 TPROXY 相关模块）。正确修复分两层：

1. **本仓库 deb 包**：`v2raya.service.d/10-nftables.conf` 给服务设置
   `Environment=V2RAYA_NFTABLES_SUPPORT=on`，并让 v2raya 依赖 `nftables`；
2. **上游后端自动检测**：仅在 `iptables --version` 明确显示 `(legacy)` 时，
   以只读的 `iptables -t mangle -L` 探测；只有输出明确符合“mangle 表初始化失败、Table does not exist”
   才切换到 nft。权限错误、其他失败和可工作的 legacy 都保持原行为。上游2.5.14已包含此检测和
   表驱动单元测试；本机`go test ./kernel/iptables -count=1`全部通过。历史审核分支为
   [HougeLangley/v2rayA 的 `v2raya-nft-auto`](https://github.com/HougeLangley/v2rayA/tree/v2raya-nft-auto)。
   上游 issue/PR 仍只由人类审核并亲自提交。

安装/恢复：

```sh
sudo apt update
sudo apt install v2raya       # 2.5.14-1 或更新
sudo systemctl daemon-reload
sudo systemctl restart v2raya
```

已在**系统链接仍指向 legacy、全局 `iptables-nft-default.service` 已停用**的条件下验证：

```sh
iptables --version            # iptables v1.8.11 (legacy)
systemctl is-active v2raya    # active
pgrep -x v2raya_core          # 恰好一个 core
sudo nft list tables          # table inet v2raya / table inet v2raya_dns
curl -x socks5h://127.0.0.1:20170 https://api.ipify.org
```

以上均通过；因此不再需要为了 v2raya 修改整个系统的 iptables 默认后端。
其他仍强制使用 legacy 的程序可以单独迁移到 nft，或自行选择全局 `xtables-nft-multi`，
但这不是 v2raya 包正常工作的前提。

2.5.14-1从上游tag `v2.5.14`（commit `92caf05bc882b2c40d1c93ee3b2a14135d4cabcc`）
干净构建，保留CGO/SQLite、KillMode=mixed及显式nftables dropin。安装重启后服务active、
GUI HTTP 200，core与新透明代理连接均正常。deb SHA-256为
`5eaf29d7ffac7cfd0d0acee04576029bc19541cae309f32007783311021fbaae`；
38包签名索引、服务器/HTTPS字节和隔离apt验证通过。启动时systemd-resolve占用53导致的
localhost DNS回退有明确日志，不构成透明代理规则失败。

## 备注

- 不要为了 legacy iptables 去改 `!PREEMPT_RT` 依赖：那会打开上游认为不安全的组合；
  用 nft 后端是等价且受支持的方案。
- `iptables-nft` 与 `iptables-legacy` 的规则集互不相通，切换后请重启 v2raya 让它重建规则。
