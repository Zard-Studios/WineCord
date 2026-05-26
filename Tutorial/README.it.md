# Tutorial WineCord

[← Torna al README](../README.md)

## 🚀 Installazione

```sh
brew install zard-studios/tap/winecord
winecord setup
```

Questo è il flusso normale. `winecord setup` installa il LaunchAgent macOS,
trova automaticamente i prefissi Wine più comuni, copia il bridge Windows in
ogni prefisso configurato, scrive il file di connessione locale e registra il
servizio Wine.

Tieni aperto Discord per macOS, poi avvia il gioco Windows dalla tua app Wine.

## 🧭 Rilevamento Automatico

WineCord cerca prefissi già inizializzati nelle posizioni macOS usate più
spesso da CrossOver, Whisky, wrapper Wineskin, Heroic, Wine standard e
`~/.wine`.

Per Whisky, installare WhiskyCmd da `Whisky > Install Whisky CLI...` dà a
WineCord il modo più pulito per eseguire l'helper dentro una bottle registrata.
Se WhiskyCmd non è disponibile, WineCord prova comunque il runner Wine incluso.

## 🧩 Setup Manuale

Usa un prefisso manuale solo quando il rilevamento automatico non trova la tua
app. Un prefisso è la cartella che contiene `drive_c`.

```sh
winecord setup --prefix "/percorso/del/prefisso"
```

Se quel prefisso richiede un binario Wine specifico:

```sh
winecord setup --prefix "/percorso/del/prefisso" --wine "/percorso/di/wine"
```

Il vecchio flag in stile CrossOver resta supportato:

```sh
winecord setup --bottle "/path/to/CrossOver/Bottles/Steam"
```

## 🧪 Controllo E Debug

```sh
winecord doctor
winecord logs --follow
```

`winecord doctor` controlla Discord IPC, il LaunchAgent, i prefissi rilevati,
il runner Wine e l'helper installato.

`winecord logs --follow` mostra l'attività del bridge in tempo reale. Quando un
gioco apre Discord IPC, vedrai la connessione alla pipe e il primo frame IPC.
Se un gioco usa solo Steam Rich Presence o il rilevamento gioco di Discord,
WineCord potrebbe non avere ancora nulla da inoltrare.

Se Discord continua a mostrare un gioco dopo la chiusura:

```sh
winecord clear
```

## 🧹 Disinstallazione

```sh
winecord uninstall
brew uninstall winecord
```

Esegui prima `winecord uninstall`. Rimuove LaunchAgent, servizio Wine, helper,
configurazione nei prefissi e log di WineCord mentre la CLI è ancora
installata.

Se un prefisso configurato si trova su un disco esterno, collega quel disco
prima della disinstallazione. Quando WineCord non riesce ad accedere a un
prefisso già configurato, mantiene configurazione e log locali, ti avvisa e ti
chiede di rieseguire `winecord uninstall` dopo aver montato il disco.

## ⬆️ Aggiornamento

```sh
winecord update
```

Questo aggiorna solo il tap Homebrew di Zard Studios, aggiorna WineCord e
rinfresca LaunchAgent e helper lato Wine già installati. Non aggiorna pacchetti
Homebrew non collegati. Se vuoi aggiornare solo il pacchetto Homebrew:

```sh
winecord update --no-setup
```

WineCord controlla se esiste una nuova release al massimo una volta al giorno e
mostra un warning giallo nel terminale quando è disponibile un aggiornamento.
Imposta `WINECORD_NO_UPDATE_CHECK=1` per disattivare quel controllo.

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

## 🛠 Manutenzione

```sh
make
make windows-helper
make package
```

Lo smoke test incluso si aspetta il close frame `Invalid Client ID` di Discord.
È utile: dimostra che i byte sono passati dalla named pipe Windows all'helper
Wine, poi all'agente macOS e infine a Discord.
