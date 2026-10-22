#!/usr/bin/env python3
"""Render the verified 0.2 measurements; never rerun or alter the benchmark."""
from __future__ import annotations

import csv
import hashlib
import json
import os
from pathlib import Path
import statistics
import tempfile

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "axeon-social-mpl"))
os.environ.setdefault("XDG_CACHE_HOME", str(Path(tempfile.gettempdir()) / "axeon-social-cache"))

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator
from PIL import Image


HERE = Path(__file__).resolve().parent
RESULTS = HERE.parent / "results"
INPUTS = (250_000, 1_000_000)
LANGUAGES = ("Axeon", "C++", "Python")
COLORS = {"Axeon": "#1762BA", "C++": "#54715A", "Python": "#B8642A"}
BACKGROUND = "#FFFFFF"
FOREGROUND = "#161616"
MUTED = "#515B68"


def verified_data():
    raw = json.loads((RESULTS / "results.json").read_text())
    with (RESULTS / "summary.csv").open(newline="") as file:
        rows = list(csv.DictReader(file))
    assert raw["status"] == "passed"
    assert raw["inputs"] == list(INPUTS)
    assert raw["trials"] == 3 and raw["warm_samples_per_process"] == 7
    assert "axeon 0.2.0;" in raw["versions"]["axeon"]
    assert "clang version 19.1.7 " in raw["versions"]["clang"]
    assert "AMD EPYC 9V74" in raw["host"]["cpuinfo"]
    assert raw["host"]["python"].startswith("3.12.14 ")
    assert len(rows) == len(LANGUAGES) * len(INPUTS)
    medians = {}
    for language in LANGUAGES:
        medians[language] = []
        for n in INPUTS:
            records = [p for p in raw["processes"] if p["language"] == language and p["n"] == n]
            samples = [sample for p in records for sample in p["warm_ns"]]
            assert len(records) == 3 and len(samples) == 21
            assert all(len(p["warm_ns"]) == 7 for p in records)
            expected = raw["reference_checksums"][str(n)]
            assert all(checksum == expected for p in records for checksum in p["checksums"])
            median = int(statistics.median(samples))
            row = next(r for r in rows if r["language"] == language and int(r["n"]) == n)
            summary = next(s for s in raw["summaries"] if s["language"] == language and s["n"] == n)
            assert row["correct"] == "True" and summary["correct"] is True
            assert int(row["warm_median_ns"]) == median == summary["warm"]["median"]
            assert summary["warm"]["count"] == 21
            medians[language].append(median)
    for phase in raw["compilation_phases"]:
        if phase["label"] in ("axeon_llvm_optimization_codegen", "cpp_frontend_optimization_codegen"):
            assert "-O3" in phase["command"]
    return medians


def render(medians):
    plt.rcParams.update({
        "font.family": "DejaVu Sans",
        "text.color": FOREGROUND,
        "axes.labelcolor": FOREGROUND,
        "xtick.color": FOREGROUND,
        "ytick.color": FOREGROUND,
        "savefig.facecolor": BACKGROUND,
        "figure.facecolor": BACKGROUND,
    })
    fig = plt.figure(figsize=(8, 10), dpi=250)
    fig.text(.135, .948, "Warm runtime by workload size", fontsize=22, weight="bold")
    fig.text(.135, .914, "Axeon 0.2, checked C++, and ordinary Python", fontsize=14.7, color=MUTED)
    fig.text(.135, .883, "Identical 32-element checked-integer CPU workload", fontsize=13.7, color=MUTED)

    ax = fig.add_axes([.16, .43, .775, .38], facecolor=BACKGROUND)
    ax.set_yscale("log")
    ax.set_ylim(.5, 220)
    ax.set_xlim(160_000, 1_090_000)
    ax.yaxis.set_major_locator(FixedLocator([1, 10, 100]))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}"))
    ax.yaxis.set_minor_locator(NullLocator())
    ax.set_xticks(INPUTS, ["250,000", "1,000,000"])
    ax.tick_params(axis="both", labelsize=15, length=4, width=.8, pad=7)
    ax.grid(axis="y", color="#E5E8EB", linewidth=.8)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    for spine in ("left", "bottom"):
        ax.spines[spine].set_color("#62676D")
        ax.spines[spine].set_linewidth(.8)
    ax.set_ylabel("Warm median runtime (ms)", fontsize=17, labelpad=10)
    ax.set_xlabel("Workload updates (N)", fontsize=16, labelpad=13)
    fig.text(.16, .832, "Logarithmic scale · lower is faster", fontsize=15.5)

    ms = {language: [value / 1_000_000 for value in values] for language, values in medians.items()}
    # Use only the two measured coordinates per series. Hollow diamonds and
    # filled circles distinguish near-overlapping native results without jitter.
    cpp, = ax.plot(INPUTS, ms["C++"], color=COLORS["C++"], linewidth=2.7,
                   linestyle=(0, (5, 3)), marker="D", markersize=10.5,
                   markerfacecolor="white", markeredgewidth=1.7, zorder=3)
    axeon, = ax.plot(INPUTS, ms["Axeon"], color=COLORS["Axeon"], linewidth=1.8,
                     marker="o", markersize=5.8, markeredgecolor="white",
                     markeredgewidth=.5, zorder=4)
    python, = ax.plot(INPUTS, ms["Python"], color=COLORS["Python"], linewidth=2,
                      marker="o", markersize=7, markeredgecolor="white",
                      markeredgewidth=.7, zorder=3)
    ax.legend([axeon, cpp, python], LANGUAGES, loc="center left",
              bbox_to_anchor=(.02, .54), frameon=False,
              fontsize=15.5, handlelength=2.0, borderaxespad=.6, labelspacing=.7)

    # A compact scientific table keeps every recorded nanosecond visible.
    fig.text(.135, .317, "Exact warm medians (ms)", fontsize=15.5)
    fig.text(.65, .280, "N = 250,000", fontsize=13.3, ha="right", color=MUTED)
    fig.text(.935, .280, "N = 1,000,000", fontsize=13.3, ha="right", color=MUTED)
    fig.add_artist(plt.Line2D([.135, .935], [.270, .270], transform=fig.transFigure,
                             color="#D7DCE1", linewidth=.8))
    for language, y in zip(LANGUAGES, [.240, .206, .172]):
        fig.text(.135, y, language, fontsize=16)
        for x, median in zip([.65, .935], medians[language]):
            fig.text(x, y, f"{median / 1_000_000:.6f}", fontsize=15.2,
                     ha="right", fontfamily="DejaVu Sans Mono")
    fig.add_artist(plt.Line2D([.135, .935], [.155, .155], transform=fig.transFigure,
                             color="#D7DCE1", linewidth=.8))

    fig.text(.135, .130, "Axeon and checked C++ have comparable medians.", fontsize=13.2)
    fig.text(.135, .106, "Clang 19.1.7 -O3 · CPython 3.12.14 · AMD EPYC 9V74", fontsize=11.6, color=MUTED)
    fig.text(.135, .084, "3 processes · 21 warm samples per point · pinned CPU", fontsize=11.9, color=MUTED)
    fig.text(.135, .056, "Two measured sizes; two actual points per series.", fontsize=12.7)
    fig.text(.135, .034, "Lines connect points; intermediate timings were not measured.", fontsize=10.9, color=MUTED)
    fig.savefig(HERE / "social.png", dpi=250, metadata={
        "Title": "Axeon 0.2 checked-integer CPU runtime",
        "Software": "Matplotlib " + matplotlib.__version__,
    })
    plt.close(fig)
    with Image.open(HERE / "social.png") as image:
        assert image.size == (2000, 2500)
        image.resize((400, 500), Image.Resampling.LANCZOS).save(HERE / "phone-preview.png")


