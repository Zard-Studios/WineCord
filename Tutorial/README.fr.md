# Tutoriel WineCord

[← Retour au README](../README.md)

## 🚀 Installation

```sh
brew install zard-studios/tap/winecord
winecord setup
```

C'est le flux normal. `winecord setup` installe le LaunchAgent macOS, détecte
automatiquement les préfixes Wine courants, copie le bridge Windows dans chaque
préfixe configuré, écrit le fichier de connexion local et enregistre le service
Wine.

Garde Discord pour macOS ouvert, puis lance le jeu Windows depuis ton app Wine.

## 🧭 Détection Automatique

WineCord recherche les préfixes déjà initialisés dans les emplacements macOS les
plus courants pour CrossOver, Whisky, les wrappers Wineskin, Heroic, Wine
standard et `~/.wine`.

Pour Whisky, installer WhiskyCmd depuis `Whisky > Install Whisky CLI...` donne
à WineCord la méthode la plus propre pour exécuter le helper dans une bottle
enregistrée. Si WhiskyCmd n'est pas disponible, WineCord essaie quand même le
runner Wine inclus.

## 🧩 Configuration Manuelle

Utilise un préfixe manuel seulement si la détection automatique ne trouve pas
ton app. Un préfixe est le dossier qui contient `drive_c`.

```sh
winecord setup --prefix "/chemin/du/prefixe"
```

Si ce préfixe nécessite un binaire Wine précis :

```sh
winecord setup --prefix "/chemin/du/prefixe" --wine "/chemin/vers/wine"
```

L'ancien flag de style CrossOver reste compatible :

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

## 🧪 Vérifier Ou Déboguer

```sh
winecord doctor
winecord logs --follow
```

`winecord doctor` vérifie Discord IPC, le LaunchAgent, les préfixes détectés,
le runner Wine et le helper installé.

`winecord logs --follow` affiche l'activité du bridge en temps réel. Quand un
jeu ouvre Discord IPC, tu verras la connexion à la pipe et le premier frame IPC.
Si un jeu utilise seulement Steam Rich Presence ou la détection de jeu de
Discord, WineCord peut ne rien avoir à transférer pour l'instant.

Si Discord continue d'afficher un jeu après sa fermeture :

```sh
winecord clear
```

## 🧹 Désinstallation

```sh
winecord uninstall
brew uninstall winecord
```

Lance d'abord `winecord uninstall`. Cela supprime le LaunchAgent, le service
Wine, le helper, la configuration dans les préfixes et les logs WineCord tant
que la CLI est encore installée.

Si un préfixe configuré se trouve sur un disque externe, connecte ce disque
avant la désinstallation. Quand WineCord ne peut pas accéder à un préfixe déjà
configuré, il conserve la configuration et les logs locaux, t'avertit et te
demande de relancer `winecord uninstall` une fois le disque monté.

## ⬆️ Mise À Jour

```sh
winecord update
```

Cette commande rafraîchit uniquement le tap Homebrew de Zard Studios, met
WineCord à jour et rafraîchit le LaunchAgent ainsi que le helper côté Wine déjà
installés. Elle ne met pas à jour les paquets Homebrew sans lien avec WineCord.
Si tu veux seulement mettre à jour le paquet Homebrew :

```sh
winecord update --no-setup
```

WineCord vérifie l'existence d'une nouvelle release au maximum une fois par
jour et affiche un avertissement jaune dans le terminal quand une mise à jour
est disponible. Définis `WINECORD_NO_UPDATE_CHECK=1` pour désactiver cette
vérification.

## 📁 Chemins

Configuration :

```text
~/Library/Application Support/WineCord/config.ini
```

Logs :

```text
~/Library/Logs/WineCord/
drive_c/users/Public/WineCord/bridge.log
```

## 🛠 Maintenance

```sh
make
make windows-helper
make package
```

Le smoke test inclus attend le close frame `Invalid Client ID` de Discord.
C'est utile : cela prouve que les octets sont passés par la named pipe Windows,
le helper Wine, l'agent macOS et Discord.
