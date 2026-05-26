# Tutorial WineCord

[← Torna al README](../README.md)

## 🚀 Installazione

```sh
brew install zard-studios/tap/winecord
winecord setup
```

Questo è il setup normale. `winecord setup` rileva la bottle Steam di CrossOver,
installa l'agente macOS, copia il bridge Windows nella bottle, scrive la
configurazione locale e registra il servizio Wine.

Dopo il setup, tieni aperto Discord per macOS e avvia il gioco Windows da
CrossOver o da Steam dentro CrossOver.

## 🍾 Se la bottle non viene rilevata

Usa questo comando quando la bottle Steam si trova in una posizione
personalizzata:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

Per più bottle, esegui il setup una volta per ogni bottle:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/GameBottle"
```

## 🧪 Controllo e debug

```sh
winecord doctor
winecord logs --follow
```

`winecord doctor` controlla il socket locale di Discord, la configurazione di
WineCord, il rilevamento di CrossOver, l'helper installato e il percorso del
LaunchAgent.

`winecord logs --follow` mostra l'attività del bridge in tempo reale. Quando un
gioco apre Discord IPC, vedrai la connessione alla pipe e il primo frame IPC. Se
un gioco usa solo Steam Rich Presence o il rilevamento gioco di Discord,
WineCord potrebbe non avere nulla da inoltrare.

Se Discord continua a mostrare un gioco dopo la chiusura, esegui:

```sh
winecord clear
```

Questo chiede a Discord di cancellare l'ultima attività vista da WineCord.

## 🧹 Disinstallazione

```sh
winecord uninstall
brew uninstall winecord
```

Esegui prima `winecord uninstall`, così WineCord può rimuovere LaunchAgent,
servizio Wine, helper, configurazione e log mentre la CLI è ancora installata.

Se la bottle di CrossOver si trova su un disco esterno, collega quel disco prima
della disinstallazione. Quando WineCord non riesce ad accedere a una bottle che
aveva configurato, mantiene configurazione e log locali, ti avvisa e ti chiede
di rieseguire `winecord uninstall` dopo aver montato il disco.

## 🛠 Note per manutentori

Build e pacchetto:

```sh
make
make windows-helper
make package
```

Smoke test locale:

```sh
make windows-smoke
CX_BOTTLE_PATH="/path/to/CrossOver/Bottles" \
  "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wine" \
  --bottle Steam --no-gui ./build/pipe-smoke.exe
```

La risposta prevista per lo smoke test incluso è il close frame `Invalid Client
ID` di Discord. È positivo: dimostra che i byte sono passati dalla named pipe
Windows all'helper Wine, poi all'agente macOS e infine a Discord.

## 📁 Percorsi

Configurazione:

```text
~/Library/Application Support/WineCord/config.ini
```

Log:

```text
~/Library/Logs/WineCord/
drive_c/users/Public/WineCord/bridge.log
```
