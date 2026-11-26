/*
 * Receive I/Q data from Icom IC-7610's USB 3 port.
 *
 * Link against libftd3xx.
 *
 * Copyright (C) 2023 Christoph Berg DF7CB <cb@df7cb.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/* SUB I/Q receive test and TCP server for the IC-7610. */

#define _GNU_SOURCE

#include <ftd3xx.h>
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { CMD_OUT = 0x02, CMD_IN = 0x82, IQ_IN = 0x84, BLOCK_SIZE = 256 * 1024 };
enum { CIV_RADIO = 0x98, CIV_PC = 0xe0, EXPECTED_BYTES_PER_SEC = 7680000 };

static volatile sig_atomic_t interrupted;

static void stop(int signo)
{
    (void)signo;
    interrupted = 1;
}

static FT_HANDLE open_icom(void)
{
    DWORD count = 0;
    FT_STATUS status = FT_CreateDeviceInfoList(&count);
    if (status != FT_OK || count == 0) {
        fprintf(stderr, "FTDI-Gerätesuche: Status %u, %u Geräte.\n", status, count);
        return NULL;
    }

    FT_DEVICE_LIST_INFO_NODE *nodes = calloc(count, sizeof(*nodes));
    if (!nodes) {
        perror("FTDI-Geräteliste");
        return NULL;
    }
    status = FT_GetDeviceInfoList(nodes, &count);
    FT_HANDLE handle = NULL;
    if (status == FT_OK) {
        for (DWORD i = 0; i < count; ++i) {
            if (nodes[i].ID != 0x0c260029) {
                continue;
            }
            if (!(nodes[i].Flags & FT_FLAGS_SUPERSPEED)) {
                fprintf(stderr, "Der IC-7610 hat keinen SuperSpeed-Link.\n");
                break;
            }
            status = FT_Create((void *)(uintptr_t)i, FT_OPEN_BY_INDEX, &handle);
            if (status != FT_OK) {
                fprintf(stderr, "IC-7610 öffnen: FTDI-Status %u.\n", status);
                handle = NULL;
            }
            break;
        }
    }
    free(nodes);
    if (handle == NULL) {
        fprintf(stderr, "IC-7610 (0c26:0029) konnte nicht geöffnet werden.\n");
    }
    return handle;
}

static int civ(FT_HANDLE handle, const uint8_t *command, size_t command_size,
               size_t prefix_size, uint8_t *reply, size_t reply_size)
{
    uint8_t request[16] = {0xfe, 0xfe, CIV_RADIO, CIV_PC};
    memcpy(request + 4, command, command_size);
    size_t length = 4 + command_size;
    request[length++] = 0xfd;
    while (length % 4 != 0) {
        request[length++] = 0xff;
    }

    ULONG transferred = 0;
    FT_STATUS status = FT_WritePipe(handle, CMD_OUT, request, length, &transferred, 1000);
    if (status != FT_OK || transferred != length) {
        fprintf(stderr, "CI-V %02X senden: FTDI-Status %u, %u/%zu Byte.\n",
                command[0], status, transferred, length);
        return 0;
    }

    for (int attempt = 0; attempt < 16; ++attempt) {
        uint8_t response[512];
        transferred = 0;
        status = FT_ReadPipe(handle, CMD_IN, response, sizeof(response), &transferred, 350);
        if (status != FT_OK && status != FT_TIMEOUT) {
            fprintf(stderr, "CI-V %02X empfangen: FTDI-Status %u.\n", command[0], status);
            return 0;
        }

        if (getenv("IQ_DEBUG")) {
            fprintf(stderr, "USB-CI-V %02X %02X RX (FTDI %u, %u Byte):",
                    command[0], command_size > 1 ? command[1] : 0, status, transferred);
            for (size_t i = 0; i < transferred && i < 64; ++i) {
                fprintf(stderr, " %02X", response[i]);
            }
            fputc('\n', stderr);
        }
        for (size_t i = 0; i + 6 <= transferred; ++i) {
            if (memcmp(response + i, (const uint8_t[]){0xfe, 0xfe, CIV_PC, CIV_RADIO}, 4) != 0) {
                continue;
            }
            const uint8_t *body = response + i + 4;
            const size_t available = transferred - i - 4;
            if (body[0] == 0xfa && body[1] == 0xfd) {
                fprintf(stderr, "IC-7610 hat CI-V %02X %02X abgewiesen.\n",
                        command[0], command_size > 1 ? command[1] : 0);
                return 0;
            }
            if (reply_size == 0) {
                if (body[0] == 0xfb && body[1] == 0xfd) {
                    return 1;
                }
            } else if (available >= prefix_size + reply_size + 1 &&
                       memcmp(body, command, prefix_size) == 0 &&
                       body[prefix_size + reply_size] == 0xfd) {
                memcpy(reply, body + prefix_size, reply_size);
                return 1;
            }
        }
        nanosleep(&(const struct timespec){.tv_nsec = 20000000L}, NULL);
    }
    fprintf(stderr, "Keine passende USB-CI-V-Antwort für %02X %02X.\n",
            command[0], command_size > 1 ? command[1] : 0);
    return 0;
}

