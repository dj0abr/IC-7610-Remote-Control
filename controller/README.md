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

Das Backend und die HTTP-/WebSocket-Bridge sind C++20-Programme. Der Build verwendet CMake ab 3.20, GNU Fortran für den FT-Decoder, Boost, Qt 5.15, FFTW für einfache Genauigkeit (fftw3f), LiquidDSP und SpeexDSP. CMake verwendet standardmäßig die vorhandene LiquidDSP-Installation des QO100-Transceivers und das SpeexDSP-Bundle aus QO100_TRX2. Andere Pfade lassen sich mit `-DLIQUID_ROOT=/pfad/zu/libs` und `-DSPEEXDSP_ROOT=/pfad/zum/bundle` setzen. Ohne passendes SpeexDSP-Bundle verwendet der Build das per pkg-config installierte SpeexDSP-Entwicklungspaket.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Bei einer Einrichtung mit privatem Neustartskript startet `./restart-local.sh` die vorgebauten Programme im Projektordner neu. Das private Skript übergibt die Funkgeräte-Zugangsdaten an das Backend. Die Startprotokolle stehen unter `.kmcontrol-run/`. Für FT4/FT8 muss das mitgebaute Programm `ft8-decoder` im selben Verzeichnis wie `kmcontrol-backend` liegen.

