import argparse
import csv
import math
from pathlib import Path
from statistics import median

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import LogFormatterMathtext, StrMethodFormatter
import numpy as np


ROOT = Path(__file__).resolve().parent
RAW = ROOT / "raw"
OUTPUT = ROOT.parent / "build" / "figure-data"
DATA = OUTPUT / "data"
FIGURES = OUTPUT / "figures"
SCHEMES = {
    "fca_unified": ("FCA", "#12649b", "-", "o"),
    "dense": ("Dense", "#aa5423", "--", "s"),
    "polynomial": ("Polynomial", "#43586b", ":", "^"),
}
SAMPLES = (128, 256, 688)
COLORS = ("#12649b", "#a75120", "#43586b")
FIELD_ORDER = 2**178
DEFAULT_N = 23_424
RTT_MS = 9.6

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 10,
    "axes.labelsize": 10,
    "axes.titlesize": 12,
    "axes.titleweight": "semibold",
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.edgecolor": "#657788",
    "axes.labelcolor": "#172c3c",
    "axes.titlecolor": "#172c3c",
    "xtick.color": "#30485a",
    "ytick.color": "#30485a",
    "grid.color": "#dce5ec",
    "grid.linewidth": 0.65,
    "pdf.fonttype": 42,
    "figure.dpi": 180,
})


def read_csv(name):
    with (RAW / name).open(newline="") as stream:
        return list(csv.DictReader(stream))


def write_csv(name, rows):
    with (DATA / name).open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def figure(title, note, headers):
    fig, axes = plt.subplots(1, 3, figsize=(12.4, 4.05))
    fig.subplots_adjust(left=.071, right=.985, top=.775, bottom=.31, wspace=.44)
    fig.text(.071, .944, title, fontsize=17, weight="semibold", color="#162b3b")
    fig.text(.071, .064, note, fontsize=8.4, color="#43586b", linespacing=1.35)
    for ax, (panel, xlabel, ylabel) in zip(axes, headers):
        ax.set_title(panel, loc="left", pad=10)
        ax.set(xlabel=xlabel, ylabel=ylabel)
        ax.grid(axis="y")
        ax.set_axisbelow(True)
    return fig, axes


def paths(fig, name):
    for extension in ("png", "pdf"):
        fig.savefig(FIGURES / f"{name}.{extension}", facecolor="white")
    plt.close(fig)


def curve(ax, scheme, x, y, **kwargs):
    _, color, ls, marker = SCHEMES[scheme]
    ax.plot(x, y, color=color, ls=ls, marker=marker, ms=4.4, lw=1.8, **kwargs)


def scheme_legend(handles=None):
    if handles is not None:
        return handles
    return [
        Line2D([0], [0], label=label, color=color, ls=ls, marker=marker, ms=4, lw=1.8)
        for label, color, ls, marker in SCHEMES.values()
    ]


def legend(fig, handles=None):
    fig.legend(handles=scheme_legend(handles), loc="lower center", bbox_to_anchor=(.53, .135),
               ncol=4, frameon=False, columnspacing=1.4, handlelength=2.4, fontsize=9)


def exact_miss(blocks, corrupt, count):
    if count > blocks - corrupt:
        return 0.0
    return math.prod((blocks - corrupt - i) / (blocks - i) for i in range(count))


def wilson(hits, repeats):
    z = 1.95996398454
    rate = hits / repeats
    scale = 1 + z**2 / repeats
    center = (rate + z**2 / (2 * repeats)) / scale
    margin = z * math.sqrt(rate * (1 - rate) / repeats + z**2 / (4 * repeats**2)) / scale
    return max(0.0, center - margin), min(1.0, center + margin)


