# WineCord

Tiny Discord Rich Presence bridge for Windows games running on macOS through
Wine or CrossOver.

[![Homebrew tap](https://img.shields.io/badge/Homebrew-zard--studios%2Ftap-blue)](https://github.com/Zard-Studios/homebrew-tap)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

WineCord is built to stay boring in the best way: no Electron, no Node runtime,
no root daemon, no user-side build step. Install it, run setup, keep Discord for
macOS open, then launch your Windows game from CrossOver.

Languages / Lingue / Idiomas / Langues:
[English](#english) | [Italiano](#italiano) | [Español](#español) | [Français](#français)

---

## English

### Install

```sh
brew install zard-studios/tap/winecord
winecord setup
```

That is the normal setup. `winecord setup` installs the macOS background agent,
copies the Windows bridge into the detected CrossOver Steam bottle, writes the
local config, and registers the Wine service.

After setup, leave Discord for macOS open and start the Windows game from
CrossOver or Steam inside CrossOver.

### If the bottle is not detected

Use this when your Steam bottle is in a custom location, or when you want to set
up a different bottle:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

For multiple bottles, run the same command once per bottle:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/GameBottle"
```

### Check or debug

```sh
winecord doctor
winecord logs --follow
```

`winecord doctor` checks the local Discord socket, WineCord config, CrossOver
detection, installed helper, and LaunchAgent path.

`winecord logs --follow` shows live bridge activity. When a game really opens
Discord IPC, you will see the pipe connection and the first IPC frame. If a game
only uses Steam Rich Presence or Discord game detection, WineCord may have
nothing to forward.

### Uninstall

```sh
winecord uninstall
brew uninstall winecord
```

Run `winecord uninstall` first so WineCord can remove the LaunchAgent, Wine
service, helper, config, and logs while the CLI is still installed.

---

## Italiano

### Installazione

```sh
brew install zard-studios/tap/winecord
winecord setup
```

Questo è il setup normale. `winecord setup` installa l'agente macOS in
background, copia il bridge Windows nella bottle Steam di CrossOver rilevata,
scrive la configurazione locale e registra il servizio Wine.

Dopo il setup, tieni aperto Discord per macOS e avvia il gioco Windows da
CrossOver o da Steam dentro CrossOver.

### Se la bottle non viene rilevata

Usa questo comando quando la bottle Steam è in una posizione personalizzata, o
quando vuoi configurare una bottle diversa:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

Per più bottle, esegui lo stesso comando una volta per ogni bottle:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/GameBottle"
```

### Controllo e debug

```sh
winecord doctor
winecord logs --follow
```

`winecord doctor` controlla il socket locale di Discord, la configurazione di
WineCord, il rilevamento di CrossOver, l'helper installato e il percorso del
LaunchAgent.

`winecord logs --follow` mostra l'attività del bridge in tempo reale. Quando un
gioco apre davvero Discord IPC, vedrai la connessione alla pipe e il primo frame
IPC. Se un gioco usa solo Steam Rich Presence o il rilevamento gioco di Discord,
WineCord potrebbe non avere nulla da inoltrare.

### Disinstallazione

```sh
winecord uninstall
brew uninstall winecord
```

Esegui prima `winecord uninstall`, così WineCord può rimuovere LaunchAgent,
servizio Wine, helper, configurazione e log mentre la CLI è ancora installata.

---

## Español

### Instalación

```sh
brew install zard-studios/tap/winecord
winecord setup
```

Esta es la configuración normal. `winecord setup` instala el agente de macOS en
segundo plano, copia el bridge de Windows en la botella Steam de CrossOver
detectada, escribe la configuración local y registra el servicio de Wine.

Después de la configuración, deja Discord para macOS abierto e inicia el juego
de Windows desde CrossOver o desde Steam dentro de CrossOver.

### Si no se detecta la botella

Usa este comando si tu botella de Steam está en una ubicación personalizada, o
si quieres configurar otra botella:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

Para varias botellas, ejecuta el mismo comando una vez por cada botella:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/GameBottle"
```

### Comprobar o depurar

```sh
winecord doctor
winecord logs --follow
```

`winecord doctor` comprueba el socket local de Discord, la configuración de
WineCord, la detección de CrossOver, el helper instalado y la ruta del
LaunchAgent.

`winecord logs --follow` muestra la actividad del bridge en tiempo real. Cuando
un juego abre realmente Discord IPC, verás la conexión a la pipe y el primer
frame IPC. Si un juego solo usa Steam Rich Presence o la detección de juegos de
Discord, puede que WineCord no tenga nada que reenviar.

### Desinstalar

```sh
winecord uninstall
brew uninstall winecord
```

Ejecuta primero `winecord uninstall` para que WineCord pueda eliminar el
LaunchAgent, el servicio de Wine, el helper, la configuración y los logs mientras
la CLI sigue instalada.

---

## Français

### Installation

```sh
brew install zard-studios/tap/winecord
winecord setup
```

C'est la configuration normale. `winecord setup` installe l'agent macOS en
arrière-plan, copie le bridge Windows dans la bottle Steam de CrossOver détectée,
écrit la configuration locale et enregistre le service Wine.

Après la configuration, garde Discord pour macOS ouvert et lance le jeu Windows
depuis CrossOver ou depuis Steam dans CrossOver.

### Si la bottle n'est pas détectée

Utilise cette commande si ta bottle Steam se trouve dans un emplacement
personnalisé, ou si tu veux configurer une autre bottle:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

Pour plusieurs bottles, lance la même commande une fois par bottle:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/GameBottle"
```

### Vérifier ou déboguer

```sh
winecord doctor
winecord logs --follow
```

`winecord doctor` vérifie le socket local de Discord, la configuration WineCord,
la détection de CrossOver, le helper installé et le chemin du LaunchAgent.

`winecord logs --follow` affiche l'activité du bridge en direct. Quand un jeu
ouvre vraiment Discord IPC, tu verras la connexion à la pipe et la première
trame IPC. Si un jeu utilise seulement Steam Rich Presence ou la détection de
jeu de Discord, WineCord peut ne rien avoir à transmettre.

### Désinstallation

```sh
winecord uninstall
brew uninstall winecord
```

Lance d'abord `winecord uninstall` pour que WineCord puisse supprimer le
LaunchAgent, le service Wine, le helper, la configuration et les logs pendant
que la CLI est encore installée.

---

## Notes For Maintainers

Build and package:

```sh
make
make windows-helper
make package
```

The release tarball contains a universal macOS binary and the Windows helper:

```text
dist/winecord-0.1.4-macos-universal.tar.gz
```

Update the Homebrew tap after publishing a new package:

```sh
cp Formula/winecord.rb ../homebrew-tap/Formula/winecord.rb
cp dist/winecord-*.tar.gz ../homebrew-tap/releases/
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

## How It Works

Discord IPC uses Windows named pipes on Windows and Unix domain sockets on
macOS. Windows games usually look for `\\?\pipe\discord-ipc-{n}` or
`\\.\pipe\discord-ipc-{n}`, while Discord for macOS exposes `discord-ipc-{n}`
under the user's runtime/temp directories.

WineCord keeps the bridge transport-level: it forwards bytes without rewriting
Rich Presence payloads. That keeps it small and compatible with games that
already speak Discord IPC correctly.

Config:

```text
~/Library/Application Support/WineCord/config.ini
```

Logs:

```text
~/Library/Logs/WineCord/
drive_c/users/Public/WineCord/bridge.log
```

References:

- [Discord RPC over IPC](https://docs.discord.com/developers/topics/rpc)
- [CodeWeavers bottle location notes](https://support.codeweavers.com/en_US/change-the-bottle-directory-in-crossover-mac)
- [EnderIce2/rpc-bridge](https://github.com/EnderIce2/rpc-bridge)

## Credits

Created by Zard Studios.

Copyright (c) 2026 Zard Studios. Released under the MIT License.