static int read_civ(FT_HANDLE handle, const uint8_t *command, size_t command_size,
                    uint8_t *reply, size_t reply_size)
{
    return civ(handle, command, command_size, command_size, reply, reply_size);
}

static int write_civ(FT_HANDLE handle, const uint8_t *command, size_t command_size)
{
    return civ(handle, command, command_size, 0, NULL, 0);
}

static uint64_t frequency(const uint8_t bcd[5])
{
    uint64_t hz = 0;
    for (int i = 4; i >= 0; --i) {
        hz = hz * 100 + 10 * (bcd[i] >> 4) + (bcd[i] & 0xf);
    }
    return hz;
}

static double seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec * 1e-9;
}

enum { TCP_PORT = 50010, SAMPLE_RATE = 1920000, SEND_TIMEOUT_MS = 250 };
static const char default_pi_address[] = "192.168.10.140";

static void write_be(uint8_t *output, size_t n, uint64_t value)
{
    for (size_t i = n; i > 0; --i, value >>= 8) {
        output[i - 1] = (uint8_t)value;
    }
}

/* Return 1 on success, 0 if the client left, and -1 on a stalled send. */
static int send_bytes(int client, const uint8_t *data, size_t size,
                      double deadline)
{
    while (size && !interrupted) {
        const double remaining_ms = (deadline - seconds()) * 1000;
        if (remaining_ms <= 0) {
            return -1;
        }
        struct pollfd socket = {.fd = client, .events = POLLOUT | POLLRDHUP};
        const int ready = poll(&socket, 1, (int)remaining_ms + 1);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            return -1;
        }
        if (socket.revents & (POLLERR | POLLHUP | POLLRDHUP | POLLNVAL)) {
            return 0;
        }
        const ssize_t sent = send(client, data, size, MSG_NOSIGNAL);
        if (sent < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        }
        if (sent <= 0) {
            return 0;
        }
        data += sent;
        size -= (size_t)sent;
    }
    return size == 0;
}

static int client_left(int client)
{
    struct pollfd socket = {.fd = client, .events = POLLRDHUP};
    return poll(&socket, 1, 0) > 0 &&
           (socket.revents & (POLLERR | POLLHUP | POLLRDHUP | POLLNVAL));
}

static void reject_extra_client(int listener)
{
    const int extra = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (extra >= 0) {
        close(extra);
    }
}

