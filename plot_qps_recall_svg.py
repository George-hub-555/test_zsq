#!/usr/bin/env python3
"""Render QPS/Recall SVG and HTML from the combined CSV without third-party packages."""

import argparse
import csv
import html
import math
import sys
from pathlib import Path


REQUIRED = {
    "scope", "variant", "top_k", "search_range", "run_count", "qps",
    "qps_min", "qps_max", "recall_at_k", "recall_min", "recall_max",
    "avg_us", "p99_us", "build_seconds", "index_bytes",
}
COLORS = {"erq9": "#059669", "rbq": "#2563eb", "zsq": "#dc2626"}
LABELS = {"erq9": "ERQ-9bit", "rbq": "RBQ (legacy)", "zsq": "ZSQ-8bit"}
BASELINE_VARIANTS = ("erq9", "rbq")


class PlotError(Exception):
    pass


def arguments():
    parser = argparse.ArgumentParser(description="Draw dependency-free QPS/Recall SVG charts")
    parser.add_argument("--csv", required=True, help="zsq_key_metrics.csv")
    parser.add_argument("--output-dir", required=True, help="directory for SVG/HTML output")
    return parser.parse_args()

def comparison_order(variants, top_k=None):
    baselines = [variant for variant in BASELINE_VARIANTS if variant in variants]
    context = "top_k={}".format(top_k) if top_k is not None else "comparison"
    if len(baselines) != 1 or "zsq" not in variants:
        raise PlotError(
            "{} must contain ZSQ and exactly one of ERQ-9bit or legacy RBQ".format(context)
        )
    expected = {baselines[0], "zsq"}
    if set(variants) != expected:
        raise PlotError(
            "{} contains unsupported mixed variants {}".format(
                context, ", ".join(sorted(variants))
            )
        )
    return baselines[0], "zsq"


def as_int(value, field, line):
    try:
        return int(value)
    except (TypeError, ValueError):
        raise PlotError("line {}: invalid {}={!r}".format(line, field, value))


def as_float(value, field, line):
    try:
        result = float(value)
    except (TypeError, ValueError):
        raise PlotError("line {}: invalid {}={!r}".format(line, field, value))
    if result != result or result in (float("inf"), float("-inf")):
        raise PlotError("line {}: non-finite {}".format(line, field))
    return result


def optional_float(value, default, field, line):
    return default if value in (None, "") else as_float(value, field, line)


def load_points(path):
    try:
        with path.open("r", encoding="utf-8-sig", newline="") as handle:
            reader = csv.DictReader(handle)
            missing = sorted(REQUIRED - set(reader.fieldnames or []))
            if missing:
                raise PlotError("{} missing columns: {}".format(path, ", ".join(missing)))
            rows = list(reader)
    except OSError as error:
        raise PlotError("cannot read {}: {}".format(path, error))

    groups = {}
    for line, raw in enumerate(rows, start=2):
        if (raw.get("scope") or "").strip().lower() != "aggregate":
            continue
        variant = (raw.get("variant") or "").strip().lower()
        if variant not in COLORS:
            raise PlotError("line {}: unsupported variant {!r}".format(line, variant))
        top_k = as_int(raw.get("top_k"), "top_k", line)
        qps = as_float(raw.get("qps"), "qps", line)
        recall = as_float(raw.get("recall_at_k"), "recall_at_k", line)
        point = {
            "variant": variant,
            "top_k": top_k,
            "search_range": as_int(raw.get("search_range"), "search_range", line),
            "run_count": as_int(raw.get("run_count"), "run_count", line),
            "qps": qps,
            "qps_min": optional_float(raw.get("qps_min"), qps, "qps_min", line),
            "qps_max": optional_float(raw.get("qps_max"), qps, "qps_max", line),
            "recall": recall,
            "recall_min": optional_float(raw.get("recall_min"), recall, "recall_min", line),
            "recall_max": optional_float(raw.get("recall_max"), recall, "recall_max", line),
            "avg_us": optional_float(raw.get("avg_us"), 0.0, "avg_us", line),
            "p99_us": optional_float(raw.get("p99_us"), 0.0, "p99_us", line),
            "build_seconds": optional_float(raw.get("build_seconds"), 0.0, "build_seconds", line),
            "index_bytes": optional_float(raw.get("index_bytes"), 0.0, "index_bytes", line),
        }
        if qps <= 0 or not 0 <= recall <= 1:
            raise PlotError("line {}: qps/recall out of range".format(line))
        groups.setdefault(top_k, {}).setdefault(variant, []).append(point)
    if not groups:
        raise PlotError("no scope=aggregate rows found")
    for top_k, variants in groups.items():
        order = comparison_order(variants, top_k)
        baseline = order[0]
        baseline_efs = {point["search_range"] for point in variants[baseline]}
        zsq_efs = {point["search_range"] for point in variants["zsq"]}
        if baseline_efs != zsq_efs:
            raise PlotError(
                "top_k={} has different {}/ZSQ search_range values".format(top_k, baseline.upper())
            )
        for points in variants.values():
            points.sort(key=lambda point: (point["recall"], point["search_range"]))
    return groups


