# Homebrew LoongArch64 maintenance

## 7.0.13 compatibility

Upstream release: <https://github.com/Homebrew/brew/releases/tag/7.0.13>.
The two prefixes are `/home/houge/infra/brew-build` and
`/home/houge/infra/brew-build2`; their lane services must not be restarted merely
because they are active. Apply a version change only when that prefix has no
running Homebrew Ruby process.

The complete local patch is
[`brew-7.0.13-loongarch64.patch`](../patches/homebrew/brew-7.0.13-loongarch64.patch).
In addition to the old install gate, architecture list, loader and Ruby platform
fixes, 7.0.13 requires `Hardware::CPU.type` and Linux `family()` to identify
LoongArch. Otherwise `SimulateSystem.current_tag` generates
`packages.dunno_linux.jws.json`, which returns 404. The fixed tag is
`loongarch64_linux`; upstream does not publish that package API, so automation
and the GUI set `HOMEBREW_NO_INSTALL_FROM_API=1` and use local taps.

Use a detached candidate worktree first and keep a shallow fetch shallow:

```sh
git fetch --depth=1 origin tag 7.0.13 --no-tags
git worktree add --detach /home/houge/infra/brew-7.0.13-candidate 7.0.13
```

A tag fetch without `--depth=1` attempted to expand the full history and stalled;
do not repeat that path. Save the old local diff and stash before a live
checkout, apply the new patch, then require `brew --version`, `brew --config`, a
local b2sum formula load, and `brew info --json=v2 --installed` to pass. Do not
run `autoremove` as an upgrade test.

The persistent safe-boundary helper is
[`upgrade-homebrew-lane.sh`](../scripts/upgrade-homebrew-lane.sh). It holds the
existing shared queue lock so no next formula can start, waits for an exact
`/proc` argv match to disappear, saves the old diff and stash, and rolls back on
any failed smoke test. It neither kills a build nor restarts a lane:

```sh
./scripts/upgrade-homebrew-lane.sh /home/houge/infra/brew-build2 7.0.13 \
  "$PWD/patches/homebrew/brew-7.0.13-loongarch64.patch"
```

Both live prefixes were upgraded by this boundary procedure. Avoid piping
`brew --version` through `head` under `set -o pipefail`: the resulting SIGPIPE
was observed as exit 141 and correctly triggered rollback before the fixed retry.

## GUI package

The official [BrewUI](https://github.com/Homebrew/BrewUI) is SwiftUI/AppKit and
macOS-only. This repository supplies an **unofficial**, independent Qt 6 Linux
frontend rather than pretending that the macOS application can run unchanged.
Source and its self-contained AOSC recipe are under
[`packages/app-admin/brew-ui-linux`](../packages/app-admin/brew-ui-linux/).

`brew-ui-linux` manages installed formula inventory, exact-name install,
selected uninstall/upgrade, confirmed upgrade-all, explicit tap update, visible
literal-argv commands, streamed output and process-group cancellation. It does
not manage casks/services/taps yet. Backend JSON and offscreen/Wayland smoke
tests are required before publishing its deb through `publish-debs.sh`.
`--check-inventory` additionally runs the real read-only QProcess/JSON/table
path and returns nonzero on failure or timeout. Revision 0.1.0-4 passed that
check independently against both 7.0.13 prefixes.

When official AOSC packages satisfy dependencies, declare and use them. Only
missing or incompatible dependencies should receive a reviewed third-party
recipe; the GUI currently uses official `qt-6`, `glibc` and `gcc-runtime`.
