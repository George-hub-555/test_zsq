#!/usr/bin/env python3
"""Plot the Faiss HNSW QPS–Recall curve from benchmark summary CSV."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--input",
        type=Path,
        default=Path("results/faiss_hnsw_summary.csv"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("results/faiss_hnsw_qps_recall.png"),
    )
    parser.add_argument("--linear-y", action="store_true")
    args = parser.parse_args()

    with args.input.open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise ValueError(f"no benchmark rows in {args.input}")

    points = sorted(
        (
            float(row["recall_at_k"]),
            float(row["median_qps"]),
            int(row["ef_search"]),
            int(row["top_k"]),
        )
        for row in rows
    )
    recalls = [point[0] for point in points]
    qps_values = [point[1] for point in points]
    top_k = points[0][3]

    figure, axis = plt.subplots(figsize=(8.2, 5.4))
    axis.plot(recalls, qps_values, marker="o", linewidth=2, label="Faiss HNSWFlat")
    for recall, qps, ef, _ in points:
        axis.annotate(
            f"ef={ef}",
            (recall, qps),
            xytext=(4, 5),
            textcoords="offset points",
            fontsize=8,
        )
    if not args.linear_y:
        axis.set_yscale("log")
    axis.set_xlabel(f"Recall@{top_k}")
    axis.set_ylabel("QPS")
    axis.set_title("Faiss HNSWFlat QPS–Recall")
    axis.grid(True, which="both", alpha=0.3)
    axis.legend()
    figure.tight_layout()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output, dpi=180)
    print(args.output)


if __name__ == "__main__":
    main()