def security_figure():
    fig, (one, repeated, detect) = figure(
        "Figure 2  |  Authentication bounds and corruption sampling",
        "(a–b) Algebraic bounds at the default field size, excluding PRF terms. (b) uses N = 23,424.\n"
        "(c) Ideal-model bound for 10,000 blocks. Dots show 100,000-sample Monte Carlo estimates "
        "at 0.1% and 1%, with 95% Wilson intervals.",
        (("(a) One audit", "Block dimension, N", "Security bits"),
         ("(b) Repeated audits", "Verification attempts, Qᵥ", "Security bits"),
         ("(c) Detection failure", "Corrupted blocks (%)", "Miss probability")),
    )
    inv_q = 1 / FIELD_ORDER
    epsilon = {
        "fca_unified": lambda n: 2 * inv_q - inv_q**2,
        "dense": lambda n: inv_q,
        "polynomial": lambda n: n * inv_q,
    }
    bit_exponents = range(10, 21, 2)
    query_exponents = range(8, 41, 4)
    single_rows, repeated_rows = [], []
    for name in SCHEMES:
        one_bits = [-math.log2(epsilon[name](2**n)) for n in bit_exponents]
        multi_bits = [-math.log2(epsilon[name](DEFAULT_N)) - q for q in query_exponents]
        curve(one, name, bit_exponents, one_bits)
        curve(repeated, name, query_exponents, multi_bits)
        single_rows.extend(dict(variant=name, block_dimension=2**n, security_bits=y,
                                algebraic_bound=epsilon[name](2**n))
                           for n, y in zip(bit_exponents, one_bits))
        repeated_rows.extend(dict(variant=name, verification_attempts=2**q,
                                  block_dimension=DEFAULT_N, security_bits=y,
                                  algebraic_bound=2**q * epsilon[name](DEFAULT_N))
                             for q, y in zip(query_exponents, multi_bits))
    write_csv("fig2a-single-audit.csv", single_rows)
    write_csv("fig2b-repeated-audits.csv", repeated_rows)
    for ax, ticks in ((one, [10, 15, 20]), (repeated, [8, 20, 32, 40])):
        ax.set_xticks(ticks, [rf"$2^{{{n}}}$" for n in ticks])

    blocks = 10_000
    corrupt_values = np.unique(np.rint(np.geomspace(10, 1000, 121)).astype(int))
    rng = np.random.default_rng(179_688)
    sampling_rows = []
    monte_carlo = []
    for color, count in zip(COLORS, SAMPLES):
        bounds = []
        for corrupt in corrupt_values:
            miss = exact_miss(blocks, int(corrupt), count)
            value = miss + (1 - miss) / (FIELD_ORDER - 1) + epsilon["fca_unified"](DEFAULT_N)
            bounds.append(value)
            sampling_rows.append(dict(m=blocks, corrupted=int(corrupt),
                                      corrupted_pct=100 * int(corrupt) / blocks, c=count,
                                      miss_probability=miss, detection_failure_bound=value))
        detect.plot(100 * corrupt_values / blocks, bounds, color=color, lw=1.8, label=f"c = {count}")
        for corrupt in (10, 100):
            repeats = 100_000
            misses = int(np.count_nonzero(rng.hypergeometric(corrupt, blocks - corrupt, count,
                                                            size=repeats) == 0))
            rate = misses / repeats
            lower, upper = wilson(misses, repeats)
            monte_carlo.append(dict(m=blocks, corrupted=corrupt,
                                    corrupted_pct=100 * corrupt / blocks, c=count,
                                    trials=repeats, misses=misses, estimate=rate,
                                    lower_95=lower, upper_95=upper))
            if misses:
                detect.errorbar(100 * corrupt / blocks, rate, [[rate - lower], [upper - rate]],
                                fmt="o", color=color, ms=4, mew=.9, mec="white", capsize=2)
    write_csv("fig2c-detection-bound.csv", sampling_rows)
    write_csv("fig2c-monte-carlo.csv", monte_carlo)
    detect.set(xscale="log", yscale="log", ylim=(1e-36, 2))
    detect.set_xticks([.1, 1, 10], ["0.1", "1", "10"])
    detect.set_yticks([1, 1e-8, 1e-16, 1e-24, 1e-32])
    detect.yaxis.set_major_formatter(LogFormatterMathtext())
    detect.legend(loc="lower left", fontsize=8.4, frameon=False)
    legend(fig)
    paths(fig, "figure-2-security")


def organize_audits():
    source = read_csv("audits.csv")
    assert len(source) == 420 and all(r["valid"] == "1" for r in source)
    groups = {}
    for row in source:
        groups.setdefault((row["variant"], int(row["c"])), []).append(row)
    assert len(groups) == 42 and all(len(rows) == 10 for rows in groups.values())
    return groups, sorted({key[1] for key in groups})


