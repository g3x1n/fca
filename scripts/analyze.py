"""Recompute Table I, sampling curves, and a local audit-latency model."""

import argparse
import csv
import json
import math
import random
import statistics
from fractions import Fraction


def miss_probability(m, bad, c):
    if c > m - bad:
        return 0.0
    return math.prod((m - bad - i) / (m - i) for i in range(c))


def sampling(repeats):
    m = 10_000
    sample_sizes = (128, 256, 688)
    rng = random.Random(179)
    trial_counts = dict.fromkeys(sample_sizes, 0)
    for _ in range(repeats):
        # Randomizing the corruption set is equivalent to randomizing the challenge.
        first_bad = min(rng.sample(range(m), 100))
        for c in sample_sizes:
            trial_counts[c] += first_bad >= c
    results = []
    for c in sample_sizes:
        for bad in (10, 100, 1_000):
            row = dict(m=m, bad=bad, c=c, exact=miss_probability(m, bad, c))
            if bad == 100:
                f = trial_counts[c] / repeats
                z = 1.95996398454
                center = (f + z**2 / (2 * repeats)) / (1 + z**2 / repeats)
                margin = z * math.sqrt(f * (1 - f) / repeats + z**2 / (4 * repeats**2)) / (
                    1 + z**2 / repeats
                )
                row.update(misses=trial_counts[c], trials=repeats,
                           empirical=f, wilson_95=[max(0, center - margin), min(1, center + margin)])
            results.append(row)
    return results


def security():
    q = 2**178
    dense = Fraction(1, q)
    polynomial = 23_424 * dense
    fca1 = dense + (1 - dense) * dense
    fca2 = dense + (1 - dense) * dense**2
    bounds = {}
    for name, epsilon in dict(dense=dense, polynomial=polynomial, fca_t1=fca1, fca_t2=fca2).items():
        bounds[name] = {
            "one_query_bits": -math.log2(float(epsilon)),
            "Qv_2pow32_bits": -math.log2(float((2**32) * epsilon)),
            "Qv_2pow32_vs_2pow_minus146": (
                "below" if (2**32) * epsilon < Fraction(1, 2**146)
                else "above" if (2**32) * epsilon > Fraction(1, 2**146)
                else "equal"
            ),
        }
    return bounds


def sizes():
    return dict(block_bytes=178 * 366 * 8,
                tag_bytes=23,
                fca_request_bytes=16 + 8 + 32,
                explicit_request_bytes_at_688=16 + 8 + 688 * (4 + 23),
                proof_bytes=16 + 8 + 178 * 366 * 8 + 23,
                cache_elements={"fca_t1": 430, "dense": 23_424, "polynomial": 1, "fca_t2": 860},
                fca_t1_cache_reduction=23_424 / 430)


def version_models():
    return [dict(versions=versions, fca_shared_24byte_state=430 * 24,
                 dense_per_file_24byte_state=23_424 * 24 * versions,
                 dense_shared_24byte_state=23_424 * 24,
                 polynomial_per_file_24byte_state=24 * versions)
            for versions in (1, 32, 1024)]


def latency_model(ablation):
    with open(ablation, newline="") as stream:
        rows = list(csv.DictReader(stream))
    result = []
    for name in ("fca_unified", "dense", "polynomial"):
        group = [row for row in rows if row["variant"] == name and row["c"] == "688"]
        if not group or not all(row["valid"] == "1" for row in group):
            raise ValueError(f"missing valid c=688 benchmark rows for {name}")
        local = statistics.median(float(row["proof_ms"]) + float(row["verify_total_ms"]) +
                                  (float(row["challenge_expand_ms"]) if name == "fca_unified" else 0)
                                  for row in group)
        wire = int(group[0]["request_bytes"]) + int(group[0]["response_bytes"])
        for mbps in (10, 25, 50, 100, 200, 480, 1_000):
            result.append(dict(variant=name, mbps=mbps, local_ms=local, paper_rtt_ms=9.6,
                               modeled_total_ms=local + 9.6 + wire * 8 / (1_000 * mbps)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ablation", default="results/ablation.csv")
    parser.add_argument("--trials", default=100_000, type=int)
    args = parser.parse_args()
    if args.trials < 1:
        parser.error("--trials must be positive")
    output = dict(default_sizes=sizes(), security_bounds=security(),
                  sampling=sampling(args.trials), version_models=version_models(),
                  bandwidth_model=latency_model(args.ablation))
    print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
