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

## 解决

RT 内核下统一使用 **nft 后端**（`nf_tables` 我们已启用：`CONFIG_NF_TABLES=m`）：

```sh
for t in iptables iptables-save iptables-restore ip6tables ip6tables-save ip6tables-restore; do
    ln -sf xtables-nft-multi /usr/sbin/$t
done
```

本仓库的安装脚本把它固化为 systemd 服务 `iptables-nft-default.service`
（在 `network-pre.target` / `v2raya.service` 之前执行），内核升级后依然生效。

验证：

```sh
iptables --version            # iptables v1.8.11 (nf_tables)
iptables -t mangle -N TP_OUT  # 成功
iptables -t mangle -A TP_OUT -p tcp -j TPROXY --on-port 52345 --tproxy-mark 0x1/0x1
ip rule add fwmark 0x1 lookup 100
ip route add local 0.0.0.0/0 dev lo table 100
```

以上命令在 RT 内核 + nft 后端下全部成功，v2raya 的 tproxy 因此可用。

## 备注

- 不要为了 legacy iptables 去改 `!PREEMPT_RT` 依赖：那会打开上游认为不安全的组合；
  用 nft 后端是等价且受支持的方案。
- `iptables-nft` 与 `iptables-legacy` 的规则集互不相通，切换后请重启 v2raya 让它重建规则。
