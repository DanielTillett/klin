# Compiler and linker hardening

Klin is built with the compiler and linker protections in the tables
below, all on by default. You need the details when a toolchain
misbehaves, when you are comparing performance, or when an auditor asks
how the binary was built.

klind, klin, the libraries, LibU and the site's pages are all compiled
with these flags, and the programs are linked with the linker flags.
**Each flag is used only if the compiler or linker accepts it** (the
`_FORTIFY_SOURCE` define is simply added), so an
older toolchain quietly gets fewer protections rather than a failed
build. To build without them:

```sh
cmake -S . -B out -DKLIN_HARDENING=OFF
```

That switches off everything in the tables below except the stack
protector, which has its own option, `KLIN_STACK_PROTECTOR`.

## GCC and Clang

| Flag | Protects against | Where | CMake option |
|---|---|---|---|
| `-D_FORTIFY_SOURCE=3` | buffer overflows in libc calls with a known destination size (bounds-checked `__*_chk` versions) | every build type except Debug; level 3 needs glibc 2.34+, older ones treat it as 2 | `KLIN_HARDENING` |
| `-fstack-protector-all` | stack buffer overflows that overwrite a return address (canary in every function) | all | `KLIN_STACK_PROTECTOR` |
| `-fstack-clash-protection` | the stack jumping over a guard page into the heap through a big frame | all | `KLIN_HARDENING` |
| `-fcf-protection=full` | return- and jump-oriented programming: Intel CET (Control-flow Enforcement Technology) indirect branch tracking and shadow stack, where the CPU and kernel have it | x86 | `KLIN_HARDENING` |
| `-mbranch-protection=standard` | the same, with ARM pointer authentication (PAC) and BTI | ARM64 | `KLIN_HARDENING` |
| `-ftrivial-auto-var-init=zero` | reading uninitialised locals: they start as zero | all | `KLIN_HARDENING` |
| `-Wformat -Werror=format-security` | format string attacks: a format string that is not a literal and has no arguments is a compile error | all | `KLIN_HARDENING` |
| `-fPIE`, `-pie` | fixed code addresses: position-independent programs get full ASLR (address space layout randomisation) | not Windows | `KLIN_HARDENING` |
| `-Wl,-z,relro` `-Wl,-z,now` | overwriting the GOT (global offset table): relocations are read-only after start, and every symbol is bound at start | ELF | `KLIN_HARDENING` |
| `-Wl,-z,noexecstack` | code injected on the stack: the stack is not executable | ELF | `KLIN_HARDENING` |

`_FORTIFY_SOURCE` needs optimisation to work, which is why Debug builds,
and the sanitizer builds, leave it out. The default build type is
Release.

MinGW (Windows) links with `--dynamicbase`, `--nxcompat` and
`--high-entropy-va` instead, for ASLR and DEP. macOS links PIE by
default, and the `-z` options do not apply there.

MSVC is not a supported build today (see
[Known limitations](../README.md#known-limitations) in the README), but if used it
would get `/GS` and `/guard:cf`, and link with `/guard:cf /DYNAMICBASE
/HIGHENTROPYVA /NXCOMPAT`.

## Size options are separate

Two other options sit next to these in CMakeLists.txt but are about size,
not security. `KLIN_LTO` (link-time optimisation, Release-type builds
only, not fuzz builds) and `KLIN_GC_SECTIONS` (put each function and
object in its own section so the linker drops unused ones) are both on
by default. Together they took a stripped klind with doc/example-site
embedded from 408,864 bytes to 290 KB (x86-64 Linux, GCC 15, Nov 2026).
Neither changes the protections above.

## Tested

The build checks (test/run_build_checks.py) read klind's ELF headers on
Linux with readelf and fail unless it is PIE, has `GNU_RELRO` and
`BIND_NOW`, a non-executable `GNU_STACK`, the stack protector
(`__stack_chk_fail`), fortified libc calls (`__*_chk`), and, on x86-64,
the IBT and SHSTK properties.

See also: [SANDBOX](SANDBOX.md) for the run-time restrictions, and
[FUZZING](FUZZING.md).
