#!/usr/bin/env python3
"""compare_stills.py [--exact [N]] A.png B.png: per-channel statistics of two stills.

Prints the mean and the standard deviation of R, G and B (values 0 to 1) of both
images and their differences. Exit 0 when every mean and every deviation differs by at
most 0.03, 1 otherwise, 2 when an image cannot be read. Statistics, not pixels: the
plugin's still and the sketch's are two realisations of the same look (other random
starts), so only their distributions can match. The images may differ in size.

--exact [N]: pixel by pixel instead, for two renders of the same run (before and after a
change that must not change the picture): the largest and the mean absolute difference
per channel in 1/255 steps, and how many pixels differ by more than N (default 1). Exit 0
when no channel of any pixel differs by more than N, 1 otherwise, 2 when an image cannot
be read or the sizes differ.

What the statistics cannot see (so the side-by-side look by eye stays): a flipped or
mirrored picture, light from the wrong side, the web's scale (same coverage at another
wavelength), soft or aliased edges, where things are.
"""
import sys

import numpy as np
from PIL import Image

TOLERANCE = 0.03


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)


def stats(path):
    rgb = load(path) / 255.0
    flat = rgb.reshape(-1, 3)
    return rgb.shape[1], rgb.shape[0], flat.mean(axis=0), flat.std(axis=0)


def exact(a_path, b_path, limit):
    a, b = load(a_path), load(b_path)
    if a.shape != b.shape:
        print(f"compare_stills: sizes differ: {a.shape[1]}x{a.shape[0]} and {b.shape[1]}x{b.shape[0]}", file=sys.stderr)
        return 2
    d = np.abs(a - b)
    print(f"{'':6} A {a_path}  ({a.shape[1]}x{a.shape[0]})")
    print(f"{'':6} B {b_path}")
    print(f"{'':6} {'max':>6} {'mean':>10}   (1/255 steps)")
    for k, name in enumerate("RGB"):
        print(f"{name:6} {d[..., k].max():6.0f} {d[..., k].mean():10.6f}")
    over = int((d.max(axis=2) > limit).sum())
    ok = over == 0
    print(f"{over} pixels differ by more than {limit}: {'same picture' if ok else 'differ'}")
    return 0 if ok else 1


def main(argv):
    args = argv[1:]
    limit = None
    if args and args[0] == "--exact":
        args = args[1:]
        limit = 1
        if len(args) == 3:
            limit = int(args[0])
            args = args[1:]
    if len(args) != 2:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        return 2
    try:
        if limit is not None:
            return exact(args[0], args[1], limit)
        a, b = stats(args[0]), stats(args[1])
    except (OSError, ValueError) as e:
        print(f"compare_stills: {e}", file=sys.stderr)
        return 2
    ok = True
    print(f"{'':6} {'A ' + args[0]}  ({a[0]}x{a[1]})")
    print(f"{'':6} {'B ' + args[1]}  ({b[0]}x{b[1]})")
    print(f"{'':6} {'mean A':>8} {'mean B':>8} {'diff':>7}   {'std A':>8} {'std B':>8} {'diff':>7}")
    for k, name in enumerate("RGB"):
        dm, ds = b[2][k] - a[2][k], b[3][k] - a[3][k]
        bad = abs(dm) > TOLERANCE or abs(ds) > TOLERANCE
        ok = ok and not bad
        print(f"{name:6} {a[2][k]:8.4f} {b[2][k]:8.4f} {dm:+7.4f}   {a[3][k]:8.4f} {b[3][k]:8.4f} {ds:+7.4f}{'   OVER' if bad else ''}")
    print(f"{'match' if ok else 'differ'} (tolerance {TOLERANCE})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
