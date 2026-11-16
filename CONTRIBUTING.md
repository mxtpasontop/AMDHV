# Contributing to AMDHV

AMDHV is an unfinished research project. Keep the distinction between source
models, software-emulated execution and physical kernel/hardware acceptance clear
in code, documentation and reports.

## Development checks

On an x86_64 Mac with Xcode command-line tools and Python 3.10+:

```sh
python3 tools/dependencies.py --fetch
make -j12 check kext
make -j12 sanitize thread-sanitize
make -j12 test BUILD_DIR=build/ndebug TEST_EXTRA_FLAGS=-DNDEBUG
```

Adjust the number of jobs for your machine. For transition assembly changes, also
run the TCG harness described in [docs/EMULATION.md](docs/EMULATION.md). Building a
smoke-test executable does not count as executing or accepting a hardware guest.

## Change expectations

- Explain the failure being fixed and how ownership/state flows across modules.
- Preserve bounded resources, explicit error propagation and nonblocking trap
  admission. Do not replace a retained lifetime with an unchecked address.
- Keep dependency pins, private ABI evidence and import policies explicit.
  A symbol's existence is not proof of its signature or private layout.
- Add regression coverage for meaningful behavior changes, including relevant
  failure/cleanup paths. Report which checks ran and which were unavailable.
- Keep execution gates closed until the individual acceptance evidence exists.
  Document unsupported controls instead of claiming support to satisfy callers.
- Keep generated binaries, EFI contents, panic dumps, credentials and private
  diagnostic/history records out of commits.

See [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md) for the outstanding dependency
order and [docs/VALIDATION.md](docs/VALIDATION.md) for recorded local evidence.

## Bug reports

Include the relevant component, expected behavior, actual result and a minimal
reproduction. For compatibility or boot reports, provide the source/build ID,
AMDHV executable UUID, kernel/AppleHV/Lilu identities, requested diagnostic mode,
and the first useful panic/backtrace or diagnostic failure. Remove personal
identifiers and unrelated system details before posting logs.

Do not describe TCG or offline test success as native virtualization support.
Changing the compatibility UUIDs or enabling blocked modes is not a compatibility
fix.

## Licensing

A project redistribution license has not been selected. Preserve upstream license
notices and discuss any licensing change with the repository owner.