def nice(value):
    if abs(value) >= 1000:
        return "{:,.0f}".format(value)
    if abs(value) >= 10:
        return "{:.1f}".format(value)
    return "{:.3g}".format(value)


def svg_chart(top_k, variants, logarithmic):
    width, height = 1120, 720
    left, right, top, bottom = 105, 45, 65, 100
    plot_w, plot_h = width - left - right, height - top - bottom
    order = comparison_order(variants, top_k)
    all_points = [point for variant in order for point in variants[variant]]
    x_min_data = min(point["recall_min"] for point in all_points)
    x_max_data = max(point["recall_max"] for point in all_points)
    x_pad = max(0.005, (x_max_data - x_min_data) * 0.08)
    x_lo = max(0.0, x_min_data - x_pad)
    x_hi = min(1.0, x_max_data + x_pad)
    if x_hi - x_lo < 0.01:
        x_lo = max(0.0, x_lo - 0.01)
        x_hi = min(1.0, x_hi + 0.01)

    positive_y = [value for point in all_points for value in (point["qps_min"], point["qps"], point["qps_max"]) if value > 0]
    if not positive_y:
        raise PlotError("top_k={} has no positive QPS".format(top_k))
    if logarithmic:
        y_lo = max(min(positive_y) * 0.8, 1e-12)
        y_hi = max(positive_y) * 1.25
        if y_hi <= y_lo:
            y_hi = y_lo * 10
    else:
        y_lo = 0.0
        y_hi = max(positive_y) * 1.12

    def x_pos(value):
        return left + (value - x_lo) / (x_hi - x_lo) * plot_w

    def y_pos(value):
        if logarithmic:
            fraction = (math.log10(max(value, y_lo)) - math.log10(y_lo)) / (math.log10(y_hi) - math.log10(y_lo))
        else:
            fraction = (value - y_lo) / (y_hi - y_lo)
        return top + plot_h * (1.0 - fraction)

    output = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" viewBox="0 0 {} {}">'.format(width, height, width, height),
        '<rect width="100%" height="100%" fill="white"/>',
        '<style>text{font-family:Arial,sans-serif;fill:#111827}.tick{font-size:13px}.label{font-size:16px;font-weight:600}.point{font-size:11px}.title{font-size:22px;font-weight:700}</style>',
        '<text x="{}" y="32" text-anchor="middle" class="title">QPS vs Recall@{} ({})</text>'.format(width / 2, top_k, "log QPS" if logarithmic else "linear QPS"),
    ]

    for index in range(6):
        value = x_lo + (x_hi - x_lo) * index / 5.0
        x = x_pos(value)
        output.append('<line x1="{0:.2f}" y1="{1}" x2="{0:.2f}" y2="{2}" stroke="#e5e7eb"/>'.format(x, top, top + plot_h))
        output.append('<text x="{:.2f}" y="{}" text-anchor="middle" class="tick">{:.1f}%</text>'.format(x, top + plot_h + 25, value * 100))
    for index in range(6):
        if logarithmic:
            value = 10 ** (math.log10(y_lo) + (math.log10(y_hi) - math.log10(y_lo)) * index / 5.0)
        else:
            value = y_lo + (y_hi - y_lo) * index / 5.0
        y = y_pos(value)
        output.append('<line x1="{0}" y1="{1:.2f}" x2="{2}" y2="{1:.2f}" stroke="#e5e7eb"/>'.format(left, y, left + plot_w))
        output.append('<text x="{}" y="{:.2f}" text-anchor="end" dominant-baseline="middle" class="tick">{}</text>'.format(left - 12, y, html.escape(nice(value))))
    output.extend([
        '<line x1="{0}" y1="{1}" x2="{0}" y2="{2}" stroke="#111827" stroke-width="1.5"/>'.format(left, top, top + plot_h),
        '<line x1="{0}" y1="{1}" x2="{2}" y2="{1}" stroke="#111827" stroke-width="1.5"/>'.format(left, top + plot_h, left + plot_w),
        '<text x="{}" y="{}" text-anchor="middle" class="label">Recall@{} (%)</text>'.format(left + plot_w / 2, height - 24, top_k),
        '<text x="25" y="{}" text-anchor="middle" transform="rotate(-90 25 {})" class="label">QPS</text>'.format(top + plot_h / 2, top + plot_h / 2),
    ])

    for variant in order:
        color = COLORS[variant]
        points = variants[variant]
        polyline = " ".join("{:.2f},{:.2f}".format(x_pos(point["recall"]), y_pos(point["qps"])) for point in points)
        output.append('<polyline points="{}" fill="none" stroke="{}" stroke-width="3"/>'.format(polyline, color))
        for point in points:
            x, y = x_pos(point["recall"]), y_pos(point["qps"])
            x1, x2 = x_pos(point["recall_min"]), x_pos(point["recall_max"])
            y1, y2 = y_pos(point["qps_min"]), y_pos(point["qps_max"])
            output.extend([
                '<line x1="{:.2f}" y1="{:.2f}" x2="{:.2f}" y2="{:.2f}" stroke="{}" opacity="0.55"/>'.format(x1, y, x2, y, color),
                '<line x1="{:.2f}" y1="{:.2f}" x2="{:.2f}" y2="{:.2f}" stroke="{}" opacity="0.55"/>'.format(x, y1, x, y2, color),
                '<circle cx="{:.2f}" cy="{:.2f}" r="5" fill="{}"><title>{}: ef={}, recall={:.6f}, qps={:.3f}</title></circle>'.format(x, y, color, LABELS[variant], point["search_range"], point["recall"], point["qps"]),
                '<text x="{:.2f}" y="{:.2f}" class="point" fill="{}">ef={}</text>'.format(x + 7, y - 7, color, point["search_range"]),
            ])
    legend_x = left + plot_w - 190
    for index, variant in enumerate(order):
        y = top + 18 + index * 25
        output.append('<line x1="{}" y1="{}" x2="{}" y2="{}" stroke="{}" stroke-width="4"/>'.format(legend_x, y, legend_x + 30, y, COLORS[variant]))
        output.append('<text x="{}" y="{}" dominant-baseline="middle" class="tick">{}</text>'.format(legend_x + 38, y, html.escape(LABELS[variant])))
    output.append("</svg>")
    return "\n".join(output)


