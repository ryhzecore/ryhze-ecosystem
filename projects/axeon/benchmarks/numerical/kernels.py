"""Ordinary binary64 Python and an explicitly separate NumPy matvec comparison."""
import argparse
import json
import math
from pathlib import Path
import struct
from time import perf_counter_ns

MATRIX = (
    (0.5, 0.125, 0.0625, 0.03125),
    (0.03125, 0.5, 0.125, 0.0625),
    (0.0625, 0.03125, 0.5, 0.125),
    (0.125, 0.0625, 0.03125, 0.5),
)
FORCING = (0.03125, -0.015625, 0.0078125, -0.00390625)
INITIAL = (0.125, -0.25, 0.375, -0.5)


def bits(value):
    if math.isnan(value):
        return 0x7ff8000000000000
    return struct.unpack("=q", struct.pack("=d", value))[0]


def scalar(n):
    x = 0.125
    y = 0.5
    i = 0
    while i < n:
        x = (x * (1.0 - x)) * 3.75
        y = (y + x) / (1.5 + x * 0.5)
        i = i + 1
    return bits(x + y)


def matrix(n):
    coefficients = [0.5, 0.125, 0.0625, 0.03125,
                    0.03125, 0.5, 0.125, 0.0625,
                    0.0625, 0.03125, 0.5, 0.125,
                    0.125, 0.0625, 0.03125, 0.5]
    forcing = [0.03125, -0.015625, 0.0078125, -0.00390625]
    state = [0.125, -0.25, 0.375, -0.5]
    next_state = [0.0, 0.0, 0.0, 0.0]
    i = 0
    while i < n:
        drive = float(i % 97) / 97.0 - 0.5
        row = 0
        while row < 4:
            acc = 0.0
            col = 0
            while col < 4:
                acc = acc + coefficients[row * 4 + col] * state[col]
                col = col + 1
            next_state[row] = acc + forcing[row] * drive
            row = row + 1
        copy = 0
        while copy < 4:
            state[copy] = next_state[copy]
            copy = copy + 1
        i = i + 1
    checksum = 0.0
    element = 0
    while element < 4:
        checksum = checksum + state[element]
        element = element + 1
    return bits(checksum)


def independent_reference(workload, n):
    """Separate control flow/data layout; preserve explicitly required FP order."""
    if workload == "scalar":
        value, filtered = .125, .5
        for _ in range(n):
            complement = 1.0 - value
            product = value * complement
            value = product * 3.75
            numerator = filtered + value
            scaled = value * .5
            denominator = 1.5 + scaled
            filtered = numerator / denominator
        return {"checksum": bits(value + filtered), "state": [value, filtered]}
    vector = tuple(INITIAL)
    for iteration in range(n):
        drive = float(iteration % 97) / 97.0 - .5
        updated = []
        for coefficients, forcing in zip(MATRIX, FORCING):
            accumulated = 0.0
            for coefficient, value in zip(coefficients, vector):
                accumulated = accumulated + coefficient * value
            updated.append(accumulated + forcing * drive)
        vector = tuple(updated)
    checksum = 0.0
    for value in vector:
        checksum = checksum + value
    return {"checksum": bits(checksum), "state": list(vector)}


def numpy_matrix(n, np):
    # Import/setup are distinct: import occurs outside timing, but each kernel
    # initializes its own arrays and observes its own checksum inside timing.
    coefficients = np.array(MATRIX, dtype=np.float64)
    forcing = np.array(FORCING, dtype=np.float64)
    state = np.array(INITIAL, dtype=np.float64)
    next_state = np.zeros(4, dtype=np.float64)
    forcing_term = np.zeros(4, dtype=np.float64)
    for iteration in range(n):
        drive = float(iteration % 97) / 97.0 - .5
        np.matmul(coefficients, state, out=next_state)
        np.multiply(forcing, drive, out=forcing_term)
        np.add(next_state, forcing_term, out=next_state)
        np.copyto(state, next_state)
    checksum = 0.0
    for value in state:
        checksum = checksum + float(value)
    return bits(checksum), state


def image_peak_rss_kib():
    for line in Path("/proc/self/status").read_text(encoding="ascii").splitlines():
        fields = line.split()
        if fields and fields[0] == "VmHWM:":
            if len(fields) != 3 or fields[2] != "kB" or int(fields[1]) <= 0:
                raise RuntimeError("positive VmHWM required")
            return int(fields[1])
    raise RuntimeError("VmHWM missing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workload", choices=["scalar", "matrix"])
    parser.add_argument("n", type=int)
    parser.add_argument("--reference", action="store_true")
    parser.add_argument("--numpy", action="store_true")
    args = parser.parse_args()
    if not 0 <= args.n <= 1_000_000:
        parser.error("N must be between 0 and 1000000")
    if args.reference:
        print(json.dumps(independent_reference(args.workload, args.n)))
        return
    if args.numpy:
        if args.workload != "matrix":
            parser.error("NumPy bulk operations are applicable only to the matrix workload")
        import numpy as np
        kernel = lambda n: numpy_matrix(n, np)
    else:
        kernel = scalar if args.workload == "scalar" else matrix
    elapsed, checksums, states = [], [], []
    for _ in range(9):
        begin = perf_counter_ns()
        result = kernel(args.n)
        elapsed.append(perf_counter_ns() - begin)
        if args.numpy:
            checksum, state = result
            states.append(state.tolist())
        else:
            checksum = result
        checksums.append(checksum)
    peak = image_peak_rss_kib()
    report = {"n": args.n, "cold_ns": elapsed[0], "warmup_ns": elapsed[1],
              "warm_ns": elapsed[2:], "checksums": checksums,
              "proc_image_peak_rss_kib": peak}
    if args.numpy:
        report.update(states=states, numpy_version=np.__version__)
    print(json.dumps(report))


if __name__ == "__main__":
    main()
