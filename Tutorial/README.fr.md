# Tutoriel WineCord

[← Retour au README](../README.md)

## 🚀 Installation

```sh
brew install zard-studios/tap/winecord
winecord setup
```

C'est la configuration normale. `winecord setup` détecte la bottle Steam de
CrossOver, installe l'agent macOS, copie le bridge Windows dans la bottle, écrit
la configuration locale et enregistre le service Wine.

Après la configuration, garde Discord pour macOS ouvert et lance le jeu Windows
depuis CrossOver ou depuis Steam dans CrossOver.

## 🍾 Si la bottle n'est pas détectée

Utilise cette commande si ta bottle Steam se trouve dans un emplacement
personnalisé :

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

Pour plusieurs bottles, lance la configuration une fois par bottle :

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/GameBottle"
```

## 🧪 Vérifier ou déboguer

```sh
winecord doctor
winecord logs --follow
```

`winecord doctor` vérifie le socket local de Discord, la configuration WineCord,
la détection de CrossOver, le helper installé et le chemin du LaunchAgent.

`winecord logs --follow` affiche l'activité du bridge en direct. Quand un jeu
ouvre Discord IPC, tu verras la connexion à la pipe et la première trame IPC. Si
un jeu utilise seulement Steam Rich Presence ou la détection de jeu de Discord,
WineCord peut ne rien avoir à transmettre.

Si Discord continue d'afficher un jeu après sa fermeture, lance :

```sh
winecord clear
```

Cela demande à Discord d'effacer la dernière activité vue par WineCord.

## 🧹 Désinstallation

```sh
winecord uninstall
brew uninstall winecord
```

Lance d'abord `winecord uninstall` pour que WineCord puisse supprimer le
LaunchAgent, le service Wine, le helper, la configuration et les logs pendant
que la CLI est encore installée.

Si ta bottle CrossOver se trouve sur un disque externe, connecte ce disque avant
la désinstallation. Quand WineCord ne peut pas accéder à une bottle qu'il avait
configurée, il conserve la configuration et les logs locaux, t'avertit et te
demande de relancer `winecord uninstall` après avoir monté le disque.

## 🛠 Notes pour les mainteneurs

Compiler et empaqueter :

```sh
make
make windows-helper
make package
```

Smoke test local:

```sh
make windows-smoke
CX_BOTTLE_PATH="/path/to/CrossOver/Bottles" \
  "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wine" \
  --bottle Steam --no-gui ./build/pipe-smoke.exe
```

La réponse attendue pour le smoke test inclus est le close frame `Invalid Client
ID` de Discord. C'est positif : cela prouve que les octets sont passés par la
named pipe Windows, le helper Wine, l'agent macOS et Discord.

## 📁 Chemins

Configuration:

```text
~/Library/Application Support/WineCord/config.ini
```

Logs:

```text
~/Library/Logs/WineCord/
drive_c/users/Public/WineCord/bridge.log
```
