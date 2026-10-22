#!/usr/bin/env python3
"""One standalone figure from saved measured results; no timings are performed here."""
import json
from pathlib import Path
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


def main():
    source, destination = map(Path, sys.argv[1:3])
    data = json.loads(source.read_text())
    if data["status"] != "passed":
        raise SystemExit("refusing to plot an unsuccessful benchmark as a completed result")
    languages = ["Axeon", "C++", "Python"]
    colors = {"Axeon": "#1565c0", "C++": "#547058", "Python": "#be682e"}
    rows = {(row["language"], row["n"]): row for row in data["summaries"]}
    fig = plt.figure(figsize=(16, 11), constrained_layout=False)
    grid = fig.add_gridspec(2, 2, left=.065, right=.975, top=.88, bottom=.23,
                           height_ratios=[1, .72], hspace=.38, wspace=.22)
    timings = fig.add_subplot(grid[0, 0])
    memory = fig.add_subplot(grid[0, 1])
    width = .24
    for index, language in enumerate(languages):
        x = [position + (index - 1) * width for position in range(len(data["inputs"]))]
        selected = [rows[language, n] for n in data["inputs"]]
        ms = [row["warm"]["median"] / 1e6 for row in selected]
        error = [[max(0, row["warm"]["median"] - row["warm_ci95_ns"][0]) / 1e6 for row in selected],
                 [max(0, row["warm_ci95_ns"][1] - row["warm"]["median"]) / 1e6 for row in selected]]
        bars = timings.bar(x, ms, width=.21, color=colors[language], label=language,
                           yerr=error, capsize=4)
        for bar, value in zip(bars, ms):
            timings.text(bar.get_x() + bar.get_width()/2, value * 1.14, f"{value:.3f}",
                         ha="center", va="bottom", fontsize=9)
        rss = [row["proc_image_peak_rss_kib"]["median"] / 1024 for row in selected]
        bars = memory.bar(x, rss, width=.21, color=colors[language], label=language)
        for bar, value in zip(bars, rss):
            memory.text(bar.get_x() + bar.get_width()/2, value, f"{value:.2f}",
                        ha="center", va="bottom", fontsize=9)
    ticks = [f"N = {n:,}" for n in data["inputs"]]
    for axis in [timings, memory]:
        axis.set_xticks(range(len(ticks)), ticks)
        axis.spines[["top", "right"]].set_visible(False)
        axis.grid(axis="y", alpha=.17)
        axis.set_axisbelow(True)
    timings.set_yscale("log")
    timings.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}"))
    timings.set_ylabel("Kernel time (milliseconds, logarithmic scale)")
    timings.set_title("Warm kernel median and cluster-bootstrap 95% interval", loc="left", fontsize=11)
    timings.margins(y=.25)
    timings.legend(frameon=False, loc="upper left")
    memory.set_ylabel("Post-exec image VmHWM (MiB; median of three processes)")
    memory.set_title("Hosted process memory includes runtime and driver", loc="left", fontsize=11)
    memory.margins(y=.2)
    table_axis = fig.add_subplot(grid[1, :])
    table_axis.axis("off")
    contents = []
    for n in data["inputs"]:
        for language in languages:
            row = rows[language, n]
            lo, hi = row["warm_ci95_ns"]
            contents.append([f"{n:,}", language,
                f"{row['warm']['median']/1e6:.6f}\n[{lo/1e6:.6f}, {hi/1e6:.6f}]",
                f"{row['cold']['median']/1e6:.6f}", f"{row['time_divided_by_axeon']:.4f}×",
                f"{row['proc_image_peak_rss_kib']['median']/1024:.4f}",
                f"{row['outside_kernel_ns']['median']/1e6:.6f}",
                f"PASS\n{row['expected_checksum']}"])
    headers = ["Iterations", "Language", "Warm ms [95% CI]", "First-call ms",
               "Time / Axeon†", "VmHWM MiB", "Outside ms‡", "Checksum"]
    table = table_axis.table(cellText=contents, colLabels=headers, loc="center", cellLoc="center",
                             colWidths=[.09, .08, .23, .13, .12, .09, .14, .12])
    table.auto_set_font_size(False)
    table.set_fontsize(9)
    table.scale(1, 2.3)
    for (r, c), cell in table.get_celld().items():
        cell.set_edgecolor("#d5d9df")
        if r == 0:
            cell.set_facecolor("#e9eef4")
            cell.set_text_props(weight="bold")
        elif r % 2:
            cell.set_facecolor("#f7f9fb")
    fig.suptitle("Axeon, checked C++, and ordinary Python", x=.065, ha="left", y=.975,
                 fontsize=22, weight="bold")
    fig.text(.065, .927, "Identical 32-element integer recurrence · runtime N · Clang 19.1.7 -O3 · CPU only",
             fontsize=12, color="#344054")
    phases = {phase["label"]: phase["wall_ns"] / 1e6 for phase in data["compilation_phases"]}
    compile_lines = [
        f"Build phases (process wall ms): Axeon frontend {phases['axeon_frontend_emit_ir']:.3f}; LLVM/codegen {phases['axeon_llvm_optimization_codegen']:.3f}; link {phases['axeon_link']:.3f}.",
        f"C++ frontend/codegen {phases['cpp_frontend_optimization_codegen']:.3f}; link {phases['cpp_link']:.3f}; Python py_compile {phases['python_bytecode_compile']:.3f}; shared runtime {phases['shared_runtime_compile']:.3f}.",
    ]
    compiler_seconds = data.get("compiler_build_wall_seconds_supplied_by_caller")
    compile_lines.append("Rust compiler build: " + (f"{compiler_seconds:.3f} s (separately measured by caller)." if compiler_seconds is not None else "not included here; reported separately by caller."))
    footer = [
        "Each series: 3 fresh processes; first call recorded, 1 further warmup, then 7 warm calls per process. Every checksum verified.",
        "† Time / Axeon: Python/Axeon or C++/Axeon. Above 1 means Axeon is faster; below 1 means Axeon is slower.",
        "‡ Process wall minus all 9 kernel calls; includes launch/imports/reporting/scheduling, not pure startup. VmHWM is not incremental kernel memory.",
        "Memory: /proc/self/status VmHWM after kernels, before formatting. Separate wait4 lifetime peaks, including pre-exec state, are retained in raw data.",
        "Intervals resample whole processes (1,000 seeded replicates); 3 clusters on a shared host provide limited uncertainty estimates.",
        *compile_lines,
        f"Pinned logical CPU {data['host']['pinned_cpu']}. Full raw samples, exact nanoseconds, commands, hashes, and assembly evidence accompany this figure.",
    ]
    fig.text(.065, .19, "\n".join(footer), va="top", fontsize=9, linespacing=1.5, color="#344054")
    fig.savefig(destination, dpi=170, facecolor="white")
    plt.close(fig)


if __name__ == "__main__":
    main()
