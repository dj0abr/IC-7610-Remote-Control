#!/usr/bin/env python3
"""Empfängt und prüft den I/Q-TCP-Strom des IC-7610."""
import argparse
from contextlib import ExitStack
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
import math
import queue
import socket
import struct
import sys
import threading
import time

STREAM_HEADER = struct.Struct("!8sIIQ")
BLOCK_HEADER = struct.Struct("!QI")
CONTROL_REPLY = struct.Struct("!IQ")
MAX_BLOCK_SIZE = 256 * 1024
SAMPLE_BYTES = 4
REPORT_INTERVAL = 60
FFT_SIZE = 16384
SPECTRUM_BANDWIDTH_HZ = 1660000
SPECTRUM_INTERVAL = 0.1
MIN_SUB_HZ, MAX_SUB_HZ = 30000, 60000000
CONTROL_ERRORS = {1: "SUB-Frequenz oder Befehl ungültig.",
                  2: "Das Funkgerät ist gerade auf Sendung."}


def receive_exact(connection, buffer):
    pending = memoryview(buffer)
    while pending:
        received = connection.recv_into(pending)
        if not received:
            raise EOFError("Der Pi hat die TCP-Verbindung vorzeitig beendet.")
        pending = pending[received:]


@dataclass(frozen=True)
class SubReply:
    status: int
    center: int

    def error(self):
        return CONTROL_ERRORS.get(self.status, f"SUB-Befehl fehlgeschlagen ({self.status}).")


class IQStream:
    def __init__(self, connection):
        self.connection = connection
        header = bytearray(STREAM_HEADER.size)
        receive_exact(connection, header)
        magic, self.rate, sample_format, self.center = STREAM_HEADER.unpack(header)
        if magic != b"IC76IQ01" or sample_format != 1 or self.rate == 0:
            raise RuntimeError("Unbekannter Protokollkopf oder unbekanntes Sampleformat.")
        self.block_header = bytearray(BLOCK_HEADER.size)
        self.buffer = memoryview(bytearray(MAX_BLOCK_SIZE))
        self.sample_count = 0

    def set_sub(self, frequency):
        self.connection.sendall(f"SET SUB {frequency}\n".encode("ascii"))

    def read_block(self):
        receive_exact(self.connection, self.block_header)
        index, length = BLOCK_HEADER.unpack(self.block_header)
        if index != self.sample_count:
            raise RuntimeError(f"Sample-Lücke: Index {index}, erwartet {self.sample_count}.")
        if length == 0:
            reply = bytearray(CONTROL_REPLY.size)
            receive_exact(self.connection, reply)
            status, center = CONTROL_REPLY.unpack(reply)
            self.center = center
            return SubReply(status, center)
        if length > MAX_BLOCK_SIZE or length % SAMPLE_BYTES:
            raise RuntimeError(f"Ungültige I/Q-Blocklänge {length}.")
        payload = self.buffer[:length]
        receive_exact(self.connection, payload)
        self.sample_count += length // SAMPLE_BYTES
        return payload


def fft_magnitude(np, payload, window, *, remove_dc=False):
    channels = np.frombuffer(payload, dtype="<i2").reshape(-1, 2)
    iq = channels[:, 0].astype(float) + 1j * channels[:, 1]
    if remove_dc:
        iq -= iq.mean()
    return np.abs(np.fft.fft(iq * window))


