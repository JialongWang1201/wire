/* wire_host.h — Shared declarations for wire-host
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WIRE_HOST_H
#define WIRE_HOST_H

/* Serial port helpers */
int wire_serial_open(const char *port, int baud);

#endif /* WIRE_HOST_H */
