#ifndef CAN_SOCKET_H
#define CAN_SOCKET_H

#include <stdbool.h>
#include <stdint.h>

#include "can/daq_state.h"

/* Linux only. Reads a SocketCAN interface without ever transmitting: the socket
 * is never written to, and can0 itself is brought up listen-only. Also reports
 * what the Pi itself lost, so it is never mistaken for a node fault. */

typedef enum {
    CAN_BUS_UNKNOWN,
    CAN_BUS_ACTIVE,
    CAN_BUS_WARNING,
    CAN_BUS_PASSIVE,
    CAN_BUS_OFF,
} can_bus_state_t;

typedef struct {
    int fd;
    char ifname[16];
    uint32_t socket_drops;      /* frames the kernel dropped because this socket was full (SO_RXQ_OVFL) */
    uint32_t error_frames;
    can_bus_state_t bus_state;  /* from controller error frames; UNKNOWN until one arrives */
} can_socket_t;

typedef struct {
    bool link_read;             /* false if the interface could not be read */
    uint64_t rx_packets, rx_errors, rx_dropped, rx_over_errors;  /* /sys/class/net/<if>/statistics */
    uint32_t bitrate;           /* 0 if unknown, for example on vcan */
    bool listen_only;
    char state[16];             /* "ERROR-ACTIVE", "BUS-OFF"... or "" */
} can_link_t;

/* Opens a raw, non-blocking socket on ifname, with error frames and drop counts on.
 * Prints the reason and returns false on failure. */
bool can_socket_open(can_socket_t *sock, const char *ifname);

/* Reads every frame waiting and feeds it to state, stamped now_ms. If raw_out is
 * not NULL, each data frame is also copied to raw_out(frame, now_ms).
 * Returns the frames read, or -1 if the socket failed. */
int can_socket_drain(can_socket_t *sock, daq_state_t *state, uint64_t now_ms,
                     void (*raw_out)(const daq_raw_frame_t *frame, uint64_t now_ms));

/* Reads the interface's counters (sysfs) and its bitrate, mode and state (`ip -details`). */
void can_socket_read_link(const char *ifname, can_link_t *link);

void can_socket_close(can_socket_t *sock);

/* CLOCK_MONOTONIC in ms. */
uint64_t can_monotonic_ms(void);

const char *can_bus_state_name(can_bus_state_t state);

#endif /* CAN_SOCKET_H */
