"""[DOS2] The touch sheet / inventory art generated for this port (owner 23.11: "a real art pass ... with access to generation"): the raw
Qwen Image 3 outputs -> assets/touchui/gen_*.png, cut / keyed / tiled by code. No game or iPad file is an input (the icon sheets were guided by
a contact sheet of our own assets/touchui icons). The raw generations and their prompts are kept outside the repository (the setup does not
need them): work/uimodes/sheetart/gen/ (damask_b.png, tray_b.png, icons_a.png, icons_b.png + p_*.txt).

  gen_damask.png   512 x 512, a seamless dark wine damask tile (edges cross-faded 24 px, x 1.45 brighter): the window body (repeating fill)
  gen_panel.png    1024 x 234, a carved dark iron frame round a plain dark inside (magenta keyed to alpha): 9 pieces, the HP box, the tab tray,
                   the stat strip, the filter row, the doll column's rim
  gen_ico_*.png    112 x 112 glyphs (green keyed): all (infinity), sort (up / down), sortby (bars + arrow), weight (kettlebell)

  python -I tools/dos2_sheet_art.py <dir of the raw generations> assets/touchui
"""
import sys
import numpy as np
from PIL import Image
gen, out = sys.argv[1], sys.argv[2]
# ---- damask: 1024 -> 512 (256 stage px at PX 0.5), seams blended: the last 24 px of each edge cross-faded with the opposite edge
d = np.asarray(Image.open(gen + '/damask_b.png').convert('RGB')).astype(np.float32)
d = np.asarray(Image.fromarray(d.astype(np.uint8)).resize((512, 512), Image.LANCZOS)).astype(np.float32) * 1.45   # a richer red (the iPad header)
B = 24
for axis in (0, 1):
    a = np.moveaxis(d, axis, 0).copy()
    for i in range(B):
        w = 0.5 * (1 - i / B)            # at the very edge: half / half
        top, bot = a[i].copy(), a[-1 - i].copy()
        a[i] = top * (1 - w) + bot * w; a[-1 - i] = bot * (1 - w) + top * w
    d = np.moveaxis(a, 0, axis)
rgba = np.concatenate([np.clip(d, 0, 255).astype(np.uint8), np.full((512, 512, 1), 255, np.uint8)], -1)
Image.fromarray(rgba, 'RGBA').save(out + '/gen_damask.png')
print('damask seams', abs(d[:, 0] - d[:, -1]).mean(), abs(d[0] - d[-1]).mean())
# ---- the panel: magenta -> alpha (soft), crop, 2048 x 512 source
p = np.asarray(Image.open(gen + '/tray_b.png').convert('RGB')).astype(np.float32)
r, g, b = p[..., 0], p[..., 1], p[..., 2]
mag = np.clip(((r + b) / 2 - g - 60) / 80.0, 0, 1)        # 1 = magenta
alpha = 1 - mag
ys, xs = np.where(alpha > 0.5)
y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
print('panel box', x0, y0, x1, y1)
# de-spill: where partly magenta, take the colour toward the panel's dark metal
col = p.copy()
spill = (mag > 0)[..., None]
col = np.where(spill, np.minimum(col, np.stack([g + 40, g + 30, g + 30], -1)), col)
img = np.concatenate([np.clip(col, 0, 255), (alpha * 255)[..., None]], -1).astype(np.uint8)[y0:y1, x0:x1]
im = Image.fromarray(img, 'RGBA')
W = 1024; H = int(round(im.size[1] * W / im.size[0]))
im = im.resize((W, H), Image.LANCZOS); im.save(out + '/gen_panel.png'); print('panel', im.size)

def cut(src, q, name):
    im = np.asarray(Image.open(src).convert('RGB')).astype(np.float32)
    h, w, _ = im.shape; r0, c0 = q; part = im[r0 * h // 2:(r0 + 1) * h // 2, c0 * w // 2:(c0 + 1) * w // 2]
    r, g, b = part[..., 0], part[..., 1], part[..., 2]
    green = np.clip((g - np.maximum(r, b) - 30) / 60.0, 0, 1)
    a = 1 - green
    col = part.copy(); spill = green > 0
    col[..., 1] = np.where(spill | (g > np.maximum(r, b)), np.minimum(g, np.maximum(r, b) + 6), g)   # de-spill the green fringe
    ys, xs = np.where(a > 0.5); y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    rgba = np.concatenate([np.clip(col, 0, 255), (a * 255)[..., None]], -1).astype(np.uint8)[y0:y1, x0:x1]
    img = Image.fromarray(rgba, 'RGBA'); s = 104.0 / max(img.size)
    img = img.resize((max(1, int(round(img.size[0] * s))), max(1, int(round(img.size[1] * s)))), Image.LANCZOS)
    sheet = Image.new('RGBA', (112, 112), (0, 0, 0, 0)); sheet.alpha_composite(img, ((112 - img.size[0]) // 2, (112 - img.size[1]) // 2))
    sheet.save('%s/%s.png' % (out, name)); print(name, img.size)
cut(gen + '/icons_a.png', (0, 0), 'gen_ico_all')
cut(gen + '/icons_a.png', (0, 1), 'gen_ico_sort')
cut(gen + '/icons_a.png', (1, 0), 'gen_ico_sortby')
cut(gen + '/icons_b.png', (1, 1), 'gen_ico_weight')
