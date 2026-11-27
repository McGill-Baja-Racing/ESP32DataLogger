/* can-monitor: shows everything on the DAQ bus in a terminal, listen-only.
 *
 * Usage: can-monitor [IFACE] [--raw]
 *   IFACE  default can0 (vcan0 for the traffic generator)
 *   --raw  one line per decoded frame instead of the refreshed table
 *
 * The same decoder and live table feed the screen in step 2. */
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "can/can_socket.h"
#include "can/daq_state.h"

#define POLL_MS 10
#define REDRAW_MS 1000

static volatile sig_atomic_t running = 1;

static void stop(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static const char *health_name(uint8_t health)
{
    static const char *const names[] = {"ok", "WARNING", "ERROR", "CRITICAL"};
    return health < 4 ? names[health] : "?";
}

static const char *reason_name(uint8_t reason)
{
    static const char *const names[] = {"?", "boot", "stop", "start", "recovery", "beacon"};
    return reason < 6 ? names[reason] : "?";
}

/* Short names of the status fault bits, in health_code_t order. */
static const char *const fault_names[HEALTH_FAULT_COUNT] = {
    "missed", "dropped", "bad-time", "no-sync", "can-tx", "rx-overflow", "no-ack",
    "bus-error", "can-passive", "bus-off", "sd", "gps-data", "gps-fix", "event-lost",
};

static void format_faults(uint32_t faults, char *out, size_t size)
{
    out[0] = '\0';
    for (unsigned bit = 0; bit < HEALTH_FAULT_COUNT; bit++) {
        if (faults & (1u << bit)) {
            size_t used = strlen(out);
            snprintf(out + used, size - used, "%s%s", used ? "," : "", fault_names[bit]);
        }
    }
    if (!out[0]) snprintf(out, size, "-");
}

static void format_frame(const daq_frame_t *frame, char *out, size_t size)
{
    switch (frame->kind) {
    case DAQ_FRAME_BEACON:
        snprintf(out, size, "%s, master up %.1f s", frame->u.beacon.recording ? "recording" : "idle",
                 (double)frame->u.beacon.master_us / 1e6);
        break;
    case DAQ_FRAME_NODE_STATE:
        snprintf(out, size, "%s (%s)", frame->u.node_state.state ? "recording" : "idle",
                 reason_name(frame->u.node_state.reason));
        break;
    case DAQ_FRAME_SAMPLE: {
        int index = daq_signal_index(frame->u.sample.id);
        const daq_signal_t *signal = index >= 0 ? &daq_signals(NULL)[index] : NULL;
        snprintf(out, size, "%s = %" PRId32 " %s @ %" PRIu32 " ms", signal ? signal->name : "unlisted signal",
                 frame->u.sample.value, signal ? signal->unit : "", frame->u.sample.timestamp_ms);
        break;
    }
    case DAQ_FRAME_STATUS: {
        char faults[160];
        format_faults(frame->u.status.faults, faults, sizeof(faults));
        snprintf(out, size, "health %s, %s, faults %s", health_name(frame->u.status.health),
                 frame->u.status.mode ? "recording" : "idle", faults);
        break;
    }
    case DAQ_FRAME_EVENT:
        snprintf(out, size, "code %u error %u subject 0x%02X seq %u count %u @ %" PRIu32 " ms",
                 frame->u.event.code, frame->u.event.error, frame->u.event.subject, frame->u.event.sequence,
                 frame->u.event.count, frame->u.event.timestamp_ms);
        break;
    case DAQ_FRAME_DIAGNOSTICS:
        snprintf(out, size, "tx-err %u rx-err %u resets %u reset-reason %u", frame->u.diagnostics.tx_errors,
                 frame->u.diagnostics.rx_errors, frame->u.diagnostics.resets, frame->u.diagnostics.reset_reason);
        break;
    case DAQ_FRAME_IDENTITY:
        snprintf(out, size, "protocol %04X%s", frame->u.identity.protocol_hash,
                 frame->u.identity.pending ? " (on trial)" : "");
        break;
    case DAQ_FRAME_AIM:
        snprintf(out, size, "%u rpm, counter %u", frame->u.aim.rpm, frame->u.aim.counter);
        break;
    case DAQ_FRAME_UPDATE_COMMAND:
        snprintf(out, size, "op %u", frame->u.update_command.op);
        break;
    case DAQ_FRAME_UPDATE_REPLY:
        snprintf(out, size, "reply %u error %u", frame->u.update_reply.reply, frame->u.update_reply.error);
        break;
    default:
        out[0] = '\0';
        break;
    }
}

static void print_raw(const daq_raw_frame_t *raw, uint64_t now_ms)
{
    daq_frame_t frame = daq_decode(raw);
    char detail[256];
    format_frame(&frame, detail, sizeof(detail));
    char bytes[3 * 8 + 1] = "";
    for (unsigned i = 0; i < raw->len; i++) snprintf(bytes + 3 * i, sizeof(bytes) - 3 * i, "%02X ", raw->data[i]);
    printf("%10.3f  %03" PRIX32 "  %-24s  node %u  %-14s %s\n", (double)now_ms / 1000.0, raw->id, bytes,
           frame.node, daq_frame_kind_name(frame.kind), detail);
}

static void draw(const can_socket_t *sock, const can_link_t *link, const daq_state_t *state, uint64_t now)
{
    printf("\033[H\033[2J");
    printf("CAN %s  %s  bitrate %" PRIu32 "  %s  state %s\n", sock->ifname, link->link_read ? "up" : "?",
           link->bitrate, link->listen_only ? "LISTEN-ONLY" : "(transmit allowed)",
           link->state[0] ? link->state : can_bus_state_name(sock->bus_state));
    printf("Pi losses: socket %" PRIu32 "  controller overruns %" PRIu64 "  rx errors %" PRIu64
           "  dropped %" PRIu64 "  | frames %" PRIu32 " unknown %" PRIu32 " malformed %" PRIu32 "\n\n",
           sock->socket_drops, link->rx_over_errors, link->rx_errors, link->rx_dropped, state->frames,
           state->unknown_frames, state->malformed_frames);

    const daq_master_state_t *master = &state->master;
    if (master->beacon_seen) {
        printf("Master: beacon %" PRIu64 " ms ago, %s, up %.1f s", now - master->beacon_ms,
               master->recording ? "RECORDING" : "idle", (double)master->master_us / 1e6);
    } else {
        printf("Master: NO BEACON");
    }
    if (master->last_command != DAQ_FRAME_UNKNOWN) {
        printf(", last %s %" PRIu64 " s ago", daq_frame_kind_name(master->last_command),
               (now - master->command_ms) / 1000);
    }
    printf("\n\nNode  online   state            health    mode       tx/rx err  resets  protocol  events  gaps*  faults\n");
    for (uint8_t id = 1; id < CAN_NODE_SLOT_COUNT; id++) {
        const daq_node_state_t *node = &state->nodes[id];
        char faults[160] = "-", state_text[32] = "-", protocol[8] = "-";
        if (node->state_seen) {
            snprintf(state_text, sizeof(state_text), "%s (%s)", node->state ? "recording" : "idle",
                     reason_name(node->reason));
        }
        if (node->status_seen) format_faults(node->faults, faults, sizeof(faults));
        if (node->identity_seen) snprintf(protocol, sizeof(protocol), "%04X", node->protocol_hash);
        printf("%-4u  %-7s  %-16s %-9s %-10s %3u/%-3u    %-6u  %-8s  %-6u  %-5" PRIu32 "  %s\n", id,
               daq_node_online(state, id, now) ? "online" : "OFFLINE", state_text,
               node->status_seen ? health_name(node->health) : "-",
               node->status_seen ? (node->mode ? "recording" : "idle") : "-", node->tx_errors, node->rx_errors,
               node->resets, protocol, node->event_total, node->sequence_gaps, faults);
    }
    printf("* event gaps can also be frames the Pi missed: compare with the Pi losses above.\n\n");

    size_t count = 0;
    const daq_signal_t *signals = daq_signals(&count);
    printf("Signal             value           rate     age\n");
    for (size_t i = 0; i < count; i++) {
        const daq_signal_state_t *signal = &state->signals[i];
        if (daq_signal_fresh(state, (int)i, now)) {
            printf("%-16s %9" PRId32 " %-6s %5.0f Hz  %4" PRIu64 " ms\n", signals[i].name, signal->value,
                   signals[i].unit, (double)signal->rate_hz, now - signal->last_ms);
        } else {
            printf("%-16s   NO DATA\n", signals[i].name);
        }
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *ifname = "can0";
    bool raw = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--raw")) raw = true;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Usage: %s [IFACE] [--raw]\n", argv[0]);
            return 2;
        } else ifname = argv[i];
    }
    can_socket_t sock;
    if (!can_socket_open(&sock, ifname)) return 1;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    static daq_state_t state;
    daq_state_init(&state);
    can_link_t link;
    uint64_t next_draw = 0;
    daq_state_update_rates(&state, can_monotonic_ms());
    while (running) {
        struct pollfd wait = {.fd = sock.fd, .events = POLLIN};
        (void)poll(&wait, 1, POLL_MS);
        uint64_t now = can_monotonic_ms();
        if (can_socket_drain(&sock, &state, now, raw ? print_raw : NULL) < 0) break;
        if (!raw && now >= next_draw) {
            daq_state_update_rates(&state, now);
            can_socket_read_link(ifname, &link);
            draw(&sock, &link, &state, now);
            next_draw = now + REDRAW_MS;
        }
        if (raw) fflush(stdout);
    }
    can_socket_close(&sock);
    return 0;
}
