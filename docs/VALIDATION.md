# Source validation — 2026-11-16

This records local source/model checks and a diagnostic bundle. The six-phase
hypervisor plan remains unfinished. The experimental assembly ran inside QEMU
TCG; no kernel probes, CPU/MSR setup or physical SVM guest ran on the host.

| Check | Result |
|---|---|
| `make -j12 check kext` | Five C++ suites, 31 Python tests, shell/plists and packaging passed |
| Main translation suite | 65 passed, zero failures |
| Decoder bounds | 65,536 two-byte inputs plus 100,000 deterministic longer inputs |
| `make -j12 sanitize thread-sanitize` | All five ASan/UBSan suites and TSan runtime suite passed |
| `make -j12 test BUILD_DIR=build/ndebug TEST_EXTRA_FLAGS=-DNDEBUG` | All five suites passed |
| Native ownership models | Actual space/vCPU hooks, initialization/teardown, descriptor retention, epoch reuse and blocked retirement passed |
| Owned trap model | Bounded fetch, code ticket release, current-vCPU leases, stack/buffer operands, reentrancy and revoked-only INVEPT passed |
| CPU/concurrency coverage | 12-CPU lifecycle model and 12-worker contention; backend CPU bound 128 |
| Kernel diagnostics | No emitted warnings; discovered Apple Clang warning exceptions restored to errors; negative compiler test passed |
| Kernel stack frames | Frames above 4KiB rejected during compilation |
| `make emulate-transition` | HLT, failed entry, exact-site failures, NPF, IF=0 host-timer cancellation and 1000 restores passed in TCG |
| `tools/verify_abi.py` | Exact image hashes/UUIDs and 21 kernel + 26 AppleHV selected spans matched |
| `tools/check_symbols.py --lilu PATH` | 332 kernel + 17 AppleHV required exports; exact USB Lilu UUID/hashes/interface matched |
| Guarded probes | Real userspace protected-page faults recovered through assembly labels |
| Smoke executable | Build/signature verified only; executable was not run |
| Incremental kext build | Unchanged build verified; no compilation/linking needed |
| Hosted CI | Workflow supplied; hosted result not observed |
| EFI/config deployment | No writes |
| New diagnostic boot/native kernel/hardware acceptance | Not performed |

## Diagnostic artifact

- Bundle: `build/AMDHV.kext`.
- Source identity: `242ec6ac3843ae746066377f64decbedcd234d990b1268ef76eb489518e26889`.
- Executable UUID: `2D2F294E-ECB5-3AB1-BBEE-5C12165A6193`.
- Executable SHA-256: `73897b50138b216448105d333cb06e0f68c819f637be57689869c183bdc7f97d`.
- Acceptance/execution enabled: **false**.
- Source/evidence checkpoint: `build/checkpoints/source-and-diagnostic-242ec6ac3843-20261116-143419.tar.gz`.
- Structured validation record: `build/research/validation-242ec6ac3843-20261116-143419.json`.
- Immutable emulation record: `build/emulation/results/transition-hne5y6ks.json`.

These checkpoint/record paths refer to local, ignored files; the archives and
raw diagnostic logs are not published in this repository.

The checkpoint includes the source, documentation, diagnostic bundle, manifest,
validation logs and TCG evidence. It excludes vendor/emulator build trees and EFI.
Pinned dependencies can be fetched using `dependencies.lock.json`. Checkpoints,
research and the local emulator survive `make clean`.

The manifest records imports, toolchain/dependency identities and reviewed ABI
spans. Packaging validates the private staged executable and the actual preserving
failure stub at `amd_svm_vcpu_run`. A separate experimental transition is compiled;
its C++ kernel wrapper remains blocked. Source identity includes nested tests and
CI configuration. Documentation is excluded to avoid a self-referential identity.

## Deployed Lilu evidence

- Read from the local boot USB's `EFI/OC/Kexts/Lilu.kext/Contents/MacOS/Lilu`.
- Version 1.7.2; x86_64 UUID `C2A303B8-6D22-3163-A905-5DB8E340D285`.
- The UUID matched the loaded Lilu previously observed in this continuation.
- Universal SHA-256: `ff98508b40a1eb5fb477029db8886412520f8e39965279ea331c99e3d2e3b274`.
- x86_64 slice SHA-256: `f10e52ce96d2e7902b841f00a30a05517445eb86e4991b20655cc7b58415f25d`.

These checks establish the selected compiled interface for the recorded binary;
they do not prove callback timing, all private layouts or another loaded revision.
The older installed AMDHV is not boot validation of this artifact.

## Remaining work

Native code-image/instruction-site ownership and unaligned operand-write handling
are incomplete. A native setup/drain/retry/hotplug/flush coordinator and framework
entry/MSR connection are also missing, followed by complete architectural semantics
and physical acceptance. These are software dependencies as well as hardware tests;
this is not a claim that only a reboot remains. See [IMPLEMENTATION.md](IMPLEMENTATION.md)
for the dependency order and [EMULATION.md](EMULATION.md) for the software harness's
limits, including the QEMU failed-entry high-word discrepancy and unrecoverable
host-auxiliary restoration boundary. No acceptance gate was lifted.
