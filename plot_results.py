#!/usr/bin/env python3
"""
Plot Speedup and unified GeoMean (Accuracy, Coverage, Timeliness) from ChampSim output directories:
- results/    : 2KB memory region
- results4kb/ : 4KB + 2KB memory region

Generates publication-style plots under ./plots/ for:
  1. 2KB region (SMS, Bingo, RICH (2KB))
  2. 4KB+2KB region (SMS, Bingo, RICH (2KB+4KB))
  3. Combined comparison (SMS, Bingo, RICH (2KB), RICH (2KB+4KB))
"""

import os
os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")

import argparse
import glob
import math
from typing import Dict, List, Tuple

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import PercentFormatter


PREFETCHERS = ["nopref", "sms", "bingo", "rich"]


def clean_trace_name(trace_dir_name: str) -> str:
    """Shorten ChampSim trace filename for x-axis labels."""
    name = trace_dir_name
    for suffix in [".champsimtrace.xz", ".champsimtrace.gz", ".trace.xz", ".trace.gz", ".xz", ".gz"]:
        if name.endswith(suffix):
            name = name[: -len(suffix)]
            break
    return name


def parse_champsim_out(filepath: str) -> Dict[str, float]:
    """Extract Core_0_* ROI statistics from a ChampSim output file."""
    stats: Dict[str, float] = {}
    if not os.path.isfile(filepath):
        return stats

    in_roi = False
    with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            stripped = line.strip()
            if stripped == "[ROI Statistics]":
                in_roi = True
                continue
            if not in_roi and not stripped.startswith("Core_0_"):
                continue
            parts = stripped.split()
            if len(parts) == 2 and parts[0].startswith("Core_0_"):
                try:
                    val = float(parts[1])
                    if not math.isnan(val):
                        stats[parts[0]] = val
                except ValueError:
                    pass
    return stats


def compute_metrics_for_dir(results_dir: str) -> Tuple[List[str], Dict[str, Dict[str, List[float]]]]:
    """
    Parse all traces in results_dir and compute:
      - speedup    : IPC_pref / IPC_nopref
      - accuracy   : pf_useful / (pf_useful + pf_useless) * 100
      - coverage   : pf_useful / (pf_useful + load_miss + RFO_miss) * 100
      - timeliness : pf_useful / (pf_useful + pf_late) * 100
    Returns (trace_names, metrics_dict[metric][prefetcher] -> list of values per trace).
    """
    trace_dirs = sorted(
        d for d in glob.glob(os.path.join(results_dir, "*")) if os.path.isdir(d)
    )
    traces: List[str] = []
    raw_stats: Dict[str, Dict[str, Dict[str, float]]] = {}

    for tdir in trace_dirs:
        tname = os.path.basename(tdir)
        per_pref: Dict[str, Dict[str, float]] = {}
        valid = True
        for pref in PREFETCHERS:
            out_file = os.path.join(tdir, f"out_{pref}.txt")
            st = parse_champsim_out(out_file)
            if "Core_0_IPC" not in st:
                valid = False
                break
            per_pref[pref] = st
        if valid:
            traces.append(tname)
            raw_stats[tname] = per_pref

    active_prefs = ["sms", "bingo", "rich"]
    metrics: Dict[str, Dict[str, List[float]]] = {
        "speedup": {p: [] for p in active_prefs},
        "accuracy": {p: [] for p in active_prefs},
        "coverage": {p: [] for p in active_prefs},
        "timeliness": {p: [] for p in active_prefs},
    }

    for tname in traces:
        base_ipc = raw_stats[tname]["nopref"]["Core_0_IPC"]
        for p in active_prefs:
            st = raw_stats[tname][p]
            ipc = st.get("Core_0_IPC", 0.0)
            pf_useful = st.get("Core_0_L2C_prefetch_useful", 0.0)
            pf_useless = st.get("Core_0_L2C_prefetch_useless", 0.0)
            pf_late = st.get("Core_0_L2C_prefetch_late", 0.0)
            load_miss = st.get("Core_0_L2C_load_miss", 0.0)
            rfo_miss = st.get("Core_0_L2C_RFO_miss", 0.0)

            speedup = (ipc / base_ipc) if base_ipc > 0 else 1.0
            acc_denom = pf_useful + pf_useless
            accuracy = (pf_useful / acc_denom * 100.0) if acc_denom > 0 else 0.0

            cov_denom = pf_useful + load_miss + rfo_miss
            coverage = (pf_useful / cov_denom * 100.0) if cov_denom > 0 else 0.0

            time_denom = pf_useful + pf_late
            timeliness = (pf_useful / time_denom * 100.0) if time_denom > 0 else 0.0

            metrics["speedup"][p].append(speedup)
            metrics["accuracy"][p].append(accuracy)
            metrics["coverage"][p].append(coverage)
            metrics["timeliness"][p].append(timeliness)

    return traces, metrics


