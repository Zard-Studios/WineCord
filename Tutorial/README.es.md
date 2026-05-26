# Tutorial de WineCord

[← Volver al README](../README.md)

## 🚀 Instalación

```sh
brew install zard-studios/tap/winecord
winecord setup
```

Ese es el flujo normal. `winecord setup` instala el LaunchAgent de macOS,
detecta automáticamente los prefijos Wine más comunes, copia el bridge de
Windows dentro de cada prefijo configurado, escribe el archivo de conexión
local y registra el servicio Wine.

Mantén Discord para macOS abierto y luego inicia el juego de Windows desde tu
aplicación Wine.

## 🧭 Detección Automática

WineCord busca prefijos ya inicializados en las ubicaciones habituales de macOS
usadas por CrossOver, Whisky, wrappers de Wineskin, Heroic, Wine estándar y
`~/.wine`.

En Whisky, instalar WhiskyCmd desde `Whisky > Install Whisky CLI...` le da a
WineCord la forma más limpia de ejecutar el helper dentro de una bottle
registrada. Si WhiskyCmd no está disponible, WineCord intenta usar el runner
Wine incluido.

## 🧩 Configuración Manual

Usa un prefijo manual solo cuando la detección automática no encuentre tu app.
Un prefijo es la carpeta que contiene `drive_c`.

```sh
winecord setup --prefix "/ruta/al/prefijo"
```

Si ese prefijo necesita un binario Wine específico:

```sh
winecord setup --prefix "/ruta/al/prefijo" --wine "/ruta/a/wine"
```

El antiguo flag de estilo CrossOver sigue funcionando:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

## 🧪 Comprobar O Depurar

```sh
winecord doctor
winecord logs --follow
```

`winecord doctor` revisa Discord IPC, el LaunchAgent, los prefijos detectados,
el runner Wine y el helper instalado.

`winecord logs --follow` muestra la actividad del bridge en tiempo real. Cuando
un juego abre Discord IPC, verás la conexión a la pipe y el primer frame IPC.
Si un juego solo usa Steam Rich Presence o la detección de juegos de Discord,
WineCord puede que todavía no tenga nada que reenviar.

Si Discord sigue mostrando un juego después de cerrarlo:

```sh
winecord clear
```

## 🧹 Desinstalación

```sh
winecord uninstall
brew uninstall winecord
```

Ejecuta primero `winecord uninstall`. Elimina el LaunchAgent, el servicio Wine,
el helper, la configuración dentro de los prefijos y los logs de WineCord
mientras la CLI sigue instalada.

Si un prefijo configurado está en una unidad externa, conecta esa unidad antes
de desinstalar. Cuando WineCord no puede acceder a un prefijo que ya había
configurado, conserva la configuración y los logs locales, te avisa y te pide
ejecutar `winecord uninstall` de nuevo cuando la unidad esté montada.

## ⬆️ Actualización

```sh
winecord update
```

Esto actualiza solo el tap de Homebrew de Zard Studios, actualiza WineCord y
refresca el LaunchAgent y el helper del lado Wine ya instalados. No actualiza
paquetes de Homebrew no relacionados. Si solo quieres actualizar el paquete de
Homebrew:

```sh
winecord update --no-setup
```

WineCord comprueba si existe una nueva release como máximo una vez al día y
muestra un aviso amarillo en la terminal cuando hay una actualización
disponible. Define `WINECORD_NO_UPDATE_CHECK=1` para desactivar esa
comprobación.

## 📁 Rutas

Configuración:

```text
~/Library/Application Support/WineCord/config.ini
```

Logs:

```text
~/Library/Logs/WineCord/
drive_c/users/Public/WineCord/bridge.log
```

## 🛠 Mantenimiento

```sh
make
make windows-helper
make package
```

El smoke test incluido espera el close frame `Invalid Client ID` de Discord. Es
útil: demuestra que los bytes pasaron por la named pipe de Windows, el helper
Wine, el agente de macOS y Discord.