def peak_offset_hz(np, magnitude, peak, rate):
    size = len(magnitude)
    neighbors = magnitude[[(peak - 1) % size, peak, (peak + 1) % size]]
    left, mid, right = np.log(np.maximum(neighbors, np.finfo(float).tiny))
    denominator = left - 2 * mid + right
    correction = 0.5 * (left - right) / denominator if denominator else 0
    signed_peak = (peak + size // 2) % size - size // 2
    return (signed_peak + correction) * rate / size


class CarrierCheck:
    def __init__(self, expected):
        try:
            import numpy
        except ImportError as error:
            raise RuntimeError("Die Trägerprüfung benötigt Python-NumPy.") from error
        self.np = numpy
        self.expected = expected
        self.low = math.inf
        self.high = -math.inf
        self.count = 0

    def check(self, payload, rate, center):
        np = self.np
        count = len(payload) // SAMPLE_BYTES
        if count < 8192:
            raise RuntimeError("Der I/Q-Block ist zu kurz für die Trägerprüfung.")
        # Keep the DC bin when the expected carrier lies in the center's FFT bin.
        remove_dc = abs(self.expected - center) > rate / count
        spectrum = fft_magnitude(np, payload, np.hanning(count), remove_dc=remove_dc)
        peak = int(np.argmax(spectrum))
        floor = float(np.median(spectrum))
        if spectrum[peak] <= 10 * floor or not spectrum[peak]:
            raise RuntimeError("Im I/Q-Block ist kein eindeutiger Träger erkennbar.")
        frequency = center + peak_offset_hz(np, spectrum, peak, rate)
        if abs(frequency - self.expected) > 100:
            raise RuntimeError(
                f"Stärkster Träger {frequency:.1f} Hz, erwartet {self.expected} Hz ±100 Hz."
            )
        self.low = min(self.low, frequency)
        self.high = max(self.high, frequency)
        self.count += 1
        print(f"Trägerprüfung {self.count}: {frequency:.1f} Hz", file=sys.stderr, flush=True)


def run(args):
    carrier = CarrierCheck(args.expect_carrier_hz) if args.expect_carrier_hz else None
    with ExitStack() as stack:
        output = None
        if args.output is not None:
            output = (sys.stdout.buffer if args.output == "-"
                      else stack.enter_context(open(args.output, "xb")))
        connection = stack.enter_context(socket.create_connection((args.host, args.port), timeout=5))
        connection.settimeout(5)

        stream = IQStream(connection)
        if args.sub_hz is not None:
            stream.set_sub(args.sub_hz)
            deadline = time.monotonic() + 6
            while time.monotonic() < deadline:
                reply = stream.read_block()
                if isinstance(reply, SubReply):
                    if reply.status:
                        raise RuntimeError(reply.error())
                    if reply.center != args.sub_hz:
                        raise RuntimeError(f"SUB meldet {reply.center} Hz, erwartet {args.sub_hz} Hz.")
                    break
            else:
                raise RuntimeError("Der Pi hat die SUB-Frequenz nicht bestätigt.")
        sample_start = stream.sample_count
        rate, center = stream.rate, stream.center
        print(f"SUB {center} Hz | {rate} komplexe Samples/s | CS16LE", file=sys.stderr, flush=True)

        blocks = 0
        start = time.monotonic()
        next_report = start + REPORT_INTERVAL
        while True:
            payload = stream.read_block()
            if isinstance(payload, SubReply):
                raise RuntimeError("Unerwartete SUB-Antwort während der Aufzeichnung.")
            if output is not None:
                output.write(payload)
            sample_count = stream.sample_count - sample_start
            blocks += 1
            now = time.monotonic()
            elapsed = now - start
            done = args.seconds > 0 and elapsed >= args.seconds
            if carrier and (blocks == 1 or now >= next_report or done):
                carrier.check(payload, rate, center)
            if now >= next_report:
                print(
                    f"{elapsed:.1f} s | {blocks} Blöcke | {sample_count * SAMPLE_BYTES / elapsed / 1e6:.3f} MB/s",
                    file=sys.stderr, flush=True,
                )
                next_report = now + REPORT_INTERVAL
            if done:
                break
        if output is not None:
            output.flush()

    total_bytes = sample_count * SAMPLE_BYTES
    bytes_per_second = total_bytes / elapsed
    print(
        f"{elapsed:.3f} s | {blocks} Blöcke | {sample_count} komplexe Samples | "
        f"{total_bytes} Byte | {bytes_per_second / 1e6:.3f} MB/s",
        file=sys.stderr,
    )
    if carrier:
        print(
            f"Träger über {carrier.count} Prüfungen: {carrier.low:.1f}–{carrier.high:.1f} Hz",
            file=sys.stderr,
        )
    if not 0.95 * rate * SAMPLE_BYTES <= bytes_per_second <= 1.05 * rate * SAMPLE_BYTES:
        raise RuntimeError("Die mittlere Datenrate liegt außerhalb von ±5 % des Sollwerts.")


def spectrum_feed(host, port, stop, latest, commands, replies):
    frame = bytearray()
    frame_length = FFT_SIZE * SAMPLE_BYTES
    publish_after = 0.0

    def publish(message):
        try:
            latest.get_nowait()
        except queue.Empty:
            pass
        latest.put_nowait(message)

    try:
        with socket.create_connection((host, port), timeout=5) as connection:
            stream = IQStream(connection)
            while not stop.is_set():
                try:
                    frequency = commands.get_nowait()
                except queue.Empty:
                    pass
                else:
                    stream.set_sub(frequency)
                payload = stream.read_block()
                if isinstance(payload, SubReply):
                    frame.clear()
                    publish_after = 0
                    try:
                        latest.get_nowait()
                    except queue.Empty:
                        pass
                    replies.put(payload)
                    continue
                now = time.monotonic()
                if now < publish_after:
                    continue
                frame.extend(payload[:frame_length - len(frame)])
                if len(frame) == frame_length:
                    publish((stream.center, stream.rate, bytes(frame)))
                    frame.clear()
                    publish_after = now + SPECTRUM_INTERVAL
    except (EOFError, OSError, RuntimeError) as error:
        if not stop.is_set():
            publish(error)


def mhz_to_hz(text):
    try:
        value = Decimal(text.strip().replace(",", "."))
        if not value.is_finite() or not Decimal("0.03") <= value <= Decimal(60):
            raise ValueError
        hz = value * 1000000
        if hz != hz.to_integral_value():
            raise ValueError
        return int(hz)
    except (InvalidOperation, ValueError):
        raise ValueError("SUB zwischen 0,03 und 60 MHz eingeben, auf 1 Hz genau.") from None


def show_spectrum(args):
    try:
        import matplotlib.pyplot as plt
        from matplotlib.ticker import FormatStrFormatter, MaxNLocator
        from matplotlib.widgets import Button, CheckButtons, TextBox
        import numpy as np
    except ImportError as error:
        raise RuntimeError("Das Live-Spektrum benötigt NumPy und Matplotlib.") from error

    ink, muted, accent = "#122a35", "#486272", "#007b78"
    plt.rcParams.update({
        "font.family": "DejaVu Sans",
        "text.color": ink,
        "axes.labelcolor": ink,
        "xtick.color": muted,
        "ytick.color": muted,
    })
    fig, ax = plt.subplots(figsize=(10.6, 6.0), facecolor="#f2f7f6")
    fig.canvas.manager.set_window_title("IC-7610 I/Q-Spektrum")
    fig.subplots_adjust(left=0.095, right=0.968, bottom=0.23, top=0.79)
    fig.text(0.095, 0.932, "IC-7610   /   SUB-Spektrum", fontsize=18, weight="bold")
    status = fig.text(0.095, 0.856, f"Verbinde mit {args.host}:{args.port} …",
                      color=muted, fontsize=10)
    readout = fig.text(0.968, 0.856, "", ha="right", color=accent, fontsize=11,
                       weight="bold")
    ax.set_facecolor("#ffffff")
    ax.set_xlabel("Frequenz (MHz)", labelpad=8)
    ax.set_ylabel("Pegel (dBFS)", labelpad=9)
    ax.set_ylim(-135, 3)
    ax.yaxis.set_major_locator(MaxNLocator(nbins=7, steps=[1, 2, 5, 10]))
    ax.xaxis.set_major_locator(MaxNLocator(nbins=7))
    ax.xaxis.set_major_formatter(FormatStrFormatter("%.3f"))
    ax.grid(color="#e0e9ea", linewidth=0.75)
    for spine in ax.spines.values():
        spine.set_color("#c8d7da")
    trace, = ax.plot([], [], color=accent, linewidth=0.85)
    peak_marker, = ax.plot([], [], marker="o", markersize=5, color="#c4752a",
                           linestyle="none", zorder=4)

    fig.text(0.095, 0.090, "SUB (MHz)", fontsize=10, va="center", color=muted)
    frequency_input = TextBox(fig.add_axes([0.176, 0.06, 0.17, 0.06]), "", color="#ffffff",
                              hovercolor="#f5fbfa")
    apply_button = Button(fig.add_axes([0.361, 0.06, 0.105, 0.06]), "Setzen",
                          color="#dbece9", hovercolor="#bedbd7")
    feedback = fig.text(0.490, 0.090, "", fontsize=10, va="center", color=muted)
    dc_axes = fig.add_axes([0.790, 0.055, 0.18, 0.07], facecolor=fig.get_facecolor())
    dc_axes.set_axis_off()
    dc_filter = CheckButtons(dc_axes, ["DC ausblenden"], [False])
    latest = queue.Queue(maxsize=1)
    commands = queue.Queue(maxsize=1)
    replies = queue.SimpleQueue()
    stop = threading.Event()
    thread = threading.Thread(target=spectrum_feed,
                              args=(args.host, args.port, stop, latest, commands, replies),
                              daemon=True)
    window = np.hanning(FFT_SIZE)
    amplitude_reference = 32768 * window.sum()
    axis_key = None
    first_frame_time = None
    failed = False
    pending_since = None

    def request_sub(_event=None):
        nonlocal pending_since
        if axis_key is None or pending_since is not None or failed:
            return
        try:
            target = mhz_to_hz(frequency_input.text)
        except ValueError as error:
            feedback.set_text(str(error))
        else:
            commands.put_nowait(target)
            pending_since = time.monotonic()
            feedback.set_text("Warte auf Bestätigung …")
        fig.canvas.draw_idle()

    apply_button.on_clicked(request_sub)
    if args.sub_hz is not None:
        frequency_input.set_val(f"{args.sub_hz / 1e6:.6f}")
        commands.put_nowait(args.sub_hz)
        pending_since = time.monotonic()
        feedback.set_text("Warte auf Bestätigung …")

    def update():
        nonlocal axis_key, hz_axis, in_band, first_frame_time, failed, pending_since
        try:
            reply = replies.get_nowait()
        except queue.Empty:
            if pending_since is not None and time.monotonic() - pending_since > 6:
                feedback.set_text("Noch keine Bestätigung vom Pi.")
                fig.canvas.draw_idle()
        else:
            pending_since = None
            if reply.status:
                feedback.set_text(reply.error())
            else:
                frequency_input.set_val(f"{reply.center / 1e6:.6f}")
                feedback.set_text(f"SUB bestätigt: {reply.center / 1e6:.6f} MHz")
            fig.canvas.draw_idle()
        try:
            message = latest.get_nowait()
        except queue.Empty:
            return True
        if isinstance(message, Exception):
            failed = True
            status.set_text(f"Stream beendet: {message}")
            status.set_color("#9b3829")
            feedback.set_text("")
            fig.canvas.draw_idle()
            return False
        if axis_key is None and pending_since is not None:
            return True
        center, rate, payload = message
        if axis_key != (center, rate):
            if axis_key is None and not frequency_input.text:
                frequency_input.set_val(f"{center / 1e6:.6f}")
            offsets = np.fft.fftfreq(FFT_SIZE, 1 / rate)
            half_bandwidth = min(rate, SPECTRUM_BANDWIDTH_HZ) / 2
            in_band = np.abs(offsets) <= half_bandwidth
            hz_axis = (center + np.fft.fftshift(offsets)) / 1e6
            ax.set_xlim((center - half_bandwidth) / 1e6, (center + half_bandwidth) / 1e6)
            status.set_text(f"SUB {center / 1e6:.6f} MHz   ·   {rate / 1e6:.3f} MS/s"
                            f"   ·   FFT {FFT_SIZE:,}".replace(",", "."))
            axis_key = (center, rate)
        magnitude = fft_magnitude(np, payload, window, remove_dc=dc_filter.get_status()[0])
        level = 20 * np.log10(np.maximum(magnitude / amplitude_reference, 1e-12))
        trace.set_data(hz_axis, np.fft.fftshift(level))
        peak = int(np.argmax(np.where(in_band, magnitude, 0)))
        if magnitude[peak] > 0:
            peak_hz = center + peak_offset_hz(np, magnitude, peak, rate)
            peak_marker.set_data([peak_hz / 1e6], [level[peak]])
            readout.set_text(f"{peak_hz / 1e6:.6f} MHz   ·   {level[peak]:.1f} dBFS")
        else:
            peak_marker.set_data([], [])
            readout.set_text("")
        now = time.monotonic()
        if first_frame_time is None:
            first_frame_time = now
            print(f"Live-Spektrum: {readout.get_text()}", file=sys.stderr, flush=True)
        fig.canvas.draw_idle()
        if args.seconds and now - first_frame_time >= args.seconds:
            plt.close(fig)
            return False
        return True

    hz_axis = in_band = None
    timer = fig.canvas.new_timer(interval=100)
    timer.add_callback(update)
    fig.canvas.mpl_connect("close_event", lambda _event: stop.set())
    thread.start()
    timer.start()
    try:
        plt.show()
    finally:
        timer.stop()
        stop.set()
        thread.join(timeout=0.3)
    return int(failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", nargs="?", default="192.168.10.140", help="Pi-Adresse")
    parser.add_argument("--port", type=int, default=50010)
    parser.add_argument("--seconds", type=float, help="Dauer in Sekunden, 0 für Dauerbetrieb")
    parser.add_argument("--spectrum", action="store_true", help="Live-FFT als Fenster öffnen")
    parser.add_argument("--sub-hz", type=int, help="SUB für diese Sitzung auf die Frequenz in Hz setzen")
    parser.add_argument("--output", help="neue Datei für rohe I/Q-Samples, - für stdout")
    parser.add_argument("--expect-carrier-hz", type=int, help="stärksten Träger mit NumPy prüfen")
    args = parser.parse_args()
    if args.seconds is not None and (not math.isfinite(args.seconds) or args.seconds < 0):
        parser.error("--seconds muss eine endliche Zahl ab null sein.")
    if args.sub_hz is not None and not MIN_SUB_HZ <= args.sub_hz <= MAX_SUB_HZ:
        parser.error("--sub-hz muss zwischen 30000 und 60000000 Hz liegen.")
    if args.spectrum and (args.output is not None or args.expect_carrier_hz is not None):
        parser.error("--spectrum ist ein eigener Anzeigemodus.")
    try:
        if args.spectrum:
            return show_spectrum(args)
        if args.seconds is None:
            args.seconds = 10
        run(args)
    except (EOFError, OSError, RuntimeError) as error:
        print(f"Fehler: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
