#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf '%s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
./install_bridge.sh       USB/IQ-Netzwerkbridge auf einem ARM64-Raspberry-Pi
./install_all.sh          Bridge und Controller auf demselben Raspberry Pi
./install_controller.sh   Backend und WebGUI auf einem Linux-Rechner

KMCONTROL_LISTEN_IP legt die lokale IPv4-Adresse für Bridge oder WebGUI fest.
KMCONTROL_PI_HOST gibt bei der separaten Controller-Installation die Adresse des Bridge-Pi vor.
LIBFT8_ROOT oder LIBFT8_SOURCE_DIR übersteuern den automatischen libft8-Build.
EOF
}

mode=${1:-all}
if (( $# )); then shift; fi
if [[ $mode == --help || ${1:-} == --help ]]; then
    usage
    exit 0
fi
(( $# == 0 )) || fail 'Unbekannte Argumente. Verwende --help für die Aufrufmöglichkeiten.'
case $mode in
    bridge) want_bridge=1; want_controller=0 ;;
    all) want_bridge=1; want_controller=1 ;;
    controller) want_bridge=0; want_controller=1 ;;
    *) fail 'Unbekannter Installationsmodus. Verwende --help für die Aufrufmöglichkeiten.' ;;
esac

[[ $(uname -s) == Linux ]] || fail 'Diese Installationsskripte benötigen Linux.'
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
project_dir=$PWD
install_dir=/usr/local/lib/kmcontrol
unit_dir=/etc/systemd/system
bridge_unit=kmcontrol-iq-bridge.service
backend_unit=kmcontrol-backend.service
web_unit=kmcontrol-web.service

sudo_cmd=()
if (( EUID != 0 )); then
    command -v sudo >/dev/null || fail 'Starte den Installer als root oder mit einem Benutzer mit sudo-Zugriff.'
    sudo_cmd=(sudo)
fi

as_root() {
    "${sudo_cmd[@]}" "$@"
}

has_systemd=0
if [[ -d /run/systemd/system ]] && command -v systemctl >/dev/null; then
    has_systemd=1
fi
if (( want_bridge )); then
    if ! command -v apt-get >/dev/null || [[ $(dpkg --print-architecture) != arm64 ]]; then
        fail 'Die USB/IQ-Bridge benötigt Raspberry Pi OS mit 64 Bit (arm64) und apt.'
    fi
    (( has_systemd )) || fail 'Die USB/IQ-Bridge benötigt ein laufendes systemd auf dem Raspberry Pi.'
fi

install_packages() {
    if command -v apt-get >/dev/null; then
        local packages=(ca-certificates curl iproute2)
        if (( want_bridge )); then
            packages+=(tar gzip udev libstdc++6)
        fi
        if (( want_controller )); then
            packages+=(git build-essential cmake gfortran pkg-config qtbase5-dev
                libboost-dev libfftw3-dev libliquid-dev libspeexdsp-dev
                libcurl4-openssl-dev nlohmann-json3-dev)
        else
            packages+=(gcc libc6-dev make)
        fi
        as_root apt-get update
        as_root apt-get install -y --no-install-recommends "${packages[@]}"
    elif command -v dnf >/dev/null; then
        as_root dnf install -y ca-certificates curl git iproute gcc gcc-c++ gcc-gfortran make cmake \
            pkgconf-pkg-config qt5-qtbase-devel boost-devel fftw-devel liquid-dsp-devel \
            speexdsp-devel libcurl-devel json-devel
    elif command -v pacman >/dev/null; then
        as_root pacman -S --needed --noconfirm ca-certificates curl git iproute2 gcc gcc-fortran \
            make cmake pkgconf qt5-base boost fftw liquid-dsp speexdsp nlohmann-json
    elif command -v zypper >/dev/null; then
        as_root zypper --non-interactive install ca-certificates curl git iproute2 gcc gcc-c++ gcc-fortran \
            make cmake pkg-config libqt5-qtbase-devel boost-devel fftw3-devel liquid-dsp-devel \
            speexdsp-devel libcurl-devel nlohmann_json-devel
    else
        printf '%s\n' 'Für diesen Paketmanager verwendet der Installer die vorhandenen Build-Abhängigkeiten.'
    fi
}

