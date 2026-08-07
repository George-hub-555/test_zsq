#!/usr/bin/env python3
"""Merge Falcon ERQ-9bit/ZSQ benchmark CSV files using Python 3.8 stdlib only."""

import argparse
import csv
import statistics
import sys
from pathlib import Path


FIELDS = [
    "scope", "run_id", "variant", "quantizer", "base_bit_len", "ext_bit_len",
    "code_bits_per_dim", "top_k", "search_range", "run_count",
    "qps", "qps_min", "qps_max", "recall_at_k", "recall_min", "recall_max",
    "avg_us", "p50_us", "p90_us", "p95_us", "p99_us", "rounds",
    "warmup_queries", "base_count", "query_count", "dim", "build_seconds",
    "save_seconds", "total_build_seconds", "index_bytes", "search_max_rss_kib",
    "build_max_rss_kib", "thread_count", "link_range", "link_candidate_size",
    "build_iter_count", "batch_size_mb", "rotator_type", "source_search_csv",
    "source_build_csv",
]

SEARCH_REQUIRED = {
    "variant", "base_count", "query_count", "dim", "top_k", "search_range",
    "rounds", "warmup_queries", "qps", "avg_us", "p50_us", "p90_us",
    "p95_us", "p99_us", "recall_at_k", "max_rss_kib",
}
BUILD_REQUIRED = {
    "variant", "base_count", "dim", "thread_count", "link_range",
    "link_candidate_size", "build_iter_count", "batch_size_mb", "rotator_type",
    "build_seconds", "save_seconds", "total_seconds", "index_bytes", "max_rss_kib",
}
SEARCH_INTS = ["base_count", "query_count", "dim", "top_k", "search_range", "rounds", "warmup_queries"]
SEARCH_FLOATS = ["qps", "avg_us", "p50_us", "p90_us", "p95_us", "p99_us", "recall_at_k", "max_rss_kib"]
BUILD_INTS = ["base_count", "dim", "thread_count", "link_range", "link_candidate_size", "build_iter_count", "batch_size_mb", "rotator_type"]
BUILD_FLOATS = ["build_seconds", "save_seconds", "total_seconds", "index_bytes", "max_rss_kib"]

BASELINE_VARIANTS = ("erq9", "rbq")
QUANTIZATION_FIELDS = ("quantizer", "base_bit_len", "ext_bit_len", "code_bits_per_dim")
EXPECTED_QUANTIZATION = {
    "erq9": {
        "quantizer": "erq", "base_bit_len": 1, "ext_bit_len": 8,
        "code_bits_per_dim": 9,
    },
    "zsq": {
        "quantizer": "zsq", "base_bit_len": 0, "ext_bit_len": 0,
        "code_bits_per_dim": 8,
    },
}
LEGACY_RBQ_QUANTIZATION = {
    "quantizer": "legacy_rbq", "base_bit_len": "", "ext_bit_len": "",
    "code_bits_per_dim": "",
}


class ResultError(Exception):
    pass


def arguments():
    parser = argparse.ArgumentParser(description="Extract and aggregate Falcon ERQ-9bit/ZSQ or legacy RBQ/ZSQ results")
    parser.add_argument("--input", required=True, help="directory containing zsq_run_*")
    parser.add_argument("--output", required=True, help="combined CSV output")
    return parser.parse_args()


def find_one(run_dir, name):
    found = sorted(path for path in run_dir.rglob(name) if path.is_file())
    if len(found) > 1:
        raise ResultError("{}: multiple {} files: {}".format(run_dir, name, found))
    return found[0] if found else None


def read_rows(path, required):
    try:
        with path.open("r", encoding="utf-8-sig", newline="") as handle:
            reader = csv.DictReader(handle)
            missing = sorted(required - set(reader.fieldnames or []))
            if missing:
                raise ResultError("{}: missing columns {}".format(path, ", ".join(missing)))
            rows = list(reader)
    except OSError as error:
        raise ResultError("cannot read {}: {}".format(path, error))
    if not rows:
        raise ResultError("{}: no data rows".format(path))
    return rows


def integer(value, path, row, field):
    try:
        return int(value)
    except (TypeError, ValueError):
        raise ResultError("{} row {}: invalid {}={!r}".format(path, row, field, value))


def number(value, path, row, field):
    try:
        result = float(value)
    except (TypeError, ValueError):
        raise ResultError("{} row {}: invalid {}={!r}".format(path, row, field, value))
    if result != result or result in (float("inf"), float("-inf")):
        raise ResultError("{} row {}: non-finite {}".format(path, row, field))
    return result