static int read_samples(FT_HANDLE handle, uint8_t *buffer, ULONG *count, int priming)
{
    *count = 0;
    FT_STATUS status = FT_ReadPipe(handle, IQ_IN, buffer, BLOCK_SIZE, count, 2000);
    if (priming && status == FT_TIMEOUT && *count == 0 && !interrupted) {
        status = FT_ReadPipe(handle, IQ_IN, buffer, BLOCK_SIZE, count, 2000);
    }
    if ((status != FT_OK && status != FT_TIMEOUT) || *count == 0 ||
        *count > BLOCK_SIZE || *count % 4 != 0) {
        fprintf(stderr, "I/Q-Lesevorgang: FTDI-Status %u, %u Byte.\n", status, *count);
        return 0;
    }
    return 1;
}

enum { CONTROL_OK = 0, CONTROL_INVALID = 1, CONTROL_TRANSMITTING = 2 };
enum { MAX_CONTROL_LINE = 63 };

static void encode_frequency(uint64_t hz, uint8_t bcd[5])
{
    for (size_t i = 0; i < 5; ++i, hz /= 100) {
        bcd[i] = (uint8_t)(((hz / 10 % 10) << 4) | (hz % 10));
    }
}

static int set_sub_frequency(FT_HANDLE handle, const uint8_t bcd[5])
{
    uint8_t command[7] = {0x25, 0x01};
    memcpy(command + 2, bcd, 5);
    return write_civ(handle, command, sizeof(command));
}

static int verify_sub_frequency(FT_HANDLE handle, const uint8_t expected[5])
{
    uint8_t observed[5];
    return read_civ(handle, (const uint8_t[]){0x25, 0x01}, 2, observed, sizeof(observed)) &&
           memcmp(observed, expected, sizeof(observed)) == 0;
}

struct control_line {
    char text[MAX_CONTROL_LINE + 1];
    size_t length;
};

/* A command can span TCP segments. At most one line is handled per I/Q block. */
static int receive_command(int client, struct control_line *command)
{
    for (;;) {
        unsigned char byte;
        const ssize_t count = recv(client, &byte, 1, MSG_DONTWAIT);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
            return 0;
        }
        if (count <= 0) {
            return -1;
        }
        if (byte == '\n') {
            command->text[command->length] = '\0';
            command->length = 0;
            return 1;
        }
        if (byte < 0x20 || byte > 0x7e || command->length == MAX_CONTROL_LINE) {
            return -1;
        }
        command->text[command->length++] = (char)byte;
    }
}

static int send_control_reply(int client, uint64_t index, unsigned status, uint64_t center)
{
    uint8_t frame[24] = {0};
    write_be(frame, 8, index);
    /* A zero payload length reserves the frame for the following control reply. */
    write_be(frame + 12, 4, status);
    write_be(frame + 16, 8, center);
    return send_bytes(client, frame, sizeof(frame), seconds() + SEND_TIMEOUT_MS / 1000.0);
}

static int handle_control(FT_HANDLE handle, int client, const char *command,
                          uint64_t sample_index, uint64_t *center, uint8_t *buffer,
                          int *changed_sub, int *changed_iq)
{
    if (strcmp(command, "GET SUB") == 0) {
        return send_control_reply(client, sample_index, CONTROL_OK, *center);
    }

    const char prefix[] = "SET SUB ";
    const char *value = command + sizeof(prefix) - 1;
    if (strncmp(command, prefix, sizeof(prefix) - 1) != 0 || *value < '0' || *value > '9') {
        return send_control_reply(client, sample_index, CONTROL_INVALID, *center);
    }
    errno = 0;
    char *end;
    const uint64_t target = strtoull(value, &end, 10);
    if (errno || *end || target < 30000 || target > 60000000) {
        return send_control_reply(client, sample_index, CONTROL_INVALID, *center);
    }
    if (target == *center) {
        return send_control_reply(client, sample_index, CONTROL_OK, *center);
    }

    uint8_t transmitting, selected, bcd[5];
    if (!read_civ(handle, (const uint8_t[]){0x1c, 0x00}, 2, &transmitting, 1)) {
        return -2;
    }
    if (transmitting) {
        return send_control_reply(client, sample_index, CONTROL_TRANSMITTING, *center);
    }

    *changed_iq = 1;
    if (!write_civ(handle, (const uint8_t[]){0x1a, 0x0b, 0x00}, 3) ||
        FT_AbortPipe(handle, IQ_IN) != FT_OK) {
        return -2;
    }
    *changed_sub = 1;
    encode_frequency(target, bcd);
    if (!set_sub_frequency(handle, bcd) ||
        !verify_sub_frequency(handle, bcd) ||
        !write_civ(handle, (const uint8_t[]){0x1a, 0x0b, 0x02}, 3) ||
        !read_civ(handle, (const uint8_t[]){0x1a, 0x0b}, 2, &selected, 1) ||
        selected != 2) {
        return -2;
    }

    /* The first USB block after enabling capture can contain settling samples. */
    ULONG discard;
    if (!read_samples(handle, buffer, &discard, 1)) {
        return -2;
    }
    *center = target;
    printf("SUB auf %" PRIu64 " Hz eingestellt und zurückgelesen.\n", target);
    return send_control_reply(client, sample_index, CONTROL_OK, *center);
}

