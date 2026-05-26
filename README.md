# WineCord

WineCord is a tiny macOS bridge for Discord Rich Presence from Windows games
running through Wine or CrossOver.

It is built as two small pieces:

- `winecord`: a native macOS agent/CLI that listens only on `127.0.0.1`, finds
  the local Discord IPC socket, and forwards bytes to Discord.
- `winecord-bridge.exe`: a Windows helper that runs inside a Wine/CrossOver
  bottle, creates `\\.\pipe\discord-ipc-0..9`, and forwards pipe traffic to the
  macOS agent using a local token.

No Electron, no Node runtime, no background service with root privileges.

## Build

```sh
make
```

The macOS binary is written to `build/winecord`.

To build the Wine helper you need a MinGW-w64 compiler:

```sh
make windows-helper
```

That produces `build/winecord-bridge.exe`.

## Quick Start With CrossOver

Start Discord for macOS, then:

```sh
./build/winecord doctor
./build/winecord install-agent
./build/winecord start
./build/winecord install-bottle --bottle "/path/to/CrossOver/Bottles/Steam"
```

`install-bottle` copies the helper into `C:\windows\winecord-bridge.exe`,
writes the shared token config into `C:\users\Public\WineCord\config.ini`, and
tries to register the helper as a Wine service through CrossOver.

If the helper has not been built yet, run `make windows-helper` first.

## Commands

```sh
winecord agent             # run the macOS forwarding agent in the foreground
winecord doctor            # show config, Discord IPC, and CrossOver diagnostics
winecord install-agent     # install a per-user LaunchAgent
winecord uninstall-agent   # remove the LaunchAgent
winecord start             # start the LaunchAgent
winecord stop              # stop the LaunchAgent
winecord install-bottle --bottle PATH [--helper PATH] [--no-register]
```

## Smoke Test

With Discord open, the LaunchAgent running, and the helper installed in the
CrossOver bottle:

```sh
make windows-smoke
CX_BOTTLE_PATH="/path/to/CrossOver/Bottles" \
  "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wine" \
  --bottle Steam --no-gui ./build/pipe-smoke.exe
```

The expected response for the bundled smoke test is Discord's `Invalid Client
ID` close frame. That is good: it proves bytes traveled through the Windows
named pipe, the Wine helper, the macOS agent, and Discord.

Config lives in:

```text
~/Library/Application Support/WineCord/config.ini
```

Logs live in:

```text
~/Library/Logs/WineCord/
```

## Design Notes

Discord's documented IPC transport uses Windows named pipes on Windows and Unix
domain sockets on Linux/macOS. Wine games usually look for
`\\?\pipe\discord-ipc-{n}` or `\\.\pipe\discord-ipc-{n}`, while Discord for
macOS exposes `discord-ipc-{n}` under the user's runtime/temp directories.

WineCord keeps the bridge transport-level: it forwards bytes without parsing or
rewriting Rich Presence payloads. That makes it small and keeps it compatible
with old and future games that already speak Discord IPC correctly.

References:

- [Discord RPC over IPC](https://docs.discord.com/developers/topics/rpc)
- [CodeWeavers bottle location notes](https://support.codeweavers.com/en_US/change-the-bottle-directory-in-crossover-mac)
- [EnderIce2/rpc-bridge](https://github.com/EnderIce2/rpc-bridge)

## Credits

Created by Zard Studios.

Copyright (c) 2026 Zard Studios. Released under the MIT License.