def stat(group, field):
    return median(float(row[field]) for row in group)


def challenge_axis(ax):
    ticks = [128, 512, 1024, 4096]
    ax.set_xscale("log", base=2)
    ax.set_xticks(ticks, [f"{n:,}" for n in ticks])
    ax.set_ylim(bottom=0)


def compute_figure(groups, challenge_sizes):
    rows = read_csv("tag-size.csv")
    assert len(rows) == 15
    expected_sizes = {1, 4, 16, 64, 256}
    for scheme in SCHEMES:
        actual_sizes = {int(row["target_gib"]) for row in rows if row["variant"] == scheme}
        assert expected_sizes == actual_sizes
    fig, (tag, server, owner) = figure(
        "Figure 3  |  Tag generation and audit performance",
        "(a) One cumulative pass on in-memory synthetic blocks, 8 workers. Each tag is computed; "
        "dataset sizes denote bytes processed.\n"
        "(b–c) Medians of 10 local audit trials per c. Block reads and aggregation are shared across "
        "schemes; verification includes masks and FCA seed expansion.",
        (("(a) Tag generation", "Input processed (GiB)", "Throughput (MB/s)"),
         ("(b) Server proof", "Challenged blocks, c", "Proof generation (ms)"),
         ("(c) Owner verification", "Challenged blocks, c", "Full verification (ms)")),
    )
    tag_rows, proof_rows, verify_rows = [], [], []
    for name in SCHEMES:
        subset = [r for r in rows if r["variant"] == name]
        subset.sort(key=lambda r: int(r["target_gib"]))
        curve(tag, name, [int(r["target_gib"]) for r in subset],
              [float(r["tag_MB_per_s"]) for r in subset])
        tag_rows.extend({key: value for key, value in r.items() if key != "checksum"} for r in subset)
        groups_for_scheme = [groups[name, c] for c in challenge_sizes]
        proof = [stat(g, "proof_ms") for g in groups_for_scheme]
        verify = [stat(g, "verify_total_ms") for g in groups_for_scheme]
        curve(server, name, challenge_sizes, proof)
        curve(owner, name, challenge_sizes, verify)
        for c, g, proof_ms, verify_ms in zip(challenge_sizes, groups_for_scheme, proof, verify):
            proof_rows.append(dict(variant=name, c=c, trials=len(g), proof_ms=proof_ms))
            verify_rows.append(dict(variant=name, c=c, trials=len(g), verify_total_ms=verify_ms,
                                    verify_eval_ms=stat(g, "verify_eval_ms"), mask_ms=stat(g, "mask_ms"),
                                    included_seed_expand_ms=stat(g, "challenge_expand_ms")
                                    if name == "fca_unified" else 0))
    write_csv("fig3a-tag-generation.csv", tag_rows)
    write_csv("fig3b-proof-generation.csv", proof_rows)
    write_csv("fig3c-client-verification.csv", verify_rows)
    tag.set(xscale="log", ylim=(0, None))
    tag.set_xticks([1, 4, 16, 64, 256], ["1", "4", "16", "64", "256"])
    tag.yaxis.set_major_formatter(StrMethodFormatter("{x:,.0f}"))
    server.yaxis.set_major_formatter(StrMethodFormatter("{x:,.0f}"))
    challenge_axis(server)
    challenge_axis(owner)
    legend(fig)
    paths(fig, "figure-3-performance")


def latency_rows(groups):
    output = []
    for name in SCHEMES:
        group = groups[name, 688]
        local = median(float(r["proof_ms"]) + float(r["verify_total_ms"]) +
                       (float(r["challenge_expand_ms"]) if name == "fca_unified" else 0)
                       for r in group)
        request = int(group[0]["request_bytes"])
        response = int(group[0]["response_bytes"])
        wire = request + response
        for mbps in (10, 25, 50, 100, 200, 480, 1000):
            transfer = 8 * wire / (1e3 * mbps)
            output.append(dict(variant=name, c=688, mbps=mbps, rtt_ms=RTT_MS,
                               request_bytes=request, response_bytes=response, wire_bytes=wire,
                               local_ms=local, transfer_ms=transfer,
                               predicted_ms=local + RTT_MS + transfer))
    return output