static int stream_tcp(FT_HANDLE handle, int listener, int client, uint64_t target_hz,
                      int *changed_sub, int *changed_iq)
{
    uint8_t buffer[BLOCK_SIZE];
    ULONG count;
    if (interrupted || client_left(client)) {
        return 1;
    }
    /* Discard the first USB block, as in the local test. */
    if (!read_samples(handle, buffer, &count, 1)) {
        return interrupted || client_left(client);
    }
    uint8_t header[24] = {'I', 'C', '7', '6', 'I', 'Q', '0', '1'};
    write_be(header + 8, 4, SAMPLE_RATE);
    write_be(header + 12, 4, 1); /* CS16 little-endian, I then Q */
    write_be(header + 16, 8, target_hz);
    int result = send_bytes(client, header, sizeof(header), seconds() + 0.25);

    uint64_t sample_index = 0;
    struct control_line command = {0};
    const double start = seconds();
    while (result > 0 && !interrupted && !client_left(client)) {
        reject_extra_client(listener);
        const int control = receive_command(client, &command);
        if (control < 0) {
            break;
        }
        if (control > 0) {
            result = handle_control(handle, client, command.text, sample_index,
                                    &target_hz, buffer, changed_sub, changed_iq);
            if (result <= 0) {
                break;
            }
        }
        if (!read_samples(handle, buffer, &count, 0)) {
            return interrupted || client_left(client);
        }
        uint8_t frame[12];
        write_be(frame, 8, sample_index);
        write_be(frame + 8, 4, count);
        const double deadline = seconds() + SEND_TIMEOUT_MS / 1000.0;
        result = send_bytes(client, frame, sizeof(frame), deadline);
        if (result > 0) {
            result = send_bytes(client, buffer, count, deadline);
        }
        if (result > 0) {
            sample_index += count / 4;
        }
    }
    fprintf(stdout, "TCP-Sitzung: %" PRIu64 " komplexe Samples in %.2f s gesendet.\n",
            sample_index, seconds() - start);
    if (result < 0 && !interrupted) {
        fprintf(stderr, "%s\n", result == -2
                ? "TCP-Sitzung beendet: SUB-Umschaltung fehlgeschlagen."
                : "TCP-Sitzung beendet: Empfänger liest zu langsam.");
        return 0;
    }
    return 1;
}

