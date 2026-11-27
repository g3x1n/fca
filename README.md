# FCA — Factorized Cyclotomic Authentication

Official C++20 implementation accompanying *FCA: Private Cloud Storage Auditing with Factorized Cyclotomic Authentication* by Zuhui Wan, Chunli Lv, and Xinjin Ge. It includes a local auditing CLI, correctness tests, and benchmarks for FCA and the dense and polynomial baselines.

The data owner and storage server run locally and exchange audit messages through files.

## Build and test

You need a C++20 compiler, Make, the OpenSSL 3 development libraries, and Python 3.9 or newer. The Makefile detects common Homebrew OpenSSL paths. If OpenSSL is installed elsewhere, pass `OPENSSL_PREFIX=/path/to/openssl` to Make.

```sh
make -j
make test
make test-portable
```

The default build uses `-march=native` and enables carryless multiplication via PMULL or PCLMUL on supported ARM64 and x86-64 processors. Use `make BUILD=build/portable ARCH_FLAGS=-DFCA_PORTABLE` to build the portable implementation. The tests compare the optimized arithmetic with independent bit-by-bit polynomial long division and exhaustively check the authenticator output distribution over `GF(4)` for rank-one and rank-two deviations with one and two vector pairs. The CLI tests cover corrupted blocks and tags, replay across audit rounds, padding, and malformed responses.

## Run a local file audit

```sh
tmp=$(mktemp -d)
mkdir "$tmp/owner"
printf 'local FCA demo\n' > "$tmp/input"
build/fca keygen "$tmp/owner/master.key"
build/fca prepare "$tmp/owner/master.key" "$tmp/input" "$tmp/server" "$tmp/owner/file.meta"
build/fca challenge "$tmp/owner/master.key" "$tmp/owner/file.meta" 1 "$tmp/request.bin"
build/fca prove "$tmp/server" "$tmp/request.bin" "$tmp/response.bin"
build/fca verify "$tmp/owner/master.key" "$tmp/owner/file.meta" "$tmp/request.bin" "$tmp/response.bin"
```

The final command prints `ACCEPT` or `REJECT` and exits with status 0 or 1, respectively. Command and local configuration errors return status 2. The owner retains the master key, trusted metadata, and the original request for the round. Each audit uses a fresh round identifier and random seed. To generate a proof, the server reads `public.bin`, `blocks.bin`, `tags.bin`, and the received request.

- The defaults are `p=179, ℓ=366, w=64, t=1`, with 521,184 bytes per block. Preparation zero-pads the final block and represents an empty file as one all-zero block. The default audit samples `min(688, m)` blocks from a file with `m` blocks. Pass `--samples C` to `prepare` to choose the sample count.
- To share authentication vectors across snapshots, generate a scope once with `build/fca scope` and pass that same value via `--scope` when preparing each snapshot. Use the same master key and parameters, and give each snapshot its own server directory and metadata file. Block masks are derived independently for each file identifier (FID). The C++ API can share one in-memory `Authenticator` across snapshots. Separate CLI processes rederive the same vectors from the master key.
- The field implementation supports primes `p <= 251` with `ord_p(2) = p - 1`. The `prepare` command takes `--prime P --columns L --width W --branches T`. The file-based CLI requires a word width divisible by eight, and the in-memory API supports word widths from 1 to 64 bits.
- Section III of the paper describes authentication and auditing. The dense baseline uses independently derived coefficients, and the polynomial baseline uses Horner evaluation. Both use the same masking and aggregation procedures as FCA.
- Parameters and records use little-endian encoding. Field elements use `ceil((p - 1) / 8)` bytes with zero padding in unused bits. Blocks are serialized column by column, with words in row order within each column and bytes in little-endian order within each word. The request is `FID[16] || round[8] || seed[32]`, and the response is `FID[16] || round[8] || aggregate_block || aggregate_tag`. This implementation specifies the HMAC-SHA256 domain labels, index conventions, and wire encoding.

The implementation targets local reproduction of static file integrity audits. An audit detects corruption when the sample contains a modified block, its contribution survives aggregation, and the aggregate authenticator does not collide. The convolution iterates over the set bits of the factors.

## Run the benchmarks

```sh
tmp=$(mktemp -d)
build/fca-bench "$tmp/default.bin" 2061 100 16 688 > results/ablation.csv
build/fca-bench "$tmp/scaling.bin" 2061 10 16 128 256 512 1024 2048 2061 > results/scaling.csv
python3 scripts/analyze.py > results/analysis.json
```

`fca-bench` takes the path for a new synthetic input file, the total block count, the number of trials per challenge size, the number of blocks timed for tag generation per trial, and one or more challenge sizes. Each benchmark command above writes approximately 1 GiB of temporary input. The benchmark uses fixed seed material for the synthetic input, keys, and audit seeds. See the [experiment report (Chinese)](results/REPORT.md) for timing definitions and results from the initial reproduction.

## Reproduce manuscript Figures 2–4

From the repository root, use the included plotting data to redraw the figures. The pinned plotting dependencies were tested with Python 3.9.6.

```sh
python3 -m venv build/plot-venv
. build/plot-venv/bin/activate
python -m pip install -r experiments/requirements.txt
python experiments/plot_manuscript_figures.py
```

The script writes `fig2`, `fig3`, and `fig4` as PDF and PNG files under `build/manuscript-figures/`. See the [figure reproduction guide](experiments/README.md) for commands to rerun the experiments and regenerate the plotting CSVs.

## License

This implementation is available under the [MIT License](LICENSE).