def state_figure(groups, challenge_sizes):
    state = read_csv("scope-cache.csv")
    assert len(state) == 33
    latency = latency_rows(groups)
    write_csv("fig4a-snapshot-cache.csv", [
        {**row, "cache_kib": int(row["cache_bytes"]) / 1024} for row in state
    ])
    write_csv("fig4b-bandwidth-model.csv", latency)
    fig, (cache, net, proof) = figure(
        "Figure 4  |  Client cache and audit costs",
        "(a) Expanded coefficients only, 32 bytes per slot; one shared scope for FCA and one cache "
        "per version for each baseline.\n"
        "(b) Model based on local timings and serialized sizes, assuming 9.6 ms RTT. "
        "(c) Local FCA stage medians, with tag-aggregation time also labeled.",
        (("(a) Cache across versions", "Snapshot versions", "Expanded state (KiB)"),
         ("(b) Bandwidth model", "Link bandwidth (Mb/s)", "Modeled audit latency (ms)"),
         ("(c) Proof-generation stages", "Challenged blocks, c", "Time (ms)")),
    )
    for name in SCHEMES:
        subset = [r for r in state if r["variant"] == name]
        curve(cache, name, [int(r["versions"]) for r in subset],
              [int(r["cache_bytes"]) / 1024 for r in subset])
        estimates = [r for r in latency if r["variant"] == name]
        curve(net, name, [r["mbps"] for r in estimates], [r["predicted_ms"] for r in estimates])
    cache.set(xscale="log", yscale="log")
    cache.set_xticks([1, 32, 1024], ["1", "32", "1,024"])
    cache.yaxis.set_major_formatter(LogFormatterMathtext())
    net.set_xscale("log")
    net.set_xticks([10, 100, 1000], ["10", "100", "1,000"])
    lower = min(r["predicted_ms"] for r in latency)
    net.set_ylim(bottom=round(lower / 25) * 25 - 25)
    net.yaxis.set_major_formatter(StrMethodFormatter("{x:,.0f}"))

    selected = [c for c in challenge_sizes if c <= 2048]
    source = [groups["fca_unified", c] for c in selected]
    bottom = np.zeros(len(selected))
    stages = [("Block reads", "read_ms", "#b1cfe2"),
              ("Block aggregation", "operator_ms", "#3c7aa2"),
              ("Tag aggregation", "tag_aggregate_ms", "#ac602f")]
    breakdown = [dict(c=c) for c in selected]
    for name, metric, color in stages:
        duration = [stat(g, metric) for g in source]
        proof.bar(range(len(selected)), duration, bottom=bottom, color=color, label=name, width=.68)
        bottom += duration
        for out, value in zip(breakdown, duration):
            out[metric] = value
    for row, total in zip(breakdown, bottom):
        row["stacked_total_ms"] = float(total)
    write_csv("fig4c-proof-breakdown.csv", breakdown)
    proof.set_xticks(range(len(selected)), [str(n) for n in selected], fontsize=8.3)
    proof.set_ylim(top=bottom[-1] * 1.30)
    proof.yaxis.set_major_formatter(StrMethodFormatter("{x:,.0f}"))
    proof.legend(loc="upper left", fontsize=7.6, frameon=False, handlelength=1.2)
    proof.annotate(f'Tag: {breakdown[-1]["tag_aggregate_ms"]:.2f} ms',
                   xy=(len(selected) - 1, bottom[-1]), xytext=(-4, 6),
                   textcoords="offset points", fontsize=7.8, color="#82421c", ha="right")
    legend(fig)
    paths(fig, "figure-4-audit-cost")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Rebuild plotting CSVs and diagnostic figures for FCA.")
    parser.add_argument("--raw-dir", type=Path, default=RAW, help="directory of raw benchmark CSVs")
    parser.add_argument("--output-dir", type=Path, default=OUTPUT, help="destination for data and figures")
    args = parser.parse_args()
    RAW = args.raw_dir
    DATA = args.output_dir / "data"
    FIGURES = args.output_dir / "figures"

    DATA.mkdir(parents=True, exist_ok=True)
    FIGURES.mkdir(parents=True, exist_ok=True)
    security_figure()
    groups, challenge_sizes = organize_audits()
    compute_figure(groups, challenge_sizes)
    state_figure(groups, challenge_sizes)
