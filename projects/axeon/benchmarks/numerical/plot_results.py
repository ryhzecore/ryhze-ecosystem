#!/usr/bin/env python3
"""Phone-friendly lines from two actual input sizes; never runs timed workloads."""
import json
import math
from pathlib import Path
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

NUMPY = "NumPy matmul (tolerance)"
COLORS = {"Axeon": "#1764c0", "C++": "#d56c24", "Python": "#8752a8", NUMPY: "#398565"}
LABELS = {"Axeon": "Axeon", "C++": "Checked C++", "Python": "Python", NUMPY: "NumPy matmul*"}
TITLES = {"scalar": "Dependent scalar recurrence", "matrix": "4 × 4 matrix–vector updates"}


def panel(axis, data, workload):
    rows = [row for row in data["summaries"] if row["workload"] == workload]
    languages = list(dict.fromkeys(row["language"] for row in rows))
    inputs = data["inputs"]
    end_values = []
    all_medians = [row["warm"]["median"] / 1e6 for row in rows]
    if min(all_medians) <= 0:
        raise ValueError("logarithmic timing chart requires positive observed medians")
    low, high = min(all_medians) / 1.8, max(all_medians) * 2.0
    if high / low < 20:
        high = low * 20
    axis.set_yscale("log")
    axis.set_ylim(low, high)
    axis.set_xlim(inputs[0] * .8, inputs[-1] * 1.75)
    axis.set_xticks(inputs, [f"{n:,}" for n in inputs])
    for language in languages:
        selected = sorted((row for row in rows if row["language"] == language), key=lambda row: row["n"])
        medians = [row["warm"]["median"] / 1e6 for row in selected]
        errors = [[max(0, row["warm"]["median"] - row["warm_ci95_ns"][0]) / 1e6 for row in selected],
                  [max(0, row["warm_ci95_ns"][1] - row["warm"]["median"]) / 1e6 for row in selected]]
        axis.errorbar(inputs, medians, yerr=errors, color=COLORS[language],
                      marker="s" if language == "Axeon" else "o",
                      markersize=8 if language == "Axeon" else 5,
                      markerfacecolor="white" if language == "Axeon" else COLORS[language],
                      linestyle="--" if language == "C++" else "-",
                      linewidth=2.5 if language == "Axeon" else 2.2, capsize=3, elinewidth=1.1)
        end_values.append((medians[-1], language))
    # Direct labels can move vertically to remain readable; observations and
    # their connecting segments stay at their actual measured coordinates.
    log_low, log_high = math.log10(low), math.log10(high)
    separation = (log_high - log_low) * .13
    labels = []
    previous = log_low - separation
    for value, language in sorted(end_values):
        position = max(math.log10(value), previous + separation)
        labels.append([position, value, language])
        previous = position
    overflow = max(0, labels[-1][0] - (log_high - separation * .4))
    for position, value, language in labels:
        label_y = 10 ** (position - overflow)
        axis.annotate(f"{LABELS[language]}\n{value:.4f} ms", xy=(inputs[-1], value),
                      xytext=(inputs[-1] * 1.16, label_y), fontsize=10, color=COLORS[language],
                      va="center", weight="bold", arrowprops={"arrowstyle": "-", "color": COLORS[language], "lw": .8})
    axis.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}"))
    axis.set_ylabel("Milliseconds · LOG SCALE", fontsize=10, weight="bold")
    axis.set_xlabel("Iterations (N) · only these two sizes measured", fontsize=10)
    axis.set_title(TITLES[workload], loc="left", fontsize=15, weight="bold", pad=16)
    axis.spines[["top", "right"]].set_visible(False)
    axis.grid(axis="y", which="major", color="#d5dae1", linewidth=.7)
    axis.set_axisbelow(True)
    axis.tick_params(labelsize=10)