def geomean(values: List[float]) -> float:
    pos = [v for v in values if v > 0]
    if not pos:
        return 0.0
    return math.exp(sum(math.log(v) for v in pos) / len(pos))


# Publication-style visual palette & hatches
STYLE_CONFIG = {
    "SMS": {"color": "#4C72B0", "hatch": "//", "edgecolor": "#1f3b6d"},
    "Bingo": {"color": "#DD8452", "hatch": "\\\\", "edgecolor": "#7a3e18"},
    "RICH (2KB)": {"color": "#55A868", "hatch": "xx", "edgecolor": "#1d592b"},
    "RICH (2KB+4KB)": {"color": "#C44E52", "hatch": "..", "edgecolor": "#6b181b"},
}


def draw_speedup_on_ax(
    ax: plt.Axes,
    traces: List[str],
    speedup_series: Dict[str, List[float]],
    title: str,
    show_values: bool = True,
) -> None:
    """Render per-workload + GeoMean Speedup bar chart on a Matplotlib Axes."""
    labels = [clean_trace_name(t) for t in traces] + ["GeoMean"]
    aug_series = {k: list(v) + [geomean(v)] for k, v in speedup_series.items()}

    n_groups = len(labels)
    n_bars = len(aug_series)
    x = np.arange(n_groups)
    total_width = 0.76
    bar_width = total_width / n_bars

    ax.grid(axis="y", linestyle="--", alpha=0.5, zorder=0)
    ax.set_axisbelow(True)

    # Highlight GeoMean column
    ax.axvspan(n_groups - 1.5, n_groups - 0.5, color="#f0f0f0", zorder=0)
    ax.axvline(n_groups - 1.5, color="#888888", linestyle=":", linewidth=1.0, zorder=1)

    for idx, (label_name, values) in enumerate(aug_series.items()):
        offset = (idx - (n_bars - 1) / 2.0) * bar_width
        style = STYLE_CONFIG.get(
            label_name,
            {"color": f"C{idx}", "hatch": "", "edgecolor": "black"},
        )
        rects = ax.bar(
            x + offset,
            values,
            width=bar_width * 0.92,
            label=label_name,
            color=style["color"],
            edgecolor=style["edgecolor"],
            hatch=style["hatch"],
            linewidth=0.8,
            zorder=3,
        )
        if show_values:
            for rect in rects:
                h = rect.get_height()
                ax.annotate(
                    f"{h:.2f}",
                    xy=(rect.get_x() + rect.get_width() / 2.0, h),
                    xytext=(0, 2),
                    textcoords="offset points",
                    ha="center",
                    va="bottom",
                    fontsize=7.5 if n_bars >= 4 else 8.5,
                    rotation=90 if n_bars >= 4 else 0,
                )

    ax.axhline(1.0, color="#333333", linestyle="--", linewidth=1.1, zorder=2, label="No Prefetch (1.0x)")
    ax.set_ylabel("Speedup (Normalized IPC)", fontsize=10, fontweight="bold")
    all_vals = [v for vals in aug_series.values() for v in vals]
    ymin = max(0.0, min(0.5, min(all_vals) - 0.15))
    ymax = max(all_vals) * 1.18
    ax.set_ylim(ymin, ymax)

    ax.set_title(title, fontsize=11, fontweight="bold", pad=8)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=20, ha="right", fontsize=9.5)
    for tick_label in ax.get_xticklabels():
        if tick_label.get_text() == "GeoMean":
            tick_label.set_fontweight("bold")

    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


