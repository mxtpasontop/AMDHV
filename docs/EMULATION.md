# Software SVM transition verification

`make emulate-transition` executes the production `src/SVMTransition.S` inside a
QEMU TCG software CPU. It does not use Hypervisor.framework, HVF/KVM, a loaded
AMDHV backend, or physical host SVM instructions. It never installs a kext.

The harness in `tests/emulation/svm_transition.S` boots a small freestanding
multiboot image, establishes long-mode host state and private tables, and runs a
protected-mode guest. Its host FS/GS/KERNEL_GS, TSS, IDT/IST, debug registers,
CR2 and flags differ from guest state. Guest FS/GS/TR bases deliberately point
at unmapped addresses to exercise restoration without using a guest TSS stack.

Covered cases:

- Guest register updates followed by HLT, host/callee register restoration,
  descriptor/MSR/debug/CR2/IF restoration and 1000 repeated transitions.
- CLGI with SVME disabled and VMLOAD/VMRUN with bad physical operands, including
  a VMRUN fault after guest VMLOAD changed GS/TR state.
- Invalid ASID entry failure and NPF from a revoked instruction mapping.
- PIT/PIC host timer cancellation of an infinite guest with IF=0. The caller's
  IF is also zero on entry; its original state is restored before return, and
  the pending interrupt is handled on the original host IDT/IST after enabling IF.

The raw primitive is separate from `amd_svm_vcpu_run`, which still returns -1.
The C++ kernel wrapper refuses execution while any acceptance gate is false.
Unrecoverable failure restoring the trusted host auxiliary VMCB halts the CPU.
The harness is not evidence that this boundary, asynchronous physical NMI/SMI,
extended-state handling or microarchitectural mitigations are accepted on hardware.

## Run

On this workspace, the local binary is already built:

```sh
make emulate-transition
# Or provide another TCG-capable QEMU build:
python3 tools/emulate_transition.py --qemu /path/to/qemu-system-x86_64
```

The runner uses `-machine pc,accel=tcg -cpu phenom,-fxsr-opt`, never HVF/KVM. It
requires an exit status of 1 from the debug-exit device and a PASS marker. Process
output is bounded and execution times out after 20 seconds. Immutable private JSON
records in `build/emulation/results/` contain source identity, command, image and
emulator hashes/version, output, result and limitations. The source identity is
rechecked after execution. The ELF linker rejects unresolved, overlapping,
unsupported or overflowing relocations.

QEMU 9.2 zero-extends the failed-entry exit code. The harness checks the low
32 bits for that case and records the discrepancy. Production AMD validation
continues to require the complete 64-bit invalid-entry value. An emulator defect
must not weaken the hardware contract.

## Local build provenance

- QEMU tag: `v9.2.4`.
- Commit: `b2995afed254e4804611df52210067a4d5dbdf0b` from the upstream repository.
- Source: `build/emulation/qemu`; output: `build/emulation/qemu-build`.
- Built binary SHA-256:
  `628d28ed82f6be183c782ec348ec0d7c7814d2092c7c35180ee05882b679ec77`.
- Local Python environment: `build/emulation/env`, Python 3.15.2,
  Meson 1.5.2, Ninja 1.11.1.4 and distlib 0.3.9.
- MacPorts dependencies used: GLib 2.88.3 and pixman 0.46.4.

From `build/emulation/qemu-build`, configure with the task's Python/Ninja paths:

```sh
../qemu/configure --target-list=x86_64-softmmu --without-default-features \
  --enable-tcg --enable-system --disable-user --disable-hvf --disable-docs \
  --disable-tools --disable-guest-agent --disable-cocoa --disable-sdl \
  --disable-gtk --disable-vnc --disable-curses --disable-capstone \
  --disable-slirp --python=../env/bin/python --ninja=../env/bin/ninja
../env/bin/ninja -j12 qemu-system-x86_64
```

The existing build used four build jobs; future invocations can use all 12 logical
CPUs. A separate Ubuntu CI job runs the same harness with its distribution QEMU;
its hosted result has not been observed. Each run records its actual emulator
identity rather than claiming the local hash applies to other builds.

`make clean` preserves this emulator, research and source checkpoints. All are
ignored build/evidence files, not vendored production dependencies.
