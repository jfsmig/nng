# AGENTS.md

Guidance for AI coding agents (and humans) working in this repository.

## Precedence

This file complements the project's own rules and never overrides them. The
authoritative documents are:

| Document | Governs |
| --- | --- |
| `docs/CONTRIBUTING.adoc` | Legal terms, issues, pull requests, commit format, copyright notices. |
| `etc/README.adoc` | Coding style, language level, naming. |
| `.clang-format` | Whitespace. |
| `.github/pull_request_template.md` | Pull request text. |
| `docs/RELEASING.md` | Versioning and releases. |
| `CODE_OF_CONDUCT.md` | Conduct. |

Where anything below disagrees with one of them, they win. Report the
discrepancy instead of picking a side silently.

## Project

NNG (nanomsg-next-generation) is a C library implementing the Scalability
Protocols (pub/sub, req/rep, pipeline, survey, bus, pair) over pluggable
transports (inproc, IPC, TCP, TLS, WebSocket, UDP, DTLS, socket), on top of
its own asynchronous I/O framework and thread pools. It also provides stream,
HTTP and TLS APIs. It is wire compatible with nanomsg and mangos, has a
public API under semantic versioning, and runs on Linux, macOS, Windows,
illumos/Solaris, FreeBSD, Android and iOS. Licensed MIT.

## Build and test

CMake (3.15 or newer) is the only build system. CI (`.github/workflows/`)
uses Ninja and Debug builds:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Drop `-G Ninja` to use the default generator. Build in `build/` or
`build.<variant>/`: both are git-ignored.

Useful options (see `cmake/NNGOptions.cmake` and the top `CMakeLists.txt`):
`-DNNG_ENABLE_TLS=ON` with `-DNNG_TLS_ENGINE=mbed|wolf|openssl` (see
`docs/BUILD_TLS.md`), `-DNNG_SANITIZER=address|thread|undefined`,
`-DNNG_ENABLE_COVERAGE=ON`,
`-DNNG_POLLQ_POLLER=auto|epoll|kqueue|ports|poll|select`,
`-DBUILD_SHARED_LIBS=OFF`, `-DNNG_TESTS=OFF`, `-DNNG_TOOLS=OFF`, and the
per-feature `NNG_PROTO_*` and `NNG_TRANSPORT_*` switches.

Tests are named after their directory. Run one with
`ctest --test-dir build -R '^nng\.core\.url_test$' --output-on-failure`.
Each test is also a standalone program: `build/src/core/url_test --list`
lists its cases and `build/src/core/url_test -v "url clone"` runs one. Build
a single test with `cmake --build build --target url_test`.

The long-running DTLS soak (`docs/DTLS_SOAK.md`) is opt-in and not part of
`ctest`.

## Layout

| Path | Contents |
| --- | --- |
| `include/nng/` | Public API: `nng.h`, `http.h`, `args.h`. |
| `src/nng.c`, `src/nng_legacy.c` | Public API entry points. |
| `src/core/` | Core framework: aio, task queues, sockets, pipes, dialers, listeners, messages, options, statistics, URLs. `platform.h` is the API every platform implements; `nng_impl.h` is the umbrella internal header. |
| `src/platform/posix/`, `src/platform/windows/` | Platform implementations, including the pollers (epoll, kqueue, ports, poll, select). |
| `src/sp/protocol/`, `src/sp/transport/` | Scalability Protocols and transports. |
| `src/supplemental/` | Code shared by several subsystems: `http`, `tls` (engines `mbedtls`, `wolfssl`, `openssl`), `websocket`. |
| `src/testing/` | NUTS test support (`nuts.h`) and the vendored `acutest.h`. Do not restyle or refactor `acutest.h`. |
| `src/tools/` | `nngcat` and the `perf` benchmarks. |
| `tests/` | The C++ compatibility test only. Unit tests live next to the code they test, as `*_test.c`. |
| `demo/` | Example programs, each with its own `CMakeLists.txt`; not part of the main build. |
| `docs/ref/` | Sources of the reference manual (mdBook). |
| `docs/` | Build guides, `CONTRIBUTING.adoc`, `RELEASING.md`. |
| `cmake/` | CMake options, helpers and find modules. |
| `tools/` | Go helpers: `release`, `pubrefman`. |
| `etc/` | Developer support files, including `format-check.sh`. |

