# IC-7610 über LAN abfragen

Unter Linux mit einem C++20-Compiler und Make bauen und starten.

```sh
make
./ic7610-status
```

Der Start ohne Argumente verwendet die Umgebungsvariablen `KMCONTROL_ICOM_USER` und `KMCONTROL_ICOM_PASSWORD`. Bei einem expliziten Benutzernamen fragt das Programm das Kennwort am Terminal verdeckt ab oder liest die erste Zeile von stdin. Die Zieladresse ist `192.168.10.133`. Der voreingestellte Control-Port ist UDP 50001. Bei abweichender Konfiguration kann er mit `--port PORT` angegeben werden. `--verbose` protokolliert Paketgrößen, Typen und Sequenzen.

Das Funkgerät muss im LAN erreichbar und der integrierte Server unter **MENU → SET → Network → Network Control** verfügbar sein. Der Client meldet sich an, öffnet den ausgehandelten CI-V-Kanal und sendet nur die CI-V-Leseanfragen `03` (Frequenz), `04` (Betriebsart) und `1A 06` (DATA-Modus). Die Sitzung wird danach geschlossen. Der Client öffnet keinen Audio-Stream und sendet keine PTT- oder CI-V-Schreibbefehle.

Bei einer erfolgreichen Abfrage druckt das Programm Frequenz in Hz und Betriebsart. Bei einem Verbindungs- oder Protokollfehler endet es mit Status 1.

