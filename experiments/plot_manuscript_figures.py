import argparse
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import FixedLocator, NullLocator
import numpy as np


ROOT = Path(__file__).resolve().parent
DATA = ROOT / "data"
OUT = ROOT.parent / "build" / "manuscript-figures"

# Grayscale publication style matching Figures 2–4 of the manuscript
SCHEMES = {
    "fca_unified": ("FCA", "#000000", "-", "o", 2.1, 4),
    "dense": ("dense", "#666666", "--", "s", 2.0, 3),
    "polynomial": ("polynomial", "#9c9c9c", ":", "^", 2.3, 2),
}

# Figure 2(c) sampling curve styles matching manuscript Fig. 2(c)
SAMPLE_STYLES = {
    128: ("#9c9c9c", ":", "^", 2.3, 2),
    256: ("#666666", "--", "s", 2.0, 3),
    688: ("#000000", "-", "o", 2.1, 4),
}

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "STIXGeneral", "Times", "DejaVu Serif"],
    "mathtext.fontset": "stix",
    "font.size": 8.0,
    "axes.labelsize": 8.0,
    "axes.titlesize": 8.5,
    "axes.titleweight": "normal",
    "xtick.labelsize": 7.5,
    "ytick.labelsize": 7.5,
    "legend.fontsize": 7.5,
    "axes.linewidth": 0.55,
    "xtick.major.width": 0.55,
    "ytick.major.width": 0.55,
    "xtick.major.size": 2.2,
    "ytick.major.size": 2.2,
    "xtick.direction": "out",
    "ytick.direction": "out",
    "xtick.major.pad": 2.0,
    "ytick.major.pad": 2.0,
    "axes.labelpad": 2.5,
    "axes.edgecolor": "#000000",
    "axes.labelcolor": "#000000",
    "xtick.color": "#000000",
    "ytick.color": "#000000",
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
    "savefig.dpi": 300,
})


def load_csv(filename):
    with (DATA / filename).open(newline="") as f:
        return list(csv.DictReader(f))


def make_figure(left=0.125, right=0.985, top=0.86, bottom=0.33, wspace=0.44):
    fig, axes = plt.subplots(1, 3, figsize=(3.5, 1.52))
    fig.subplots_adjust(left=left, right=right, top=top, bottom=bottom, wspace=wspace)
    for ax, label in zip(axes, ("(a)", "(b)", "(c)")):
        ax.set_title(label, pad=3.5)
        ax.grid(False)
        for spine in ax.spines.values():
            spine.set_visible(True)
    return fig, axes


def add_scheme_legend(fig, y=0.025):
    handles = [
        Line2D(
            [0], [0],
            label=label,
            color=color,
            ls=ls,
            marker=marker,
            ms=ms,
            lw=0.85,
            markeredgewidth=0.0,
        )
        for label, color, ls, marker, ms, _ in SCHEMES.values()
    ]
    fig.legend(
        handles=handles,
        loc="lower center",
        bbox_to_anchor=(0.54, y),
        ncol=3,
        frameon=False,
        columnspacing=1.1,
        handlelength=1.8,
        handletextpad=0.4,
        borderaxespad=0.0,
    )


def plot_scheme(ax, scheme, x, y, markevery=1):
    _, color, ls, marker, ms, zorder = SCHEMES[scheme]
    ax.plot(
        x, y,
        color=color,
        ls=ls,
        marker=marker,
        ms=ms,
        lw=0.9,
        markeredgewidth=0.0,
        markevery=markevery,
        zorder=zorder,
    )


def save_figure(fig, stem):
    OUT.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT / f"{stem}.pdf", facecolor="white")
    fig.savefig(OUT / f"{stem}.png", dpi=300, facecolor="white")
    plt.close(fig)


