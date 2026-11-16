# Six-phase remediation status — 2026-11-16

**The complete hypervisor plan remains unfinished.** The source now includes
AppleHV space/vCPU lifecycle hooks, an owned trap adapter, and a separate SVM
transition exercised inside QEMU TCG. Native setup, instruction-site/image
ownership, complete architectural semantics and hardware acceptance remain.
All five acceptance gates are false. Full/trap modes are rejected before callback
registration. The framework entry still returns failure; the experimental
transition is not connected to it.

No EFI file, boot argument or installed kext was changed. The USB supplied
read-only Lilu evidence. The older deployed AMDHV does not validate this source.
Current check results and artifact identity are in [VALIDATION.md](VALIDATION.md).

## 1. Dependencies, compatibility and provenance

Implemented:

- Pinned upstream Lilu/MacKernelSDK headers and libraries; local approximations
  are test doubles. Verification rejects dirty or wrong dependency revisions.
- Correct two-argument XNU trap callback and exported CPU/thread accessors.
- Exact installed-binary SHA-256/UUIDs and **21 XNU + 26 AppleHV** reviewed spans
  in `config/abi.json`, including descriptor preparation/destruction, interface
  ownership, VMCS initialization and current-thread stack/interrupt contracts.
  Preflight checks these spans in loaded images before accepting its stage.
- Correct AppleHV vCPU VM/space/interface offsets, VMCS descriptor +0x88 and PA
  +0x90, launch flag +0x385 and RW-interface +0xc80 GPR frame. Space +0x18 can
  contain EPTP flags; normalize and validate them instead of treating them as PA.
- Parent VM RW/RO descriptors are +0x280/+0x270. Their retained kernel buffers
  survive release of user mappings. RW interfaces have a 4K stride; RO pointers
  can be packed and are not assumed page aligned or individually 4K long.
- Native wired-buffer getters resolve the inspected kernel methods directly.
  Qualified C++ calls compiled through SDK vtables and IOByteCount widths;
  those assumptions are avoided here. A safe class cast is not a lifetime hold.
- USB Lilu 1.7.2 matches the previously observed loaded UUID and the exact
  recorded universal/slice hashes and compiled import interface.
- Strict ABI policy/Mach-O parsing, bounded thin/fat mappings, atomic generated
  headers, staged validation and atomic bundle exchange. File-only __CTF data
  is not interpreted as mapped code. Source identity includes nested tests and
  CI workflows; generated caches and these documentation files are excluded.
- Packaging checks the main SVM entry's preserving failure stub. The manifest
  separately records that the experimental transition is compiled but disabled.

Remaining: complete private MSR/control/operand contracts and diagnostic boot
proof for this artifact. `layout_verified` and `hardware_verified` stay false;
selected matching instruction spans do not establish the entire ABI.

## 2. Ownership, setup and lifecycle

Implemented:

- Bounded exclusive VMCS leases, CPU bindings, preallocated VMCBs/host-save pages,
  and physically contiguous wired deny-all I/O/MSR bitmaps. Partial setup rolls
  back. Kernel exception paths cannot allocate a missing slot.
- `SVMCPU::Driver` uses native rendezvous and guarded MSRs. It validates CPU
  vendor/SVM/NPT/NRIPS, physical width, ASIDs, BIOS disable, foreign ownership
  and host PAT policy. Original EFER/HSAVE values are recorded before any write;
  enable and restore are read back. Rollback includes a partially modified CPU.
- CPU readiness is distinct from rollback ownership: it is published only after
  successful readback and cleared before restoration, even if rendezvous fails.
  Lifecycle admission closes before draining; suspend/resume and last-user
  release preserve that closure. Tests include a 12-CPU model and failure paths.
- Priority power notifications serialize removal/callbacks, preserve power
  references and handle sleep veto/wake/cancel. Installation requires a bounded
  cancellation provider; none is installed. Removal atomically detaches the
  unowned controller before releasing its callbacks.
- `GuestMemory::Registry` retains the actual guest vm_map and tracks each kernel
  wire. Partial-range unwire handles removal, trimming and splitting; successful
  unwires are committed individually, so a later failure cannot cause a second
  unwire of an already released range. Failed unwires retain cleanup identity
  and refuse further ownership publication. Native map page shift must be 12.
- `SpaceLifecycle` connects init/map/map_kernel/fault/protect/unmap/invalidate/free
  hooks to retained source maps and NPT keys. Same-thread nested hooks share one
  revoked update ticket. Cross-thread mutation refuses admission. The last
  scope releases the ticket without depending on another gate acquisition.
- `VcpuLifecycle` registers initialization before original init_vmcs runs. The
  first VMCLEAR can claim only the newly prepared VMCS descriptor/PA stored by
  that initializer on its own thread. Arbitrary physical memory is not accepted.
- Move-only metadata leases retain RW/RO/VMCS buffers. Software VMCS slots are
  tagged atomically with owner/lifetime/space; untagged or other-lifetime state
  cannot be adopted. Initialization requires the final VMCS to be unbound.
