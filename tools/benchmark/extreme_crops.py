"""Create nearest-neighbour inspection crops from a binary PPM result.

This is offline analysis only; it is not part of the production image path.
"""

from pathlib import Path
import argparse


def read_ppm(path: Path):
    with path.open("rb") as handle:
        if handle.readline().strip() != b"P6":
            raise ValueError("only P6 PPM is supported")
        width, height = (int(value) for value in handle.readline().split())
        if int(handle.readline()) != 255:
            raise ValueError("only 8-bit PPM is supported")
        return width, height, bytearray(handle.read())


def write_ppm(path: Path, width: int, height: int, pixels: bytearray):
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + pixels)


def crop(width, height, pixels, center_x, center_y, size):
    left = max(0, min(width - size, center_x - size // 2))
    top = max(0, min(height - size, center_y - size // 2))
    output = bytearray(size * size * 3)
    for y in range(size):
        src = ((top + y) * width + left) * 3
        output[y * size * 3:(y + 1) * size * 3] = pixels[src:src + size * 3]
    return size, size, output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--x", type=int, default=None)
    parser.add_argument("--y", type=int, default=None)
    parser.add_argument("--output", type=Path, default=Path("benchmark/crops"))
    args = parser.parse_args()
    width, height, pixels = read_ppm(args.image)
    x = width // 2 if args.x is None else args.x
    y = height // 2 if args.y is None else args.y
    args.output.mkdir(parents=True, exist_ok=True)
    for scale in (100, 200, 400, 800):
        side = max(8, min(width, height) // max(1, scale // 100))
        w, h, data = crop(width, height, pixels, x, y, side)
        write_ppm(args.output / f"crop{scale}.ppm", w, h, data)


if __name__ == "__main__":
    main()