def plot_fig2():
    fig, (ax_a, ax_b, ax_c) = make_figure(left=0.135, right=0.98, top=0.86, bottom=0.34, wspace=0.46)

    # (a) Single-query bound vs block dimension N
    rows_a = load_csv("fig2a-single-audit.csv")
    for scheme in SCHEMES:
        sub = [r for r in rows_a if r["variant"] == scheme]
        x = [int(r["block_dimension"]) for r in sub]
        y = [float(r["security_bits"]) for r in sub]
        plot_scheme(ax_a, scheme, x, y)
    ax_a.set_xscale("log", base=2)
    ax_a.set_xticks([2**10, 2**15, 2**20])
    ax_a.set_xticklabels([r"$2^{10}$", r"$2^{15}$", r"$2^{20}$"])
    ax_a.xaxis.set_minor_locator(NullLocator())
    ax_a.set_yticks([155, 165, 175])
    ax_a.set_ylim(150, 182)
    ax_a.set_xlabel(r"$N$")
    ax_a.set_ylabel("bits")

    # (b) Multi-query bound vs Q_v
    rows_b = load_csv("fig2b-repeated-audits.csv")
    for scheme in SCHEMES:
        sub = [r for r in rows_b if r["variant"] == scheme]
        x = [int(r["verification_attempts"]) for r in sub]
        y = [float(r["security_bits"]) for r in sub]
        plot_scheme(ax_b, scheme, x, y)
    ax_b.set_xscale("log", base=2)
    ax_b.set_xticks([2**8, 2**20, 2**32])
    ax_b.set_xticklabels([r"$2^{8}$", r"$2^{20}$", r"$2^{32}$"])
    ax_b.xaxis.set_minor_locator(NullLocator())
    ax_b.set_yticks([120, 140, 160])
    ax_b.set_ylim(112, 176)
    ax_b.set_xlabel(r"$Q_v$")

    # (c) Detection failure bound (theoretical curves) + Monte Carlo points (95% Wilson CI)
    rows_c_bound = load_csv("fig2c-detection-bound.csv")
    rows_c_mc = load_csv("fig2c-monte-carlo.csv")
    for c_val in (128, 256, 688):
        color, ls, marker, ms, zorder = SAMPLE_STYLES[c_val]
        sub_b = [r for r in rows_c_bound if int(r["c"]) == c_val]
        xb = [float(r["corrupted_pct"]) for r in sub_b]
        yb = [float(r["detection_failure_bound"]) for r in sub_b]
        ax_c.plot(xb, yb, color=color, ls=ls, lw=0.9, zorder=zorder, label=f"$c={c_val}$")

        sub_m = [r for r in rows_c_mc if int(r["c"]) == c_val]
        for r in sub_m:
            est = float(r["estimate"])
            lo = float(r["lower_95"])
            hi = float(r["upper_95"])
            if est > 0:
                ax_c.errorbar(
                    float(r["corrupted_pct"]),
                    est,
                    yerr=[[max(0.0, est - lo)], [max(0.0, hi - est)]],
                    fmt=marker,
                    color=color,
                    ms=ms + 0.5,
                    mec="white",
                    mew=0.35,
                    elinewidth=0.65,
                    capsize=1.3,
                    capthick=0.55,
                    zorder=zorder + 5,
                )

    ax_c.set_xscale("log")
    ax_c.set_yscale("log")
    ax_c.set_xlim(0.08, 12.5)
    ax_c.set_ylim(1e-9, 2.2)
    ax_c.set_xticks([0.1, 1.0, 10.0])
    ax_c.set_xticklabels(["0.1%", "1%", "10%"])
    ax_c.set_yticks([1e-8, 1e-4, 1.0])
    ax_c.set_yticklabels([r"$10^{-8}$", r"$10^{-4}$", "1"])
    ax_c.xaxis.set_minor_locator(NullLocator())
    ax_c.yaxis.set_minor_locator(NullLocator())
    ax_c.set_xlabel("corrupted")
    ax_c.legend(
        loc="lower left",
        fontsize=5.8,
        frameon=False,
        handlelength=1.4,
        handletextpad=0.3,
        labelspacing=0.18,
        borderpad=0.15,
    )

    add_scheme_legend(fig)
    save_figure(fig, "fig2")


def plot_fig3():
    fig, (ax_a, ax_b, ax_c) = make_figure(left=0.135, right=0.98, top=0.86, bottom=0.34, wspace=0.42)

    # (a) Tag generation throughput vs file size (GiB)
    rows_a = load_csv("fig3a-tag-generation.csv")
    for scheme in SCHEMES:
        sub = sorted(
            [r for r in rows_a if r["variant"] == scheme],
            key=lambda r: int(r["target_gib"]),
        )
        x = [int(r["target_gib"]) for r in sub]
        y = [float(r["tag_MB_per_s"]) for r in sub]
        plot_scheme(ax_a, scheme, x, y)
    ax_a.set_xscale("log", base=2)
    ax_a.set_xticks([1, 16, 256])
    ax_a.set_xticklabels(["1", "16", "256"])
    ax_a.xaxis.set_minor_locator(NullLocator())
    ax_a.set_yticks([0, 3000, 6000])
    ax_a.set_yticklabels(["0", "3k", "6k"])
    ax_a.set_ylim(0, 6500)
    ax_a.set_xlabel("file size (GiB)")
    ax_a.set_ylabel("MB/s")

    # (b) Server proof generation vs challenge size c
    rows_b = load_csv("fig3b-proof-generation.csv")
    for scheme in SCHEMES:
        sub = sorted(
            [r for r in rows_b if r["variant"] == scheme],
            key=lambda r: int(r["c"]),
        )
        x = [int(r["c"]) for r in sub]
        y = [float(r["proof_ms"]) for r in sub]
        plot_scheme(ax_b, scheme, x, y)
    ax_b.set_xscale("log", base=2)
    ax_b.set_xticks([128, 1024, 4096])
    ax_b.set_xticklabels(["128", "1k", "4k"])
    ax_b.xaxis.set_minor_locator(NullLocator())
    ax_b.set_yticks([0, 1000, 2000, 3000])
    ax_b.set_yticklabels(["0", "1k", "2k", "3k"])
    ax_b.set_ylim(0, 3000)
    ax_b.set_xlabel(r"$c$")

    # (c) Client verification vs challenge size c
    rows_c = load_csv("fig3c-client-verification.csv")
    for scheme in SCHEMES:
        sub = sorted(
            [r for r in rows_c if r["variant"] == scheme],
            key=lambda r: int(r["c"]),
        )
        x = [int(r["c"]) for r in sub]
        y = [float(r["verify_total_ms"]) for r in sub]
        plot_scheme(ax_c, scheme, x, y)
    ax_c.set_xscale("log", base=2)
    ax_c.set_xticks([128, 1024, 4096])
    ax_c.set_xticklabels(["128", "1k", "4k"])
    ax_c.xaxis.set_minor_locator(NullLocator())
    ax_c.set_yticks([0, 3, 6, 9])
    ax_c.set_ylim(0, 9.5)
    ax_c.set_xlabel(r"$c$")

    add_scheme_legend(fig)
    save_figure(fig, "fig3")