Für einen manuellen Start `KMCONTROL_ICOM_USER` und `KMCONTROL_ICOM_PASSWORD` in der Umgebung von `./build/kmcontrol-backend` setzen. In einem zweiten Terminal `./build/kmcontrol-web` starten und [kmcontrol im Browser öffnen](http://127.0.0.1:8086/). `kmcontrol-web --port PORT` wählt einen anderen HTTP-/WebSocket-Port. Standardmäßig lauscht die Web-Bridge lokal auf `127.0.0.1`. Sie liefert die eingebetteten Browserdateien und verbindet `/ws` mit dem TCP-Backend auf `127.0.0.1:50020`. Die Browseroberfläche gibt Bedienbefehle als JSON weiter und zeichnet beide FFT-Ansichten mit Canvas.

Für direkten Zugriff im LAN die Web-Bridge mit `kmcontrol-web --listen 192.168.10.140 --port 8086` auf der lokalen IPv4-Adresse starten. Die Oberfläche ist dann unter `http://192.168.10.140:8086/` erreichbar. Der Browser verlangt für das Mikrofon einen sicheren Kontext oder eine entsprechende Ausnahme für diese Adresse. Der Webserver erlaubt den Gerätezugriff aus dem LAN ohne Anmeldung.

Backend und Web-Bridge können zusammen mit der USB-I/Q-Bridge auf dem Raspberry Pi 4 (ARM64) laufen. `--pi` beziehungsweise **Setup → Raspberry Pi** erhält in diesem Fall die eigene IPv4-Adresse des Pi, auf der die USB-I/Q-Bridge lauscht. Linux leitet den I/Q-Strom intern weiter. Der Browser kann von einem anderen Rechner im LAN zugreifen.

Der IC-7610 hat pro Backend einen Bedienplatz. Das Backend gibt den I/Q-Strom beim Schließen des aktiven Browser-Clients an den Pi zurück. Der Backend-Parameter `--pi HOST` übersteuert beim Start die gespeicherte Pi-Adresse. Die Vorgaben für IC-7610 und Pi sind 192.168.10.133 und 192.168.10.140.

### Bedienung

Die Anzeige zeigt MAIN und SUB oben, die aufklappbaren Einstellungen links und Fullband und Zoom rechts. Auf schmalen Bildschirmen stehen die Einstellungen über den Diagrammen und sind beim Laden eingeklappt. Die WebGUI wählt beim ersten Start 40 m. Ein Link wie `/?band=80m` öffnet direkt das 80-m-Band. Die letzte Bandauswahl bleibt im Browser gespeichert.

Bei der Bandauswahl stimmt das Backend MAIN direkt per CI-V auf die Bandmitte ab, sofern MAIN am Gerät noch außerhalb des gewählten Bands liegt. Parallel stimmt der Pi SUB auf die I/Q-Mitte des Bands ab. Ein Klick oder Tipp in das Spektrum oder den Wasserfall setzt MAIN auf die Frequenz an der gewählten Position. Das Mausrad verstellt MAIN je Rastschritt um 50 Hz. MAIN-Anzeige und Markierungen folgen sofort der Softwarefrequenz. Ein eigener Thread synchronisiert sie per CI-V mit dem Funkgerät und liest Änderungen am Abstimmknopf zurück.

Für beide Ansichten wertet das Backend dieselbe Hann-gefensterte FFT mit 131.072 komplexen I/Q-Samples aus. Bei 1,92 MS/s beträgt der Binabstand rund 14,65 Hz. Die FFT-Fenster überlappen sich. Aus den I/Q-Daten entsteht alle 50 ms ein neues FFT-Fenster. Ein eigener Thread berechnet die FFT und leitet daraus Fullband und Zoom ab. Bei Rückstau kann er Fenster überspringen. Fullband zeigt den Maximalpegel je Anzeigespalte. Der Zoom zeigt die FFT-Bins im Bereich MAIN ±30 kHz, auf den nächstgelegenen Bin zentriert. Die Spektrumkurve mittelt die lineare Leistung über bis zu acht Messungen und zeigt das Ergebnis in dBFS an. Die Wasserfälle schieben ihre vorhandenen Pixel zeilenweise nach unten. Beim Abstimmen richtet der Zoom den Verlauf an den ursprünglichen Empfangsfrequenzen aus.

Im Menü **Wasserfall** setzen **Untere Schwelle** und **Obere Schwelle** den gemeinsamen dBFS-Anzeigebereich und färben vorhandene Zeilen entsprechend ein. Der Browser speichert beide Schwellen pro Band und stellt sie beim Bandwechsel wieder her. Für Bänder ohne eigene Werte gelten die früher global gespeicherten Schwellen oder standardmäßig −120 und −25 dBFS. Die **Diagrammfarbe** gilt für alle Bänder und für beide Spektren und Wasserfälle.

**Audio einschalten** oben rechts neben der Empfangsanzeige aktiviert den Empfangston und öffnet das gewählte Mikrofon. Wenn der Browser den Mikrofonzugriff blockiert, bleibt der Empfangston verfügbar. **Audio ausschalten** beendet beides. Nach einer Verbindungsunterbrechung lässt sich Audio mit demselben Button wieder einschalten. Unter **Setup → Wiedergabegerät** stehen der Systemstandard und die vom Browser freigegebenen lokalen Audioausgänge zur Auswahl. Die Auswahl schaltet auch bei laufendem Empfang sofort um und wird im Browser gespeichert.

Unter **Setup → Eingabegerät** lässt sich das Mikrofon auswählen. Steht zunächst nur **Systemstandard** zur Auswahl, zeigt der Browser nach **Soundkarten freigeben …** oder **Mikrofone freigeben …** seine Geräte an. Die Auswahl wird im Browser gespeichert. Im Empfangsbetrieb wechselt ein bereits geöffnetes Mikrofon sofort auf das neu gewählte Gerät.

Unter dem Zoom-Wasserfall stehen Empfangslautstärke, Sendeleistung und PTT. Der Regler **Sendeleistung** zeigt den RF-POWER-Sollwert des IC-7610 in Prozent. Im Empfang sendet die WebGUI Änderungen beim Loslassen des Reglers und zeigt den zurückgelesenen Wert. Änderungen direkt am Funkgerät übernimmt sie im Empfang etwa alle 500 ms. Während TX ist der Regler gesperrt und nach der Rückkehr zu RX wird der Wert neu gelesen. PTT ist verfügbar, sobald das Mikrofon geöffnet ist, der Icom RX meldet und der LAN-Audiokanal bereit ist. Unter **Setup → PTT-Modus** speichert der Browser die Wahl zwischen **Halten** und **Rasten**. Im Haltemodus endet TX beim Loslassen. Im Rastmodus endet TX bei der nächsten Betätigung oder nach fünf Minuten. Die **Leertaste** arbeitet außerhalb von Eingabefeldern im gewählten Modus.

Unter **Empfang → RX-Vorverstärker (SUB)** stehen Aus, Stufe 1 und Stufe 2 zur Auswahl. Das betrifft den SUB-Empfänger, der die I/Q-Daten liefert. Unter **Setup → TX-Kompressor** lässt sich der Sprachkompressor ein- und ausschalten. Die WebGUI übernimmt im Empfang den jeweils vom IC-7610 bestätigten Zustand.

Beim Aufbau der LAN-CI-V-Verbindung und bei jedem Bandwechsel setzt das Backend **MIC GAIN** auf 50 % (CI-V-Wert 128). Falls das Gerät sendet, wartet das Backend damit bis zur Rückkehr zu RX.

Zum Senden muss der IC-7610 auf SSB ohne DATA und ohne Split stehen. Im Gerätemenü unter **Connectors → MOD Input** muss **DATA OFF MOD** auf **LAN** stehen. Der separate **LAN MOD Level** bestimmt den Sendepegel. Der Browser überträgt während Software-PTT Mono-PCM16 mit 16 kHz. Der IC-7610 bestätigt TX und RX über CI-V.

Neben der **PTT** zeigt die WebGUI beim Empfang das S-Meter des auf die Empfangsfrequenz abgestimmten MAIN-Empfängers. Der Balken reicht von S0 bis S9 +60 dB. Der Zahlenwert wird zwischen den dokumentierten CI-V-Kennpunkten angenähert. Beim Senden wechselt die Anzeige auf die vom IC-7610 gemeldete Ausgangsleistung und das SWR. Die Wattangabe ist aus den Kennpunkten der Po-Anzeige abgeleitet. Das SWR erscheint bei gemeldeter HF-Leistung und wird oberhalb des letzten dokumentierten Kennpunkts als **> 3:1** angezeigt.

Die Wasserfälle halten während TX an. Fokusverlust, fehlende Audiodaten und ein ausbleibendes PTT-Lebenszeichen lösen die Software-PTT in beiden Modi. Bei Rast-PTT erzwingt das Backend nach fünf Minuten die Rückkehr zu RX. Der **Time-Out Timer (CI-V)** am Icom begrenzt die Sendedauer auch bei einem Ausfall der LAN-Strecke zum Gerät.

Wenn Chromium zunächst nur den Systemstandard anzeigt, öffnet **Soundkarten freigeben …** den Freigabedialog für die Geräteliste. Chromium verwendet hierfür die Mikrofonberechtigung. kmcontrol schließt den Mikrofonstream sofort und verarbeitet daraus keine Audiodaten. Browser mit einem eigenen Freigabedialog für Audioausgänge bieten **Wiedergabegerät freigeben …** an. Browser ohne direkte Geräteauswahl verwenden den Systemstandard.

Die IPv4-Adressen des IC-7610 und des Raspberry Pi stehen ebenfalls direkt unter **Setup**. Enter oder das Verlassen des jeweiligen Felds übernimmt eine gültige Adresse und baut die entsprechende Verbindung neu auf. Das Backend speichert Adressen und Lautstärke benutzerspezifisch. Es speichert USB oder LSB pro Band und stellt die Auswahl beim Bandwechsel wieder her. Im Sprachbetrieb gilt das Seitenband für die Software-Demodulation und für MAIN am IC-7610. Für Bänder ohne eigene Einstellung gilt die früher global gespeicherte Auswahl oder standardmäßig LSB.

Unter **Filter** stehen wie bei QO100 die Presets **3,9**, **2,7** und **2,4 kHz**, die beiden Filtergrenzen und **Rauschreduktion**. Die Grenzen liegen im Bereich 100–4000 Hz, lassen sich in 0,5-Hz-Schritten einstellen und bleiben mindestens 500 Hz auseinander. Ein Preset behält die untere Grenze, soweit seine Bandbreite Platz hat. Der Zoom markiert den Durchlass bei USB rechts und bei LSB links von MAIN. Das Backend speichert die Filtergrenzen und Rauschreduktion gemeinsam für alle Bänder. Die Vorgabe lautet 100–4000 Hz und 0 % Rauschreduktion.

Das Backend mischt die Differenz zwischen der MAIN-Softwarefrequenz und der bestätigten SUB-Frequenz aus dem I/Q-Strom auf Basisband. LiquidDSP dezimiert auf 12 kSamples/s und filtert das gewählte Seitenband mit dem 513-Tap-Kaiser-FIR aus dem QO100-Konzept. Bei einer Änderung der Filtergrenzen überblendet das Backend zwischen den Filtern über 20 ms. Die automatische Verstärkungsregelung nutzt den mittleren Pegel hinter dem Seitenbandfilter und kann schwache Signale um bis zu 72 dB anheben. SpeexDSP verarbeitet das Audio bei 12 kHz mit bis zu 30 dB eingestellter Rauschdämpfung und überblendet beim Aktivieren oder Deaktivieren. Bei 0 % bleibt das trockene Audio erhalten. Anschließend wird Mono-Audio mit 48 kSamples/s und Spitzenbegrenzung an den Browser geliefert.

Unter **Filter → Notch** stehen **Aus**, **Man** und **Auto** zur Auswahl. Im manuellen Modus stellt der kleine Regler die Sperrfrequenz von 100 bis 4000 Hz in 1-Hz-Schritten ein. Im Auto-Modus sucht das Backend bei eingeschaltetem Audio nach einem schmalen, stabilen Ton im Durchlassbereich und zeigt dessen Frequenz an. In beiden Modi dämpft ein LiquidDSP-Notch mit 400 Hz Bandbreite bei −3 dB den Ton nach dem Seitenbandfilter und vor der Verstärkungsregelung. Das Backend speichert Modus und manuelle Frequenz gemeinsam für alle Bänder. Die Voreinstellung ist Aus mit 1000 Hz für Man.

Die Empfangsausschnitte folgen dem [IARU-Region-1-HF-Bandplan](https://www.iaru-r1.org/wp-content/uploads/2019/08/hf_r1_bandplan.pdf). 10 m verwendet die gewünschten Anzeigeabschnitte 28–29 und 29–30 MHz. Der CB-Ausschnitt umfasst [alle 80 deutschen CB-Kanäle](https://www.bundesnetzagentur.de/865198). Die Ränder sind mit 5 bis 25 kHz Reserve schattiert. Bei 160 bis 12 m liegt die Mitte 15 kHz unter dem unteren Anzeigerand. Bei 10 m und CB liegt die Mitte innerhalb des Ausschnitts. Bei CB ist sie mit 26,620 MHz nahe am unteren Bandrand.

### FT4 und FT8

Unter **Empfangsansicht → FT4/FT8** erscheint die Digitalkachel anstelle des Zooms. Beim Öffnen sowie bei einem Wechsel von FT-Betriebsart oder Band stimmt MAIN automatisch auf die hinterlegte Standardfrequenz ab. Die Kachel zeigt einen 0–4-kHz-Wasserfall, UTC-Slotfortschritt, alle empfangenen Decodes und eine Liste für das eigene Rufzeichen. Eine synchronisierte UTC-Systemuhr am Backend ist für die 15-s-Slots (FT8) und 7,5-s-Slots (FT4) erforderlich. [WSJT-X-Standardfrequenzen](https://github.com/WSJTX/wsjtx/blob/master/models/FrequencyList.cpp)

Die Dekodierung verwendet USB-Audio mit 12 kHz direkt aus dem I/Q-Strom, vor der Sprach-AGC, dem Notch und Speex. Sie läuft auch bei ausgeschaltetem Browser-Audio. FT8 versucht nach 11,808 Sekunden eine frühe Dekodierung und ergänzt Treffer am Slotende. Die Rechenprozesse laufen getrennt vom Empfang. Die übernommenen WSJT-X-Quellen und ihre Lizenz liegen unter [external/ft8](external/ft8/ORIGIN.md).

Beim Öffnen der FT-Ansicht werden Notch, Rauschreduktion und der TX-Kompressor am IC-7610 vorübergehend ausgeschaltet. MAIN schaltet am IC-7610 automatisch auf USB ohne DATA. Das aktive MAIN-ZF-Filter wird auf 3,6 kHz gestellt und Twin PBT zentriert. Die Seitenbandauswahl speichert während FT weiterhin das Seitenband für den Sprachbetrieb. Beim Wechsel zurück zu Zoom stellt das Backend die Filtereinstellungen wieder her und schaltet auf das gespeicherte Seitenband des aktiven Bands. Bestätigte Aussendungen erscheinen in **Nachrichten mit eigenem Rufzeichen** mit dem Kennzeichen **TX**.

Für FT-TX muss Split am IC-7610 ausgeschaltet sein. `DATA OFF MOD = LAN` muss aktiv sein. **Einmal senden** plant die eingegebene Nachricht in der gewählten UTC-Slotfolge. Der 16-kHz-Ton wird im Backend erzeugt und per LAN übertragen. **TX-Pegel** regelt die Amplitude des Tons. Der Regler **Sendeleistung** steuert die HF-Leistung am Gerät. Die erste PCM-Übergabe erfolgt 500 ms nach Beginn des Slots.

Mit **Auto-QSO** startet ein Klick auf eine empfangene CQ-Zeile das QSO. **Auto-Antwort** nimmt an das eigene Rufzeichen gerichtete Anrufe an. **Auto-CQ** ruft in der gewählten Slotfolge und lässt den Gegenslot frei. **Auto-CQ-Antwort** wählt CQ-Rufe von Stationen, die in den letzten 72 Stunden im ADIF-Logbuch fehlen. **Abbrechen**, der Wechsel zurück zu Zoom, eine neue MAIN-Frequenz oder der Verbindungsverlust beenden die Automatik. Nach einem Neustart oder Neuladen müssen automatische Rufe oder Antworten erneut aktiviert werden.

QSOs mit ausgetauschten Rapporten werden in `ft4-ft8.adi` im lokalen Datenverzeichnis des Backends gespeichert. Unter Linux ist das standardmäßig `~/.local/share/kmcontrol/kmcontrol-backend/`. Das Logbuch hält die tatsächlichen Audiofrequenzversätze in den RX- und TX-Frequenzen fest.

Mit `kmcontrol-backend --qrz-api-key="$KMCONTROL_QRZ_API_KEY"` überträgt das Backend neu gespeicherte FT4/FT8-QSOs zusätzlich per HTTPS an QRZ.com. Das unter **Eigenes Rufzeichen** eingetragene Rufzeichen geht als `STATION_CALLSIGN` in den ADIF-Datensatz und muss zum Logbuch des Keys passen. Die FT-Ansicht zeigt die Upload-Bestätigung oder den Fehler. Bei einem Fehler bleibt das QSO im lokalen ADIF-Logbuch für den späteren Import erhalten. Bereits vorhandene Einträge werden nicht nachträglich hochgeladen. [QRZ Logbook API](https://www.qrz.com/docs/logbook/QRZLogbookAPI.html)

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
- Derselbe Befehl akzeptiert optional `"notch_mode":"off"` (oder `"manual"`, `"auto"`) und `"notch_hz":1000`. Die Antwort enthält die Notch-Einstellungen und `notch_detected_hz`. Im Auto-Modus signalisiert `{"type":"notch-frequency","hz":1234}` eine neue erkannte Frequenz. Ohne erkannten Ton ist der Wert `null`.
- `ft_mode` wählt mit `mode` den Decoder (`FT8`, `FT4` oder `off`). `ft-state` meldet den Decoderzustand und `ft-decode` die Ergebnisse mit Dialfrequenz und UTC-Slot. `ft8_tx_start` plant eine Aussendung, `ft8_tx_stop` bricht sie ab und `ft-tx` meldet Status und Slot. `ft_qso_log` speichert einen ADIF-Eintrag. `ft-qrz-status` meldet bei aktiviertem QRZ-Upload den asynchronen Status für das QSO. Befehle mit `request_id` erhalten eine korrelierte `ft-reply`-Antwort.
- `{"type":"tx-meters","po":143,"swr":48}` enthält die CI-V-Rohwerte des Po- und SWR-Meters während TX. Fehlende Werte sind `null`. Der Po-Messwert entspricht bei 143 ungefähr 50 W, SWR 48 entspricht 1,5:1.
- `{"type":"rx-meter","level":120}` enthält im Empfang den CI-V-Rohwert des MAIN-S-Meters (`29 00 15 02`). 0 entspricht S0, 120 entspricht S9 und 241 entspricht S9 +60 dB. Bei fehlender Antwort steht dort `null`.
- `{"op":"rf_power","percent":25}` setzt den RF-POWER-Sollwert im Bereich 0 bis 100 über CI‑V. `{"type":"rf-power","percent":25,"text":""}` meldet den am IC-7610 gelesenen Wert. Bei fehlendem Rücklesewert steht dort `null`.
- `{"op":"compressor","value":1}` schaltet den TX-Kompressor ein (`0` aus). `{"op":"preamp","value":2}` setzt den SUB-Vorverstärker (`0` aus, `1` Stufe 1, `2` Stufe 2). Das Backend meldet den Rücklesewert mit `{"type":"compressor","value":1,"text":""}` beziehungsweise `{"type":"preamp","value":2,"text":""}`. Bei fehlendem Rücklesewert steht dort `null`.
- `{"op":"sideband","band":"40m","mode":"USB"}` wählt und speichert USB oder LSB für das ausgewählte Band. Beim Bandwechsel meldet das Backend die wiederhergestellte Einstellung mit `{"type":"sideband","band":"40m","mode":"USB"}`. `main-sideband` meldet das per CI-V bestätigte Seitenband oder einen Fehler in `text`. `{"op":"volume","percent":35}` setzt die Lautstärke im Bereich 0 bis 100.
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
