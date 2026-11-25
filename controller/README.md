# IC-7610 über LAN abfragen

Unter Linux mit einem C++20-Compiler und Make bauen und starten.

```sh
make
./ic7610-status
```

Der Start ohne Argumente verwendet die Umgebungsvariablen `KMCONTROL_ICOM_USER` und `KMCONTROL_ICOM_PASSWORD`. Bei einem expliziten Benutzernamen fragt das Programm das Kennwort am Terminal verdeckt ab oder liest die erste Zeile von stdin. Die Zieladresse ist `192.168.10.133`. Der voreingestellte Control-Port ist UDP 50001. Bei abweichender Konfiguration kann er mit `--port PORT` angegeben werden. `--verbose` protokolliert Paketgrößen, Typen und Sequenzen.

Das Funkgerät muss im LAN erreichbar und der integrierte Server unter **MENU → SET → Network → Network Control** verfügbar sein. Der Client meldet sich an, öffnet den ausgehandelten CI-V-Kanal und sendet nur die CI-V-Leseanfragen `03` (Frequenz), `04` (Betriebsart) und `1A 06` (DATA-Modus). Die Sitzung wird danach geschlossen. Der Client öffnet keinen Audio-Stream und sendet keine PTT- oder CI-V-Schreibbefehle.

Bei einer erfolgreichen Abfrage druckt das Programm Frequenz in Hz und Betriebsart. Bei einem Verbindungs- oder Protokollfehler endet es mit Status 1.

Der [Quellenindex](../doc/Quellen.md) verlinkt die CI-V-Referenz von Icom, die wfview-Paketdefinitionen, die CardSat-Protokollnotizen und die IC-7610-Mitschnitte von RigPlane.

## Backend und WebGUI

Das Backend und die HTTP-/WebSocket-Bridge sind C++20-Programme. Der Build verwendet CMake, Boost, Qt 5.15, FFTW für einfache Genauigkeit (fftw3f), LiquidDSP und SpeexDSP. CMake verwendet standardmäßig die vorhandene LiquidDSP-Installation des QO100-Transceivers und das SpeexDSP-Bundle aus QO100_TRX2. Andere Pfade lassen sich mit `-DLIQUID_ROOT=/pfad/zu/libs` und `-DSPEEXDSP_ROOT=/pfad/zum/bundle` setzen. Ohne passendes SpeexDSP-Bundle verwendet der Build das per pkg-config installierte SpeexDSP-Entwicklungspaket.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Bei einer Einrichtung mit privatem Neustartskript startet `./restart-local.sh` die vorgebauten Programme im Projektordner neu. Das private Skript übergibt die Funkgeräte-Zugangsdaten an das Backend. Die Startprotokolle stehen unter `.kmcontrol-run/`.