def draw_acc_cov_time_geomean_on_ax(
    ax: plt.Axes,
    series_by_metric: Dict[str, Dict[str, List[float]]],
    title: str,
    show_values: bool = True,
) -> None:
    """
    Render a single grouped bar plot with X-axis = [Accuracy, Coverage, Timeliness]
    and Y-axis = GeoMean (%) from 0% to 100%, matching the research paper figure.
    """
    categories = ["Accuracy", "Coverage", "Timeliness"]
    metric_keys = ["accuracy", "coverage", "timeliness"]

    pref_names = list(series_by_metric["accuracy"].keys())
    n_groups = len(categories)
    n_bars = len(pref_names)
    x = np.arange(n_groups)
    total_width = 0.72
    bar_width = total_width / n_bars

    ax.grid(axis="y", linestyle="-", alpha=0.35, zorder=0)
    ax.set_axisbelow(True)

    for idx, pref in enumerate(pref_names):
        geomean_vals = [geomean(series_by_metric[m][pref]) for m in metric_keys]
        offset = (idx - (n_bars - 1) / 2.0) * bar_width
        style = STYLE_CONFIG.get(
            pref,
            {"color": f"C{idx}", "hatch": "", "edgecolor": "black"},
        )
        rects = ax.bar(
            x + offset,
            geomean_vals,
            width=bar_width * 0.90,
            label=pref,
            color=style["color"],
            edgecolor=style["edgecolor"],
            hatch=style["hatch"],
            linewidth=1.0,
            zorder=3,
        )
        if show_values:
            for rect in rects:
                h = rect.get_height()
                ax.annotate(
                    f"{h:.1f}%",
                    xy=(rect.get_x() + rect.get_width() / 2.0, h),
                    xytext=(0, 3),
                    textcoords="offset points",
                    ha="center",
                    va="bottom",
                    fontsize=7.5 if n_bars >= 4 else 8.5,
                    fontweight="bold",
                )

    ax.set_ylabel("Geometric Mean (%)", fontsize=10, fontweight="bold")
    ax.set_ylim(0, 114)
    ax.set_yticks(np.arange(0, 101, 20))
    ax.yaxis.set_major_formatter(PercentFormatter(xmax=100, decimals=0))

    ax.set_title(title, fontsize=11, fontweight="bold", pad=8)
    ax.set_xticks(x)
    ax.set_xticklabels(categories, fontsize=11, fontweight="bold")

    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


def save_suite_plots(
    traces: List[str],
    series_by_metric: Dict[str, Dict[str, List[float]]],
    prefix: str,
    suite_title: str,
    out_dir: str,
) -> List[str]:
    """Save Speedup plot, unified Accuracy/Coverage/Timeliness GeoMean plot, and side-by-side summary."""
    os.makedirs(out_dir, exist_ok=True)
    saved_files: List[str] = []

    # 1. Speedup plot
    fig, ax = plt.subplots(figsize=(9.5, 4.6), dpi=300)
    draw_speedup_on_ax(
        ax,
        traces,
        series_by_metric["speedup"],
        title=f"IPC Speedup over No Prefetcher — {suite_title}",
        show_values=True,
    )
    ax.legend(frameon=True, facecolor="white", edgecolor="#cccccc", fontsize=9, loc="best")
    fig.tight_layout()
    speedup_path = os.path.join(out_dir, f"{prefix}_speedup.png")
    fig.savefig(speedup_path, dpi=300)
    plt.close(fig)
    saved_files.append(speedup_path)

    # 2. Unified Accuracy, Coverage, Timeliness (GeoMean %) plot like in the research paper
    fig, ax = plt.subplots(figsize=(8.0, 4.8), dpi=300)
    draw_acc_cov_time_geomean_on_ax(
        ax,
        series_by_metric,
        title="",
        show_values=True,
    )
    handles, legend_labels = ax.get_legend_handles_labels()
    fig.suptitle(f"GeoMean Accuracy, Coverage & Timeliness ({suite_title})", fontsize=12, fontweight="bold", y=0.97)
    fig.legend(
        handles,
        legend_labels,
        loc="upper center",
        bbox_to_anchor=(0.5, 0.91),
        ncol=len(legend_labels),
        frameon=True,
        facecolor="white",
        edgecolor="#cccccc",
        fontsize=9.5,
    )
    fig.tight_layout(rect=[0, 0, 1, 0.85])
    act_path = os.path.join(out_dir, f"{prefix}_acc_cov_time.png")
    fig.savefig(act_path, dpi=300)
    plt.close(fig)
    saved_files.append(act_path)

    # 3. Side-by-side 2-panel figure: (a) Speedup and (b) GeoMean Accuracy, Coverage, Timeliness
    fig, (ax1, ax2) = plt.subplots(
        1, 2, figsize=(15, 5.0), dpi=300, gridspec_kw={"width_ratios": [1.35, 1.0]}
    )
    draw_speedup_on_ax(
        ax1,
        traces,
        series_by_metric["speedup"],
        title="(a) Speedup (Normalized IPC)",
        show_values=True,
    )
    draw_acc_cov_time_geomean_on_ax(
        ax2,
        series_by_metric,
        title="(b) GeoMean Accuracy, Coverage & Timeliness (%)",
        show_values=True,
    )
    handles, legend_labels = ax1.get_legend_handles_labels()
    fig.suptitle(f"Prefetcher Evaluation ({suite_title})", fontsize=13, fontweight="bold", y=0.98)
    fig.legend(
        handles,
        legend_labels,
        loc="upper center",
        bbox_to_anchor=(0.5, 0.92),
        ncol=len(legend_labels),
        frameon=True,
        facecolor="white",
        edgecolor="#cccccc",
        fontsize=9.5,
    )
    fig.tight_layout(rect=[0, 0, 1, 0.86])
    summary_path = os.path.join(out_dir, f"{prefix}_all_metrics.png")
    fig.savefig(summary_path, dpi=300)
    plt.close(fig)
    saved_files.append(summary_path)

    return saved_files


