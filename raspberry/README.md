# IC-7610 I/Q-Empfangstest auf dem Raspberry Pi 4

Nach der [Treiberinstallation](#ftdi-treiber-für-arm64) auf dem ARM64-Pi aus diesem Verzeichnis einmal `make` und danach `./iq-test` ausführen. Die USB-Gerätenummer `0c26:0029` muss mit `5000M` verbunden und für den ausführenden Benutzer les- und schreibbar sein. Der Test ist für den Empfangsbetrieb vorgesehen.

Das Programm prüft den I/Q-Strom auf SUB bei 18,118 MHz für zehn Sekunden. Es misst Byte- und Samplerate und zählt komplexe Samples, bei denen mindestens eine Komponente von null abweicht. Es fragt die ursprüngliche SUB-Frequenz, Dualwatch und I/Q-Quellenauswahl ab, stellt sie nach dem Test wieder her und kontrolliert die Werte. MAIN wird vor und nach der Messung gelesen. Die Samples werden im Speicher ausgewertet. `--probe` liest nur die aktive I/Q-Quelle. Mit `--sub-hz` lässt sich die Testfrequenz in Hz vorgeben.

Für einen Generator auf 28,200 MHz stellt dieses Beispiel SUB auf 28,150 MHz ein und schreibt einen I/Q-Ausschnitt nach stdout. Die Textausgabe geht dann auf stderr. Der Ausschnitt enthält verschachtelte vorzeichenbehaftete 16-Bit-I/Q-Samples in Little Endian bei 1,92 MS/s.

```sh
./iq-test --sub-hz 28150000 --snapshot > iq-snapshot.cs16
```

## FTDI-Treiber für ARM64

Vor dem ersten Build das [FTDI-D3XX-Archiv für ARMv8 1.0.16](https://ftdichip.cn/Drivers/D3XX/Linux/libftd3xx-linux-arm-v8-1.0.16.tgz) von FTDI herunterladen und entpacken. `ftd3xx.h`, `Types.h` und `libftd3xx.so` unter `raspberry/ftdi/` ablegen. Das Makefile bindet die Bibliothek dort ein.

Die USB-Anbindung folgt der [Icom I/Q-Referenz](https://www.icomjapan.com/support/manual/1792/) und dem Beispiel [ic7610ftdi von DF7CB](https://github.com/df7cb/ic7610ftdi).

## I/Q über Ethernet

Der Pi lauscht standardmäßig auf `192.168.10.140:50010`. Eine andere lokale Pi-Adresse mit `--listen IPv4` angeben. Ein TCP-Verbindungsaufbau startet die I/Q-Ausgabe. Pro Sitzung empfängt ein Arbeitsplatz. Bei Verbindungsende prüft der Pi die Wiederherstellung der temporären SUB-Einstellungen. Der Pi verwendet die beim Start angegebene SUB-Frequenz als Ausgangswert für neue Sitzungen.

Auf dem Pi:

```sh
make
./iq-test --serve --sub-hz 28150000
```

Am Arbeitsplatz:

```sh
python3 iq-receive.py 192.168.10.140 --seconds 10
```

`--output DATEI` schreibt die Samples ohne Netzwerkrahmen als CS16LE in eine neue Datei. Mit `--output -` gehen die Samples nach stdout. `--seconds 0` empfängt bis zum Abbruch. `--expect-carrier-hz 28200000` prüft den stärksten Träger beim Start, etwa einmal pro Minute und am Ende. Für diese Option wird NumPy am Arbeitsplatz benötigt.

### Live-Spektrum am Arbeitsplatz

```sh
python3 iq-receive.py --spectrum
```

Im Feld **SUB (MHz)** die gewünschte Frequenz eingeben und **Setzen** drücken. Der Pi liest die neue Frequenz vom IC-7610 zurück. Mit dem nächsten I/Q-Block auf der neuen Frequenz übernimmt das Fenster die neue Achse. Die Einstellung gilt für die laufende TCP-Sitzung. Beim Trennen stellt der Pi den Ausgangszustand wieder her.

Im Terminal setzt beispielsweise `python3 iq-receive.py --sub-hz 28100000 --seconds 10` SUB für eine zehnsekündige Messung auf 28,100 MHz.

Das Fenster benötigt NumPy, Matplotlib und ein grafisches Matplotlib-Backend. Die Frequenzachse verwendet die vom Pi gemeldete SUB-Mitte. Angezeigt werden die nutzbaren 1,66 MHz, der relative Pegel in dBFS und die Frequenz des stärksten Signals. **DC ausblenden** entfernt den Mittelwert der I/Q-Samples. Die Option ist standardmäßig aus, damit ein Träger auf der SUB-Mittenfrequenz sichtbar bleibt. Wenn das Fenster geschlossen wird, endet die TCP-Sitzung. Der Pi bedient jeweils einen Empfänger.

### TCP-Format

Die Zahlenfelder des Protokolls sind Big Endian. Der Stream beginnt mit 24 Byte. Der Kopf enthält die acht ASCII-Bytes `IC76IQ01`, die Samplefrequenz in komplexen Samples/s als u32, den Formatcode 1 (CS16LE) als u32 und die SUB-Frequenz in Hz als u64.

Auf jeden Blockkopf aus u64-Startindex (komplexe Samples seit Sitzungsbeginn) und u32-Nutzlänge in Byte folgen die I/Q-Samples. Jeder Block enthält höchstens 262.144 Byte. Ein komplexes Sample besteht aus vier Byte in der Folge I, Q (jeweils int16 Little Endian).

### Steuerbefehle vom Arbeitsplatz

ASCII-Zeilen mit LF am Ende gehen über dieselbe TCP-Verbindung in die Gegenrichtung. Die Zeile darf bis zu 63 druckbare Zeichen enthalten.

| Befehl | Funktion |
| --- | --- |
| `GET SUB` | Die zuletzt vom Pi bestätigte SUB-Mittenfrequenz lesen. |
| `SET SUB 28100000` | SUB auf 28,100 MHz setzen. Der Wert ist eine Ganzzahl in Hz. |

Die Antwort steht zwischen zwei I/Q-Blöcken. Ihr 12-Byte-Blockkopf enthält den Index des nächsten Samples und eine Nutzlänge von null. Darauf folgen u32-Status und u64-SUB-Frequenz in Hz (Big Endian). Status 0 bedeutet bestätigt, 1 steht für einen ungültigen Befehl oder eine ungültige Frequenz und 2 für Sendebetrieb. Die Antwort ist der Umschaltpunkt für die Frequenzachse. Beim Retune pausiert der USB-Datenstrom kurz und der Sampleindex zählt danach die übertragenen Samples weiter. Verbindungen, die keine Steuerbefehle senden, empfangen weiterhin das ursprüngliche Format.
