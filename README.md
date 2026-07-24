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

Development build (dynamic, uses the system OpenSSL):

```sh
make           # produces build/piki
make test      # runs all suites (normal + AddressSanitizer)
```

Static release binaries for Linux (musl + static OpenSSL):

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
`--version`, `--help`.

### REPL commands

```
/model [id]    show or change the model
/models        list the provider's models
/tools         enable/disable tool use
/save <file>   save the conversation (JSON)
/load <file>   load a conversation
/new           start a new conversation
/help          help
/quit          exit (also Ctrl-D)
```

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