- vCPUs hold a separate NPT source-owner ticket. It permits mapping updates but
  prevents root/source retirement while an idle vCPU still references the space.
- Retirement closes metadata admission, refuses outstanding leases/CPU bindings,
  retires software state before original free, and holds descriptors through that
  call. Cleanup then touches only retained descriptors, allowing original free
  to destroy/poison the vCPU. Address reuse receives a fresh epoch.

Remaining: a sleepable native setup/teardown coordinator must configure and retain
all providers before advertising support. A busy original free currently stops
cleanup and latches failure; a bounded native drain/deferred-free retry protocol
is still needed. Do not assume AppleHV automatically retries it. CPU hotplug and
forced-sleep cancellation are also missing. Reviewed XNU callback unregistration
panics, and callback return values cannot veto CPU shutdown; a permanent callback
owner and explicit drain protocol are required.

## 3. Isolation and invalidation

Implemented:

- Nonzero hardware-bounded ASIDs and full guest TLB flush request on every entry.
  NPT execution leases prevent mutation/reclamation. Failed synchronous flush
  callbacks quarantine roots. Owner/lifetime/EPTP keys prevent stale-root reuse.
- Source mutations synchronously revoke before unwiring or modifying original
  mappings. Retirement checks vCPU owner holds before releasing source memory.
- Native pmap admission requires ordinary preemptible thread context with
  interrupts enabled. XNU's try lock can spin on its interlock and is not used
  from arbitrary exception context. Source reads validate the retained root,
  descendant links, cycles, reserved bits, permissions and WB policy.
- Every hardware leaf requires a complete GPA/HPA/permission wire proof. Large
  source leaves are split into 4K shadows; one wired fault page cannot authorize
  a 1GiB mapping. Shadows are keyed by destination GPA prefix/level, preventing
  source-table aliases from exposing mappings validated for another GPA.
- Fault hooks revoke/unwire before the original source fault, then inspect under
  the update and wire the resulting page. Synchronization separately rewalks and
  verifies ownership before publishing a hardware leaf.
- Nested INVEPT preserves its existing update ticket. The owned trap path uses
  `acknowledgeRevoked`: every matching root must already be revoked, with no
  executing reference or cached CPU. It never dispatches a shootdown from the
  exception handler. Ordinary control-plane invalidation retains its flush API.

Remaining: implement and accept real cross-CPU cancellation/quiescence/flush
ordering. Model flush callbacks and prepared TLB-control bits do not establish a
hardware shootdown. Native source hierarchy locking still requires control-plane
context; guest NPF repair must return to that context rather than block in a trap.

## 4. Entry, traps, events and exits

Implemented:

- Bounded decoding, conservative prefixes, validated VMCS aliases and complete
  register encodings. Unsupported features and malformed MSR lists fail explicitly.
- `TrapRecovery` accepts only exact fault PCs, trap types, CPU and thread identity.
  Recursive admission cannot reset the one-fault limit. Guarded byte reads and
  aligned 1/8-byte stores restore interrupt state. Real userspace protected-page
  faults exercise the same recovery labels; kernel recovery remains unaccepted.
- Trap-hook readiness is published immediately after its route succeeds, even
  when a later route fails. Armed probe recovery precedes ordinary forwarding.
- `TrapAdapter` retains a code-image ticket and current vCPU metadata, including
  register-only instructions. It bounds instruction reads to 15 bytes and admits
  operands only from the current thread stack, retained buffers or pinned
  read-only image regions. Physical reads expose only the owned VMCS revision.
- Native stack ownership uses the current thread's exported stack accessor and
  kernel stack size, not an invented window around saved RSP. IF restoration
  occurs after all trap resources are released; the native setter may run an AST.
- Native VMX emulation uses verified SVM CPU ownership instead of inventing a
  VMXON physical page. That path refuses VMXON/VMXOFF and missing owned slots.
  The standalone offline VMXON model remains separately available to tests.
- Conservative supported controls, typed entry/exit results and MSR policy.
  Pending injection survives failed entry; guest progress/vectoring is saved
  before NPF retry. Unknown or unreliable exits fail without fabricated metadata.
- Physical INTR/NMI/SMI/INIT interception is mandatory. Events not requested by
  VMX pin controls return `HostEvent`, without inventing a guest VM exit.
- `SVMTransition.S` is a separate real primitive using a private 256-vector IDT
  with IST zero during the guest GS/TR window. It saves/restores host VMLOAD
  state, IDTR, CR2/DR6/DR7, IF, GPRs and callee registers. It uses no FP/XSTATE
  instructions; the inspected outer AppleHV FP/debug ownership must be preserved.
- Exact-site CLGI/VMLOAD/VMSAVE/VMRUN faults restore host state before returning
  failure. Unexpected events return fatal status after restoration. Failure
  while restoring the trusted host auxiliary VMCB halts that CPU; this remains
  an explicit unrecoverable boundary and prevents hardware acceptance.
