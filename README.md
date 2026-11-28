# IC-7610 Remote Control

### Icom IC-7610 Web-Fernsteuerung mit einem Raspberry PI 4

## Eigenschaften:

* nutzt IQ Daten des IC-7610 für Wasserfall und Demodulatoren

* Hochwertige RX Audiofilter, Rauschunterdrückung, Auto-Notch

* vollständig integriert: FT4 und FT8, keine Zusatzprogramme erforderlich

* zwei Spektren/Wasserfälle für optimale Auflösung und feinfühlige Abstimmung

* Benutzeroberfläche läuft auf jedem Gerät: PC, Table, Smartphone usw.

* vollständiger ferngesteuerter QSO Betrieb in SSB oder FT4/8

* für den lokalen Betrieb zuhause oder über das Internet

![](/home/kurt/.config/marktext/images/dd5437dfe72be5dd0f90098980542843ef4e5fbe.png)

## Installation

Die Installation läuft auf einem Raspberry Pi 4 mit [Raspberry Pi OS Lite (64 Bit)](https://www.raspberrypi.com/software/operating-systems/). Richte zuvor die [Anschlüsse](#anschlüsse) ein.

Als Erstes laden wir die Software von GitHub.

```bash
sudo apt update
sudo apt install -y git
git clone https://github.com/dj0abr/IC-7610-Remote-Control.git "$HOME/kmcontrol"
```

Für FT4/FT8 braucht das Backend zusätzlich libft8 ab Version 0.3.1. Kopiere den libft8-Quellbaum auf den Pi, zum Beispiel nach `~/libft8`. Das Installationsskript baut die Library mit. Für eine bereits installierte ARM64-Version kann alternativ `LIBFT8_ROOT` auf deren Installationspräfix zeigen.

```bash
cd "$HOME/kmcontrol"
LIBFT8_SOURCE_DIR="$HOME/libft8" ./build.sh
```

Fehlen die Netzwerk-Anmeldedaten des IC-7610, fragt das Skript danach. Die IP des Pi erkennt es selbst. Am Ende zeigt es die Webadresse an.

## Lokaler Zugriff zuhause

Im Heimnetz bedienst du den IC-7610 direkt im Browser auf PC, Tablet oder Smartphone.

Öffne zum Beispiel [http://192.168.10.140:8086/](http://192.168.10.140:8086/) und ersetze die Beispiel-IP durch die Adresse deines Pi. Beim Wechsel auf ein anderes Gerät schließe zuerst den bisherigen Tab, damit der Bedienplatz frei wird.

## Zugriff über das Internet

Über ein VPN ins Heimnetz erreichst du die Weboberfläche von unterwegs unter derselben Adresse.

Falls dein VPN eine Portfreigabe braucht, leite den Port des VPN-Dienstes im Heimrouter weiter. Den Web-Port `8086` lässt du am Router geschlossen, da die Funksteuerung darüber ohne Anmeldung erreichbar ist.

## Anschlüsse

Über Ethernet steuerst du den IC-7610. Die I/Q-Daten für Empfang und Wasserfälle kommen per USB zum Pi.

- **IC-7610:** Schließe das Funkgerät per Ethernet ans Heimnetz an. Die Adresse steht unter **MENU → SET → Network → IP Address**. Schalte dort **Network Control** ein und richte unter **Network User1** oder **Network User2** die Anmeldedaten ein.
- **Raspberry Pi:** Verbinde den Pi mit dem Heimnetz. Die IP findest du in der DHCP-Geräteliste des Routers oder am Pi mit `hostname -I`. Reserviere die Pi-Adresse im Router, damit sie beim Neustart gleich bleibt. Danach kann der Pi ohne Monitor, Tastatur und Maus laufen.
- **USB 3.0:** Verbinde die rückseitige USB-3.0-Buchse des IC-7610 mit einem blauen USB-3.0-Port am Pi. Verwende ein USB-3.0-Kabel. USB 2.0 reicht für den I/Q-Strom nicht aus.

## Grundlegende Einstellungen

Im **Setup** legst du fest, welches Funkgerät und welche Audiogeräte die Weboberfläche verwendet.

Trage **IP des IC-7610** ein. Die Pi-Adresse ist nach der Skriptinstallation bereits gesetzt. Wähle unter **Wiedergabegerät** und **Eingabegerät** die Lautsprecher oder Kopfhörer und das Mikrofon deines Browser-Geräts. Wenn nur **Systemstandard** erscheint, gib die Geräte über die entsprechende Schaltfläche frei.

Stelle für den Sendeton am IC-7610 **MENU → SET → Connectors → MOD Input → DATA OFF MOD** auf **LAN** und schalte **Split** aus.

## Audio einschalten

Ein Klick auf **Audio einschalten** oben rechts startet den Empfangston und aktiviert das Mikrofon. Erlaube den Mikrofonzugriff, wenn der Browser danach fragt.

Bei Zugriff per HTTP auf die Pi-IP braucht das Mikrofon eine Browser-Ausnahme. Öffne in Chrome oder Chromium die folgende Adresse, trage die Webadresse deines Pi ein, setze die Option auf **Enabled** und starte den Browser neu.

```text
chrome://flags/#unsafely-treat-insecure-origin-as-secure
```

```text
http://192.168.10.140:8086
```

Bei HTTPS oder `localhost` genügt die normale Mikrofonfreigabe. [Chromium-Dokumentation](https://www.chromium.org/Home/chromium-security/deprecating-powerful-features-on-insecure-origins/)

## Ein typisches SSB-QSO

Mit dem Fullband-Wasserfall findest du ein Signal, im Zoom stimmst du es fein ab. Zum Senden nutzt du die PTT unter den Wasserfällen.

1. Wähle unter **Empfang** das Band und das passende Seitenband. Klicke für die Grobabstimmung in den **Fullband**-Wasserfall und für die Feinabstimmung in den **Zoom**-Wasserfall.
2. Wähle unter **Setup → PTT-Modus** zwischen **Halten** und **Rasten**. Bei **Halten** endet die Aussendung beim Loslassen, bei **Rasten** beim nächsten Druck oder nach fünf Minuten.
3. Stelle die **Sendeleistung** ein, betätige **PTT** und sprich ins Mikrofon. Außerhalb von Eingabefeldern funktioniert auch die **Leertaste**.

Bei Bedarf kannst du im **Setup** den **TX-Kompressor** zuschalten. Im Menü **Filter** stellst du die Empfangsbandbreite, die Rauschreduktion und das Notch-Filter ein.

## FT4- und FT8-Betrieb

Für FT4 und FT8 erscheint statt des Zoom-Wasserfalls eine Ansicht mit dekodierten Meldungen und eigenem Audio-Wasserfall. Das Funkgerät stellt die Standardfrequenz des Bands automatisch ein.

1. Wähle unter **Empfangsansicht** den Eintrag **FT4/FT8** und dann die gewünschte **Betriebsart**. Trage **Eigenes Rufzeichen** und **QTH-Locator** ein. Die Uhr des Pi muss für die Zeitschlitze synchronisiert sein.
2. Aktiviere **Auto-QSO** und klicke auf **CQ rufen** oder auf eine empfangene CQ-Zeile. **Auto-Antwort** reagiert auf Rufe an dein Rufzeichen, **Auto-CQ** wiederholt deinen CQ-Ruf und **Auto-CQ-Antwort** wählt passende empfangene CQ-Rufe aus.

[Details zum FT-Betrieb und ADIF-Logbuch](controller/README.md#ft4-und-ft8)
