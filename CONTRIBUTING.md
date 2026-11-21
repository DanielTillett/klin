# Contributing to Klin

Thank you for helping. Bug reports, fixes, tests and documentation are all
welcome.

**Security problems:** do not open an issue. Report them privately as
[SECURITY.md](SECURITY.md) describes.

## Before you start

- For anything larger than a small fix, open an issue first and describe
  what you want to change and why, so that we can agree on the approach
  before you write the code.
- Klin is used on small devices: a change should not add a dependency,
  a thread or a large amount of memory without a good reason.
- Behaviour that makes a deployment less safe (a weaker default, a check
  removed) needs a setting to turn it on, documentation, and a ChangeLog
  entry.

## Building and testing

The [README](README.md#quick-start-linux-wsl-macos) lists what to
install. Then, from the repository:

```sh
python3 build/build.py --test               # release build, all tests
python3 build/build.py --sanitize --test    # ASan, LSan and UBSan
python3 test/run_build_checks.py            # the CMake build itself
python3 test/run_fuzz.py --runs 20000       # every fuzz target (Linux, clang)
```

A change is ready when these pass with no new compiler warnings. Continuous
integration runs them, and more (BSD make, Windows, CodeQL, clang-tidy,
fuzzing), on every pull request.

## Writing the change

- Follow [doc/CONVENTIONS.md](doc/CONVENTIONS.md): C99, the style of the
  file you edit, its return values, ownership and error macros, and
  `klin/crypto.h` for all cryptography.
- Add a test for every fix and feature: `test/run_server_tests.py` for
  anything a client can see, `test/regressions.c` for a function on its
  own, `fuzz/` for a new parser.
- Update the documentation the change touches: `doc/*.md`, the manual
  pages in `doc/man`, and the README for anything user-visible.
- Add a ChangeLog entry for anything a user of Klin would notice.

## Pull requests

- One change per pull request, with a description of what it changes,
  why, and how it was tested.
- Keep commits focused; the commit message says what changed and why.
- Workflows pin actions to commit SHAs and give jobs only the permissions
  they need; `test/run_release_checks.py` checks this (see
  [doc/RELEASES.md](doc/RELEASES.md)).

By contributing, you agree that your contribution is licensed under the
project's [BSD 3-Clause License](https://github.com/DanielTillett/klin/blob/master/LICENSE).