- Host IF is set immediately before VMRUN while GIF is clear, then cleared on
  return. The TCG harness verifies that a host timer exits an IF=0 looping guest
  even when the caller's IF was initially zero, and restores the caller's IF.

Remaining software dependencies, in order:

1. Implement the actual exact-instruction-site policy and kernel/AppleHV image
   pin/unload provider required by `TrapAdapter`. No native code provider is
   configured. Copying a known-looking address is insufficient ownership.
2. Complete native operand-write contracts. Some inspected AppleHV VMREAD stack
   destinations are unaligned; FaultMemory intentionally rejects unaligned
   8-byte stores. Do not allow possibly partial cross-page stores or pretend
   those call sites are supported. Per-vCPU RO write spans also need review.
3. Configure the native ownership/setup/drain/flush providers described above,
   including wired per-CPU transition contexts and separate host auxiliary
   VMCBs. Hook readiness now requires the trap and both lifetime registries.
4. Connect the framework entry with retained vCPU/MSR snapshots, host run lease,
   CPU binding and NPT execution lease. Bridge guest DR6 and original FP/debug
   handling, preserve pending events on failed entry, and handle host events in
   a bounded cancellation loop. The framework entry still returns failure.
5. Complete guest-state validation, required AppleHV controls, CR/DR decode
   assistance, interrupt/NMI blocking/windows, cancellation and timer semantics.
   The advertised subset must remain honest while these are incomplete.
6. Accept real transition recovery, asynchronous events, SMI/NMI behavior and
   microarchitectural mitigation/state ownership on the target hardware. TCG
   success does not remove these requirements.

`EntryResult::Ready` means translation preparation only. `vmrunReturned` means
VMRUN returned, including failed entry; it is not proof that the guest ran.
The main `SVMEntry.S` remains a preserving failure stub. The accepted kernel
wrapper refuses the experimental primitive while any acceptance gate is false.

## 5. Verification and build workflow

Five C++ suites cover translation, startup, decoder bounds, allocation failures,
12-worker contention, lifecycle/epoch reuse, nested source hooks, partial unwire,
owned trap emulation, real fault probes and CPU rollback. Native hooks are tested
with actual hook implementations and retained-memory mocks. Python tests cover
malformed binaries/profiles, provenance, publication, cleanup and emulation tools.
ASan/UBSan, TSan and NDEBUG builds pass locally; hosted CI is not claimed.

The freestanding TCG harness executes the production transition assembly inside
an emulated CPU. It covers HLT, failed entry, exact-site failures, NPF, host-timer
cancellation and 1000 restores with hostile guest GS/TR and separate host state.
See [EMULATION.md](EMULATION.md) for reproduction and emulator limitations.

Production objects have independent Make targets, so `make -j12 check kext` can
use this machine's 12 logical CPUs. Unchanged builds skip compilation. Cleanup
preserves `build/checkpoints`, `build/research` and `build/emulation`. Kernel
warning groups downgraded by Apple Clang are explicitly restored to errors; a
negative compiler test checks enforcement. Frames above 4KiB fail compilation.

Remaining: sustained fuzzing, hosted CI, kernel-context failure injection and all
hardware acceptance below. A userspace deadline cannot repair a transition that
leaves the physical CPU in a broken interrupt/GIF state.

## 6. Hardware acceptance

Keep every result with source/artifact/kernel/AppleHV/Lilu identities, boot UUID,
requested mode, full output and failure detail. Complete software prerequisites
before advancing to the corresponding hardware stage:

1. Boot the new diagnostic bootstrap/preflight artifact with a retained recovery
   EFI, verifying both callback stages and all runtime instruction evidence.
2. Accept trap forwarding and guarded kernel probes before native VMX emulation.
3. Accept reversible per-CPU setup/restore, partial failures, online/offline,
   shared/exclusive ownership and sleep/wake with no guest running.
4. Accept stopped vCPU/root ownership, mapping churn, source reclamation, ASID
   reuse and synchronous cross-CPU quiescence/flushes.
5. Inject transition failures and verify restoration, then run a bounded single
   vCPU with verified cancellation. Extend to multiple VMs/CPUs, teardown,
   mapping churn, supported guest modes and sleep/wake.
6. Lift each gate only after its individual evidence exists. Redistribution still
   requires the owner's license choice; no project license is invented.

No new diagnostic boot or physical SVM guest success is claimed. EFI/source
backups are outside diagnostic JSON pruning. Local checkpoints preserve this
work without deploying it.

## Primary evidence

- Exact installed kernel/AppleHV identities and spans: `config/abi.json`.
- Pinned XNU source reference: `f6217f891ac0bb64f3d375211650a4c1ff8ca1ea`,
  especially `osfmk/vm/vm_map.h`, `osfmk/kern/machine.c`,
  `osfmk/i386/pmap.h` and current-thread stack/interrupt routines. Source alone
  does not establish the installed image layout; its disassembly supplied that.
- QEMU source/build identity and limitations: [EMULATION.md](EMULATION.md).