static int test_stream(FT_HANDLE handle, FILE *report, uint8_t *snapshot, size_t *snapshot_size)
{
    uint8_t buffer[BLOCK_SIZE];
    uint64_t bytes = 0, nonzero = 0;
    unsigned max_value = 0;
    double first = 0, last = 0;
    for (;;) {
        ULONG count;
        if (!read_samples(handle, buffer, &count, 0)) {
            return 0;
        }
        last = seconds();
        if (first == 0) {
            first = last;
            continue;
        }

        if (snapshot && count > *snapshot_size) {
            memcpy(snapshot, buffer, count);
            *snapshot_size = count;
        }
        bytes += count;
        for (size_t i = 0; i < count; i += 4) {
            const int in_phase = (int16_t)(buffer[i] | (buffer[i + 1] << 8));
            const int quadrature = (int16_t)(buffer[i + 2] | (buffer[i + 3] << 8));
            nonzero += in_phase != 0 || quadrature != 0;
            const unsigned i_peak = abs(in_phase);
            const unsigned q_peak = abs(quadrature);
            const unsigned peak = i_peak > q_peak ? i_peak : q_peak;
            if (peak > max_value) {
                max_value = peak;
            }
        }

        if (last - first >= 10 || interrupted) {
            break;
        }
    }

    const double elapsed = last - first;
    const double rate = bytes / elapsed;
    fprintf(report, "I/Q: %" PRIu64 " Byte, %" PRIu64 " komplexe Samples in %.3f s\n",
           bytes, bytes / 4, elapsed);
    fprintf(report, "Datenrate: %.3f MB/s, %.6f MS/s (Soll: 7.680 MB/s, 1.920 MS/s)\n",
           rate / 1e6, rate / 4e6);
    fprintf(report, "Samples mit Signal: %" PRIu64 "/%" PRIu64 ", Spitzenwert: %u/32768\n",
           nonzero, bytes / 4, max_value);
    return !interrupted && nonzero > 0 &&
           rate > EXPECTED_BYTES_PER_SEC * 0.95 && rate < EXPECTED_BYTES_PER_SEC * 1.05;
}