def table_html(variants):
    rows = []
    for variant in comparison_order(variants):
        for point in sorted(variants[variant], key=lambda item: item["search_range"]):
            rows.append(
                "<tr><td>{}</td><td>{}</td><td>{:.6f}</td><td>{:.6f}–{:.6f}</td>"
                "<td>{:.3f}</td><td>{:.3f}–{:.3f}</td><td>{:.3f}</td><td>{:.3f}</td><td>{}</td></tr>".format(
                    html.escape(LABELS[variant]), point["search_range"], point["recall"],
                    point["recall_min"], point["recall_max"], point["qps"], point["qps_min"],
                    point["qps_max"], point["avg_us"], point["p99_us"], point["run_count"])
            )
    return "\n".join(rows)


def html_report(top_k, variants, linear_svg, log_svg, manifest):
    counts = sorted({point["run_count"] for points in variants.values() for point in points})
    note = "Aggregated from {} complete run(s).".format(min(counts))
    if min(counts) < 3:
        note += " Fewer than 3 runs: variation statistics are less reliable."
    order = comparison_order(variants)
    comparison = "{} vs {}".format(LABELS[order[0]], LABELS[order[1]])
    return """<!doctype html>
<html><head><meta charset="utf-8"><title>QPS vs Recall@{top_k}</title>
<style>body{{font-family:Arial,sans-serif;margin:24px;color:#111827}}.chart{{overflow-x:auto;border:1px solid #ddd;margin:18px 0}}table{{border-collapse:collapse;width:100%}}th,td{{border:1px solid #ddd;padding:6px;text-align:right}}th:first-child,td:first-child{{text-align:left}}pre{{white-space:pre-wrap;background:#f3f4f6;padding:12px}}</style></head>
<body><h1>Falcon {comparison} QPS–Recall@{top_k}</h1><p>{note}</p>
<h2>Linear QPS</h2><div class="chart">{linear}</div>
<h2>Log QPS</h2><div class="chart">{logarithmic}</div>
<h2>Aggregated data</h2><table><thead><tr><th>Variant</th><th>ef</th><th>Recall</th><th>Recall range</th><th>QPS</th><th>QPS range</th><th>Avg us</th><th>P99 us</th><th>Runs</th></tr></thead><tbody>{rows}</tbody></table>
<h2>Build manifest</h2><pre>{manifest}</pre></body></html>""".format(
        top_k=top_k, comparison=html.escape(comparison), note=html.escape(note), linear=linear_svg, logarithmic=log_svg,
        rows=table_html(variants), manifest=html.escape(manifest or "manifest.txt not found"))


