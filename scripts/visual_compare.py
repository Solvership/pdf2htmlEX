#!/usr/bin/env python3
"""
Pixel comparison of two HTML renderings (run inside the dev container).

    visual_compare.py OLD.html NEW.html [OLD2.html NEW2.html ...]

Every page is rendered with headless Chromium at the same size and the
screenshots are compared pixel by pixel. Exit code 1 if any pair differs.
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops

WIDTH, HEIGHT = 1000, 1450


def shot(html, png, width=WIDTH, height=HEIGHT):
    subprocess.run(
        ["chromium", "--headless=new", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
         "--force-device-scale-factor=1", "--run-all-compositor-stages-before-draw",
         "--virtual-time-budget=3000", f"--window-size={width},{height}",
         f"--screenshot={png}", "file://" + os.path.abspath(html)],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return Image.open(png).convert("RGB")


def diff_pixels(a, b):
    if a.size != b.size:
        return -1
    d = ImageChops.difference(a, b).convert("L")
    return sum(1 for v in d.getdata() if v > 0)


def main(argv):
    rc = 0
    tmp = tempfile.mkdtemp()
    pairs = list(zip(argv[0::2], argv[1::2]))
    for i, (old, new) in enumerate(pairs):
        a = shot(old, os.path.join(tmp, f"{i}a.png"))
        b = shot(new, os.path.join(tmp, f"{i}b.png"))
        n = diff_pixels(a, b)
        status = "PIXEL-IDENTICAL" if n == 0 else f"DIFFERENT ({n} px)"
        if n != 0:
            rc = 1
            a.save(os.path.join(os.path.dirname(new) or ".", os.path.basename(new) + ".old.png"))
            b.save(os.path.join(os.path.dirname(new) or ".", os.path.basename(new) + ".new.png"))
        print(f"{status:24s} {os.path.basename(new)}")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
