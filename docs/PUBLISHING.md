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
