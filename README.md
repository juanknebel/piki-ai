# piki

A chat client for LLM models for the terminal, **ultra-lightweight and with
no runtime dependencies**. Designed to run anywhere, including old 32-bit
laptops (Linux i686) and Haiku OS.

Talks to any OpenAI-compatible API endpoint: OpenRouter (by default) and
local servers like **Ollama** or **llama.cpp** (`llama-server`).

- Written in C99. Only build dependency: OpenSSL (for TLS).
- Release binaries are **static** (musl + embedded OpenSSL): a single
  file that you copy and run, with nothing to install.
- CA certificates embedded in the binary — works even on systems whose
  certificate store has grown old.
- Streaming responses token by token; `Ctrl-C` cancels without closing the
  program.
- Line editor with history, optional tool use (read/write files, run
  commands with confirmation), and save/load conversations.

## Build

### Requirements

For the development build (compiled natively, links against the system
OpenSSL) you need:

- A C99 compiler (`gcc` or `clang`) and `make`.
- OpenSSL headers and libraries (`libssl` + `libcrypto`), version 1.1.1 or 3.x.
- The `openssl` CLI and a system CA bundle (`/etc/ssl/cert.pem`): the build
  generates `src/certs.h` from it once, via `tools/mkcerts.sh`.

Only the static release build (`make release`) additionally needs `perl`
(OpenSSL's `Configure`), `musl-gcc`, and the `i686-linux-musl` cross
toolchain — none of that is required to compile and run natively.

**Void Linux** (i686 or x86_64, glibc or musl):

```sh
sudo xbps-install -S base-devel openssl-devel ca-certificates
```

**Slackware** — a full install already includes everything (gcc, make,
openssl, ca-certificates). On a minimal install:

```sh
sudo slackpkg install gcc make binutils openssl ca-certificates
```

**Debian/Ubuntu**: `build-essential libssl-dev ca-certificates openssl` ·
**Arch**: `base-devel openssl ca-certificates` ·
**Fedora**: `gcc make openssl-devel ca-certificates openssl`

### Development build (dynamic, uses the system OpenSSL)

```sh
make           # produces build/piki
make test      # runs all suites (normal + AddressSanitizer)
```

### Static release build (musl + static OpenSSL)

```sh
make release   # produces dist/piki-linux-x86_64 and dist/piki-linux-i686
```

The first `make release` compiles OpenSSL for each architecture (it gets
cached in `deps/`); subsequent ones take seconds. Requires the tarballs
in `deps/` (see `tools/build-release.sh`).

On **Haiku** the build is native: `pkgman install openssl3_devel`, then
`make` (the `-lnetwork` is added automatically).

## Configuration

`~/.config/piki/config` (or `$XDG_CONFIG_HOME/piki/config`):

```ini
[provider "openrouter"]
url = https://openrouter.ai/api/v1
key = sk-or-...

[provider "ollama"]
url = http://192.168.1.10:11434/v1    ; no key: plain HTTP, no auth

[defaults]
provider = openrouter
model = anthropic/claude-haiku-4.5
system = Answer in English.
max_history = 40
```

Environment variables (override the config):

- `OPENROUTER_API_KEY` — key for the `openrouter` provider.
- `PIKI_BASE_URL` — alternative endpoint, e.g. `http://host:11434/v1`.

## Usage

```sh
piki                       # interactive REPL
piki "capital of France?"      # one-shot and exit
piki -m openai/gpt-4o-mini "hi"
piki -p ollama -m llama3.2 "hi"
piki -t "list the .c files in the current directory"   # with tool use

# local model on another machine on the LAN:
PIKI_BASE_URL=http://192.168.1.10:11434/v1 piki -m llama3.2 "hi"
```

Options: `-m model`, `-p provider`, `-s system_prompt`, `-t` (tool use),
`-w` (web search), `--version`, `--help`.

### Web search

`-w` (or `/web` in the REPL) enables OpenRouter's web-search plugin, so the
model can answer with up-to-date information. When active, the REPL prompt
changes to `web>` and the banner shows `[web]`. Web search is billed
separately by OpenRouter. Equivalent to appending `:online` to the model
slug (e.g. `-m anthropic/claude-haiku-4.5:online`), which also works without
the flag.

### REPL commands

```
/model [id]    show or change the model
/models        list the provider's models
/tools         enable/disable tool use
/web           enable/disable web search
/save <file>   save the conversation (JSON)
/load <file>   load a conversation
/new           start a new conversation
/help          help
/quit          exit (also Ctrl-D)
!cmd           run a shell command (output shown to you)
!!cmd          run a shell command and add its output to the chat
```

`!cmd` is a local shell escape — handy to check something without leaving the
REPL. `!!cmd` additionally feeds the command's output (stdout + stderr, with
its exit code) into the conversation, so you can then ask the model about it
(e.g. `!!make 2>&1` then "why does it fail?").

Line editing: ←/→ arrows, Home/End, ↑/↓ for history,
Ctrl-A/E/K/U/W. History persists in `~/.config/piki/history`.

### Tool use

With `-t` (or `/tools` in the REPL) the model can request:

- `read_file` — read a file.
- `write_file` — write/overwrite (asks for confirmation).
- `run_command` — run a shell command (asks for confirmation).

Actions that modify the system require your `y/N` before running.
Requires a model with function calling support.

## Structure

```
src/buf     dynamic buffer        src/sse     Server-Sent Events parser
src/net     TCP + TLS (OpenSSL)   src/api     OpenAI-compatible layer + agent
src/http    HTTP/1.1 + chunked    src/chat    history + context window
src/json    JSON parser/writer    src/config  INI file
src/edit    line editor           src/term    line reading
src/tools   agent tools
tools/mkcerts.sh        generates src/certs.h (embedded CA roots)
tools/build-release.sh  static builds with musl
```

Everything with the C standard library plus OpenSSL; HTTP, JSON, SSE and the
chunked encoding parser are our own.

## Author

Juan Knebel — juanknebel@gmail.com ·
<https://github.com/juanknebel/piki-ai>

## License

GPL-2.0. See [LICENSE](LICENSE).