Für einen manuellen Start `KMCONTROL_ICOM_USER` und `KMCONTROL_ICOM_PASSWORD` in der Umgebung von `./build/kmcontrol-backend` setzen. In einem zweiten Terminal `./build/kmcontrol-web` starten und [kmcontrol im Browser öffnen](http://127.0.0.1:8086/). `kmcontrol-web --port PORT` wählt einen anderen lokalen HTTP-/WebSocket-Port. Die Bridge lauscht auf dem Rechner mit dem Backend unter `127.0.0.1`. Sie liefert die eingebetteten Browserdateien und verbindet `/ws` mit dem TCP-Backend auf `127.0.0.1:50020`. Die Browseroberfläche gibt Bedienbefehle als JSON weiter und zeichnet beide FFT-Ansichten mit Canvas.

Der IC-7610 hat pro Backend einen Bedienplatz. Das Backend gibt den I/Q-Strom beim Schließen des aktiven Browser-Clients an den Pi zurück. Der Backend-Parameter `--pi HOST` übersteuert beim Start die gespeicherte Pi-Adresse. Die Vorgaben für IC-7610 und Pi sind 192.168.10.133 und 192.168.10.140.

### Bedienung

Die Anzeige zeigt MAIN und SUB oben, die aufklappbaren Einstellungen links und Fullband und Zoom rechts. Auf schmalen Bildschirmen stehen die Einstellungen über den Diagrammen und sind beim Laden eingeklappt. Die WebGUI wählt beim ersten Start 40 m. Ein Link wie `/?band=80m` öffnet direkt das 80-m-Band. Die letzte Bandauswahl bleibt im Browser gespeichert.

Ein Klick oder Tipp in das Spektrum oder den Wasserfall setzt MAIN auf die Frequenz an der gewählten Position. Das Mausrad verstellt MAIN je Rastschritt um 50 Hz. MAIN-Anzeige und Markierungen folgen sofort der Softwarefrequenz. Ein eigener Thread synchronisiert sie per CI-V mit dem Funkgerät und liest Änderungen am Abstimmknopf zurück. SUB und seine I/Q-Mitte folgen der Bandauswahl. Das Backend stimmt SUB über den Pi ab und wertet die bestätigte Mittenfrequenz aus.

Für beide Ansichten wertet das Backend dieselbe Hann-gefensterte FFT mit 131.072 komplexen I/Q-Samples aus. Bei 1,92 MS/s beträgt der Binabstand rund 14,65 Hz. Die FFT-Fenster überlappen sich. Pro 50 ms I/Q-Daten entsteht ein Spektrenpaar. Fullband zeigt den Maximalpegel je Anzeigespalte. Der Zoom zeigt die FFT-Bins im Bereich MAIN ±30 kHz, auf den nächstgelegenen Bin zentriert. Die Spektrumkurve mittelt die lineare Leistung über bis zu acht Messungen und zeigt das Ergebnis in dBFS an. Die Wasserfälle schieben ihre vorhandenen Pixel zeilenweise nach unten. Beim Abstimmen richtet der Zoom den Verlauf an den ursprünglichen Empfangsfrequenzen aus.

Im Menü **Wasserfall** setzen **Untere Schwelle** und **Obere Schwelle** den gemeinsamen dBFS-Anzeigebereich und färben vorhandene Zeilen entsprechend ein. Der Browser speichert beide Schwellen pro Band und stellt sie beim Bandwechsel wieder her. Für Bänder ohne eigene Werte gelten die früher global gespeicherten Schwellen oder standardmäßig −120 und −25 dBFS. Die **Diagrammfarbe** gilt für alle Bänder und für beide Spektren und Wasserfälle.

**Audio einschalten** unter dem Zoom-Wasserfall aktiviert den Empfangston und öffnet das gewählte Mikrofon. Wenn der Browser den Mikrofonzugriff blockiert, bleibt der Empfangston verfügbar. **Audio ausschalten** beendet beides. Nach einer Verbindungsunterbrechung lässt sich Audio mit demselben Button wieder einschalten. Unter **Setup → Wiedergabegerät** stehen der Systemstandard und die vom Browser freigegebenen lokalen Audioausgänge zur Auswahl. Die Auswahl schaltet auch bei laufendem Empfang sofort um und wird im Browser gespeichert.

Unter **Setup → Eingabegerät** lässt sich das Mikrofon auswählen. Steht zunächst nur **Systemstandard** zur Auswahl, zeigt der Browser nach **Soundkarten freigeben …** oder **Mikrofone freigeben …** seine Geräte an. Die Auswahl wird im Browser gespeichert. Im Empfangsbetrieb wechselt ein bereits geöffnetes Mikrofon sofort auf das neu gewählte Gerät.

Unter dem Zoom-Wasserfall stehen Empfangslautstärke, Sendeleistung, Audio und PTT. Der Regler **Sendeleistung** zeigt den RF-POWER-Sollwert des IC-7610 in Prozent. Im Empfang sendet die WebGUI Änderungen beim Loslassen des Reglers und zeigt den zurückgelesenen Wert. Änderungen direkt am Funkgerät übernimmt sie im Empfang etwa alle 500 ms. Während TX ist der Regler gesperrt und nach der Rückkehr zu RX wird der Wert neu gelesen. PTT ist verfügbar, sobald das Mikrofon geöffnet ist, der Icom RX meldet und der LAN-Audiokanal bereit ist. Unter **Setup → PTT-Modus** speichert der Browser die Wahl zwischen **Halten** und **Rasten**. Im Haltemodus endet TX beim Loslassen. Im Rastmodus endet TX bei der nächsten Betätigung oder nach fünf Minuten. Die **Leertaste** arbeitet außerhalb von Eingabefeldern im gewählten Modus.

Unter **Empfang → RX-Vorverstärker (SUB)** stehen Aus, Stufe 1 und Stufe 2 zur Auswahl. Das betrifft den SUB-Empfänger, der die I/Q-Daten liefert. Unter **Setup → TX-Kompressor** lässt sich der Sprachkompressor ein- und ausschalten. Die WebGUI übernimmt im Empfang den jeweils vom IC-7610 bestätigten Zustand.

Beim Aufbau der LAN-CI-V-Verbindung und bei jedem Bandwechsel setzt das Backend **MIC GAIN** auf 50 % (CI-V-Wert 128). Falls das Gerät sendet, wartet das Backend damit bis zur Rückkehr zu RX.

Zum Senden muss der IC-7610 auf SSB ohne DATA und ohne Split stehen. Im Gerätemenü unter **Connectors → MOD Input** muss **DATA OFF MOD** auf **LAN** stehen. Der separate **LAN MOD Level** bestimmt den Sendepegel. Der Browser überträgt während Software-PTT Mono-PCM16 mit 16 kHz. Der IC-7610 bestätigt TX und RX über CI-V.

Die Wasserfälle halten während TX an. Fokusverlust, fehlende Audiodaten und ein ausbleibendes PTT-Lebenszeichen lösen die Software-PTT in beiden Modi. Bei Rast-PTT erzwingt das Backend nach fünf Minuten die Rückkehr zu RX. Der **Time-Out Timer (CI-V)** am Icom begrenzt die Sendedauer auch bei einem Ausfall der LAN-Strecke zum Gerät.

Wenn Chromium zunächst nur den Systemstandard anzeigt, öffnet **Soundkarten freigeben …** den Freigabedialog für die Geräteliste. Chromium verwendet hierfür die Mikrofonberechtigung. kmcontrol schließt den Mikrofonstream sofort und verarbeitet daraus keine Audiodaten. Browser mit einem eigenen Freigabedialog für Audioausgänge bieten **Wiedergabegerät freigeben …** an. Browser ohne direkte Geräteauswahl verwenden den Systemstandard.

Die IPv4-Adressen des IC-7610 und des Raspberry Pi stehen ebenfalls direkt unter **Setup**. Enter oder das Verlassen des jeweiligen Felds übernimmt eine gültige Adresse und baut die entsprechende Verbindung neu auf. Das Backend speichert Adressen und Lautstärke benutzerspezifisch. Es speichert USB oder LSB pro Band und stellt die Auswahl beim Bandwechsel wieder her. Für Bänder ohne eigene Einstellung gilt die früher global gespeicherte Auswahl oder standardmäßig LSB.

Unter **Filter** stehen wie bei QO100 die Presets **3,9**, **2,7** und **2,4 kHz**, die beiden Filtergrenzen und **Rauschreduktion**. Die Grenzen liegen im Bereich 100–4000 Hz, lassen sich in 0,5-Hz-Schritten einstellen und bleiben mindestens 500 Hz auseinander. Ein Preset behält die untere Grenze, soweit seine Bandbreite Platz hat. Der Zoom markiert den Durchlass bei USB rechts und bei LSB links von MAIN. Das Backend speichert die Filtergrenzen und Rauschreduktion gemeinsam für alle Bänder. Die Vorgabe lautet 100–4000 Hz und 0 % Rauschreduktion.

Das Backend mischt die Differenz zwischen der MAIN-Softwarefrequenz und der bestätigten SUB-Frequenz aus dem I/Q-Strom auf Basisband. LiquidDSP dezimiert auf 12 kSamples/s und filtert das gewählte Seitenband mit dem 513-Tap-Kaiser-FIR aus dem QO100-Konzept. Bei einer Änderung der Filtergrenzen überblendet das Backend zwischen den Filtern über 20 ms. Die automatische Verstärkungsregelung nutzt den mittleren Pegel hinter dem Seitenbandfilter und kann schwache Signale um bis zu 72 dB anheben. SpeexDSP verarbeitet das Audio bei 12 kHz mit bis zu 30 dB eingestellter Rauschdämpfung und überblendet beim Aktivieren oder Deaktivieren. Bei 0 % bleibt das trockene Audio erhalten. Anschließend wird Mono-Audio mit 48 kSamples/s und Spitzenbegrenzung an den Browser geliefert.

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
- `{"op":"rx_filter","low_hz":300,"high_hz":2700,"nr_percent":30,"request_id":1}` setzt den RX-Filter und die Rauschreduktion. Das Backend meldet seine Werte bei Verbindungsaufbau und nach Änderungen als `type:"rx-filter"`, mit der Request-ID und bei Bedarf einer Meldung in `text`.
- `{"op":"rf_power","percent":25}` setzt den RF-POWER-Sollwert im Bereich 0 bis 100 über CI‑V. `{"type":"rf-power","percent":25,"text":""}` meldet den am IC-7610 gelesenen Wert. Bei fehlendem Rücklesewert steht dort `null`.
- `{"op":"compressor","value":1}` schaltet den TX-Kompressor ein (`0` aus). `{"op":"preamp","value":2}` setzt den SUB-Vorverstärker (`0` aus, `1` Stufe 1, `2` Stufe 2). Das Backend meldet den Rücklesewert mit `{"type":"compressor","value":1,"text":""}` beziehungsweise `{"type":"preamp","value":2,"text":""}`. Bei fehlendem Rücklesewert steht dort `null`.
- `{"op":"sideband","band":"40m","mode":"USB"}` wählt und speichert USB oder LSB für das ausgewählte Band. Beim Bandwechsel meldet das Backend die wiederhergestellte Einstellung mit `{"type":"sideband","band":"40m","mode":"USB"}`. `{"op":"volume","percent":35}` setzt die Lautstärke im Bereich 0 bis 100.
- `{"op":"audio_output","output":"browser"}` schaltet Browser-Audio ein. `off` schaltet den Ton aus. Eine neue TCP-Verbindung beginnt mit ausgeschaltetem Ton.
- `{"type":"audio","pcm":"…"}` enthält genau 960 Mono-Samples in PCM16 Little Endian, Base64-kodiert. `{"type":"audio-reset"}` verwirft gepuffertes Browser-Audio, etwa beim Wechsel der I/Q-Mitte.
- `{"op":"ptt","action":"start","id":1,"mode":"toggle"}` fordert Rast-PTT mit Fünf-Minuten-Timeout an. `hold` ist der Standardmodus. `keepalive` und `stop` verwenden dieselbe ID. `{"op":"tx_audio","id":1,"pcm":"…"}` übergibt einen Base64-kodierten Block mit 320 PCM16-Mono-Samples bei 16 kHz. `{"type":"tx","phase":"tx","ready":true,"text":""}` meldet bestätigtes TX. Die Phase `rx` meldet bestätigten Empfang.

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

Für Browser-Mikrofondaten nimmt die Bridge 648 Byte lange Binärnachrichten an. Auf `ICTX` und die vier Byte lange PTT-ID in Little Endian folgen 640 Byte Mono-PCM16 in Little Endian.

Der Backend-Anschluss zum Pi folgt dem [bestehenden I/Q-Protokoll](../raspberry/README.md#tcp-format).
