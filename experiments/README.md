# Reproduce manuscript Figures 2–4

Run the commands from the repository root. The checked-in [`raw/`](raw/) directory contains the original benchmark records. The checked-in [`data/`](data/) directory contains the CSVs consumed by `plot_manuscript_figures.py`.

## Redraw the figures from the saved data

```sh
python3 -m venv build/plot-venv
. build/plot-venv/bin/activate
python -m pip install -r experiments/requirements.txt
python experiments/plot_manuscript_figures.py
```

The script writes `fig2.pdf`, `fig3.pdf`, `fig4.pdf`, and their PNG counterparts to `build/manuscript-figures/`. To rebuild the plotting CSVs from the saved raw records first, run the commands below. `plot_figures.py` also writes diagnostic plots alongside the regenerated CSVs.

```sh
python experiments/plot_figures.py
python experiments/plot_manuscript_figures.py --data-dir build/figure-data/data
```

## Rerun the experiments

The original measurements used an Apple M5 Pro with 24 GB of memory and macOS 27.2, Apple Clang 21.0.0 with `-O3 -march=native`, and OpenSSL 3.6.4. The implementation revision was [`08e3347`](https://github.com/g3x1n/fca/commit/08e334703ac76d80b617a8ed481a257dcfd3bcf0). Build with the same compiler and OpenSSL when comparing absolute timings. The Makefile detects common Homebrew OpenSSL paths and accepts an explicit `OPENSSL_PREFIX`.

With the Python environment above active, build the tools, collect fresh measurements, and render the figures into a temporary directory.

```sh
make -j figure-experiments
run_dir=$(mktemp -d)
mkdir -p "$run_dir/raw"

build/fca-bench "$run_dir/blocks.bin" 4119 10 16 \
  128 256 512 688 1024 2048 4096 > "$run_dir/raw/audits.csv"
build/fca-figure-experiments tag-sweep > "$run_dir/raw/tag-size.csv"
build/fca-figure-experiments scope-state > "$run_dir/raw/scope-cache.csv"

python experiments/plot_figures.py --raw-dir "$run_dir/raw" --output-dir "$run_dir"
python experiments/plot_manuscript_figures.py \
  --data-dir "$run_dir/data" --output-dir "$run_dir/manuscript-figures"
printf '%s\n' "$run_dir/manuscript-figures"
```

The challenge sweep writes about 2 GiB of temporary input. The tag sweep reuses in-memory buffers and took about nine minutes on the original machine. Timings depend on the machine and build flags.

## What each panel measures

| Panel | Source and method |
| --- | --- |
| Fig. 2(a–b) | `plot_figures.py` computes the single-query bounds at field size `q = 2^178`. For multiple queries, it multiplies the single-query bounds by the verification budget at `N = 23,424`. The vertical axis is the negative base-2 logarithm of the algebraic bound. |
| Fig. 2(c) | The same script computes the hypergeometric sampling bound for 10,000 blocks and `c = 128, 256, 688`. At 0.1% and 1% corruption it runs 100,000 sampling trials per combination using seed `179688` and reports 95% Wilson intervals. |
| Fig. 3(a) | `tag-sweep` computes tags with eight threads and records cumulative throughput at 1, 4, 16, 64, and 256 GiB. The timing includes mask derivation and starts after expanding coefficients. All input buffers are in memory. |
| Fig. 3(b–c) | `fca-bench` runs 10 trials for each of seven challenge sizes over 4,119 synthetic blocks. The plots show medians for FCA, the dense baseline, and the polynomial baseline. Full client verification includes mask accumulation and, for FCA, seed expansion. |
| Fig. 4(a) | `scope-state` counts expanded coefficients across up to 1,024 snapshots, at 32 bytes per field element. FCA shares one authenticator across versions, and each baseline uses one per version. |
| Fig. 4(b) | The latency model combines local timings, message sizes, and a 9.6 ms round-trip time. The transfer term is `8 × wire_bytes / (1000 × mbps)` in milliseconds. |
| Fig. 4(c) | The script reuses the challenge sweep from Fig. 3(b–c) and takes medians separately for block reads, block aggregation, and tag aggregation. |