Protokollquellen sind die [Icom CI-V-Referenz](https://www.icomjapan.com/support/manual/1745/), die [wfview-Paketdefinitionen](https://github.com/wf-group/wfview/blob/fb79cd09ec334d06c923b8f087eed2dc77caf2ab/include/packettypes.h), die [CardSat-Protokollnotizen](https://github.com/prstoetzer/CardSat/blob/8933bcfe7a5c208d143c0b3bb945434dfd80023e/docs/interfaces/ICOM_LAN_PROTOCOL.md) und die [IC-7610-Mitschnitte von RigPlane](https://github.com/rigplane/rigplane-core/blob/fc301e1fd76debd638bdf8080670001212165437/tests/fixtures/ic7610/README.md).

## Backend und WebGUI

Das Backend und die HTTP-/WebSocket-Bridge sind C++20-Programme. Der Build verwendet CMake, Boost, Qt 5.15, FFTW für einfache Genauigkeit (fftw3f) und LiquidDSP. CMake sucht LiquidDSP in den üblichen Systempfaden. Für eine separate Installation kann `-DLIQUID_ROOT=/pfad/zu/liquid-dsp` gesetzt werden.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Das Backend benötigt die Anmeldedaten des IC-7610. In Bash lässt sich das Kennwort verdeckt einlesen und nur an den Backend-Prozess weitergeben.

```bash
read -r -p 'IC-7610 Benutzername: ' KMCONTROL_ICOM_USER
read -r -s -p 'IC-7610 Kennwort: ' KMCONTROL_ICOM_PASSWORD
printf '\n'
KMCONTROL_ICOM_USER="$KMCONTROL_ICOM_USER" KMCONTROL_ICOM_PASSWORD="$KMCONTROL_ICOM_PASSWORD" ./build/kmcontrol-backend
```

In einem zweiten Terminal `./build/kmcontrol-web` starten und [kmcontrol im Browser öffnen](http://127.0.0.1:8086/). `kmcontrol-web --port PORT` wählt einen anderen lokalen HTTP-/WebSocket-Port. Die Bridge lauscht auf dem Rechner mit dem Backend unter `127.0.0.1`. Sie liefert die eingebetteten Browserdateien und verbindet `/ws` mit dem TCP-Backend auf `127.0.0.1:50020`. Die Browseroberfläche gibt Bedienbefehle als JSON weiter und zeichnet beide FFT-Ansichten mit Canvas.

Der IC-7610 hat pro Backend einen Bedienplatz. Das Backend gibt den I/Q-Strom beim Schließen des aktiven Browser-Clients an den Pi zurück. Der Backend-Parameter `--pi HOST` übersteuert beim Start die gespeicherte Pi-Adresse. Die Vorgaben für IC-7610 und Pi sind 192.168.10.133 und 192.168.10.140.

### Bedienung

Die Anzeige zeigt MAIN und SUB oben, die aufklappbaren Einstellungen links und Fullband und Zoom rechts. Auf schmalen Bildschirmen stehen die Einstellungen über den Diagrammen und sind beim Laden eingeklappt. Die WebGUI wählt beim ersten Start 40 m. Ein Link wie `/?band=80m` öffnet direkt das 80-m-Band. Die letzte Bandauswahl bleibt im Browser gespeichert.

Ein Klick oder Tipp in das Spektrum oder den Wasserfall setzt MAIN auf die Frequenz an der gewählten Position. Das Mausrad verstellt MAIN je Rastschritt um 50 Hz. MAIN-Anzeige und Markierungen folgen sofort der Softwarefrequenz. Ein eigener Thread synchronisiert sie per CI-V mit dem Funkgerät und liest Änderungen am Abstimmknopf zurück. SUB und seine I/Q-Mitte folgen der Bandauswahl. Das Backend stimmt SUB über den Pi ab und wertet die bestätigte Mittenfrequenz aus.

Für beide Ansichten wertet das Backend dieselbe Hann-gefensterte FFT mit 131.072 komplexen I/Q-Samples aus. Bei 1,92 MS/s beträgt der Binabstand rund 14,65 Hz. Die FFT-Fenster überlappen sich. Pro 50 ms I/Q-Daten entsteht ein Spektrenpaar. Fullband zeigt den Maximalpegel je Anzeigespalte. Der Zoom zeigt die FFT-Bins im Bereich MAIN ±30 kHz, auf den nächstgelegenen Bin zentriert. Die Spektrumkurve mittelt die lineare Leistung über bis zu acht Messungen und zeigt das Ergebnis in dBFS an. Die Wasserfälle schieben ihre vorhandenen Pixel zeilenweise nach unten. Beim Abstimmen richtet der Zoom den Verlauf an den ursprünglichen Empfangsfrequenzen aus.

Im Menü **Wasserfall** setzen **Untere Schwelle** und **Obere Schwelle** den gemeinsamen dBFS-Anzeigebereich und färben vorhandene Zeilen entsprechend ein. Der Browser speichert beide Schwellen pro Band und stellt sie beim Bandwechsel wieder her. Für Bänder ohne eigene Werte gelten die früher global gespeicherten Schwellen oder standardmäßig −120 und −25 dBFS. Die **Diagrammfarbe** gilt für alle Bänder und für beide Spektren und Wasserfälle.

**Audio einschalten** aktiviert den Empfangston im Browser. Unter **Setup → Wiedergabegerät** stehen der Systemstandard und die vom Browser freigegebenen lokalen Audioausgänge zur Auswahl. Die Auswahl schaltet auch bei laufendem Empfang sofort um und wird im Browser gespeichert.

Wenn Chromium zunächst nur den Systemstandard anzeigt, öffnet **Soundkarten freigeben …** den Freigabedialog für die Geräteliste. Chromium verwendet hierfür die Mikrofonberechtigung. kmcontrol schließt den Mikrofonstream sofort und verarbeitet daraus keine Audiodaten. Browser mit einem eigenen Freigabedialog für Audioausgänge bieten **Wiedergabegerät freigeben …** an. Browser ohne direkte Geräteauswahl verwenden den Systemstandard.

Die IPv4-Adressen des IC-7610 und des Raspberry Pi stehen ebenfalls direkt unter **Setup**. Enter oder das Verlassen des jeweiligen Felds übernimmt eine gültige Adresse und baut die entsprechende Verbindung neu auf. Das Backend speichert Adressen und Lautstärke benutzerspezifisch. Es speichert USB oder LSB pro Band und stellt die Auswahl beim Bandwechsel wieder her. Für Bänder ohne eigene Einstellung gilt die früher global gespeicherte Auswahl oder standardmäßig LSB.

Das Backend mischt die Differenz zwischen der MAIN-Softwarefrequenz und der bestätigten SUB-Frequenz aus dem I/Q-Strom auf Basisband. LiquidDSP dezimiert auf 12 kSamples/s, filtert das gewählte Seitenband im Bereich von ungefähr 300 bis 2700 Hz und erzeugt daraus Mono-Audio mit 48 kSamples/s. Die automatische Verstärkungsregelung nutzt den mittleren Pegel hinter dem Seitenbandfilter und kann schwache Signale um bis zu 72 dB anheben. Ein separater Spitzenbegrenzer schützt die Audioausgabe vor Übersteuerung.

Die Empfangsausschnitte folgen dem [IARU-Region-1-HF-Bandplan](https://www.iaru-r1.org/wp-content/uploads/2019/08/hf_r1_bandplan.pdf). 10 m verwendet die gewünschten Anzeigeabschnitte 28–29 und 29–30 MHz. Der CB-Ausschnitt umfasst [alle 80 deutschen CB-Kanäle](https://www.bundesnetzagentur.de/865198). Die Ränder sind mit 5 bis 25 kHz Reserve schattiert. Bei 160 bis 12 m liegt die Mitte 15 kHz unter dem unteren Anzeigerand. Bei 10 m und CB liegt die Mitte innerhalb des Ausschnitts. Bei CB ist sie mit 26,620 MHz nahe am unteren Bandrand.

### TCP-Schnittstelle der Web-Bridge

Das Backend lauscht auf `127.0.0.1:50020` (TCP). Nachrichten sind UTF-8-JSON-Objekte, jeweils mit einem LF abgeschlossen. Frequenzen werden in Hz angegeben.

- Nach Verbindungsaufbau sendet das Backend `{"type":"bands","version":1,"zoom_half_span_hz":30000,"bands":[…]}`. Jedes Band enthält `id`, `label`, `low_hz`, `high_hz`, `display_low_hz`, `display_high_hz` und `center_hz`.
- Die GUI wählt ein Band mit `{"op":"band","id":"40m"}`. Die anderen IDs kommen aus der Bandliste.
- Der Status hat die Form `{"type":"state","phase":"live","band":"40m","text":"Live"}`. Phasen sind `idle`, `connecting`, `tuning`, `live` und `error`. Das Feld `band` fehlt bei Meldungen ohne Bezug zu einer Bandauswahl.
- `{"type":"spectrum","band":"40m","levels_db10":[…]}` enthält von links nach rechts gleichmäßig verteilte Maximalpegel pro Anzeigespalte. Die Werte in `levels_db10` sind Zehntel-dBFS.
- `{"type":"zoom-spectrum","band":"40m","center_hz":7100000,"levels_db10":[…]}` übergibt 4.097 FFT-Bins von links nach rechts. Die Mitte ist der FFT-Bin, der MAIN am nächsten liegt. Außerhalb des nutzbaren I/Q-Bereichs stehen die Pegel auf −160 dBFS.
- Die GUI fordert mit `{"op":"set_main","hz":7100000}` eine MAIN-Frequenz in Hz innerhalb des aktuellen Spektrums an.
- `{"op":"step_main","steps":3}` verstellt MAIN innerhalb des aktuellen Spektrums relativ um drei Schritte zu je 50 Hz. Negative Schritte senken die Frequenz. Mehrere Eingaben werden lokal zusammengezählt. Der CI-V-Thread übernimmt jeweils den neuesten Sollwert.
- `{"type":"main","hz":7100000,"confirmed":false}` meldet die sofort aktive Softwarefrequenz. `confirmed:true` liegt nach der CI-V-Rückmeldung vor. `hz:0` steht für einen unbekannten Wert. `{"type":"main-status","text":"…"}` enthält die Rückmeldung der MAIN-Steuerung. Das Backend setzt und liest MAIN per CI-V `25 00`. Ein vorübergehender LAN-Ausfall lässt die Demodulation auf der zuletzt bekannten Softwarefrequenz weiterlaufen.

- `{"op":"get_setup"}` fordert die Einstellungen erneut an. Das Backend sendet sie auch beim Verbindungsaufbau.
- `{"type":"setup","radio_ip":"192.168.10.133","pi_ip":"192.168.10.140","mode":"LSB","volume":35}` enthält die aktiven Einstellungen.
- `{"op":"setup","pi_ip":"192.168.10.140","request_id":1}` übernimmt eine oder beide mitgegebenen IPv4-Adressen (`radio_ip` und `pi_ip`). Das Backend meldet das Ergebnis mit `{"type":"setup-result","request_id":1,"ok":true,"text":""}` und spiegelt dabei die optionale `request_id`.
- `{"op":"sideband","band":"40m","mode":"USB"}` wählt und speichert USB oder LSB für das ausgewählte Band. Beim Bandwechsel meldet das Backend die wiederhergestellte Einstellung mit `{"type":"sideband","band":"40m","mode":"USB"}`. `{"op":"volume","percent":35}` setzt die Lautstärke im Bereich 0 bis 100.
- `{"op":"audio_output","output":"browser"}` schaltet Browser-Audio ein. `off` schaltet den Ton aus. Eine neue TCP-Verbindung beginnt mit ausgeschaltetem Ton.
- `{"type":"audio","pcm":"…"}` enthält genau 960 Mono-Samples in PCM16 Little Endian, Base64-kodiert. `{"type":"audio-reset"}` verwirft gepuffertes Browser-Audio, etwa beim Wechsel der I/Q-Mitte.

### Browser-WebSocket

`/ws` akzeptiert JSON-Objekte als Textnachrichten. Statusmeldungen des Backends kommen ebenfalls als JSON-Text. Die Bridge sendet Spektren und Browser-Audio als Binärnachrichten mit 28 Byte Kopf und anschließenden vorzeichenbehafteten 16-Bit-Werten. Mehrbytefelder sind Little Endian.

| Offset | Inhalt |
| --- | --- |
| 0 | ASCII `IC76` |
| 4 | Formatversion 1 als Byte |
| 5 | 1 für Fullband, 2 für MAIN-Zoom, 3 für Browser-Audio |
| 6 | Anzahl der Werte als uint16 |
| 8 | MAIN-Mitte in Hz für Zoom, 0 für Fullband, Samplefrequenz 48000 für Audio, jeweils uint32 |
| 12 | Band-ID mit bis zu 15 ASCII-Zeichen und Nullauffüllung, bei Audio nullgefüllt |
| 28 | FFT-Pegel in Zehntel-dBFS oder PCM-Samples als int16 |

Der Backend-Anschluss zum Pi folgt dem [bestehenden I/Q-Protokoll](../raspberry/README.md#tcp-format).
