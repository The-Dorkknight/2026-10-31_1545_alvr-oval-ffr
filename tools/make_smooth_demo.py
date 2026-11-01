"""Turn the PPMs written by tests/smooth_demo and tests/overlay_demo into captioned PNGs.

Developer tool (needs Pillow, which the app itself does not). Usage:
    python tools/make_smooth_demo.py <smooth_demo.ppm> docs/smooth_demo.png
    python tools/make_smooth_demo.py --overlay <overlay_demo.ppm> docs/overlay_demo.png
"""
import sys

from PIL import Image, ImageDraw

CAPTIONS = ["Full detail (what the game would draw)",
            "Low-res edge today (4x4 blocks)",
            "With Smooth low-res edge (60 %)"]


OVERLAY_CAPTIONS = ["VRS reached the image: yellow 2x2, red 4x4 (VRS check 100 %)",
                    "Game drew it at full detail: purple (VRS check 14 %: smooth areas look alike)"]


def main(src, dst, captions=None):
    captions = captions or CAPTIONS
    im = Image.open(src).convert("RGB")
    bar = 30
    out = Image.new("RGB", (im.width, im.height + bar), "white")
    out.paste(im, (0, bar))
    d = ImageDraw.Draw(out)
    gap = 8 if len(captions) == 3 else 12
    panel = (im.width + gap) // len(captions)
    for i, text in enumerate(captions):
        d.text((i * panel + 6, 9), text, fill=(20, 20, 20))
    out.save(dst, optimize=True)
    print("wrote", dst, out.size)


if __name__ == "__main__":
    if sys.argv[1] == "--overlay":
        main(sys.argv[2], sys.argv[3], OVERLAY_CAPTIONS)
    else:
        main(sys.argv[1], sys.argv[2])
