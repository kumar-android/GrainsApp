"""Derive a bounded radial flat-field polynomial from PGM-like text samples.

The production pipeline consumes measured coefficients through the XML sensor
section. This helper intentionally stays offline and dependency-free.
"""

from pathlib import Path
import argparse
import math


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("samples", type=Path, help="text rows: x y normalized_gain")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = []
    for line in args.samples.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        x, y, gain = (float(v) for v in line.split())
        rows.append((x * x + y * y, gain))
    if len(rows) < 4:
        raise SystemExit("at least four samples are required")
    # Solve a small normal-equation system for gain = a0+a1*r2+a2*r4+a3*r6.
    matrix = [[0.0] * 5 for _ in range(4)]
    for radius2, gain in rows:
        basis = [1.0, radius2, radius2 * radius2, radius2 * radius2 * radius2]
        for r in range(4):
            for c in range(4):
                matrix[r][c] += basis[r] * basis[c]
            matrix[r][4] += basis[r] * gain
    for pivot in range(4):
        scale = matrix[pivot][pivot]
        for c in range(pivot, 5):
            matrix[pivot][c] /= scale
        for row in range(4):
            if row == pivot:
                continue
            factor = matrix[row][pivot]
            for c in range(pivot, 5):
                matrix[row][c] -= factor * matrix[pivot][c]
    coefficients = [max(0.25, min(4.0, matrix[i][4])) for i in range(4)]
    args.output.write_text(
        "<sensor lensRadial0=\"{0:.8f}\" lensRadial1=\"{1:.8f}\" lensRadial2=\"{2:.8f}\" lensRadial3=\"{3:.8f}\" />\n".format(*coefficients),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