def print_geomean_summary(combined_metrics: Dict[str, Dict[str, List[float]]]) -> None:
    """Print GeoMean summary table for Speedup, Accuracy, Coverage, and Timeliness."""
    pref_names = list(combined_metrics["speedup"].keys())
    print("\n=== GEOMEAN SUMMARY ACROSS WORKLOADS ===")
    header = f"{'Metric':<15} | " + " | ".join(f"{p:>15}" for p in pref_names)
    print(header)
    print("-" * len(header))
    for metric in ["speedup", "accuracy", "coverage", "timeliness"]:
        fmt = "{:>14.4f}x" if metric == "speedup" else "{:>14.2f}%"
        row = f"{metric.capitalize():<15} | " + " | ".join(
            fmt.format(geomean(combined_metrics[metric][p])) for p in pref_names
        )
        print(row)


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot Speedup and GeoMean Accuracy, Coverage, Timeliness.")
    parser.add_argument("--results-2kb", default="results", help="Path to 2KB results directory")
    parser.add_argument("--results-4kb", default="results4kb", help="Path to 4KB+2KB results directory")
    parser.add_argument("--out-dir", default="plots", help="Directory to save generated plots")
    args = parser.parse_args()

    traces_2kb, metrics_2kb = compute_metrics_for_dir(args.results_2kb)
    traces_4kb, metrics_4kb = compute_metrics_for_dir(args.results_4kb)

    # 1. Suite for 2KB Region (results/)
    suite_2kb = {
        m: {
            "SMS": metrics_2kb[m]["sms"],
            "Bingo": metrics_2kb[m]["bingo"],
            "RICH (2KB)": metrics_2kb[m]["rich"],
        }
        for m in ["speedup", "coverage", "accuracy", "timeliness"]
    }
    saved_2kb = save_suite_plots(
        traces_2kb,
        suite_2kb,
        prefix="2kb",
        suite_title="2KB Memory Region",
        out_dir=args.out_dir,
    )

    # 2. Suite for 4KB + 2KB Region (results4kb/)
    suite_4kb = {
        m: {
            "SMS": metrics_4kb[m]["sms"],
            "Bingo": metrics_4kb[m]["bingo"],
            "RICH (2KB+4KB)": metrics_4kb[m]["rich"],
        }
        for m in ["speedup", "coverage", "accuracy", "timeliness"]
    }
    saved_4kb = save_suite_plots(
        traces_4kb,
        suite_4kb,
        prefix="4kb",
        suite_title="4KB + 2KB Memory Region",
        out_dir=args.out_dir,
    )

    # 3. Combined comparison across common traces (SMS, Bingo, RICH 2KB, RICH 2KB+4KB)
    common_traces = [t for t in traces_2kb if t in traces_4kb]
    idx_2kb = [traces_2kb.index(t) for t in common_traces]
    idx_4kb = [traces_4kb.index(t) for t in common_traces]

    suite_combined = {
        m: {
            "SMS": [metrics_2kb[m]["sms"][i] for i in idx_2kb],
            "Bingo": [metrics_2kb[m]["bingo"][i] for i in idx_2kb],
            "RICH (2KB)": [metrics_2kb[m]["rich"][i] for i in idx_2kb],
            "RICH (2KB+4KB)": [metrics_4kb[m]["rich"][i] for i in idx_4kb],
        }
        for m in ["speedup", "coverage", "accuracy", "timeliness"]
    }
    saved_combined = save_suite_plots(
        common_traces,
        suite_combined,
        prefix="combined",
        suite_title="2KB vs. 4KB+2KB Region Comparison",
        out_dir=args.out_dir,
    )

    print_geomean_summary(suite_combined)
    print(f"\nSaved plots in '{args.out_dir}/':")
    for p in saved_2kb + saved_4kb + saved_combined:
        print(f"  - {p}")


if __name__ == "__main__":
    main()
