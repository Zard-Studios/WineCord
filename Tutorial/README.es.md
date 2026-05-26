# Tutorial de WineCord

[← Volver al README](../README.md)

## 🚀 Instalación

```sh
brew install zard-studios/tap/winecord
winecord setup
```

Esta es la configuración normal. `winecord setup` detecta la botella Steam de
CrossOver, instala el agente de macOS, copia el bridge de Windows dentro de la
botella, escribe la configuración local y registra el servicio de Wine.

Después de la configuración, deja Discord para macOS abierto e inicia el juego
de Windows desde CrossOver o desde Steam dentro de CrossOver.

## 🍾 Si no se detecta la botella

Usa este comando si tu botella de Steam está en una ubicación personalizada:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

Para varias botellas, ejecuta la configuración una vez por cada botella:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/GameBottle"
```

## 🧪 Comprobar o depurar

```sh
winecord doctor
winecord logs --follow
```

`winecord doctor` comprueba el socket local de Discord, la configuración de
WineCord, la detección de CrossOver, el helper instalado y la ruta del
LaunchAgent.

`winecord logs --follow` muestra la actividad del bridge en tiempo real. Cuando
un juego abre Discord IPC, verás la conexión a la pipe y el primer frame IPC. Si
un juego solo usa Steam Rich Presence o la detección de juegos de Discord, puede
que WineCord no tenga nada que reenviar.

Si Discord sigue mostrando un juego después de cerrarlo, ejecuta:

```sh
winecord clear
```

Esto le pide a Discord que borre la última actividad que vio WineCord.

## 🧹 Desinstalar

```sh
winecord uninstall
brew uninstall winecord
```

Ejecuta primero `winecord uninstall` para que WineCord pueda eliminar el
LaunchAgent, el servicio de Wine, el helper, la configuración y los logs mientras
la CLI sigue instalada.

Si tu botella de CrossOver está en una unidad externa, conecta esa unidad antes
de desinstalar. Cuando WineCord no puede acceder a una botella que configuró
antes, conserva la configuración y los logs locales, te avisa y te pide ejecutar
`winecord uninstall` otra vez después de montar la unidad.

## 🛠 Notas para mantenedores

Compilar y empaquetar:

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

La respuesta esperada para el smoke test incluido es el close frame `Invalid
Client ID` de Discord. Es una buena señal: demuestra que los bytes pasaron por
la named pipe de Windows, el helper de Wine, el agente de macOS y Discord.

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