def plot_fig4():
    fig, (ax_a, ax_b, ax_c) = make_figure(left=0.155, right=0.98, top=0.86, bottom=0.34, wspace=0.45)

    # (a) Client state vs versions per scope
    rows_a = load_csv("fig4a-snapshot-cache.csv")
    for scheme in SCHEMES:
        sub = sorted(
            [r for r in rows_a if r["variant"] == scheme],
            key=lambda r: int(r["versions"]),
        )
        x = [int(r["versions"]) for r in sub]
        y = [float(r["cache_kib"]) for r in sub]
        plot_scheme(ax_a, scheme, x, y, markevery=2)
    ax_a.set_xscale("log", base=2)
    ax_a.set_yscale("log")
    ax_a.set_xticks([1, 32, 1024])
    ax_a.set_xticklabels(["1", "32", "1024"])
    ax_a.set_yticks([1e-1, 1e1, 1e3, 1e5])
    ax_a.set_yticklabels([r"$10^{-1}$", r"$10^{1}$", r"$10^{3}$", r"$10^{5}$"])
    ax_a.set_ylim(1.5e-2, 2.5e6)
    ax_a.xaxis.set_minor_locator(NullLocator())
    ax_a.yaxis.set_minor_locator(NullLocator())
    ax_a.set_xlabel("versions")
    ax_a.set_ylabel("state (KiB)")

    # (b) Modeled audit latency vs bandwidth
    rows_b = load_csv("fig4b-bandwidth-model.csv")
    for scheme in SCHEMES:
        sub = sorted(
            [r for r in rows_b if r["variant"] == scheme],
            key=lambda r: int(r["mbps"]),
        )
        x = [int(r["mbps"]) for r in sub]
        y = [float(r["predicted_ms"]) for r in sub]
        plot_scheme(ax_b, scheme, x, y)
    ax_b.set_xscale("log")
    ax_b.set_xticks([10, 100, 1000])
    ax_b.set_xticklabels(["10", "100", "1000"])
    ax_b.set_yticks([450, 650, 850])
    ax_b.set_ylim(415, 920)
    ax_b.xaxis.set_minor_locator(NullLocator())
    ax_b.set_xlabel("Mbps")

    # (c) Proof-generation time breakdown (stacked bars)
    rows_c = sorted(load_csv("fig4c-proof-breakdown.csv"), key=lambda r: int(r["c"]))
    c_vals = [int(r["c"]) for r in rows_c]
    read_ms = np.array([float(r["read_ms"]) for r in rows_c])
    oper_ms = np.array([float(r["operator_ms"]) for r in rows_c])
    tag_ms = np.array([float(r["tag_aggregate_ms"]) for r in rows_c])

    pos = np.arange(len(c_vals))
    w = 0.62
    ax_c.bar(pos, read_ms, width=w, color="#e3e3e3", edgecolor="#000000", linewidth=0.45, label="read")
    ax_c.bar(pos, oper_ms, width=w, bottom=read_ms, color="#969696", edgecolor="#000000", linewidth=0.45, label="oper")
    ax_c.bar(pos, tag_ms, width=w, bottom=read_ms + oper_ms, color="#333333", edgecolor="#000000", linewidth=0.45, label="tag")

    ax_c.set_xticks([0, 2, 5])
    ax_c.set_xticklabels(["128", "512", "2048"])
    ax_c.set_yticks([0, 600, 1200])
    ax_c.set_yticklabels(["0", "600", "1.2k"])
    ax_c.set_ylim(0, 1480)
    ax_c.set_xlabel(r"$c$")
    ax_c.legend(
        loc="upper left",
        fontsize=6.3,
        frameon=False,
        handlelength=1.0,
        handleheight=0.8,
        handletextpad=0.35,
        labelspacing=0.2,
        borderpad=0.25,
    )

    add_scheme_legend(fig)
    save_figure(fig, "fig4")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Render FCA Figures 2–4 in manuscript style.")
    parser.add_argument("--data-dir", type=Path, default=DATA, help="directory of plotting CSVs")
    parser.add_argument("--output-dir", type=Path, default=OUT, help="destination for PDF and PNG files")
    args = parser.parse_args()
    DATA = args.data_dir
    OUT = args.output_dir

    plot_fig2()
    plot_fig3()
    plot_fig4()
    print(f"Saved Figures 2–4 to {OUT}")
