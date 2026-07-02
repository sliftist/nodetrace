# Build notes (nodetrace fork)

Empirical findings from building this fork on a RunPod instance, 2026-05-25.
Captured so a fresh clone / fresh pod doesn't have to re-discover the same
numbers. Node.js's own [`BUILDING.md`](BUILDING.md) remains the canonical
upstream guide; this file is just the local cheat-sheet.

## Hardware reference (the box these numbers came from)

- AMD EPYC 7663, 56 physical cores / 112 SMT threads, ~3.5 GHz max boost
- 251 GB host RAM, **77.3 GB cgroup limit**, 0 swap (hard kill at the cap)
- NVMe-backed `/workspace`, 200 GB free

The cgroup cap is the most important constraint: this isn't "use all 256 GB",
it's "stay under 77 GB or processes are SIGKILL'd". Per-cc1plus peak RSS on
V8's heaviest files is **1.5-2 GB**, so naive `-j$(nproc)` (= -j112) will OOM
during V8 compilation. The first attempt did exactly that.

## One-time setup

```bash
apt-get install -y git make ninja-build time
./configure --ninja        # ~2s; emits build.ninja under out/Release
```

`./configure` will warn about missing `cargo`/`rustc` — that only disables
Temporal support and is safe to ignore for this fork.

## Build it

```bash
ninja -C out/Release -j32
```

**Use `-j32`. Not `-j$(nproc)`, not `-j64`.** See parallelism findings below.

## Parallelism findings — worst-case incremental rebuild

Methodology: full clean build first, then `touch deps/v8/src/runtime/runtime.h`
(triggers ~1090 of ~4500 ninja steps — a transitive cascade across most of V8)
and time the rebuild.

| `-j` | Wall time | User CPU | Peak mem | vs. best |
|-----:|----------:|---------:|---------:|---------:|
|   16 |     17:28 |  253 min |   ~37 GB |    +79%  |
| **32** | **9:46** | **269 min** | **~37 GB** | **best** |
|   48 |     10:47 |  299 min |   ~53 GB |    +10%  |
|   64 |     13:17 |  372 min |   ~66 GB |    +36%  |

Why `-j32` wins on a 56-core / 112-thread box:

- At -j32, every job sits on its own physical core. No SMT pairing.
- With only ~32 of 56 cores busy, the EPYC sustains a higher all-core boost
  frequency. (`lscpu` reported CPUs throttled to ~65% of max under heavy load.)
- At -j48 you start using more cores → frequency drops → small wall regression.
- At -j64 you cross into SMT pairing on every job. V8's template-heavy
  translation units are particularly hostile to SMT (decoder/L1 contention),
  and per-job CPU time inflates ~40%.
- Memory was never the binding constraint at -j32 or -j48.

If you're tempted to go higher because "we have 112 threads": don't. The user
CPU column shows you'd be burning more total work for a worse wall time.

## Build-system choice

- **Use ninja.** Reconfigure with `./configure --ninja`.
- Full clean build: `make` and `ninja` are roughly tied (~16-21 min at -j16).
  Ninja was actually slightly slower than make on the full build in our tests
  — surprising, possibly a GYP→ninja generator quirk.
- **Null/no-op rebuild: ninja 0.14s vs. make ~30-60s.** This is the headline
  win, and it matters every time you save a file.
- Single-`.cc`-file edit: rebuild + relink in roughly **30-90s**, dominated
  by the final `LINK node` step.

## Memory budget gotchas

- The cgroup cap on this pod is 77 GB. Run-of-the-mill V8 cc1plus invocations
  peak at 1.5-2 GB. Multiply by `-j` to estimate worst-case concurrent draw.
- Watch real-time with: `watch -n 5 'awk "{printf \"%.1f GB\\n\", \$1/1073741824}" /sys/fs/cgroup/memory.current'`
- If you see `g++: fatal error: Killed signal terminated program cc1plus`,
  that's the kernel OOM killer. Drop `-j`.

## Things that would help further (not yet tried)

- **ccache**: would speed up cold rebuilds when bouncing branches; doesn't
  help worst-case header touches because every file genuinely differs.
- **More physical RAM in the pod config**: would let us push past -j32, but
  only marginally — frequency-scaling regression kicks in well before the
  memory limit.
- **Splitting V8's runtime.h** or other widely-included headers: the real
  fix for "worst-case incremental" pain, but invasive.

## Windows build

Cross-compiling Node.js from Linux to Windows is not supported upstream.
Attempted paths and the reason each failed on this box:

- **clang-cl + xwin sysroot** (`xwin splat` → `/root/winsdk/root`): the
  compiler and Windows SDK headers/libs are in place, but Node's
  `configure.py --dest-os=win` calls into `gyp/msvs_emulation.py` which
  probes for an actual Visual Studio install
  (`Could not locate Visual Studio installation`). Would require
  invasive GYP patches to fake the VS registry/env.
- **msvc-wine** (`/root/msvc-wine/msvc/`): full MSVC + MSBuild running
  under Wine; the toolchain executes correctly, but the Linux
  `configure.py` still can't detect it, and running `vcbuild.bat` under
  Wine fails because it re-invokes `python configure.py` inside the
  Wine env, which needs Windows Python and a native VS.

The practical route is a **windows-2022 GitHub Actions runner** that
executes the canonical `vcbuild.bat`. See
`.github/workflows/windows-build.yml`. On every push to `main`, or via
`workflow_dispatch`, the workflow builds `node.exe` and publishes it as
a GitHub Release artifact tagged `windows-build-<sha>`.

To download the latest Windows build:
1. Go to https://github.com/sliftist/nodetrace/releases (or
   https://github.com/sliftist/nodetrace/actions for older artifacts).
2. Download `node-windows-x64` → `node.exe`.
