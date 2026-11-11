#!/usr/bin/env python3
"""
Fidelity of a conversion: compares the first page of the PDF rasterized by
Poppler (pdftoppm) with the first page of the HTML rendered by Chromium.
Run inside the dev container:

    fidelity_metric.py FILE.pdf FILE.html [SCALE]

Prints the mean absolute difference (0-255) and the share of pixels that differ
noticeably (> 64 grey levels). Lower is better; anti-aliasing differences
between the two renderers mean that 0 is not reachable.
"""
import os
import re
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops, ImageStat


def main(pdf, html, scale=2.0):
    tmp = tempfile.mkdtemp()
    # PDF raster
    subprocess.run(['pdftoppm', '-r', str(72 * scale), '-f', '1', '-l', '1', '-png', '-singlefile', pdf,
                    os.path.join(tmp, 'pdf')], check=True)
    ref = Image.open(os.path.join(tmp, 'pdf.png')).convert('L')
    w, h = ref.size

    # HTML: only the first page, at the top left corner, without decorations
    with open(html, encoding='utf-8') as f:
        doc = f.read()
    style = ('<style>#sidebar{display:none!important}#page-container{position:static!important;'
             'background:none!important;overflow:visible!important}'
             '.pf{margin:0!important;box-shadow:none!important;border:0!important}'
             'body{margin:0!important}.pf~.pf{display:none!important}</style>')
    doc = doc.replace('</head>', style + '</head>', 1)
    test = os.path.join(tmp, 'page.html')
    with open(test, 'w', encoding='utf-8') as f:
        f.write(doc)
    shot = os.path.join(tmp, 'html.png')
    subprocess.run(['chromium', '--headless=new', '--no-sandbox', '--disable-gpu', '--hide-scrollbars',
                    f'--force-device-scale-factor={scale}', '--virtual-time-budget=3000',
                    f'--window-size={int(round(w / scale))},{int(round(h / scale))}',
                    f'--screenshot={shot}', 'file://' + os.path.abspath(test)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    img = Image.open(shot).convert('L').resize(ref.size)
    diff = ImageChops.difference(ref, img)
    mean = ImageStat.Stat(diff).mean[0]
    hist = diff.histogram()
    bad = sum(hist[65:]) / float(w * h)
    print(f'{os.path.basename(html):45s} mean-diff {mean:6.3f}   pixels>64: {bad * 100:6.3f}%')
    return mean, bad


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 2.0)