def quantization_data(raw, variant, path, row):
    values = {field: (raw.get(field) or "").strip() for field in QUANTIZATION_FIELDS}
    present = [field for field, value in values.items() if value != ""]
    if not present:
        if variant == "erq9":
            raise ResultError(
                "{} row {}: ERQ-9bit CSV is missing quantization metadata".format(path, row)
            )
        if variant == "zsq":
            result = dict(EXPECTED_QUANTIZATION["zsq"])
        else:
            result = dict(LEGACY_RBQ_QUANTIZATION)
        result["_quantization_metadata_present"] = False
        return result
    missing = [field for field, value in values.items() if value == ""]
    if missing:
        raise ResultError(
            "{} row {}: incomplete quantization metadata: {}".format(
                path, row, ", ".join(missing)
            )
        )
    result = {
        "quantizer": values["quantizer"].lower(),
        "base_bit_len": integer(values["base_bit_len"], path, row, "base_bit_len"),
        "ext_bit_len": integer(values["ext_bit_len"], path, row, "ext_bit_len"),
        "code_bits_per_dim": integer(values["code_bits_per_dim"], path, row, "code_bits_per_dim"),
        "_quantization_metadata_present": True,
    }
    expected = EXPECTED_QUANTIZATION.get(variant)
    if expected and any(result[field] != expected[field] for field in QUANTIZATION_FIELDS):
        raise ResultError(
            "{} row {}: {} quantization metadata {} != expected {}".format(
                path, row, variant, {f: result[f] for f in QUANTIZATION_FIELDS}, expected
            )
        )
    return result
def search_data(path, variant):
    output = []
    keys = set()
    for line, raw in enumerate(read_rows(path, SEARCH_REQUIRED), start=2):
        actual = (raw.get("variant") or "").strip().lower()
        if actual != variant:
            raise ResultError("{} row {}: variant {} != {}".format(path, line, actual, variant))
        row = {"variant": variant}
        row.update(quantization_data(raw, variant, path, line))
        for field in SEARCH_INTS:
            row[field] = integer(raw.get(field), path, line, field)
        for field in SEARCH_FLOATS:
            row[field] = number(raw.get(field), path, line, field)
        key = (row["top_k"], row["search_range"])
        if key in keys:
            raise ResultError("{}: duplicate point {}".format(path, key))
        if row["qps"] <= 0 or not 0 <= row["recall_at_k"] <= 1:
            raise ResultError("{} row {}: qps/recall out of range".format(path, line))
        keys.add(key)
        output.append(row)
    return output


def build_data(path, variant):
    rows = read_rows(path, BUILD_REQUIRED)
    if len(rows) != 1:
        raise ResultError("{}: expected one build row, got {}".format(path, len(rows)))
    raw = rows[0]
    actual = (raw.get("variant") or "").strip().lower()
    if actual != variant:
        raise ResultError("{}: variant {} != {}".format(path, actual, variant))
    row = {"variant": variant}
    row.update(quantization_data(raw, variant, path, 2))
    for field in BUILD_INTS:
        row[field] = integer(raw.get(field), path, 2, field)
    for field in BUILD_FLOATS:
        row[field] = number(raw.get(field), path, 2, field)
    return row


def relative(path, root):
    if path is None:
        return ""
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def blank_row():
    return {field: "" for field in FIELDS}


def run_row(run_id, search, build, search_path, build_path, root):
    row = blank_row()
    row.update({
        "scope": "run", "run_id": run_id, "variant": search["variant"],
        "quantizer": search["quantizer"], "base_bit_len": search["base_bit_len"],
        "ext_bit_len": search["ext_bit_len"], "code_bits_per_dim": search["code_bits_per_dim"],
        "top_k": search["top_k"], "search_range": search["search_range"],
        "run_count": 1, "qps": search["qps"], "recall_at_k": search["recall_at_k"],
        "avg_us": search["avg_us"], "p50_us": search["p50_us"],
        "p90_us": search["p90_us"], "p95_us": search["p95_us"],
        "p99_us": search["p99_us"], "rounds": search["rounds"],
        "warmup_queries": search["warmup_queries"], "base_count": search["base_count"],
        "query_count": search["query_count"], "dim": search["dim"],
        "search_max_rss_kib": search["max_rss_kib"],
        "source_search_csv": relative(search_path, root),
        "source_build_csv": relative(build_path, root),
    })
    if build:
        row.update({
            "build_seconds": build["build_seconds"], "save_seconds": build["save_seconds"],
            "total_build_seconds": build["total_seconds"], "index_bytes": build["index_bytes"],
            "build_max_rss_kib": build["max_rss_kib"], "thread_count": build["thread_count"],
            "link_range": build["link_range"], "link_candidate_size": build["link_candidate_size"],
            "build_iter_count": build["build_iter_count"], "batch_size_mb": build["batch_size_mb"],
            "rotator_type": build["rotator_type"],
        })
    return row