def details(data, output):
    for workload in ("scalar", "matrix"):
        rows = [row for row in data["summaries"] if row["workload"] == workload]
        content = []
        for row in rows:
            lo, hi = row["warm_ci95_ns"]
            content.append([LABELS[row["language"]], f"{row['n']:,}",
                            f"{row['warm']['median']}\n[{lo}, {hi}]", str(row['cold']['median']),
                            f"{row['time_divided_by_axeon']:.4f}×", f"{row['proc_image_peak_rss_kib']['median']/1024:.3f}",
                            f"{row['outside_kernel_ns']['median']/1e6:.3f}"])
        fig, axis = plt.subplots(figsize=(12.5, 6.2))
        axis.axis("off")
        table = axis.table(cellText=content,
                           colLabels=["Implementation", "N", "Warm ns [95% interval]", "First ns", "Time / Axeon", "VmHWM MiB", "Outside ms"],
                           cellLoc="center", loc="center", colWidths=[.16, .09, .26, .13, .13, .12, .11])
        table.auto_set_font_size(False)
        table.set_fontsize(10)
        table.scale(1, 2.4)
        for (row, _), cell in table.get_celld().items():
            cell.set_edgecolor("#d5dae1")
            if row == 0:
                cell.set_facecolor("#e9eef5")
                cell.set_text_props(weight="bold")
            elif row % 2:
                cell.set_facecolor("#f5f7fa")
        fig.suptitle(TITLES[workload] + " · exact timing table", x=.055, ha="left", fontsize=17, weight="bold")
        footnote = ("\n*NumPy: tiny 4x4 library calls; componentwise tolerance 2e-12 + 2e-12*abs(reference), not exact semantic equivalence."
                    if workload == "matrix" else "")
        fig.text(.055, .035, "Warm: 21 samples across 3 processes; all samples retained. Time/Axeon >1 favors Axeon.\n"
                 "VmHWM includes hosted runtime/imports. Outside time includes launch, imports, reporting, scheduling; not pure startup.\n"
                 "Intervals resample whole processes; only 3 clusters. Full checksums, tolerances, compile phases and raw data accompany this image." + footnote,
                 fontsize=9, color="#344054")
        fig.subplots_adjust(left=.03, right=.97, top=.9, bottom=.2)
        fig.savefig(output.parent / f"{workload}-details.png", dpi=170, facecolor="white")
        plt.close(fig)


def main():
    source, output = map(Path, sys.argv[1:3])
    data = json.loads(source.read_text())
    if data.get("status") != "passed" or data.get("stage") != "measure":
        raise SystemExit("only successful measured results may be plotted")
    fig, axes = plt.subplots(2, 1, figsize=(6.75, 12.0))
    fig.subplots_adjust(left=.145, right=.985, top=.85, bottom=.29, hspace=.50)
    for axis, workload in zip(axes, ("scalar", "matrix")):
        panel(axis, data, workload)
    fig.text(.08, .97, "Strict f64 · measured CPU work", ha="left", va="top", fontsize=21, weight="bold")
    fig.text(.08, .921, "Axeon vs checked C++, ordinary Python,\nand a NumPy matrix-library comparison", va="top", fontsize=12, color="#344054")
    numpy_rows = [row for row in data["summaries"] if row["language"] == NUMPY]
    max_error = max(row["numerical_validation"]["max_abs_error"] for row in numpy_rows)
    notes = [
        "Lower is faster. Both panels use logarithmic time axes.",
        "Dots are actual medians; lines connect only N=1,000 and 4,000.",
        "Native: Clang 19.1.7 -O3, no fast math or FMA, no LTO.",
        "3 fresh processes × 7 warm calls; first call + warmup separate.",
        "Error bars: process-bootstrap 95% intervals; only 3 clusters.",
        "Ordered Axeon/C++/Python checksums match exact f64 bits.",
        f"*NumPy: state tolerance 2e-12 + 2e-12×|reference|;",
        f"  observed max absolute state error {max_error:.3g}.",
        "  Library reduction order may differ; no exact-semantics claim.",
        "  Tiny 4×4 calls include Python dispatch, not large-BLAS throughput.",
        f"Pinned CPU {data['host']['pinned_cpu']}; shared-host noise remains.",
        "Raw samples, exact tables, memory and compile phases included.",
    ]
    fig.text(.08, .245, "\n".join(notes), va="top", fontsize=9, linespacing=1.55, color="#344054")
    fig.savefig(output, dpi=160, facecolor="white")
    fig.savefig(output.with_suffix(".svg"), facecolor="white")
    plt.close(fig)
    details(data, output)


if __name__ == "__main__":
    main()
