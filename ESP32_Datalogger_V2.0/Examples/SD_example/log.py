import numpy as np
import os
import glob
import csv

# ===============================
# USER CONFIGURATION
# ===============================

# Number of int32 fields per sample
NUM_FIELDS_PER_SAMPLE = 5  

# Column headers for CSV (must match fields)
COLUMN_TITLES = ["t_ms", "raw", "mv", "val3", "val4"]

# Folder where log_*.bin files are located
LOG_DIR = "./output/"


# ===============================
# Helper: safe CSV filename
# ===============================
def get_csv_path(bin_path):
    base, _ = os.path.splitext(bin_path)
    return base + ".csv"


# ===============================
# Process a single log file
# ===============================
def process_log_file(filepath):
    print(f"\n=== Processing {filepath} ===")

    raw = np.fromfile(filepath, dtype=np.int32)

    if len(raw) % NUM_FIELDS_PER_SAMPLE != 0:
        print(f"⚠ File size not divisible by {NUM_FIELDS_PER_SAMPLE}. Size={len(raw)}")
        return

    samples = raw.reshape(-1, NUM_FIELDS_PER_SAMPLE)
    t = samples[:, 0]

    # Compute dt and convert to int64 to avoid overflow issues
    dt = np.diff(t).astype(np.int64)

    # Handle 32-bit signed wraparound:
    WRAP_JUMP = -2**32
    dt_fixed = dt.copy()
    wrap_indices = np.where(dt == WRAP_JUMP)[0]
    dt_fixed[wrap_indices] = 1

    print(f"Total samples: {len(samples)}")
    print("First timestamps:", t[:10])
    print(f"Δt stats (ms): min={dt_fixed.min()}, max={dt_fixed.max()}, mean={dt_fixed.mean():.3f}")

    bad = np.where(dt_fixed != 1)[0]
    if len(bad) == 0:
        print("✔ Timing OK — evenly spaced 1 ms.")
    else:
        print(f"❌ REAL timing anomalies at indices: {bad[:10]}")
        print(f"Example Δt values: {dt_fixed[bad[:10]]}")

    # ============================
    # Write CSV
    # ============================
    csv_path = get_csv_path(filepath)
    print(f"→ Writing CSV: {csv_path}")

    with open(csv_path, "w", newline="") as f:
        writer = csv.writer(f)

        # Header
        if len(COLUMN_TITLES) != NUM_FIELDS_PER_SAMPLE:
            print("⚠ COLUMN_TITLES length mismatch — writing generic headers")
            writer.writerow([f"field_{i}" for i in range(NUM_FIELDS_PER_SAMPLE)])
        else:
            writer.writerow(COLUMN_TITLES)

        # Data rows
        writer.writerows(samples)

    print("CSV saved.")


# ===============================
# MAIN
# ===============================
def main():
    files = sorted(glob.glob(os.path.join(LOG_DIR, "LOG_*.BIN")))
    if not files:
        print("No log_*.bin files found.")
        return

    print(f"Found {len(files)} log files.")

    for f in files:
        process_log_file(f)


if __name__ == "__main__":
    main()



