#include "can/can_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>

uint64_t can_monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

const char *can_bus_state_name(can_bus_state_t state)
{
    switch (state) {
    case CAN_BUS_ACTIVE: return "error-active";
    case CAN_BUS_WARNING: return "warning";
    case CAN_BUS_PASSIVE: return "error-passive";
    case CAN_BUS_OFF: return "bus-off";
    default: return "unknown";
    }
}

bool can_socket_open(can_socket_t *sock, const char *ifname)
{
    memset(sock, 0, sizeof(*sock));
    sock->fd = -1;
    snprintf(sock->ifname, sizeof(sock->ifname), "%s", ifname);

    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        fprintf(stderr, "can: socket: %s\n", strerror(errno));
        return false;
    }
    can_err_mask_t errors = CAN_ERR_MASK;
    int on = 1;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &errors, sizeof(errors)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof(on)) < 0) {
        fprintf(stderr, "can: setsockopt: %s\n", strerror(errno));
        close(fd);
        return false;
    }
    unsigned index = if_nametoindex(ifname);
    if (index == 0) {
        fprintf(stderr, "can: no interface %s\n", ifname);
        close(fd);
        return false;
    }
    struct sockaddr_can address = {.can_family = AF_CAN, .can_ifindex = (int)index};
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "can: bind %s: %s\n", ifname, strerror(errno));
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    sock->fd = fd;
    return true;
}

/* Controller state changes arrive as error frames. */
static void handle_error_frame(can_socket_t *sock, const struct can_frame *frame)
{
    sock->error_frames++;
    if (frame->can_id & CAN_ERR_BUSOFF) {
        sock->bus_state = CAN_BUS_OFF;
    } else if (frame->can_id & CAN_ERR_RESTARTED) {
        sock->bus_state = CAN_BUS_ACTIVE;
    } else if (frame->can_id & CAN_ERR_CRTL) {
        uint8_t status = frame->data[1];
        if (status & (CAN_ERR_CRTL_RX_PASSIVE | CAN_ERR_CRTL_TX_PASSIVE)) {
            sock->bus_state = CAN_BUS_PASSIVE;
        } else if (status & (CAN_ERR_CRTL_RX_WARNING | CAN_ERR_CRTL_TX_WARNING)) {
            sock->bus_state = CAN_BUS_WARNING;
        } else if (status & CAN_ERR_CRTL_ACTIVE) {
            sock->bus_state = CAN_BUS_ACTIVE;
        }
    }
}

int can_socket_drain(can_socket_t *sock, daq_state_t *state, uint64_t now_ms,
                     void (*raw_out)(const daq_raw_frame_t *frame, uint64_t now_ms))
{
    int count = 0;
    while (true) {
        struct can_frame frame;
        char control[CMSG_SPACE(sizeof(uint32_t))];
        struct iovec io = {.iov_base = &frame, .iov_len = sizeof(frame)};
        struct msghdr message = {
            .msg_iov = &io, .msg_iovlen = 1,
            .msg_control = control, .msg_controllen = sizeof(control),
        };
        ssize_t bytes = recvmsg(sock->fd, &message, 0);
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return count;
            if (errno == EINTR) continue;
            fprintf(stderr, "can: recvmsg: %s\n", strerror(errno));
            return -1;
        }
        if ((size_t)bytes < sizeof(frame)) continue;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
                memcpy(&sock->socket_drops, CMSG_DATA(c), sizeof(sock->socket_drops));
            }
        }
        if (frame.can_id & CAN_ERR_FLAG) {
            handle_error_frame(sock, &frame);
            continue;
        }
        daq_raw_frame_t raw = {
            .extended = (frame.can_id & CAN_EFF_FLAG) != 0,
            .remote = (frame.can_id & CAN_RTR_FLAG) != 0,
            .len = frame.can_dlc > 8 ? 8 : frame.can_dlc,
        };
        raw.id = frame.can_id & (raw.extended ? CAN_EFF_MASK : CAN_SFF_MASK);
        memcpy(raw.data, frame.data, raw.len);
        daq_state_feed(state, &raw, now_ms);
        if (raw_out) raw_out(&raw, now_ms);
        count++;
    }
}

static uint64_t read_counter(const char *ifname, const char *name, bool *ok)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", ifname, name);
    FILE *file = fopen(path, "r");
    unsigned long long value = 0;
    if (!file || fscanf(file, "%llu", &value) != 1) *ok = false;
    if (file) fclose(file);
    return value;
}

void can_socket_read_link(const char *ifname, can_link_t *link)
{
    memset(link, 0, sizeof(*link));
    bool ok = true;
    link->rx_packets = read_counter(ifname, "rx_packets", &ok);
    link->rx_errors = read_counter(ifname, "rx_errors", &ok);
    link->rx_dropped = read_counter(ifname, "rx_dropped", &ok);
    link->rx_over_errors = read_counter(ifname, "rx_over_errors", &ok);
    link->link_read = ok;

    char command[64];
    snprintf(command, sizeof(command), "ip -details link show dev %s 2>/dev/null", ifname);
    FILE *ip = popen(command, "r");
    if (!ip) return;
    char line[512];
    while (fgets(line, sizeof(line), ip)) {
        const char *found;
        if ((found = strstr(line, "bitrate "))) link->bitrate = (uint32_t)strtoul(found + 8, NULL, 10);
        if (strstr(line, "LISTEN-ONLY")) link->listen_only = true;
        if ((found = strstr(line, "can state "))) sscanf(found + 10, "%15s", link->state);
        else if ((found = strstr(line, " state ")) && strstr(line, "<")) {
            /* "can <LISTEN-ONLY> state ERROR-ACTIVE ..." */
            if (strstr(line, "can ")) sscanf(found + 7, "%15s", link->state);
        }
    }
    pclose(ip);
}

void can_socket_close(can_socket_t *sock)
{
    if (sock->fd >= 0) close(sock->fd);
    sock->fd = -1;
}
