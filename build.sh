#!/usr/bin/env bash
set -euo pipefail

if (( EUID == 0 )); then
    printf '%s\n' 'Starte ./build.sh als normaler Benutzer. Für die Systemeinrichtung verwendet das Skript sudo.' >&2
    exit 1
fi
if [[ $(dpkg --print-architecture) != arm64 ]]; then
    printf '%s\n' 'Für die Installation ist Raspberry Pi OS mit 64 Bit (arm64) erforderlich.' >&2
    exit 1
fi

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

libft8_prefix=${LIBFT8_ROOT:-}
libft8_source=${LIBFT8_SOURCE_DIR:-../../ft8/libft8}
if [[ -n $libft8_prefix ]]; then
    libft8_prefix=$(realpath -- "$libft8_prefix")
elif [[ -f $libft8_source/include/libft8/libft8.hpp ]]; then
    libft8_source=$(realpath -- "$libft8_source")
else
    printf '%s\n' 'Setze LIBFT8_SOURCE_DIR auf das libft8-Projekt oder LIBFT8_ROOT auf die installierte libft8 ab Version 0.2.1.' >&2
    exit 1
fi

# Prefer the source address of the default IPv4 route without sending packets.
pi_ip=$(ip -4 route get 1.1.1.1 2>/dev/null |
    awk '{for (i = 1; i < NF; i++) if ($i == "src") {print $(i+1); exit}}' || true)
if [[ -z $pi_ip ]]; then
    pi_ip=$(ip -4 -o addr show up scope global | awk 'NR == 1 {split($4, address, "/"); print address[1]}')
fi
if [[ -z $pi_ip ]]; then
    printf '%s\n' 'Keine IPv4-Adresse gefunden. Verbinde den Raspberry Pi mit dem Heimnetz und starte ./build.sh erneut.' >&2
    exit 1
fi
printf 'Raspberry Pi: %s\n' "$pi_ip"

work_dir=$(mktemp -d)
trap 'rm -rf -- "$work_dir"' EXIT

sudo apt update
sudo apt install -y ca-certificates curl build-essential cmake gfortran pkg-config \
    qtbase5-dev libboost-dev libfftw3-dev libliquid-dev libspeexdsp-dev \
    libcurl4-openssl-dev nlohmann-json3-dev

driver_dir=raspberry/ftdi
if [[ ! -s $driver_dir/ftd3xx.h || ! -s $driver_dir/Types.h || ! -s $driver_dir/libftd3xx.so ]]; then
    printf '%s\n' 'Lade den USB-3.0-Treiber von FTDI ...'
    curl -fsSL --retry 3 -o "$work_dir/ftdi.tgz" \
        https://ftdichip.cn/Drivers/D3XX/Linux/libftd3xx-linux-arm-v8-1.0.16.tgz
    tar -xzf "$work_dir/ftdi.tgz" -C "$work_dir" \
        linux-arm-v8/ftd3xx.h linux-arm-v8/Types.h linux-arm-v8/libftd3xx.so
    install -d "$driver_dir"
    install -m 0644 "$work_dir/linux-arm-v8/"{ftd3xx.h,Types.h,libftd3xx.so} "$driver_dir/"
fi

if [[ -z $libft8_prefix ]]; then
    libft8_prefix=$work_dir/libft8-install
    cmake -S "$libft8_source" -B "$work_dir/libft8-build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
    cmake --build "$work_dir/libft8-build" --parallel 2
    cmake --install "$work_dir/libft8-build" --prefix "$libft8_prefix"
fi

printf '%s\n' 'Baue I/Q-Bridge, Backend und Webserver ...'
make -C raspberry
cmake -S controller -B controller/build -DCMAKE_BUILD_TYPE=Release \
    -DLIQUID_ROOT=/usr -Dlibft8_ROOT="$libft8_prefix"
cmake --build controller/build --parallel 2