## Conventions in this codebase

- **Language**: the code is C99 (`etc/README.adoc`, `CMAKE_C_STANDARD 99`).
  Use anything newer only behind a configure check with a fallback, as
  `<stdatomic.h>` is (`NNG_HAVE_STDATOMIC`).
- **Formatting**: `.clang-format` decides whitespace: tabs for indentation,
  8 columns wide, lines up to 79 columns, the return type of a function
  definition on its own line, aligned consecutive declarations and
  assignments. In addition, per `docs/CONTRIBUTING.adoc`: return values are
  enclosed in parentheses (`return (rv);`), and conditional clauses are
  always enclosed in braces and span multiple lines. Comments use `//`.
  Format only the lines you change (`git clang-format`): `.clang-format` is
  "not necessarily complete" and `clang-format` versions differ, so where the
  tool and the surrounding code disagree, match the surrounding code. The
  test helpers have a style of their own.
- **Naming**: `nng_` and `NNG_` are for symbols exported to consumers; they
  form the public API. `nni_` and `NNI_` are for internal symbols and must
  never be used by users. Statics may have any non-conflicting name; most
  files prefix them with the module (`url_`, `tcptran_`).
- **Errors**: functions return an `nng_err` (`NNG_OK` on success, an `NNG_E*`
  code otherwise); there is no `errno`. Release resources in reverse order of
  acquisition on failure. Both early returns and `goto` unwinding exist;
  follow the file you are in.
- **Invariants** are checked with `NNI_ASSERT()`, which is compiled out under
  `NDEBUG` and so must have no side effects. `nni_panic()` is for
  unrecoverable internal faults. Conditions that a caller or a peer can
  trigger are reported through the return value, never asserted.
- **Asynchronous operations** go through `nni_aio`; `src/core/aio.h` is the
  contract. A provider calls `nni_aio_start()` with a cancel function before
  filing an operation, and stops touching the aio if it returns false. Every
  started aio is finished with `nni_aio_finish*()`, and must honour
  cancellation (`NNG_ECANCELED`), timeouts (`NNG_ETIMEDOUT`), closing
  (`NNG_ECLOSED`) and stopping (`NNG_ESTOPPED`). Never call `nni_aio_stop()`
  from a callback, and hold no lock when calling `nni_aio_finish_sync()`.
- **Memory**: use `nni_alloc()`, `nni_zalloc()` and `nni_free()`, or
  `NNI_ALLOC_STRUCT()` and `NNI_FREE_STRUCT()`; freeing takes the size.
- **Public types** stay opaque: allocate dynamically and hand back a pointer
  rather than exposing a structure that would get baked into binaries. Any
  exception needs very careful review (`etc/README.adoc`).
- **Symbols**: public declarations carry `NNG_DECL`; shared builds hide
  everything else where the compiler supports it. Deprecated API is marked
  `NNG_DEPRECATED` and guarded by `NNG_ELIDE_DEPRECATED`.
- **Byte order**: read and write wire data with the `NNI_GET*` and `NNI_PUT*`
  macros (`src/core/defs.h`).
- **Platform code** lives in `src/platform/` behind `src/core/platform.h`.
  Optional system features are detected in CMake with `nng_check_sym()` or
  `nng_check_func()`, which define `NNG_HAVE_*`.
- **New files** use the MIT boilerplate of a neighbouring file and are added
  to the `CMakeLists.txt` of their directory with `nng_sources()`,
  `nng_sources_if()` or `nng_headers()`.
