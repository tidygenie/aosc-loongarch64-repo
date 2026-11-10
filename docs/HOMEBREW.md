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

## Formula-generation safeguards (2026-11-10)

The triplet generator now handles `system "../dist/configure", *args`, variable
`configure` calls at the start of an install body, every quoted `./configure`
call in multi-library formulas, and `dist/config.sub`/`dist/config.guess`.
Repeat injection is idempotent; real Berkeley DB and dvanalyzer formulas plus
legacy configure forms were used as assertions.

A formula registered in `needs-build-triplet.txt` now enters local-tap
finalization before its **first** build. Finalization or live-tap injection
failure records a deduplicated failure and skips the package instead of silently
building the unmodified core formula. Active-lane requeue/claim guards remain in
force; healthy lanes are not restarted for these changes.

Two former failures completed build, upstream test, bottle pour, and poured-test
verification:

- `dvanalyzer 1.4.2`, bottle SHA-256
  `8efaa63a4554b2ce1bc4c7e6277a6e6eb7458e34b7a37a767f175e4371eb1c8e`;
  all three configure calls received the LoongArch triplet.
- `berkeley-db 18.1.40_4`, bottle SHA-256
  `2f62e153a11f1a54b569cb16645e78c656ad9fa15dd50f618167fb4f08fc849e`.
  Its protected custom formula uses `ENV.append_to_cflags` rather than
  command-line CFLAGS, preserving Berkeley's required `-I. -I../src` and
  `-D_REENTRANT` while adding GCC 16 compatibility flags.

All four filename aliases for each bottle were verified byte-identical locally,
on the server, and over HTTPS. The tap `main` branch was pushed through commit
`7c3e73be2a733f133698a8c7a5eaa4dda96961b4`. This reduces current failures but
does not claim that the remaining queue or all formulas are fixed.