local_ipv4() {
    # The route lookup does not send any packets.
    local address
    address=$(ip -4 route get 1.1.1.1 2>/dev/null |
        awk '{for (i = 1; i < NF; i++) if ($i == "src") {print $(i+1); exit}}' || true)
    if [[ -z $address ]]; then
        address=$(ip -4 -o addr show up scope global 2>/dev/null |
            awk 'NR == 1 {split($4, ip, "/"); print ip[1]}' || true)
    fi
    printf '%s\n' "$address"
}

valid_ipv4() {
    [[ $1 =~ ^(0|[1-9][0-9]{0,2})(\.(0|[1-9][0-9]{0,2})){3}$ ]] || return 1
    local octet
    local -a octets
    IFS=. read -r -a octets <<< "$1"
    for octet in "${octets[@]}"; do
        (( octet <= 255 )) || return 1
    done
    [[ $1 != 0.0.0.0 ]]
}

build_bridge() {
    local build_dir=$work_dir/bridge
    local driver=$build_dir/ftdi
    local driver_files=(ftd3xx.h Types.h libftd3xx.so)
    local archive=${KMCONTROL_FTDI_ARCHIVE:-}
    install -d "$driver"
    install -m 0644 raspberry/Makefile raspberry/iq-test.c "$build_dir/"

    if [[ -z $archive && -s raspberry/ftdi/ftd3xx.h && -s raspberry/ftdi/Types.h &&
          -s raspberry/ftdi/libftd3xx.so ]]; then
        install -m 0644 "${driver_files[@]/#/raspberry/ftdi/}" "$driver/"
    else
        if [[ -z $archive ]]; then
            archive=$work_dir/ftdi.tgz
            printf '%s\n' 'Lade den ARM64-Treiber von FTDI.'
            curl -fsSL --connect-timeout 10 --max-time 45 --retry 2 -o "$archive" \
                https://ftdichip.cn/Drivers/D3XX/Linux/libftd3xx-linux-arm-v8-1.0.16.tgz ||
                fail 'Der FTDI-Treiber konnte nicht geladen werden. Mit KMCONTROL_FTDI_ARCHIVE kannst du ein lokales Archiv angeben.'
        fi
        tar -xzf "$archive" -C "$driver" --no-same-owner --no-same-permissions \
            --strip-components=1 "${driver_files[@]/#/linux-arm-v8/}"
    fi
    make -C "$build_dir"
}

build_controller() {
    local source=${LIBFT8_SOURCE_DIR:-}
    local prefix=${LIBFT8_ROOT:-}
    # 0.3.1, the revision tested with this controller on the Raspberry Pi.
    local revision=e281d1a411f0980a914c9e0b9fd097f4c0d17500
    if [[ -n $prefix ]]; then
        prefix=$(realpath -e -- "$prefix")
    else
        if [[ -n $source ]]; then
            source=$(realpath -e -- "$source")
            [[ -f $source/include/libft8/libft8.hpp ]] || fail 'LIBFT8_SOURCE_DIR enthält keinen libft8-Quellbaum.'
        else
            source=$work_dir/libft8-source
            printf '%s\n' 'Lade libft8 0.3.1 von GitHub.'
            git init --quiet "$source"
            git -C "$source" fetch --quiet --depth=1 https://github.com/dj0abr/libft8.git "$revision"
            git -C "$source" checkout --quiet --detach "$revision"
        fi
        prefix=$work_dir/libft8-install
        cmake -S "$source" -B "$work_dir/libft8-build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
        cmake --build "$work_dir/libft8-build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
        cmake --install "$work_dir/libft8-build" --prefix "$prefix"
    fi

    cmake -S "$project_dir/controller" -B "$work_dir/controller-build" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DLIQUID_ROOT="${LIQUID_ROOT:-/usr}" -Dlibft8_ROOT="$prefix"
    cmake --build "$work_dir/controller-build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-1}"
}

