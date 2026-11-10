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
literal-argv commands, streamed output and process-group cancellation. It also
reads real `brew services list --json` data, starts/stops/restarts a selected
service after confirmation, lists taps, adds an exact `user/repository` tap and
untaps a selected entry after confirmation. macOS casks are not emulated on
Linux. Backend JSON and offscreen/Wayland smoke tests are required before
publishing its deb through `publish-debs.sh`.
`--check-inventory` runs the real read-only QProcess/JSON/table path;
`--check-management` additionally requires the real service and tap tables to
parse, returning nonzero on any failure or timeout. Revision 0.1.0-5 passed both
checks independently against the two 7.0.13 prefixes; malformed services JSON
was independently verified to fail closed without attempting a mutation. Its
published deb SHA-256 is
`f31a56110f420390f32cb2f795b8984109f41187382cbd4cc7e374184c30f481`.

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

## libcap and pgformatter source-build repairs (2026-11-10)

Two additional protected custom formulas completed build, upstream test, bottle
pour and poured-test verification:

- `libcap 2.78`: the initial `good-names.go` diff error was secondary; the source
  build also compiles several Go cap/psx programs. The formula prepends the
  installed native Go toolchain only at build time, sets `GOTOOLCHAIN=local` and
  `GOPROXY=off`, and keeps libcap's vendored modules. The name differential and
  real Go application builds passed. Bottle SHA-256:
  `7be1098bf7232709b53af2f463ed6e4781b97bcc781fb9b863928704f2ec826a`.
  `capsh --print` was also run from the independently extracted final bottle.
- `pgformatter 5.11`: the local Perl installs to `usr/{bin,share}`, while the
  core formula unconditionally renamed `usr/local/man`. The custom formula
  installs the observed man directory and `usr/bin/site_perl/pg_format` directly;
  it does not replace the system Perl. Bottle SHA-256:
  `b280ef8e18f00e14898812c3659a1b0d035c727baf0281dbea47c43ded7c3a1a`.
  The final bottle's `pg_format` correctly formatted `SELECT * FROM foo`.

Both formula SHA values match the repacked archives. All four aliases of each
bottle were verified byte-identical locally and through HTTPS. Tap `main` and the
remote bare repository are synchronized at
`0282da1e20b4c91f94340fd60d41317ab868a950`. These targeted fixes do not imply
that every remaining failed formula is portable or buildable.

## liburing native configure and RT-aware testing (2026-11-10)

`liburing 2.15` explicitly uses a non-autotools configure. Generic
`--build=loongarch64-unknown-linux-gnu` injection was therefore rejected as an
unknown option. The formula is now custom-protected in its upstream form, removed
from `needs-build-triplet.txt`, and finalized without that option.

The first source build then exposed a test-environment assumption: core expected
`io_uring_queue_init` to fail with EPERM, but the RT kernel returned success. The
test now accepts either a real ring creation followed by `io_uring_queue_exit`,
or explicit EPERM in a restricted environment; every other error still fails.
This is not a disabled or ignored test. Final build/test/bottle/pour all passed,
and an independent GNU-C program using the final bottle printed
`queue_init: success`. Bottle SHA-256:
`28163d3823503d95985c0f18973524d9b3cc4bf49b2b6589ab622b756a50bf0d`.
All four aliases match locally and over HTTPS; tap `main` contains the final
formula at `af2385d74cb5ef98c4da78a14b4c60dc4dadf9b0`.

The lane's incomplete-result branch now calls `record_failure`: only
`test=1,pour=1` may clear a failure. A subsequent unrelated `chadwick` 0/0 result
was retained in the deduplicated failure list, providing a live regression check
that incomplete retries cannot silently improve the reported failure rate.

## libaio official-source relocation (2026-11-10)

The local core formula still referenced the removed Pagure archive and failed with
404. Current [Homebrew core](https://github.com/Homebrew/homebrew-core/blob/master/Formula/lib/libaio.rb)
uses the upstream Codeberg release for the same `libaio 0.3.113`; the two archive
layouts have different SHA-256 values and are not interchangeable. The protected
formula now matches current core, and the Codeberg archive independently matched
`2c44d1c5fd0d43752287c9ae1eb9c023f04ef848ea8d4aafa46e9aedb678200b`.

Build, upstream test, bottle pour and poured test all passed. An additional strict
C program linked against the final bottle created an eight-event Linux AIO context
and destroyed it, printing `libaio io_setup/io_destroy: success` on the RT kernel
(`CONFIG_AIO=y`). This is stronger than the formula's declaration-only smoke test.
Bottle SHA-256:
`a4a9285d3a5d7ae2bd496d2804ec6b47959afab9a9f2038e9a17aa30676e4baf`.
All four aliases were byte-identical locally and over HTTPS. Tap `main` and the
bare repository contain the final formula at
`f5f45c97d39fcb064fd1535ed900aaf77932a1bf`.
