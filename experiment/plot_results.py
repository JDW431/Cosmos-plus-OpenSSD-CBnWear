#!/usr/bin/env python3
"""Classify exported block metadata and create the experiment summary plots."""
import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt


def read_blocks(path: Path):
    rows = []
    with path.open(newline="") as source:
        for row in csv.DictReader(source):
            if row["age"] == "NA" or row["bad"] == "1" or row["free"] == "1":
                continue
            row["die"] = int(row["die"])
            row["virtual_block"] = int(row["virtual_block"])
            row["erase_count"] = int(row["erase_count"])
            row["age"] = int(row["age"])
            rows.append(row)
    return rows


def mean(values):
    return sum(values) / len(values) if values else 0.0


def variance(values):
    if not values:
        return 0.0
    average = mean(values)
    return sum((value - average) ** 2 for value in values) / len(values)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--blocks", required=True, type=Path)
    parser.add_argument("--threshold", required=True, type=int)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()

    rows = read_blocks(args.blocks)
    if not rows:
        raise SystemExit("No allocated blocks with a defined age were found in blocks.csv.")

    # Must match the firmware policy: age > threshold is cold.
    for row in rows:
        row["temperature"] = "cold" if row["age"] > args.threshold else "hot"

    args.output_dir.mkdir(parents=True, exist_ok=True)
    classified = args.output_dir / "classified_blocks.csv"
    with classified.open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=[
            "die", "virtual_block", "erase_count", "age", "temperature"
        ])
        writer.writeheader()
        writer.writerows(rows)

    groups = {"hot": [], "cold": []}
    for row in rows:
        groups[row["temperature"]].append(row)

    summary = args.output_dir / "summary.csv"
    with summary.open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=[
            "temperature", "block_count", "mean_age", "mean_erase_count",
            "erase_count_variance", "min_erase_count", "max_erase_count"
        ])
        writer.writeheader()
        for temperature in ("hot", "cold"):
            erase_counts = [row["erase_count"] for row in groups[temperature]]
            ages = [row["age"] for row in groups[temperature]]
            writer.writerow({
                "temperature": temperature,
                "block_count": len(groups[temperature]),
                "mean_age": f"{mean(ages):.3f}",
                "mean_erase_count": f"{mean(erase_counts):.3f}",
                "erase_count_variance": f"{variance(erase_counts):.3f}",
                "min_erase_count": min(erase_counts) if erase_counts else "",
                "max_erase_count": max(erase_counts) if erase_counts else "",
            })

    colors = {"hot": "#e45756", "cold": "#4c78a8"}
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5))
    for temperature in ("hot", "cold"):
        subset = groups[temperature]
        axes[0].scatter(
            [row["age"] for row in subset],
            [row["erase_count"] for row in subset],
            s=18, alpha=0.75, label=temperature, color=colors[temperature]
        )
    axes[0].axvline(args.threshold, color="black", linestyle="--", linewidth=1,
                    label=f"threshold={args.threshold}")
    axes[0].set(xlabel="age", ylabel="erase count (P/E cycles)", title="Age and wear")
    axes[0].legend()

    labels = ["hot", "cold"]
    axes[1].bar(labels, [len(groups[label]) for label in labels],
                color=[colors[label] for label in labels])
    axes[1].set(ylabel="allocated block count", title="Hot/cold classification")

    box_values = [[row["erase_count"] for row in groups[label]] for label in labels]
    nonempty_labels = [label for label, values in zip(labels, box_values) if values]
    nonempty_values = [values for values in box_values if values]
    if nonempty_values:
        axes[2].boxplot(nonempty_values, tick_labels=nonempty_labels)
    axes[2].set(ylabel="erase count (P/E cycles)", title="Wear distribution")

    fig.suptitle(f"OpenSSD GC age experiment (threshold: {args.threshold})")
    fig.tight_layout()
    fig.savefig(args.output_dir / "wear_age_summary.png", dpi=160)


if __name__ == "__main__":
    main()