def one_value(rows, field, context, allow_blank=False):
    values = [row[field] for row in rows if not allow_blank or row[field] != ""]
    if not values:
        return ""
    if len(set(values)) != 1:
        raise ResultError("{}: inconsistent {} values {}".format(context, field, sorted(set(values))))
    return values[0]


def median(rows, field):
    values = [float(row[field]) for row in rows if row[field] != ""]
    return statistics.median(values) if values else ""


def aggregate(rows):
    first = rows[0]
    context = "{} top_k={} search_range={}".format(first["variant"], first["top_k"], first["search_range"])
    qps = [float(row["qps"]) for row in rows]
    recall = [float(row["recall_at_k"]) for row in rows]
    result = blank_row()
    result.update({
        "scope": "aggregate", "run_id": "ALL", "variant": first["variant"],
        "quantizer": first["quantizer"], "base_bit_len": first["base_bit_len"],
        "ext_bit_len": first["ext_bit_len"], "code_bits_per_dim": first["code_bits_per_dim"],
        "top_k": first["top_k"], "search_range": first["search_range"],
        "run_count": len(rows), "qps": statistics.median(qps), "qps_min": min(qps),
        "qps_max": max(qps), "recall_at_k": statistics.median(recall),
        "recall_min": min(recall), "recall_max": max(recall),
    })
    for field in ("rounds", "warmup_queries", "base_count", "query_count", "dim"):
        result[field] = one_value(rows, field, context)
    for field in ("avg_us", "p50_us", "p90_us", "p95_us", "p99_us",
                  "build_seconds", "save_seconds", "total_build_seconds", "index_bytes",
                  "search_max_rss_kib", "build_max_rss_kib"):
        result[field] = median(rows, field)
    for field in ("thread_count", "link_range", "link_candidate_size", "build_iter_count",
                  "batch_size_mb", "rotator_type"):
        result[field] = one_value(rows, field, context, allow_blank=True)
    for field in QUANTIZATION_FIELDS:
        result[field] = one_value(rows, field, context, allow_blank=True)
    return result