def main():
    args = arguments()
    csv_path = Path(args.csv).expanduser().resolve()
    output_dir = Path(args.output_dir).expanduser().resolve()
    if not csv_path.is_file():
        print("ERROR: CSV not found: {}".format(csv_path), file=sys.stderr)
        return 2
    try:
        groups = load_points(csv_path)
        output_dir.mkdir(parents=True, exist_ok=True)
        manifest_path = csv_path.parent.parent / "manifest.txt"
        manifest = manifest_path.read_text(encoding="utf-8", errors="replace") if manifest_path.is_file() else ""
        warnings = []
        for top_k in sorted(groups):
            variants = groups[top_k]
            linear = svg_chart(top_k, variants, False)
            logarithmic = svg_chart(top_k, variants, True)
            prefix = "qps_recall_top{}".format(top_k)
            linear_path = output_dir / "{}_linear.svg".format(prefix)
            log_path = output_dir / "{}_log.svg".format(prefix)
            html_path = output_dir / "{}.html".format(prefix)
            linear_path.write_text(linear + "\n", encoding="utf-8")
            log_path.write_text(logarithmic + "\n", encoding="utf-8")
            html_path.write_text(html_report(top_k, variants, linear, logarithmic, manifest), encoding="utf-8")
            run_count = min(point["run_count"] for points in variants.values() for point in points)
            if run_count < 3:
                warnings.append("top_k={}: only {} complete run(s)".format(top_k, run_count))
            print("WROTE {}, {}, {}".format(linear_path, log_path, html_path))
        warning_path = output_dir / "analysis_warnings.txt"
        warning_path.write_text(("\n".join(warnings) if warnings else "No warnings.") + "\n", encoding="utf-8")
    except (PlotError, OSError) as error:
        print("ERROR: {}".format(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
