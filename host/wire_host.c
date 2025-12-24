/* wire_host.c — GDB TCP proxy for wire targets
 *
 * Acts as a transparent bridge:
 *
 *   GDB  ←──[raw TCP RSP]──→  wire-host  ←──[raw UART RSP]──→  firmware
 *
 * Usage:
 *   wire-host --port /dev/ttyUSB0 --baud 115200 [--tcp-port 3333]
 *   wire-host --port /dev/pts/3   --baud 0      [--tcp-port 3333]   (QEMU PTY)
 *
 * Then in GDB:
 *   (gdb) target remote :3333
 *
 * SPDX-License-Identifier: MIT
 */

#include "wire_host.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE    4096
#define DEFAULT_TCP 3333

/* ── Argument parsing ────────────────────────────────────────────────────── */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s --port <device> --baud <rate> [--tcp-port <port>]\n"
        "\n"
        "  --port      serial device (e.g. /dev/ttyUSB0) or QEMU PTY (e.g. /dev/pts/3)\n"
        "  --baud      baud rate (e.g. 115200); use 0 for PTY/virtual devices\n"
        "  --tcp-port  TCP port for GDB to connect to (default: %d)\n"
        "\n"
        "Example (hardware):\n"
        "  wire-host --port /dev/ttyUSB0 --baud 115200\n"
        "  arm-none-eabi-gdb firmware.elf -ex 'target remote :3333'\n"
        "\n"
        "Example (QEMU):\n"
        "  qemu-system-arm -M mps2-an385 -kernel firmware.elf -serial pty\n"
        "  # QEMU prints: char device redirected to /dev/pts/3\n"
        "  wire-host --port /dev/pts/3 --baud 0\n",
        argv0, DEFAULT_TCP);
}

/* ── TCP server ──────────────────────────────────────────────────────────── */

static int tcp_listen(int port)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); return -1;
    }
    if (listen(srv, 1) < 0) {
        perror("listen"); close(srv); return -1;
    }
    return srv;
}

/* ── Proxy event loop ────────────────────────────────────────────────────── */

static void proxy_loop(int gdb_fd, int uart_fd)
{
    uint8_t buf[BUF_SIZE];

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(gdb_fd,  &rfds);
        FD_SET(uart_fd, &rfds);
        int maxfd = (gdb_fd > uart_fd ? gdb_fd : uart_fd) + 1;

        int n = select(maxfd, &rfds, NULL, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* GDB → UART */
        if (FD_ISSET(gdb_fd, &rfds)) {
            ssize_t r = read(gdb_fd, buf, sizeof(buf));
            if (r <= 0) {
                fprintf(stderr, "wire-host: GDB disconnected\n");
                break;
            }
            ssize_t w = write(uart_fd, buf, (size_t)r);
            if (w != r) { perror("write(uart)"); break; }
        }

        /* UART → GDB */
        if (FD_ISSET(uart_fd, &rfds)) {
            ssize_t r = read(uart_fd, buf, sizeof(buf));
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                perror("read(uart)");
                break;
            }
            if (r == 0) continue;  /* timeout (VTIME), no data */
            ssize_t w = write(gdb_fd, buf, (size_t)r);
            if (w != r) { perror("write(gdb)"); break; }
        }
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *port    = NULL;
    int         baud    = -1;
    int         tcp_port = DEFAULT_TCP;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = argv[++i];
        else if (strcmp(argv[i], "--baud") == 0 && i + 1 < argc)
            baud = atoi(argv[++i]);
        else if (strcmp(argv[i], "--tcp-port") == 0 && i + 1 < argc)
            tcp_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]); return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("wire-host 1.0.0\n"); return 0;
        } else {
            fprintf(stderr, "wire-host: unknown option: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    if (!port || baud < 0) {
        usage(argv[0]); return 1;
    }

    /* Open serial / PTY */
    int uart_fd = wire_serial_open(port, baud);
    if (uart_fd < 0) return 1;

    /* Create TCP server */
    int srv_fd = tcp_listen(tcp_port);
    if (srv_fd < 0) { close(uart_fd); return 1; }

    fprintf(stderr, "wire-host: listening on :%d  (device: %s, baud: %s)\n",
            tcp_port, port, baud == 0 ? "pty" : argv[0]); /* argv[0] won't show baud, see below */
    fprintf(stderr, "wire-host: connect GDB with:  target remote :%d\n", tcp_port);
    fprintf(stderr, "wire-host: waiting for GDB connection...\n");

    /* Accept loop: reconnect if GDB disconnects */
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int gdb_fd = accept(srv_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (gdb_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }

        /* Disable Nagle for lower latency */
        int flag = 1;
        setsockopt(gdb_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        fprintf(stderr, "wire-host: GDB connected\n");
        proxy_loop(gdb_fd, uart_fd);
        close(gdb_fd);
        fprintf(stderr, "wire-host: waiting for next GDB connection...\n");
    }

    close(srv_fd);
    close(uart_fd);
    return 0;
}
