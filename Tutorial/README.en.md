# WineCord Tutorial

[← Back to README](../README.md)

## 🚀 Install

```sh
brew install zard-studios/tap/winecord
winecord setup
```

This is the normal setup. `winecord setup` detects the CrossOver Steam bottle,
installs the macOS agent, copies the Windows bridge into the bottle, writes the
local configuration, and registers the Wine service.

After setup, keep Discord for macOS open and launch the Windows game from
CrossOver or from Steam inside CrossOver.

## 🍾 If the bottle is not detected

Use this command when your Steam bottle is stored in a custom location:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

For multiple bottles, run setup once per bottle:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/GameBottle"
```

## 🧪 Check or debug

```sh
winecord doctor
winecord logs --follow
```

`winecord doctor` checks the local Discord socket, WineCord configuration,
CrossOver detection, installed helper, and LaunchAgent path.

`winecord logs --follow` shows live bridge activity. When a game opens Discord
IPC, you will see the pipe connection and the first IPC frame. If a game only
uses Steam Rich Presence or Discord game detection, WineCord may have nothing to
forward.

If Discord keeps showing a game after it has closed, run:

```sh
winecord clear
```

This asks Discord to clear the last activity WineCord saw.

## 🧹 Uninstall

```sh
winecord uninstall
brew uninstall winecord
```

Run `winecord uninstall` first so WineCord can remove the LaunchAgent, Wine
service, helper, configuration, and logs while the CLI is still installed.

If your CrossOver bottle is on an external drive, connect that drive before
uninstalling. When WineCord cannot access a bottle it previously configured, it
keeps the local config and logs, warns you, and asks you to run `winecord
uninstall` again after the drive is mounted.

## 🛠 Maintainer notes

Build and package:

```sh
make
make windows-helper
make package
```

Useful local smoke test:

```sh
make windows-smoke
CX_BOTTLE_PATH="/path/to/CrossOver/Bottles" \
  "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wine" \
  --bottle Steam --no-gui ./build/pipe-smoke.exe
```

The expected response for the bundled smoke test is Discord's `Invalid Client
ID` close frame. That is good: it proves bytes traveled through the Windows
named pipe, the Wine helper, the macOS agent, and Discord.

## 📁 Paths

Config:

```text
~/Library/Application Support/WineCord/config.ini
```

Logs:

```text
~/Library/Logs/WineCord/
drive_c/users/Public/WineCord/bridge.log
```
