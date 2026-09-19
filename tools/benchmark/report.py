"""Build a small reproducible HTML index from CLI diagnostics JSON files."""

from pathlib import Path
import argparse
import html
import json


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args()
    output = args.output or args.directory / "report.html"
    rows = []
    for path in sorted(args.directory.glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        rows.append(
            "<tr><td>{}</td><td>{}</td><td>{}/{}</td><td>{:.4f}</td><td>{:.4f}</td><td>{:.2f} ms</td></tr>".format(
                html.escape(path.stem),
                html.escape(data.get("profile", "")),
                data.get("acceptedFrames", 0),
                data.get("inputFrames", 0),
                data.get("mergeConfidence", 0.0),
                data.get("motionFraction", 0.0),
                data.get("processingMilliseconds", 0.0),
            )
        )
    document = """<!doctype html>
<meta charset="utf-8"><title>GCam Computational Lab benchmark</title>
<style>body{{font:14px system-ui;margin:2rem}} table{{border-collapse:collapse}} th,td{{padding:.45rem .7rem;border:1px solid #ccc;text-align:left}}</style>
<h1>Benchmark diagnostics</h1>
<p>Metrics are diagnostic signals, not a claim of exact reference equivalence.</p>
<table><tr><th>Run</th><th>Profile</th><th>Accepted</th><th>Merge confidence</th><th>Motion fraction</th><th>Time</th></tr>{}</table>
""".format("".join(rows))
    output.write_text(document, encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
