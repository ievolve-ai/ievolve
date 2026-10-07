"""Headless packing check: JSON metrics and an SVG, with no plotting dependency."""

import argparse
import json
from pathlib import Path
import sys
import tempfile

from types import SimpleNamespace

from evaluator import assess_packing, evaluate_request


def packing_svg(packing, metrics):
    """Render a validated packing as a standalone SVG using unit coordinates."""
    size, offset = 560, 40
    lines = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="640" height="680" viewBox="0 0 640 680">',
        '<rect width="640" height="680" fill="white"/>',
        '<title>26-circle packing, independently recomputed geometry</title>',
        '<rect x="40" y="40" width="560" height="560" fill="#f8fafc" stroke="#334155"/>',
    ]
    for index, ((x, y), radius) in enumerate(zip(packing["centers"], packing["radii"])):
        cx, cy = offset + size * x, offset + size * (1 - y)
        lines.append(
            f'<circle cx="{cx:.9f}" cy="{cy:.9f}" r="{size * radius:.9f}" '
            'fill="#38bdf8" fill-opacity="0.45" stroke="#0369a1" stroke-width="1"/>'
        )
        lines.append(
            f'<text x="{cx:.9f}" y="{cy:.9f}" text-anchor="middle" '
            f'dominant-baseline="central" font-family="sans-serif" font-size="11">{index}</text>'
        )
    lines.extend([
        '<text x="40" y="632" font-family="sans-serif" font-size="18">'
        f'26 circles | sum of radii: {metrics["sum_radii"]:.12f}</text>',
        '<text x="40" y="657" font-family="sans-serif" font-size="13">'
        f'Valid within 1e-12 | reference ratio: {metrics["target_ratio"]:.9f}</text>',
        '</svg>',
    ])
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", nargs="?", type=Path,
                        default=Path(__file__).with_name("initial_program.py"))
    parser.add_argument("--packing-json", type=Path,
                        help="Validate saved packing geometry without executing a candidate")
    parser.add_argument("--output", type=Path, default=Path("build/circle_packing_check"))
    args = parser.parse_args(argv)
    if args.packing_json:
        packing = json.loads(args.packing_json.read_text())
        result = assess_packing(packing["centers"], packing["radii"], packing.get("reported_sum"))
    else:
        with tempfile.TemporaryDirectory(prefix="packing-check-") as workspace:
            outcome = evaluate_request({
                "protocol_version": 1,
                "candidate": {"argv": [sys.executable, str(args.program.resolve())], "cwd": workspace},
                "limits": {"evaluation_timeout_ms": 60000, "max_artifact_bytes": 1024 * 1024},
                "enable_artifacts": True,
            })
        result = SimpleNamespace(metrics=outcome["metrics"], artifacts=outcome.get("artifacts", {}))
    args.output.mkdir(parents=True, exist_ok=True)
    for name in ("validation.json", "packing.json"):
        if name in result.artifacts:
            (args.output / name).write_text(result.artifacts[name] + "\n")
        else:
            (args.output / name).unlink(missing_ok=True)
    metrics_json = json.dumps(result.metrics, indent=2, allow_nan=False, sort_keys=True)
    (args.output / "metrics.json").write_text(metrics_json + "\n")
    if result.metrics["validity"] == 1.0:
        packing = json.loads(result.artifacts["packing.json"])
        (args.output / "packing.svg").write_text(packing_svg(packing, result.metrics))
    else:
        # Prevent a prior successful check's picture from representing this run.
        (args.output / "packing.svg").unlink(missing_ok=True)
    print(metrics_json)
    return 0 if result.metrics["validity"] == 1.0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
