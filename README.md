# ccbus — a chat-app view of your Claude Code sessions

Run Claude Code sessions in tmux (locally or on a server). `ccbus-tui` shows them like a
messaging app in your terminal: who's running, who's waiting for you, who needs permission.
It also sends desktop notifications and lets you reply, approve prompts, or attach to the
real Claude UI.

```
Laptop                                         Server (or the same machine)
┌──────────────┐  ssh -T host ccbus serve      ┌─────────────────────────────────────┐
│  ccbus-tui   │◄────── JSON lines ───────────►│ ccbus serve --stdio                 │
│  list + chat │                               │   reads SQLite, tails transcripts,  │
│  notify (D-Bus)                              │   drives tmux                       │
└──────────────┘                               │ claude ──hooks──► ccbus hook ──► SQLite
                                               │ tmux: cc-<name> sessions            │
                                               └─────────────────────────────────────┘
```

* **State comes from Claude Code hooks**, not from scraping the screen. `SessionStart`,
  `UserPromptSubmit`, `Stop`, `Notification` (permission), `PostToolUse` and `SessionEnd`
  call `ccbus hook`, which records state in `~/.local/state/ccbus/ccbus.db`.
* **Chat history** is the session's transcript JSONL (`~/.claude/projects/...`). It is parsed
  on the server and streamed to the client.
* **Sending** uses `tmux paste-buffer -p` (bracketed paste) followed by Enter.
* There is **no daemon**: hooks write SQLite directly, and `serve` polls it. A dead tmux pane is
  detected by `serve` and shown as *exited*.

## Build

Built with Bazel 9 (`.bazelversion`) and a C++20 compiler. SQLite, nlohmann_json and
[FTXUI](https://github.com/ArthurSonzogni/FTXUI) 6 come from the Bazel Central Registry.
For desktop notifications the TUI links the system's libsystemd (sd-bus), found with
`pkg-config`; without it the TUI builds without notifications.

```sh
bazel build //:ccbus //:ccbus-tui     # -> bazel-bin/ccbus, bazel-bin/ccbus-tui
bazel run //:install                  # copies both to ~/.local/bin
bazel run //:install -- /usr/local    # or another prefix

# headless server: only the ccbus binary
bazel build //:ccbus && install -m 755 bazel-bin/ccbus ~/.local/bin/
```

Install before running `ccbus install-hooks`. The hooks point at the `ccbus` binary you
run it from, and paths under `bazel-bin/` change between builds.

For clangd, `bazel/compile_commands.sh` writes `compile_commands.json` from Bazel's compile
actions.

The older Qt client is still in `client/`, but it has no build any more.

## Set up (on every machine that runs Claude sessions)

```sh
ccbus install-hooks        # merges hooks into ~/.claude/settings.json (backup: .bak-ccbus)
ccbus install-hooks --uninstall
```

Hooks apply to **all** Claude Code sessions, including ones you start yourself in tmux or
in a plain terminal. Sessions outside tmux are listed, but you can't send to them or
attach to them.

## Use

```sh
ccbus-tui                  # the app
ccbus new api ~/src/api -- --model opus   # or start sessions from the CLI
ccbus ls
```

The list is grouped like an inbox: **needs you** (`!` permission prompt, `●` unread reply),
**running** (spinner), **idle** (`○`, `✕` exited). It switches to a stacked layout below
100 columns.

| Key | |
|---|---|
| `j` `k` / arrows, `g` `G` | move |
| `enter` / `i` | reply. In the reply box: `enter` sends, `alt+enter` or `\` `enter` adds a newline, `esc` goes back |
| `n` | new session: host, directory (`↑↓` for recent ones), name, claude flags. Creates tmux session `cc-<name>` |
| `1`-`9` | answer the prompt on the live screen (works from an empty reply box too) |
| `s` | type straight into Claude's terminal; `ctrl+]` stops |
| `p` | show/hide the live screen. It opens on its own for permission prompts and while starting (folder trust) |
| `a` | attach to the real Claude UI (below) |
| `r` | resume an exited session with `claude --resume <id>` and its original flags |
| `x` / `D` | kill / remove from the list (asks first) |
| `/` | filter; `pgup` `pgdn` (`ctrl+b/f/u/d`) scroll the chat; `?` help; `q` quit |

**Attach** depends on where the TUI runs:
* Inside tmux, for a local session: the tmux client switches to it, and `prefix L` comes back.
* Inside tmux, for a remote session (or a different tmux socket): it opens in a new tmux window.
  The TUI keeps running.
* In a plain terminal: the TUI steps aside for `tmux attach` (`ssh -t` for remote hosts) and
  comes back when you detach.

**Notifications** go to your notification daemon (dunst, mako...). They fire when a session
stops or needs permission, unless you're looking at that session. Clicking one selects the
session and, inside tmux, brings the TUI's window forward. They close once you've read the
session or replied. Focus tracking uses terminal focus reports; inside tmux, turn on
`set -g focus-events on`.

### Remote hosts

Hosts live in `~/.config/ccbus/config.json`:

```json
{
  "hosts": [
    {"name": "local"},
    {"name": "box", "ssh": "me@box"},
    {"name": "gpu", "ssh": "gpu", "command": "/opt/ccbus/bin/ccbus serve --stdio"}
  ],
  "notify": true
}
```

For a one-off run: `ccbus-tui -H box=me@box -H local=local`. The TUI runs
`ssh -T -o BatchMode=yes <dest> ~/.local/bin/ccbus serve --stdio`, so key or agent auth must
work without a prompt. It reconnects with backoff. While disconnected, sessions keep running,
and anything that finished in the meantime is notified once you're back. A `ControlMaster`
setup in `~/.ssh/config` makes attach instant.

## Environment knobs

| Variable | Effect |
|---|---|
| `CCBUS_DB` | SQLite path (default `$XDG_STATE_HOME/ccbus/ccbus.db`) |
| `CCBUS_CLAUDE` | Command used to launch claude (default `claude`) |
| `CCBUS_TMUX_SOCKET` | Keep ccbus sessions on a separate tmux server (`tmux -L name`) |

## Protocol (`ccbus serve --stdio`)

One JSON object per line. Requests are `{"id":N,"op":...}` and replies are
`{"re":N,"ok":true,...}`. Ops: `hello`, `list_sessions`, `subscribe {since_seq}`,
`watch {key}`, `unwatch`, `send {key,text}`, `keys {key,keys[]}`, `capture {key,ansi?}`,
`new {name,cwd,args[]}`, `resume {key}`, `kill {key}`, `remove {key}`, `mark_read {key}`, `ping`.
Pushed events: `{"ev":"session",kind,seq,session}`, `{"ev":"removed",key}`, and
`{"ev":"transcript",key,reset,truncated,msgs[]}`.

## Not done yet

* No phone push (e.g. ntfy) for when the laptop is closed.
* Remote (ssh) hosts are implemented but have only been exercised locally.
* Hosts are edited in the config file; there is no settings screen.
* Subagent (sidechain) messages and thinking blocks are hidden from the chat view.