- **Copyright notices** follow `docs/CONTRIBUTING.adoc`:
  `// Copyright <year> <owner> <email or url>`, most recent year only, no
  `(C)`, no "All rights reserved"; a new notice goes immediately below the
  existing ones, just before the license boilerplate. Whose name goes there
  is a legal decision for the human submitter: ask, and never invent, remove
  or re-date a notice on your own.
- **Tests** use NUTS (`src/testing/nuts.h`): `NUTS_PASS()`,
  `NUTS_FAIL(call, NNG_E...)`, `NUTS_TRUE()`, `NUTS_MATCH()`, and a
  `NUTS_TESTS` table at the end of the file. A new test file is registered
  with `nng_test()` or `nng_test_if()` in the `CMakeLists.txt` of its
  directory. Tests link against the static `nng_testing` library, so they can
  reach internal `nni_` functions. `nngcat` is tested with shell scripts.
- **Documentation** is hand-written Markdown in `docs/ref/`, built with
  `mdbook build docs` (needs `mdbook` and `mdbook-indexing`). An API change
  updates the matching page under `docs/ref/api/`, `proto/` or `tran/`, and
  new symbols get an entry in `docs/ref/xref.md`. User-visible features are
  announced in `docs/ref/whatsnew/`, breaking changes in `docs/ref/migrate/`.
  Never commit the generated HTML; the published site is written by CI only.
- **API and ABI**: the `NNG_*_VERSION` and `NNG_RELEASE_SUFFIX` macros in
  `include/nng/nng.h` are the single source of the version, and the library
  SOVERSION is the major version. They are changed only by the release helper
  (`docs/RELEASING.md`), never by hand. Flag explicitly any change to
  `include/nng/*.h` that affects the API or the ABI. Wire formats, protocol
  numbers and limits such as `NNG_MAXADDRLEN` are fixed by interoperability
  with other implementations.

## Engineering guidelines

### Before changing code

- Read the code you are about to change, and its callers, before proposing
  anything. Follow the existing patterns of the surrounding file over personal
  preference or generic best practice.
- Establish what the request actually requires. If it is ambiguous in a way
  that changes the design, ask; otherwise pick the conventional reading, say
  which one you picked, and proceed.
- Reproduce a bug before fixing it, and find its cause rather than patching
  the symptom.

### Scope

- Make the smallest change that fully solves the problem. No drive-by
  refactoring, reformatting, renaming or dependency changes in the same patch.
- Do not add abstractions, options or extension points for needs that do not
  exist yet.
- Do not add dependencies. The library depends only on the C library and the
  platform's own threading and networking; a TLS engine (Mbed TLS, wolfSSL or
  OpenSSL) is the one optional exception. The Go helpers in `tools/` use the
  standard library only.
- Report unrelated problems you notice instead of fixing them unasked.

### Correctness

- Handle every error return. Every allocation, file descriptor, aio,
  reference and lock must be released on every path, including the error,
  cancellation and close paths.
- Treat everything read from the network as hostile: validate lengths before
  using them, check for integer overflow in size arithmetic, and never trust a
  peer-supplied size for an allocation or a copy. Receive paths honour
  `NNG_OPT_RECVMAXSZ`.
- Keep the code portable. Windows is a first-class platform: no POSIX-only or
  Linux-only calls outside `src/platform/posix/`, and no assumptions about
  endianness, alignment or type widths beyond what C99 guarantees. 32-bit
  targets are supported.
- The library is multi-threaded: completions, timers and pollers run on their
  own threads. Protect shared state explicitly (`nni_mtx`, the `nni_atomic_*`
  types, `nni_refcnt`) rather than relying on which thread happens to call.
- The compiler should stay quiet under `-Wall -Wextra`. Fix warnings; do not
  silence them with casts or pragmas unless the reason is written next to
  them.

### Testing

- Every behaviour change comes with a test that fails without it.
- Bug fixes come with a regression test. Write the test first, run it, and
  check that it fails because of the bug and for no other reason; then write
  the fix without modifying the test. Both go in the same commit (see
  "Commits and reviews").
