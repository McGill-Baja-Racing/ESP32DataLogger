# Master Stable Baseline

Build with:

```bash
pio run -e MasterStable
```

This is intentionally the only master build profile. It includes SD logging,
fixed sensor IDs, global start/stop control, graceful file closure, and a
100 ms time-synchronization beacon. The master also monitors the CAN controller
and automatically initiates recovery after bus-off. See
[MASTER_ARCHITECTURE.md](MASTER_ARCHITECTURE.md).

Before vehicle use, verify repeated SD mounting, valid decoded logs, repeated
start/stop cycles, matching node timestamps, and at least 30 minutes of CAN
logging without reset.
