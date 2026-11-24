# IC-7610 Remote Control

kmcontrol steuert einen Icom IC-7610 per Ethernet und stellt den Empfang im Browser dar. Ein Raspberry Pi 4 liest die komplexen I/Q-Daten per USB vom Funkgerät und streamt sie über TCP zum Controller.

- [Controller und WebGUI](controller/README.md) mit Fullband- und Zoom-Spektrum, Wasserfällen, Abstimmung und SSB-Browser-Audio.
- [Raspberry-Pi-Bridge](raspberry/README.md) mit Einrichtung des FTDI-Treibers, USB-Empfang und I/Q-TCP-Protokoll.
- [Quellen und Protokollreferenzen](doc/Quellen.md).

Die WebGUI ist standardmäßig auf dem Controller-Rechner unter `http://127.0.0.1:8086/` erreichbar.