- Run the whole `ctest` suite before declaring work finished. For anything
  touching aio, object lifetimes, locking or memory ownership, also run it
  with `-DNNG_SANITIZER=address` and `-DNNG_SANITIZER=thread`; CI runs
  address, undefined and thread on every pull request.
- TLS, DTLS and WSS code is only compiled with `-DNNG_ENABLE_TLS=ON`. A change
  there is not tested until it has been built and run with at least one
  engine.
- Tests must be deterministic: no dependence on external hosts, no fixed
  ports (use `NUTS_ADDR()`, `nuts_scratch_addr()` or `nuts_next_port()`), and
  no tight wall-clock timing. Each test has a 180 second budget.
- Never weaken, skip or delete a failing test to make the suite pass. If a
  test is wrong, say why and fix the test in its own commit.

### Readability

- Comments are welcome, but they never replace readable code. Readability
  comes first from symbols with meaningful names: functions, variables, types
  and constants whose names say what they are for.
- Prefer a short function with a comment above it to a comment on a block of
  code, and avoid comments on individual lines most of all. A block that needs
  a comment to be understood is usually a function waiting to be extracted and
  named.
- Comments explain why, not what. Keep existing comments accurate when the
  code under them changes.
- Prefer plain, explicit code over clever code. Asynchronous completion,
  cancellation and object teardown are subtle enough here; do not add to that
  without a measured reason.
- Optimise only with a benchmark from `src/tools/perf/` showing the gain.

### Commits and reviews

- The tree is "always release ready" (`docs/CONTRIBUTING.adoc`): a release
  could be cut at any commit boundary. Every commit is functionally complete,
  builds, and passes the tests. It is usually best to address one issue per
  pull request, with one squashed commit.
- This is why a regression test is committed together with its fix, and never
  before it: a commit holding only a failing test would break the tree for
  `git bisect`.
- Commit messages use the format of `docs/CONTRIBUTING.adoc`, which is also
  the pull request template:

  ```
  fixes #<issue number> <issue synopsis>

  <Comments describing the issue or the fix. Not every change needs them.>
  ```

  One `fixes` line per issue. Never invent an issue number: when you do not
  know it, ask. When a change has no issue to reference, the history uses a
  short imperative subject, optionally prefixed with the area
  (`url: fix cloning of long and path-only URLs`, `docs:`, `ci:`).
- Keep commit messages concise. Use the body to explain why the change is
  needed, not to restate the diff. Every line must earn its place: no
  restating of the subject, no file-by-file inventory, no account of how the
  change was arrived at, no filler. A trivial change needs its first line and
  nothing else.
- A pull request is based on the latest `main` and has no merge conflicts.
- AI attribution: never add a `Co-Authored-By` trailer for an AI tool, under
  any circumstances. Use an `Assisted-By` trailer instead. It names the tools
  used (for example `Assisted-By: Claude Code`), one trailer per tool. It does
  not name the model or its version, and it never contains an email address:
  hosting platforms resolve trailer addresses to accounts and would link the
  commit to one. The same applies to pull request descriptions.
- Never commit generated output (build trees, `docs/book/`, generated HTML,
  coverage reports, the `.context/` output of soak runs) or secrets. The
  certificates and keys in `src/testing/certs.c` and
  `src/tools/nngcat/nngcat_tls_*.pem` are throwaway test fixtures, not
  credentials to reuse.
- Do not rewrite published history, force-push, or commit and push without
  being asked. Version changes, release commits and tags are made only with
  the release helper, and only on request.

### Reporting

- State what was done, what was verified and how, and what was not: which
  configurations were built and tested (platform, TLS engine, sanitizer,
  poller) and which were left to CI. A build that was not run, or a test that
  still fails, is said plainly.
- When blocked, describe the obstacle and the options instead of working
  around it with a hack.