/* The return value 2 marks an unverified restore. The listener stops on it. */
static int radio_session(int probe, uint64_t target_hz, int capture_snapshot,
                         int listener, int client)
{
    FILE *report = capture_snapshot ? stderr : stdout;
    const uint8_t get_main[] = {0x25, 0x00};
    const uint8_t get_sub[] = {0x25, 0x01};
    const uint8_t get_dual[] = {0x07, 0xc2};
    const uint8_t get_iq[] = {0x1a, 0x0b};
    const uint8_t get_tx[] = {0x1c, 0x00};
    uint8_t target_sub[5], snapshot[BLOCK_SIZE];
    size_t snapshot_size = 0;
    encode_frequency(target_hz, target_sub);
    uint8_t main_before[5], main_at_cleanup[5], sub_before[5], observed[5];
    uint8_t dual_before, iq_before, tx, selected;
    int changed_dual = 0, changed_sub = 0, changed_iq = 0;
    int passed = 0, restored = 1, main_verified = 0;

    FT_HANDLE handle = open_icom();
    if (!handle) {
        return 1;
    }
    FT_SetPipeTimeout(handle, CMD_IN, 350);
    FT_SetPipeTimeout(handle, IQ_IN, 2000);

    if (!read_civ(handle, get_iq, sizeof(get_iq), &iq_before, 1) || iq_before > 2) {
        fprintf(stderr, "I/Q-Auswahl konnte nicht gelesen werden.\n");
        goto cleanup;
    }
    fprintf(report, "I/Q-Auswahl: %u (0 = aus, 1 = MAIN, 2 = SUB).\n", iq_before);
    if (probe) {
        passed = 1;
        goto cleanup;
    }

    if (!read_civ(handle, get_tx, sizeof(get_tx), &tx, 1) || tx != 0) {
        fprintf(stderr, "Der Test setzt voraus, dass das Gerät im Empfang ist.\n");
        goto cleanup;
    }
    if (!read_civ(handle, get_main, sizeof(get_main), main_before, sizeof(main_before)) ||
        !read_civ(handle, get_sub, sizeof(get_sub), sub_before, sizeof(sub_before)) ||
        !read_civ(handle, get_dual, sizeof(get_dual), &dual_before, 1) ||
        dual_before > 1) {
        fprintf(stderr, "Der Ausgangszustand konnte nicht vollständig gelesen werden.\n");
        goto cleanup;
    }
    fprintf(report, "Ausgangszustand: MAIN %" PRIu64 " Hz, SUB %" PRIu64
           " Hz, Dualwatch %u, I/Q-Auswahl %u\n",
           frequency(main_before), frequency(sub_before), dual_before, iq_before);

    if (interrupted || (client >= 0 && client_left(client))) {
        passed = 1;
        goto cleanup;
    }

    if (!dual_before) {
        changed_dual = 1;
        if (!write_civ(handle, (const uint8_t[]){0x07, 0xc2, 0x01}, 3)) {
            goto cleanup;
        }
    }
    if (memcmp(sub_before, target_sub, sizeof(target_sub)) != 0) {
        changed_sub = 1;
        if (!set_sub_frequency(handle, target_sub)) {
            goto cleanup;
        }
    }
    if (!verify_sub_frequency(handle, target_sub) ||
        !read_civ(handle, get_main, sizeof(get_main), observed, sizeof(observed)) ||
        memcmp(observed, main_before, sizeof(main_before)) != 0) {
        fprintf(stderr, "Frequenzkontrolle fehlgeschlagen.\n");
        goto cleanup;
    }

    if (iq_before != 2) {
        changed_iq = 1;
        if (!write_civ(handle, (const uint8_t[]){0x1a, 0x0b, 0x02}, 3)) {
            goto cleanup;
        }
    }
    if (!read_civ(handle, get_iq, sizeof(get_iq), &selected, 1) || selected != 2) {
        fprintf(stderr, "I/Q-Quelle SUB konnte nicht bestätigt werden.\n");
        goto cleanup;
    }

    if (client >= 0) {
        fprintf(report, "SUB I/Q bei %" PRIu64 " Hz über TCP.\n", target_hz);
        passed = stream_tcp(handle, listener, client, target_hz, &changed_sub, &changed_iq);
    } else {
        fprintf(report, "SUB I/Q bei %" PRIu64 " Hz, Messdauer 10 s.\n", target_hz);
        passed = test_stream(handle, report, capture_snapshot ? snapshot : NULL, &snapshot_size);
    }

cleanup:
    if (changed_iq || changed_sub || changed_dual) {
        /* MAIN may have been tuned by the operator while SUB was streaming. */
        main_verified = read_civ(handle, get_main, sizeof(get_main),
                                 main_at_cleanup, sizeof(main_at_cleanup));
    }
    if (changed_iq) {
        restored &= write_civ(handle, (const uint8_t[]){0x1a, 0x0b, iq_before}, 3);
    }
    if (changed_sub) {
        restored &= set_sub_frequency(handle, sub_before);
    }
    if (changed_dual) {
        restored &= write_civ(handle, (const uint8_t[]){0x07, 0xc2, dual_before}, 3);
    }
    if (changed_iq || changed_sub || changed_dual) {
        restored &= main_verified &&
                    read_civ(handle, get_main, sizeof(get_main), observed, sizeof(observed)) &&
                    memcmp(observed, main_at_cleanup, sizeof(main_at_cleanup)) == 0;
        restored &= read_civ(handle, get_sub, sizeof(get_sub), observed, sizeof(observed)) &&
                    memcmp(observed, sub_before, sizeof(sub_before)) == 0;
        restored &= read_civ(handle, get_dual, sizeof(get_dual), &selected, 1) &&
                    selected == dual_before;
        restored &= read_civ(handle, get_iq, sizeof(get_iq), &selected, 1) &&
                    selected == iq_before;
        fprintf(report, "Ausgangszustand %s.\n", restored ? "wiederhergestellt und geprüft"
                                                  : "nicht vollständig bestätigt");
    }
    FT_AbortPipe(handle, IQ_IN);
    FT_Close(handle);
    if (passed && restored && capture_snapshot &&
        (fwrite(snapshot, 1, snapshot_size, stdout) != snapshot_size || fflush(stdout) != 0)) {
        perror("I/Q-Ausschnitt ausgeben");
        return 1;
    }
    return restored ? (passed ? 0 : 1) : 2;
}

