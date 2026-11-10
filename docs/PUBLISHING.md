# 发布到第三方源

仓库采用 Debian `dists` 布局：

```
pool/main/<首字母>/<pkg>/<pkg>_<ver>-<rel>_loongarch64.deb
dists/stable/main/binary-loongarch64/Packages(.gz)
dists/stable/Release
dists/stable/InRelease        # gpg 签名
```

## 流程

```bash
./scripts/publish-debs.sh --dry-run build/*.deb   # 演练
./scripts/publish-debs.sh --deploy build/*.deb    # 发布
```

发布器内部顺序（不可打乱）：

1. **pool 收编**：按包名归档、原子落盘（先写临时文件再 rename）。
2. **索引重建**：`Packages` + `Packages.gz`。
3. **签名**：用既有仓库身份生成 `Release`、`InRelease`、`Release.gpg`。
4. **部署**：`pool → Packages → Release/InRelease` 顺序 rsync，尽量让客户端抓到一致索引。
5. **验证**：
   - 本地索引内部一致性（MD5）；
   - 本地与服务器索引 md5 一致；
   - HTTPS 可达且包数一致；
   - 隔离 apt（临时 sources.list）能 `apt-get update`。

## 密钥

仓库签名密钥指纹记录在 `repo/key.gpg` 对应的私钥环境中（`~/.gnupg` 或 `INFRA/gnupg`）。
**私钥不入库**，仓库只发布公钥与 `sources.list` 片段：

```bash
gpg --armor --export <fingerprint> > repo/key.gpg
```

## 回滚

- 删除 `pool/` 中对应 deb 后重新 `--index-only --deploy` 即可从索引中移除；
- 如需彻底撤回，再同步删除服务器上的 pool 文件（发布器不会自动删除远端文件）。

## 站点上的公钥

发布器会把公钥同步到站点根：

```
https://homebrewloongarch64.site/aosc/key.gpg     # ASCII armored，给 gpg --dearmor 用
https://homebrewloongarch64.site/aosc/thirdparty.gpg  # 二进制 keyring
```

`repo/key.gpg` 是同一份公钥的仓库副本。**改动签名密钥后必须同时更新这三处**，
否则用户按 README 安装时会得到 `NO_PUBKEY`。`srv/aosc/key.gpg` 曾经是 0 字节，
就是这类漂移的实例（2026-11-04 修复）。

## README 全量 deb 清单（强制对账）

每次新增、升级或撤回 deb 后，必须让根 `README.md` 与实际 Packages 索引重新同步；
不能只写最新版，也不能只保留手工挑选的八个包。生成器会列出索引中的每个
Package/Version/Architecture/Filename 条目及 SHA-256，包括同一包的多个已发布版本：

```bash
PKGIDX=/home/houge/infra/srv/aosc/dists/stable/main/binary-loongarch64/Packages
POOL=/home/houge/infra/srv/aosc
python3 -B scripts/sync-readme-package-index.py "$PKGIDX" README.md --pool-root "$POOL"
python3 -B scripts/sync-readme-package-index.py "$PKGIDX" README.md --pool-root "$POOL" --check
```

`--pool-root` 会逐个读取并核对全部本地 deb 的 SHA-256；`--check` 在 README 缺项、
版本/哈希漂移时非零退出。标记之间的生成表格不要手工编辑。发布 deb 与提交 README
是两个独立动作，推送前还必须检查远端 Packages 字节一致及所有生成下载链接可用。