install_credentials() {
    as_root install -d -m 0700 /etc/kmcontrol
    if as_root test -s "$credentials"; then
        return
    fi
    if [[ -s $project_dir/icom.env ]]; then
        as_root install -m 0600 "$project_dir/icom.env" "$credentials"
        return
    fi

    local icom_user=${KMCONTROL_ICOM_USER:-}
    local icom_password=${KMCONTROL_ICOM_PASSWORD:-}
    if [[ -z $icom_user ]]; then
        IFS= read -r -p 'Netzwerk-Benutzername am IC-7610: ' icom_user ||
            fail 'Gib die IC-7610-Zugangsdaten ein oder setze KMCONTROL_ICOM_USER und KMCONTROL_ICOM_PASSWORD.'
    fi
    if [[ -z $icom_password ]]; then
        IFS= read -rs -p 'Netzwerk-Passwort am IC-7610: ' icom_password ||
            fail 'Gib das IC-7610-Netzwerk-Passwort ein oder setze KMCONTROL_ICOM_PASSWORD.'
        printf '\n'
    fi
    [[ -n $icom_user && -n $icom_password ]] || fail 'Der IC-7610 benötigt Netzwerk-Benutzername und -Passwort.'
    # Quote for systemd's EnvironmentFile, which does not expand shell variables.
    icom_user=${icom_user//\\/\\\\}
    icom_password=${icom_password//\\/\\\\}
    printf 'KMCONTROL_ICOM_USER="%s"\nKMCONTROL_ICOM_PASSWORD="%s"\n' \
        "${icom_user//\"/\\\"}" "${icom_password//\"/\\\"}" > "$work_dir/icom.env"
    as_root install -m 0600 "$work_dir/icom.env" "$credentials"
}

write_bridge_service() {
    as_root groupadd -f plugdev
    printf '%s\n' 'SUBSYSTEM=="usb", ATTR{idVendor}=="0c26", ATTR{idProduct}=="0029", GROUP="plugdev", MODE="0660"' |
        as_root tee /etc/udev/rules.d/70-ic7610-iq.rules >/dev/null
    as_root udevadm control --reload-rules
    as_root udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=0c26 --attr-match=idProduct=0029
    as_root udevadm settle

    local web_dependency=
    if [[ $mode == all ]]; then
        web_dependency="PartOf=$web_unit"
    fi
    cat > "$work_dir/$bridge_unit" <<EOF
[Unit]
Description=IC-7610 USB I/Q bridge
Wants=network-online.target
After=network-online.target
$web_dependency
StartLimitIntervalSec=0

[Service]
Type=exec
User=$service_uid
SupplementaryGroups=plugdev
ExecStart=$install_dir/iq-test --serve --listen $listen_ip
Restart=on-failure
RestartSec=3s

[Install]
WantedBy=multi-user.target
EOF
    as_root install -m 0644 "$work_dir/$bridge_unit" "$unit_dir/"
}

write_controller_services() {
    local bridge_dependency=
    if [[ $mode == all ]]; then
        bridge_dependency=$(printf 'Wants=%s\nAfter=%s' "$bridge_unit" "$bridge_unit")
    fi
    cat > "$work_dir/$backend_unit" <<EOF
[Unit]
Description=IC-7610 control backend
$bridge_dependency
PartOf=$web_unit

[Service]
Type=exec
User=$service_uid
EnvironmentFile=$credentials
ExecStart=$install_dir/kmcontrol-backend${pi_host:+ --pi $pi_host}
Restart=on-failure
RestartSec=3s
EOF
    cat > "$work_dir/$web_unit" <<EOF
[Unit]
Description=IC-7610 web interface
Wants=network-online.target
Requires=$backend_unit
After=network-online.target $backend_unit
StartLimitIntervalSec=0

[Service]
Type=exec
User=$service_uid
ExecStart=$install_dir/kmcontrol-web --listen $listen_ip --port 8086
Restart=on-failure
RestartSec=3s

[Install]
WantedBy=multi-user.target
EOF
    as_root install -m 0644 "$work_dir/$backend_unit" "$work_dir/$web_unit" "$unit_dir/"
}

wait_for_bridge() {
    local attempt
    for (( attempt = 0; attempt < 12; ++attempt )); do
        if ss -ltnH 'sport = :50010' | awk -v address="$listen_ip:50010" \
                '$4 == address { ready=1 } END { exit !ready }'; then
            return
        fi
        sleep 1
    done
    fail "Die I/Q-Bridge lauscht nicht unter $listen_ip:50010. Prüfe journalctl -u $bridge_unit."
}

install_packages
listen_ip=${KMCONTROL_LISTEN_IP:-$(local_ipv4)}
if [[ -z $listen_ip ]]; then
    (( !want_bridge )) || fail 'Keine IPv4-Adresse gefunden. Setze bei Bedarf KMCONTROL_LISTEN_IP.'
    listen_ip=127.0.0.1
fi
valid_ipv4 "$listen_ip" || fail 'KMCONTROL_LISTEN_IP muss eine konkrete lokale IPv4-Adresse sein.'
pi_host=
if [[ $mode == all ]]; then
    pi_host=$listen_ip
elif [[ $mode == controller && -n ${KMCONTROL_PI_HOST:-} ]]; then
    [[ $KMCONTROL_PI_HOST =~ ^[A-Za-z0-9][A-Za-z0-9.-]*$ ]] ||
        fail 'KMCONTROL_PI_HOST muss eine IPv4-Adresse oder ein Hostname sein.'
    pi_host=$KMCONTROL_PI_HOST
fi

umask 077
work_dir=$(mktemp -d)
trap 'rm -rf -- "$work_dir"' EXIT
if (( want_bridge )); then build_bridge; fi
if (( want_controller )); then build_controller; fi

credentials=/etc/kmcontrol/icom.env
if (( has_systemd )); then
    service_uid=$EUID
    if (( EUID == 0 )); then
        if [[ ${SUDO_UID:-} =~ ^[1-9][0-9]*$ ]]; then
            service_uid=$SUDO_UID
        else
            if ! id -u kmcontrol >/dev/null 2>&1; then
                as_root useradd --system --create-home --home-dir /var/lib/kmcontrol --shell /bin/false kmcontrol
            fi
            service_uid=$(id -u kmcontrol)
        fi
    fi
    if (( want_controller )); then install_credentials; fi
fi

start_units=()
as_root install -d -m 0755 "$install_dir"
if (( want_bridge )); then
    as_root install -d -m 0755 "$install_dir/ftdi"
    as_root install -m 0755 "$work_dir/bridge/iq-test" "$install_dir/"
    as_root install -m 0644 "$work_dir/bridge/ftdi/libftd3xx.so" "$install_dir/ftdi/"
    write_bridge_service
    start_units+=("$bridge_unit")
fi
if (( want_controller )); then
    as_root cmake --install "$work_dir/controller-build" --prefix "$install_dir"
    if (( has_systemd )); then
        write_controller_services
        start_units+=("$web_unit")
    fi
fi

if (( !has_systemd )); then
    printf 'Controller installiert unter %s.\n' "$install_dir"
    printf '%s\n' 'Für den manuellen Start des Backends setze KMCONTROL_ICOM_USER und KMCONTROL_ICOM_PASSWORD.'
    printf '%s/kmcontrol-backend%s\n' "$install_dir" "${pi_host:+ --pi $pi_host}"
    printf '%s/kmcontrol-web --listen %s --port 8086\n' "$install_dir" "$listen_ip"
    exit 0
fi

as_root systemctl daemon-reload
as_root systemctl enable "${start_units[@]}"
as_root systemctl restart "${start_units[@]}"
if (( want_bridge )); then
    wait_for_bridge
    printf 'I/Q-Bridge unter %s:50010\n' "$listen_ip"
fi
if (( want_controller )); then
    web_url=http://$listen_ip:8086
    curl -fsS --noproxy '*' --retry 10 --retry-delay 1 --retry-connrefused --max-time 2 \
        -o /dev/null "$web_url/health" ||
        fail "Die WebGUI antwortet nicht. Prüfe journalctl -u $web_unit."
    start_units+=("$backend_unit")
fi
for unit in "${start_units[@]}"; do
    systemctl is-active --quiet "$unit" || fail "Dienst $unit läuft nicht. Prüfe journalctl -u $unit."
done
if (( want_controller )); then
    printf 'Weboberfläche unter %s/\n' "$web_url"
fi