static int serve(uint64_t target_hz, const char *pi_address)
{
    const int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener < 0) {
        perror("TCP-Socket öffnen");
        return 1;
    }
    const int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(TCP_PORT)};
    if (inet_pton(AF_INET, pi_address, &address.sin_addr) != 1) {
        fprintf(stderr, "Ungültige Pi-Adresse.\n");
        close(listener);
        return 1;
    }
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 1) < 0) {
        perror("TCP-Server starten");
        close(listener);
        return 1;
    }
    printf("I/Q-TCP-Server wartet auf %s:%d (SUB %" PRIu64 " Hz).\n",
           pi_address, TCP_PORT, target_hz);

    int exit_status = 0;
    while (!interrupted) {
        struct pollfd socket = {.fd = listener, .events = POLLIN};
        int ready = poll(&socket, 1, 1000);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            perror("Auf I/Q-Client warten");
            exit_status = 1;
            break;
        }
        if (ready == 0) {
            continue;
        }
        struct sockaddr_in peer;
        socklen_t peer_size = sizeof(peer);
        const int client = accept4(listener, (struct sockaddr *)&peer, &peer_size,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        }
        if (client < 0) {
            perror("I/Q-Client annehmen");
            exit_status = 1;
            break;
        }
        const int buffer_size = BLOCK_SIZE;
        setsockopt(client, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
        char peer_ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
        printf("I/Q-Client %s:%u verbunden.\n", peer_ip, ntohs(peer.sin_port));
        const int result = radio_session(0, target_hz, 0, listener, client);
        close(client);
        if (result == 2) {
            fprintf(stderr, "Server stoppt, weil die Wiederherstellung nicht bestätigt wurde.\n");
            exit_status = 1;
            break;
        }
        printf("I/Q-Client %s:%u getrennt.\n", peer_ip, ntohs(peer.sin_port));
    }
    close(listener);
    return exit_status;
}

int main(int argc, char **argv)
{
    const int probe = argc == 2 && strcmp(argv[1], "--probe") == 0;
    const int server = argc > 1 && strcmp(argv[1], "--serve") == 0;
    const char *pi_address = default_pi_address;
    uint64_t target_hz = server ? 28150000 : 18118000;
    int capture_snapshot = 0;
    if (!probe) {
        for (int i = server ? 2 : 1; i < argc; ++i) {
            if (!server && strcmp(argv[i], "--snapshot") == 0) {
                capture_snapshot = 1;
            } else if (server && strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
                pi_address = argv[++i];
            } else if (strcmp(argv[i], "--sub-hz") == 0 && i + 1 < argc) {
                const char *value = argv[++i];
                char *end;
                errno = 0;
                target_hz = strtoull(value, &end, 10);
                if (errno || end == value || *end || target_hz < 30000 || target_hz > 60000000) {
                    fprintf(stderr, "Ungültige SUB-Frequenz in Hz.\n");
                    return 2;
                }
            } else {
                fprintf(stderr, "Aufruf: %s [--probe | --serve [--listen IPv4] [--sub-hz Hz] | "
                        "[--sub-hz Hz] [--snapshot]]\n", argv[0]);
                return 2;
            }
        }
    }
    struct sigaction handler = {.sa_handler = stop};
    sigemptyset(&handler.sa_mask);
    sigaction(SIGINT, &handler, NULL);
    sigaction(SIGTERM, &handler, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    return server ? serve(target_hz, pi_address) : radio_session(probe, target_hz, capture_snapshot, -1, -1);
}
