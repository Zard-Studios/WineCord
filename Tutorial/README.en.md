# WineCord Tutorial

[← Back to README](../README.md)

## 🚀 Install

```sh
brew install zard-studios/tap/winecord
winecord setup
```

That is the normal flow. `winecord setup` installs the macOS LaunchAgent,
finds common Wine prefixes automatically, copies the Windows bridge into each
configured prefix, writes the local connection file, and registers the Wine
service.

Keep Discord for macOS open, then launch the Windows game from your Wine app.

## 🧭 Automatic Detection

WineCord looks for initialized prefixes in the common macOS locations used by
CrossOver, Whisky, Wineskin wrappers, Heroic, regular Wine, and `~/.wine`.

For Whisky, installing WhiskyCmd from `Whisky > Install Whisky CLI...` gives
WineCord the cleanest way to run the helper inside a registered Whisky bottle.
If WhiskyCmd is not available, WineCord still tries the bundled Wine runner.

## 🧩 Manual Setup

Use a manual prefix only when auto-detection misses your app. A prefix is the
folder that contains `drive_c`.

```sh
winecord setup --prefix "/path/to/prefix"
```

If that prefix needs a specific Wine binary:

```sh
winecord setup --prefix "/path/to/prefix" --wine "/path/to/wine"
```

The older CrossOver-style flag is still accepted:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

## 🧪 Check Or Debug

```sh
winecord doctor
winecord logs --follow
```

`winecord doctor` checks Discord IPC, the LaunchAgent, detected prefixes, the
Wine runner, and the installed helper.

`winecord logs --follow` shows live bridge activity. When a game opens Discord
IPC, you will see the pipe connection and the first IPC frame. If a game only
uses Steam Rich Presence or Discord game detection, WineCord may have nothing
to forward yet.

If Discord keeps showing a game after it closes:

```sh
winecord clear
```

## 🧹 Uninstall

```sh
winecord uninstall
brew uninstall winecord
```

Run `winecord uninstall` first. It removes the LaunchAgent, Wine service,
helper, prefix configuration, and WineCord logs while the CLI is still
installed.

If a configured prefix is on an external drive, connect that drive before
uninstalling. When WineCord cannot access a prefix it previously configured, it
keeps the local config and logs, warns you, and asks you to run `winecord
uninstall` again after the drive is mounted.

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

## 🛠 Maintainers

```sh
make
make windows-helper
make package
```

The bundled smoke test expects Discord's `Invalid Client ID` close frame. That
is useful: it proves bytes traveled through the Windows named pipe, the Wine
helper, the macOS agent, and Discord.
