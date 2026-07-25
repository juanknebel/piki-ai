# CLAUDE.md

Guidance for working in this repo. `piki` is an ultra-lightweight terminal LLM
chat client in C99, targeting old 32-bit hardware (Linux i686) and Haiku OS.
User-facing docs live in `README.md`.

## Commands

```sh
make            # dev build (dynamic, system OpenSSL) -> build/piki
make test       # all suites, normal build + AddressSanitizer/UBSan
make release    # static binaries -> dist/piki-linux-{x86_64,i686}
make clean
tools/mkcerts.sh   # regenerate src/certs.h (embedded CA roots) from the system bundle
```

There is no single-test target; `make test` builds and runs every
`tests/test_*.c` twice (plain + `.asan`). To run one suite: `make build/test_json && ./build/test_json`.

## Hard constraints (do not violate)

- **Only dependency is OpenSSL** (for TLS). Everything else — HTTP/1.1, JSON,
  SSE, chunked decoding, INI, line editing — is hand-written against the C
  standard library + POSIX. Do not add third-party libraries.
- **C99, portable POSIX.** Must compile with `gcc`/`clang`, `musl-gcc`, the
  `i686-linux-musl` cross toolchain, and native gcc on Haiku. No GNU
  extensions, no `Date.now`-style nondeterminism. Keep it 32-bit clean
  (validate with `-m32`).
- **Everything is ASCII English** — comments, strings, docs. Test *data*
  literals may contain UTF-8 (e.g. `"café"`, `"ñandú"`) on purpose.
- Warnings are errors in spirit: builds must be clean under `-Wall -Wextra`.
  Every new module gets a `tests/test_*.c` suite wired into the Makefile
  (both plain and `.asan` targets).

## Architecture

Layered, each module depends only on lower ones. `buf` is the foundation.

```
buf     dynamic string buffer (aborts on OOM)     -> used by everything
net     TCP + TLS (OpenSSL); use_tls=0 = plain HTTP for local providers
http    HTTP/1.1 request/response + incremental chunked decoder
json    recursive-descent parser (arena, one free/doc) + escaping writer
sse     incremental Server-Sent Events parser
api     OpenAI-compatible layer: api_chat_stream, api_agent_turn, api_models
chat    message history + context window (chat_window truncates)
config  INI parser (~/.config/piki/config)
edit    raw-termios line editor + command history (pure ops are testable)
term    line reading; delegates to edit on a TTY, falls back to fgets
tools   agent tools: read_file / write_file / run_command
main    arg parsing, provider resolution, REPL, agent loop
```

**Design invariants worth preserving:**
- Parsers (`json`, `sse`, `http` chunked decoder) are **incremental state
  machines** — they must tolerate input split at arbitrary byte boundaries.
  Tests feed input byte-by-byte to prove this; keep that property.
- `net_interrupt` (set by the SIGINT handler) lets a blocking read return
  `NET_EINTR` so Ctrl-C cancels streaming without killing the process. Read
  paths honor it; `net_read` also has an inactivity timeout (`NET_TIMEOUT`).
- CA roots are **embedded** in the binary via `src/certs.h` (generated,
  gitignored) — the client never trusts the system store, which is dead on
  2007-era systems.
- Provider resolution precedence in `main.c`: `PIKI_BASE_URL` env >
  `-p`/config provider > built-in openrouter. API key is optional (local
  providers like Ollama/llama-server send no `Authorization`).
- **Tool-use turns are non-streaming** (`api_agent_turn`); normal chat
  streams token-by-token. This is deliberate — assembling `tool_calls` from
  SSE deltas is not worth the complexity. The agent loop rebuilds the raw
  messages JSON array by hand (assistant messages carry `tool_calls`, tool
  results carry `tool_call_id`).
- Dangerous tools (`write_file`, `run_command`) require a y/N confirmation
  before running; a rejection is reported back to the model as a tool result.

## Release builds

`make release` runs `tools/build-release.sh`: it compiles a trimmed static
OpenSSL 3.x with musl per arch (cached in `deps/openssl-<arch>/`, slow only
the first time) and links a fully static binary. The tarballs and the
`i686-linux-musl` toolchain live in `deps/` (gitignored). Two OpenSSL build
flags matter: `no-ktls` and `-DOPENSSL_NO_SECURE_MEMORY` avoid kernel headers
the system musl-gcc lacks.

**When editing the source file list**, update it in BOTH places: the Makefile
`OBJS` and the `SRCS` variable in `tools/build-release.sh` (they are separate
lists — a mismatch produces link errors only in the release build).

## Version bumps

`PIKI_VERSION` lives only in `src/version.h` — bump that one line. It feeds
`--version`, the REPL banner, and the HTTP `User-Agent`. Tagging `vX.Y.Z` and
pushing triggers a GitHub Release with the static binaries attached.

## Not built yet

Native Haiku build and validation on real 2007 hardware — the code is
prepared for both (Makefile adds `-lnetwork` on Haiku) but neither has been
compiled/run on those platforms; they need the actual machine/VM.
