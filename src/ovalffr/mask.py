"""Python mirror of layer/src/ovalffr_core.cpp (mask maths + savings estimate).

Used for the live preview in the UI. tests/test_mask_parity.py compares it with the
C++ output, so the picture the user sees matches what the layer builds.
"""
import math

RATE_1X1, RATE_2X2, RATE_4X4 = 0, 5, 10   # D3D12 encoding: (log2 x << 2) | log2 y


def make_mask(cfg, width_px, height_px, tile, cx=0.5, cy=0.5, max_log2=2):
    c = cfg.clamped()
    tile = int(tile)
    if tile < 1:
        tile = 8                                   # same as the C++ core
    width_px, height_px = max(0, int(width_px)), max(0, int(height_px))
    if not math.isfinite(cx):
        cx = 0.5
    if not math.isfinite(cy):
        cy = 0.5
    tw = (width_px + tile - 1) // tile
    th = (height_px + tile - 1) // tile
    if tw * th > 16000000:                         # refuse absurd sizes instead of eating all memory
        return []
    mask = [[RATE_1X1] * tw for _ in range(th)]
    if not c.enabled or width_px <= 0 or height_px <= 0:
        return mask
    outer = min(c.outer, max(1, max_log2))
    outer_code = (outer << 2) | outer
    a, b = c.h * 0.5, c.v * 0.5
    inv = 1.0 / c.shape
    for ty in range(th):
        y = ((ty + 0.5) * tile) / height_px
        uy = abs(y - cy) / b
        for tx in range(tw):
            x = ((tx + 0.5) * tile) / width_px
            ux = abs(x - cx) / a
            s = (ux ** c.shape + uy ** c.shape) ** inv
            if s <= 1.0:
                code = RATE_1X1
            elif s <= 1.0 + c.band:
                code = RATE_2X2
            else:
                code = outer_code
            mask[ty][tx] = code
    return mask


def shading_cost(mask):
    total = count = 0
    for row in mask:
        for code in row:
            lx, ly = (code >> 2) & 3, code & 3
            total += 1.0 / ((1 << lx) * (1 << ly))
            count += 1
    return total / count if count else 1.0


def estimated_saving(mask):
    return 1.0 - shading_cost(mask)


def centre_from_fov(left, right, up, down):
    try:
        tl, tr, tu, td = math.tan(left), math.tan(right), math.tan(up), math.tan(down)
    except (ValueError, TypeError):
        return 0.5, 0.5
    if not all(math.isfinite(t) for t in (tl, tr, tu, td)):
        return 0.5, 0.5
    w, h = tr - tl, tu - td
    if w > 1e-4 and h > 1e-4:
        return min(0.8, max(0.2, -tl / w)), min(0.8, max(0.2, tu / h))
    return 0.5, 0.5