credentials=/etc/kmcontrol/icom.env
sudo install -d -m 0700 /etc/kmcontrol
if ! sudo test -s "$credentials"; then
    if [[ -s icom.env ]]; then
        sudo install -m 0600 icom.env "$credentials"
    else
        icom_user=${KMCONTROL_ICOM_USER:-}
        icom_password=${KMCONTROL_ICOM_PASSWORD:-}
        if [[ -z $icom_user ]]; then
            IFS= read -r -p 'Netzwerk-Benutzername am IC-7610: ' icom_user
        fi
        if [[ -z $icom_password ]]; then
            IFS= read -rs -p 'Netzwerk-Passwort am IC-7610: ' icom_password
            printf '\n'
        fi
        if [[ -z $icom_user || -z $icom_password ]]; then
            printf '%s\n' 'Der IC-7610 benötigt einen Netzwerk-Benutzernamen und ein Passwort.' >&2
            exit 1
        fi
        # Quote the values for systemd's EnvironmentFile format.
        icom_user=${icom_user//\\/\\\\}
        icom_password=${icom_password//\\/\\\\}
        printf 'KMCONTROL_ICOM_USER="%s"\nKMCONTROL_ICOM_PASSWORD="%s"\n' \
            "${icom_user//\"/\\\"}" "${icom_password//\"/\\\"}" > "$work_dir/icom.env"
        sudo install -m 0600 "$work_dir/icom.env" "$credentials"
        unset icom_user icom_password
    fi
fi

printf '%s\n' 'Richte den Autostart ein ...'
install_dir=/usr/local/lib/kmcontrol
sudo install -d -m 0755 "$install_dir/ftdi"
sudo install -m 0755 raspberry/iq-test "$install_dir/"
sudo cmake --install controller/build --prefix "$install_dir"
sudo install -m 0644 "$driver_dir/libftd3xx.so" "$install_dir/ftdi/"

sudo groupadd -f plugdev
printf '%s\n' 'SUBSYSTEM=="usb", ATTR{idVendor}=="0c26", ATTR{idProduct}=="0029", GROUP="plugdev", MODE="0660"' |
    sudo tee /etc/udev/rules.d/70-ic7610-iq.rules >/dev/null
sudo udevadm control --reload-rules
sudo udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=0c26 --attr-match=idProduct=0029
sudo udevadm settle

cat > "$work_dir/kmcontrol-iq-bridge.service" <<EOF
[Unit]
Description=IC-7610 USB-I/Q-Bridge
Wants=network-online.target
After=network-online.target
PartOf=kmcontrol-web.service
StartLimitIntervalSec=0

[Service]
Type=exec
User=$EUID
SupplementaryGroups=plugdev
ExecStart=$install_dir/iq-test --serve --listen $pi_ip
Restart=on-failure
RestartSec=3s
EOF

cat > "$work_dir/kmcontrol-backend.service" <<EOF
[Unit]
Description=IC-7610 Backend
Wants=kmcontrol-iq-bridge.service
After=kmcontrol-iq-bridge.service
PartOf=kmcontrol-web.service

[Service]
Type=exec
User=$EUID
EnvironmentFile=$credentials
ExecStart=$install_dir/kmcontrol-backend --pi $pi_ip
Restart=on-failure
RestartSec=3s
EOF

cat > "$work_dir/kmcontrol-web.service" <<EOF
[Unit]
Description=IC-7610 Webserver
Requires=kmcontrol-backend.service
After=kmcontrol-backend.service
StartLimitIntervalSec=0

[Service]
Type=exec
User=$EUID
ExecStart=$install_dir/kmcontrol-web --listen $pi_ip --port 8086
Restart=on-failure
RestartSec=3s

[Install]
WantedBy=multi-user.target
EOF

sudo install -m 0644 "$work_dir/"*.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable kmcontrol-web.service
sudo systemctl restart kmcontrol-web.service

web_url="http://$pi_ip:8086/"
if ! curl -fsS --noproxy '*' --retry 5 --retry-connrefused --max-time 2 -o /dev/null "$web_url"; then
    printf '%s\n' 'Der Webserver antwortet noch nicht. Prüfe das Protokoll mit sudo journalctl -u kmcontrol-web.service.' >&2
    exit 1
fi
for unit in kmcontrol-iq-bridge.service kmcontrol-backend.service kmcontrol-web.service; do
    if ! systemctl is-active --quiet "$unit"; then
        printf 'Dienst %s läuft nicht. Prüfe das Protokoll mit sudo journalctl -u %s.\n' "$unit" "$unit" >&2
        exit 1
    fi
done
printf '\nWeboberfläche: %s\n' "$web_url"
