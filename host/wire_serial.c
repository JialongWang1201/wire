/* wire_serial.c — Serial port / PTY helpers for the host tool
 *
 * Supports:
 *   - POSIX serial ports (/dev/ttyUSB0, /dev/cu.usbserial-…)
 *   - PTY devices (/dev/pts/N) from QEMU -serial pty
 *
 * baud=0 means PTY mode: skip baud-rate configuration.
 *
 * SPDX-License-Identifier: MIT
 */

#include "wire_host.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default:
        fprintf(stderr, "wire-host: unsupported baud rate %d, defaulting to 115200\n", baud);
        return B115200;
    }
}

int wire_serial_open(const char *port, int baud)
{
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "wire-host: cannot open %s: %s\n", port, strerror(errno));
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));

    if (tcgetattr(fd, &tty) != 0) {
        /* PTY or pipe: skip termios configuration */
        if (baud == 0) return fd;
        fprintf(stderr, "wire-host: tcgetattr(%s): %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (baud != 0) {
        speed_t spd = baud_to_speed(baud);
        cfsetispeed(&tty, spd);
        cfsetospeed(&tty, spd);
    }

    /* 8N1, raw mode */
    tty.c_cflag  = (tcflag_t)(CS8 | CLOCAL | CREAD);
    tty.c_iflag  = 0;
    tty.c_oflag  = 0;
    tty.c_lflag  = 0;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;   /* 100 ms read timeout */

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "wire-host: tcsetattr(%s): %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    /* Switch back to blocking with a short timeout */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    return fd;
}