def write_readme(medians):
    hashes = {name: hashlib.sha256((RESULTS / name).read_bytes()).hexdigest() for name in ("summary.csv", "results.json")}
    ratios = [medians["Python"][index] / medians["Axeon"][index] for index in range(2)]
    rows = "\n".join(f"| {language} | {medians[language][0] / 1e6:.6f} | {medians[language][1] / 1e6:.6f} |" for language in LANGUAGES)
    content = f"""# Axeon 0.2 social benchmark chart

`social.png` is a 2000 × 2500 PNG (4:5), with one line chart and a compact exact-value table. Its white canvas, restrained typography, blue/green/orange palette, subtle axes, and unadorned labels follow the original `../results/benchmark.png` scientific report. There are no promotional headlines, decorative cards, gradients, logos, or oversized speedup callouts. `phone-preview.png` is its 400 × 500 inspection copy. No benchmark was rerun and no source measurements were changed.

Reproduce from the repository root:

```sh
python3 benchmarks/social-0.2/plot_social.py
```

Requires Python, Matplotlib (rendered with {matplotlib.__version__}), and Pillow. The script verifies every median against both source summaries and the 21 raw warm samples before rendering, checks the saved correctness results, and checks benchmark version/hardware/toolchain metadata.

| Warm median, ms | 250,000 updates | 1,000,000 updates |
| --- | ---: | ---: |
{rows}

Data: `../results/summary.csv` and `../results/results.json`; verified Axeon 0.2.0 integer-state recurrence on a local 32-element array. Each point pools seven warm kernel samples from each of three fresh processes (21 samples); one cold call and one extra warmup per process are excluded. Initialization and checksum are timed; imports, process startup and printing are not. All retained checksums passed.

Axeon and optimized checked C++ use Clang 19.1.7 `-O3`, x86-64 Linux, no LTO; ordinary CPython is 3.12.14. Saved host: AMD EPYC 9V74, pinned logical CPU 0. Python/Axeon warm-median ratios are {ratios[0]:.9f}× and {ratios[1]:.9f}× for this measured workload and host; the figure uses the measured times rather than a speedup callout. Native medians differ by under 2%, with overlapping saved 95% intervals, so no Axeon-over-C++ speedup is claimed. This is not a floating-point, numerical-library, tensor, GPU, or general application benchmark.

The y-axis is prominently labeled logarithmic; it has no zero origin. Exactly two real measurements per series are connected by straight guide lines in plot coordinates. There is no fitted curve, no added data, and no claim of measured intermediate timings or scaling outside these sizes. Native curves nearly overlap; hollow green diamonds and filled blue circles remain at their exact coordinates. The table prints all six medians to the recorded nanosecond precision in milliseconds. This compact chart does not show uncertainty bands; use the original report for IQRs and process-cluster bootstrap intervals. Three process clusters and uncontrolled shared-host interference limit precision and generalization.

Source SHA-256:

- `summary.csv`: `{hashes['summary.csv']}`
- `results.json`: `{hashes['results.json']}`
"""
    (HERE / "README.md").write_text(content)


if __name__ == "__main__":
    data = verified_data()
    render(data)
    write_readme(data)
    print(HERE / "social.png")
