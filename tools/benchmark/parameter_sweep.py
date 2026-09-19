"""Run a small XML parameter sweep against a RAWPACK burst."""

from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("cli", type=Path)
    parser.add_argument("burst", type=Path)
    parser.add_argument("profile", type=Path)
    parser.add_argument("--attribute", default="fineSharpen")
    parser.add_argument("--values", nargs="+", type=float, default=[0.0, 0.05, 0.10, 0.15])
    parser.add_argument("--output", type=Path, default=Path("benchmark/sweep"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    original = args.profile.read_text(encoding="utf-8")
    for value in args.values:
        tuned = original.replace(f'{args.attribute}="0.10"', f'{args.attribute}="{value}"')
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "profile.xml"
            profile.write_text(tuned, encoding="utf-8")
            destination = args.output / f"{args.attribute}_{value:.3f}.ppm"
            subprocess.run([str(args.cli), "process", str(args.burst), "--profile", str(profile), "--output", str(destination)], check=True)
    print(f"wrote {len(args.values)} sweep results to {args.output}")


if __name__ == "__main__":
    main()
