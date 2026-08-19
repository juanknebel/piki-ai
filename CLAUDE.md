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
api     OpenAI-compatible layer: chat_stream, agent_turn, responses_turn,
        models; web search and token_usage accounting flow through here.
        Web search is per-provider via api_web_kind (api.h): OpenRouter's
        plugin rides on chat/completions, Meta uses the Responses API
        ({base}/responses, non-streaming, url_citation sources), and
        API_WEB_LOCAL (the default for unknown hosts) uses piki's own
        web tools (see web below); new server-side providers (e.g.
        Anthropic server tools) add an enum value, a builder/parser in
        api.c and a detection case in main.c (provider_web_kind,
        overridable with the web_search config key)
chat    message history + context window (chat_window truncates)
config  INI parser (~/.config/piki/config)
edit    raw-termios line editor + history + Tab completion (via a callback;
        pure buffer ops are testable). Arrow/Home/End keys are accepted
        as both CSI (ESC [ letter, the common case) and SS3 (ESC O
        letter, application cursor-key mode) -- Haiku's Terminal
        defaults to SS3.
term    line reading; delegates to edit on a TTY, falls back to fgets
tools   agent tools: read-only (read_file, list_files, search_files) and
        confirmed (edit_file, write_file, run_command)
md      incremental markdown-to-ANSI renderer for streamed replies
web     client-side web search: the web_search (DuckDuckGo Lite; Brave
        is a config seam, engine/key in [search], not implemented yet)
        and fetch_url agent tools, plus their pure parts (reentrant URL
        splitter, percent enc/dec, incremental html-to-text stripper,
        DDG results parser). Uses its own throwaway connections, never
        api.c's cached one.
main    arg parsing, provider resolution, REPL, agent loop, status line
```

REPL extras (in `main.c`): `!cmd` / `!!cmd` shell escape (`!!` feeds output
back into the chat), `/web` + `-w` web-search toggle, Tab completion of `/`
commands (`complete_command`), a per-prompt status line (`print_status`:
cwd, model, context gauge with 75%/90% color warnings, session token
totals), `/system` + `/trim` + `/paste`, and named chats
(`/chats`, `/switch`, `/rename`, `/delete`) stored one JSON file each under
`~/.config/piki/chats/` and autosaved after every turn (`chat_autosave`).
A model turn is sent through `send_user_turn` — both the normal input path
and `/paste` go through it. The line editor stays generic — REPL-specific
completion lives in `main.c` behind `el_completer`.

**Design invariants worth preserving:**
- Parsers (`json`, `sse`, `http` chunked decoder) are **incremental state
  machines** — they must tolerate input split at arbitrary byte boundaries.
  Tests feed input byte-by-byte to prove this; keep that property.
- `net_interrupt` (set by the SIGINT handler) lets a blocking read return
  `NET_EINTR` so Ctrl-C cancels streaming without killing the process. Read
  paths honor it; `net_read` also has an inactivity timeout (`NET_TIMEOUT`).
- **Only the connect is retried** (3 attempts, 1 s/2 s backoff): at that
  point nothing has been sent, so a retry cannot duplicate a request or a
  charge. TLS/certificate failures are never retried (not transient), and
  mid-stream failures are reported, not retried. One extension, same
  spirit: `api_send` (api.c) re-sends a request ONCE, on a fresh
  connection, only when a REUSED keep-alive connection failed at write
  time or the server closed it before sending a single response byte —
  the idle-close race (RFC 9112 9.4) where nothing of a response was
  consumed, so the retry still cannot duplicate a reply or a charge. Any
  failure after the first response byte is reported, never retried.
- **The TLS connection is reused between turns** (one cached conn in
  api.c, keyed by host/port/tls; the SSL_CTX with the parsed roots is
  per-process in net.c): on the target i686 hardware the handshake is
  the most expensive part of a turn. Reuse is gated by
  `http_resp_reusable` — body fully read with real framing (chunked or
  Content-Length), no surplus bytes, no `Connection: close` — so "when
  in doubt, close" is the failure mode. A stream cut early (Ctrl-C,
  error) always closes. The update check runs in its own process and
  never touches the cache.
- **Two distinct history limits, do not conflate them.** `chat_t.max_bytes`
  (config `max_memory`, KB) caps what is KEPT in RAM — the oldest messages
  are evicted and freed, which is what keeps a long session off swap on a
  1 GB machine. `chat_window`'s budget (config `max_context_tokens`, via
  `send_limits`) caps what is SENT per turn. There is no tokenizer; the
  budget is estimated at ~4 bytes per token.
- CA roots are **embedded** in the binary via `src/certs.h` — generated by
  `tools/mkcerts.sh` but **committed** (public roots) so the project builds on
  any distro; the client never trusts the system store, dead on 2007 systems.
  Since fetch_url can reach arbitrary HTTPS hosts, the committed certs.h
  now carries the FULL system bundle (~120 roots, ~200 KB; parsed once
  per process into the shared SSL_CTX, so the cost is a one-time few ms).
  `tools/mkcerts.sh --minimal` regenerates the old curated 11-root list
  (OpenRouter/GitHub/Meta only) for a smaller binary.
- The startup update check (`update_check` in `main.c`, config
  `check_updates`) never blocks and never speaks on failure: the REPL only
  reads the `latest-release` cache file; a doubly-forked detached child
  (survives the terminal closing) refreshes it from the GitHub API at most
  once a day. Only newer-than-`PIKI_VERSION` prints a notice.
- Provider resolution precedence in `main.c`: `PIKI_BASE_URL` env >
  `-p`/config provider > built-in openrouter. API key is optional (local
  providers like Ollama/llama-server send no `Authorization`).
- **Tool-use turns are non-streaming** (`api_agent_turn`); normal chat
  streams token-by-token. This is deliberate — assembling `tool_calls` from
  SSE deltas is not worth the complexity. The agent loop rebuilds the raw
  messages JSON array by hand (assistant messages carry `tool_calls`, tool
  results carry `tool_call_id`).
- Tools split by `tool_is_dangerous`: read-only ones (`read_file`,
  `list_files`, `search_files`) run unattended, while the ones that change
  the system (`edit_file`, `write_file`, `run_command`) require a y/N/a
  confirmation ('a' skips further asks for that tool, session-only, never
  persisted); a rejection is reported back to the model as a tool result.
  That confirmation is also the mitigation for untrusted file contents
  trying to steer the model — do not widen the 'a' grant beyond the
  session. The web tools (`web_search`, `fetch_url`) ALWAYS confirm even
  though they do not modify the system: fetched pages are untrusted
  input that can steer the model into exfiltrating conversation or file
  data through the next search/fetch, and the prompt showing the exact
  query/URL is the mitigation. They are dispatched in main.c's agent
  loop (web_tool_is/web_tool_run), not through tool_run, so tools.c
  stays network-free and test_tools links without OpenSSL. The tools
  array is assembled per turn in agent_turn from TOOLS_ITEMS +
  WEB_TOOLS_ITEMS; web tools are only declared while /web is on with
  API_WEB_LOCAL.
- `edit_file` requires its `old_string` to match **exactly once** — zero or
  several matches is an error returned to the model, which pushes it to
  quote more context instead of guessing. It also refuses files bigger than
  `MAX_READ`, since `slurp` truncates and writing that back would destroy
  the tail of the file.
- Token usage is requested via `stream_options.include_usage` (streaming) and
  read from the response (agent turns). Providers that omit it leave the
  counts at 0 — handle that gracefully, never assume usage is present. The
  REPL then falls back to a ~4 bytes/token estimate and marks the session
  totals with a leading `~` in the status line.
- The REPL autosaves to `~/.config/piki/session.json` after every turn (and
  after `/new` and `/load`, to keep the file in sync). Resuming is **never**
  automatic — only `--resume` — and a failed save warns once and never
  interrupts the chat.

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

## Platform status

Validated on real old 32-bit (i686) hardware — an Acer Aspire One netbook
with 1 GB RAM runs the static i686 release binary. The native Haiku
build (x86, 32-bit) has been built from source on real hardware: the
default `cc` on Haiku x86 is the legacy gcc2 kept for BeOS ABI
compatibility (no `-std=c99`/`-Wextra`/`-MP`), so the Makefile swaps to
the modern `gcc-x86` secondary-architecture compiler automatically
(`pkgman install gcc_x86` if it's missing) unless `CC` is set
explicitly. Still pending: running the compiled binary end-to-end there
(chatting, TLS, tool use).
