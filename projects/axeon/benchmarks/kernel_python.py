"""Ordinary CPython list/integer workload and timing wrapper; no numerical libraries."""
import json
import sys
from time import perf_counter_ns


def kernel(n):
    state = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
             16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31]
    acc = 17
    for i in range(n):
        idx = i % 32
        j = (idx + 1) % 32
        acc = (state[idx] * 1664525 + state[j] + acc) % 1000003
        state[idx] = acc
    return sum(state)


def independent_reference(n):
    """Separate for-loop implementation; used outside measured kernels."""
    values = list(range(32))
    previous = 17
    for step in range(n):
        slot = step % len(values)
        neighbor = (slot + 1) % len(values)
        next_value = (1664525 * values[slot] + values[neighbor] + previous) % 1000003
        values[slot] = next_value
        previous = next_value
    return sum(values)


def image_peak_rss_kib():
    with open("/proc/self/status", encoding="ascii") as status:
        for line in status:
            fields = line.split()
            if fields and fields[0] == "VmHWM:":
                if len(fields) != 3 or fields[2] != "kB":
                    raise RuntimeError("unexpected VmHWM format")
                value = int(fields[1])
                if value <= 0:
                    raise RuntimeError("VmHWM must be positive")
                return value
    raise RuntimeError("VmHWM missing from /proc/self/status")


def main():
    reference = len(sys.argv) == 3 and sys.argv[1] == "--reference"
    n = int(sys.argv[-1])
    if n < 0 or n > 1000000:
        raise SystemExit("N must be between 0 and 1000000")
    if reference:
        print(independent_reference(n))
        return
    elapsed, checksums = [], []
    for _ in range(9):
        start = perf_counter_ns()
        answer = kernel(n)
        elapsed.append(perf_counter_ns() - start)
        checksums.append(answer)
    peak_rss_kib = image_peak_rss_kib()
    print(json.dumps({"n": n, "cold_ns": elapsed[0], "warmup_ns": elapsed[1],
                      "warm_ns": elapsed[2:], "checksums": checksums,
                      "proc_image_peak_rss_kib": peak_rss_kib}))


if __name__ == "__main__":
    main()