def collect(root):
    run_dirs = sorted(path for path in root.glob("zsq_run_*") if path.is_dir())
    if not run_dirs:
        raise ResultError("{} has no zsq_run_* directories".format(root))
    warnings = []
    rows = []
    expected_points = None
    expected_configs = {}
    expected_baseline = None
    for run_dir in run_dirs:
        baseline_paths = {
            variant: find_one(run_dir, "search_{}.csv".format(variant))
            for variant in BASELINE_VARIANTS
        }
        present_baselines = [variant for variant, path in baseline_paths.items() if path is not None]
        if len(present_baselines) > 1:
            raise ResultError(
                "{}: contains both legacy RBQ and ERQ-9bit search CSV files".format(run_dir.name)
            )
        if not present_baselines:
            warnings.append(
                "skip {}: missing search_erq9.csv or search_rbq.csv".format(run_dir.name)
            )
            continue
        baseline = present_baselines[0]
        searches = {
            baseline: baseline_paths[baseline],
            "zsq": find_one(run_dir, "search_zsq.csv"),
        }
        missing = [
            variant for variant in (baseline, "zsq")
            if searches[variant] is None or searches[variant].stat().st_size == 0
        ]
        if missing:
            warnings.append(
                "skip {}: missing/empty {}".format(
                    run_dir.name,
                    ", ".join("search_{}.csv".format(variant) for variant in missing),
                )
            )
            continue
        if expected_baseline is None:
            expected_baseline = baseline
        elif expected_baseline != baseline:
            raise ResultError(
                "{} mixes {} runs with {} runs; aggregate them in separate input directories".format(
                    root, expected_baseline, baseline
                )
            )
        variants = (baseline, "zsq")
        search_rows = {variant: search_data(searches[variant], variant) for variant in variants}
        if baseline == "erq9":
            for variant in variants:
                if not all(row["_quantization_metadata_present"] for row in search_rows[variant]):
                    raise ResultError(
                        "{}: new ERQ-9bit/ZSQ CSV files must include quantization metadata".format(
                            run_dir.name
                        )
                    )
        point_sets = {
            variant: {(row["top_k"], row["search_range"]) for row in search_rows[variant]}
            for variant in variants
        }
        if point_sets[baseline] != point_sets["zsq"]:
            raise ResultError(
                "{}: {}/ZSQ search points differ".format(run_dir.name, baseline.upper())
            )
        baseline_by_point = {
            (row["top_k"], row["search_range"]): row for row in search_rows[baseline]
        }
        zsq_by_point = {
            (row["top_k"], row["search_range"]): row for row in search_rows["zsq"]
        }
        for point in sorted(point_sets[baseline]):
            for field in ("base_count", "query_count", "dim", "rounds", "warmup_queries"):
                if baseline_by_point[point][field] != zsq_by_point[point][field]:
                    raise ResultError(
                        "{}: {}/ZSQ {} differs at point {}".format(
                            run_dir.name, baseline.upper(), field, point
                        )
                    )
        if expected_points is None:
            expected_points = point_sets[baseline]
        elif expected_points != point_sets[baseline]:
            raise ResultError("{}: search points differ from previous complete runs".format(run_dir.name))
        builds = {}
        build_paths = {}
        for variant in variants:
            build_paths[variant] = find_one(run_dir, "build_{}.csv".format(variant))
            path = build_paths[variant]
            if path is None or path.stat().st_size == 0:
                builds[variant] = None
                warnings.append(
                    "{}: build_{}.csv missing/empty; build fields blank".format(
                        run_dir.name, variant
                    )
                )
            else:
                builds[variant] = build_data(path, variant)
                for field in QUANTIZATION_FIELDS:
                    if builds[variant][field] != search_rows[variant][0][field]:
                        raise ResultError(
                            "{}: {} build/search {} differs".format(
                                run_dir.name, variant, field
                            )
                        )
        if builds[baseline] is not None and builds["zsq"] is not None:
            for field in BUILD_INTS:
                if builds[baseline][field] != builds["zsq"][field]:
                    raise ResultError(
                        "{}: {}/ZSQ build {} differs".format(
                            run_dir.name, baseline.upper(), field
                        )
                    )
        for variant in variants:
            for search in search_rows[variant]:
                key = (variant, search["top_k"], search["search_range"])
                config = tuple(
                    search[field]
                    for field in ("base_count", "query_count", "dim", "rounds", "warmup_queries")
                )
                if key in expected_configs and expected_configs[key] != config:
                    raise ResultError("{}: inconsistent configuration for {}".format(run_dir.name, key))
                expected_configs[key] = config
                rows.append(
                    run_row(
                        run_dir.name, search, builds[variant], searches[variant],
                        build_paths[variant], root
                    )
                )
    if not rows:
        raise ResultError(
            "no complete run has ERQ-9bit/ZSQ or legacy RBQ/ZSQ search CSV pairs"
        )
    groups = {}
    for row in rows:
        key = (row["variant"], int(row["top_k"]), int(row["search_range"]))
        groups.setdefault(key, []).append(row)
    summary = [aggregate(groups[key]) for key in sorted(groups, key=lambda x: (x[1], x[0], x[2]))]
    rows.sort(key=lambda row: (
        row["run_id"], row["variant"], int(row["top_k"]), int(row["search_range"])
    ))
    return rows + summary, warnings


def text(value):
    if value == "" or value is None:
        return ""
    if isinstance(value, str):
        return value
    value = float(value)
    if value.is_integer() and abs(value) < 1e16:
        return str(int(value))
    return "{:.10g}".format(value)


def write_csv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS, lineterminator="\n")
        writer.writeheader()
        for row in rows:
            writer.writerow({field: text(row.get(field, "")) for field in FIELDS})


def main():
    args = arguments()
    root = Path(args.input).expanduser().resolve()
    output = Path(args.output).expanduser().resolve()
    if not root.is_dir():
        print("ERROR: input directory not found: {}".format(root), file=sys.stderr)
        return 2
    try:
        rows, warnings = collect(root)
        write_csv(output, rows)
    except (ResultError, OSError) as error:
        print("ERROR: {}".format(error), file=sys.stderr)
        return 1
    for warning in warnings:
        print("WARNING: {}".format(warning), file=sys.stderr)
    run_ids = {row["run_id"] for row in rows if row["scope"] == "run"}
    print("WROTE {}".format(output))
    print("complete_runs={} run_rows={} aggregate_rows={}".format(
        len(run_ids), sum(r["scope"] == "run" for r in rows), sum(r["scope"] == "aggregate" for r in rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
