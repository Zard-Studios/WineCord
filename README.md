# WineCord

WineCord is a tiny macOS bridge for Discord Rich Presence from Windows games
running through Wine or CrossOver.

It is built as two small pieces:

- `winecord`: a native macOS agent/CLI that listens only on `127.0.0.1`, finds
  the local Discord IPC socket, and forwards bytes to Discord.
- `winecord-bridge.exe`: a Windows helper that runs inside a Wine/CrossOver
  bottle, creates `\\.\pipe\discord-ipc-0..9`, and forwards pipe traffic to the
  macOS agent using a local token.

No Electron, no Node runtime, no background service with root privileges, and
no build step for normal users.

## Install

The Homebrew package is prepared, but the public tap must exist before this
command works. The tap repository must be:

```text
https://github.com/Zard-Studios/homebrew-tap
```

Once that repository exists and contains `Formula/winecord.rb`:

```sh
brew install zard-studios/tap/winecord
winecord setup
```

Later, if WineCord is accepted into Homebrew core, the install command becomes:

```sh
brew install winecord
winecord setup
```

For a single command that installs through Homebrew and immediately configures
the detected CrossOver Steam bottle after the tap is published:

```sh
curl -fsSL https://raw.githubusercontent.com/Zard-Studios/WineCord/main/scripts/install.sh | sh
```

If your bottle is not auto-detected:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

`winecord setup` installs the per-user LaunchAgent, starts the native macOS
agent, copies the bundled `winecord-bridge.exe` into the CrossOver bottle,
writes the shared token config, and registers the Wine service. Users do not
need MinGW, Xcode, or any build tool.

### Publishing The Tap

Maintainers need to do this once:

1. Create the GitHub repository `Zard-Studios/homebrew-tap`.
2. Copy [Formula/winecord.rb](Formula/winecord.rb) into
   `homebrew-tap/Formula/winecord.rb`.
3. Publish a WineCord release with
   `dist/winecord-0.1.4-macos-universal.tar.gz`.
4. Verify:

```sh
brew install zard-studios/tap/winecord
winecord setup
```

## Uninstall

```sh
winecord uninstall
brew uninstall winecord
```

`winecord uninstall` removes the LaunchAgent, the Wine service, the helper from
the bottle, WineCord config, and WineCord logs. Use it before `brew uninstall`
so the bottle is cleaned while the CLI is still available.

## Commands

```sh
winecord setup             # one-command local configuration
winecord uninstall         # remove LaunchAgent, bottle helper, config, and logs
winecord doctor            # show config, Discord IPC, and CrossOver diagnostics
winecord logs --follow     # watch WineCord agent and bridge activity live
winecord agent             # run the macOS forwarding agent in the foreground
```

Advanced commands are still available for package managers and diagnostics:

```sh
winecord install-agent
winecord uninstall-agent
winecord start
winecord stop
winecord install-bottle --bottle PATH [--no-register]
```

## Build From Source

```sh
make
make windows-helper
make package
```

The release tarball contains a universal macOS binary and the Windows helper:
`dist/winecord-0.1.4-macos-universal.tar.gz`.

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

The Windows bridge log also lives inside the bottle:

```text
drive_c/users/Public/WineCord/bridge.log
```

When a Windows game really opens Discord IPC, `winecord logs --follow` shows the
pipe connection and the first IPC frame. That makes it clear whether WineCord is
receiving Discord RPC traffic or the game is only using Steam/game activity
detection.

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
