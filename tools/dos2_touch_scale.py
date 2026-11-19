"""[DOS2] UI mode TOUCH, the physical scale for fingers (docs/dos2/TOUCH_MODES_PLAN.md "Implemented: physical scale for fingers"): the host's
factors for representative devices, and pictures of the whole touch UI at those factors and screen shapes.

The numbers are the HOST's own: dm/host/tests/dos2_touch_scale_test.c (the rule dm/host/hle_dos2_touch_scale.h) is compiled with the host's
clang and run with --table; nothing here re-derives them. The pictures are computed layouts (the movies' own art where the patched code puts
it, each HUD object scaled about its anchor as the engine's fixed layout does; panels at the host's cell and bottom limit with the hot bar back
at factor 1, as the host does while a panel is open). They are pictures of the NUMBERS: no Flash player ran, nothing ran on a phone.

  python -I tools/dos2_touch_scale.py table
  python -I tools/dos2_touch_scale.py render <Switch Game.pak | dir of the PC movies> <out dir>
        per device: <dev>_world.png (the HUD in the world), _inv / _sheet / _equip / _skills / _craft / _journal / _trade / _container /
        _reward / _splitter / _dialog (the panels), <dev>_sheet.png (all of it, a contact sheet), table.json / table.txt
Environment: DOS2_CC = the C compiler (default C:/Program Files/LLVM/bin/clang.exe, then clang / cc on PATH).
"""
import json, os, shutil, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
ROOT = Path(__file__).resolve().parent.parent


def table():
    """[dict per device] from the host test's --table"""
    cc = os.environ.get('DOS2_CC') or next((c for c in ('C:/Program Files/LLVM/bin/clang.exe', shutil.which('clang') or '', shutil.which('cc') or '') if c and os.path.exists(c)), None)
    if not cc: sys.exit('no C compiler (set DOS2_CC)')
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, 'ts.exe')
        subprocess.check_call([cc, '-I', str(ROOT / 'dm' / 'host'), str(ROOT / 'dm' / 'host' / 'tests' / 'dos2_touch_scale_test.c'), '-o', exe] + ([] if os.name == 'nt' else ['-lm']))
        out = subprocess.check_output([exe, '--table']).decode()
    return [json.loads(l) for l in out.splitlines() if l.startswith('{')]


def text(rows):
    head = '%-24s %6s %6s %5s %5s %5s %5s %5s %5s %5s %5s %4s %5s %5s %5s' % ('device', 'mm', 'aspect', 'want', 'hud', 'mmap', 'side', 'pi', 'hbmm', 'cellw', 'cell', 'rows', 'cellm', 'menu', 'dialog')
    lines = [head]
    for r in rows:
        lines.append('%-24s %6.1f %6.3f %5.2f %5.2f %5.2f %5.2f %5.2f %5.1f %5d %5d %4d %5.1f %5.2f %5.2f' % (r['device'], r['mm'], r['aspect'], r['want'], r['hud'], r['mmap'], r['side'], r['pi'],
                     r['hb_mm'], r['cell_want'], r['cell'], r['rows'], r['cell_mm'], r['menu'], r['dialog']))
    return '\n'.join(lines)


def render(src_path, out):
    from PIL import Image, ImageDraw
    import dos2_pchud_touch as hud, dos2_ppanel_inv as ppi, dos2_ppanel_skills as pps, dos2_ppanel_trade as ppt
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    rows = table(); src = hud.sources(src_path); made = hud.build(src)
    (out / 'table.json').write_text(json.dumps(rows, indent=1)); (out / 'table.txt').write_text(text(rows) + '\n'); print(text(rows))
    def strips(img, sw, note):
        ov = Image.new('RGBA', img.size, (0, 0, 0, 0)); d = ImageDraw.Draw(ov)
        d.rectangle([0, 0, 79, 1079], fill=(200, 40, 40, 60)); d.rectangle([sw - 80, 0, sw, 1079], fill=(200, 40, 40, 60))   # the notch strips
        img.alpha_composite(ov); d = ImageDraw.Draw(img); d.rectangle([90, 1050, 90 + 9 * len(note), 1076], fill=(0, 0, 0, 200)); d.text((96, 1056), note, fill=(250, 240, 200, 255))
        return img
    def centred(base, stage, s, bottom=False):
        im = stage.resize((max(1, int(stage.size[0] * s)), max(1, int(stage.size[1] * s))), Image.LANCZOS); W = base.size[0]
        x = int(round((W - im.size[0]) / 2.0)); y = int(round(1080 - im.size[1])) if bottom else int(round((1080 - im.size[1]) / 2.0))
        c = base.copy(); c.alpha_composite(im, (x, y)) if x >= 0 and y >= 0 else c.paste(im, (x, y), im); return c
    for r in rows:
        sw = int(round(r['sw'])); tag = r['device'].split(' ')[0].replace('.', '_') + '_' + r['device'].split(' ')[1].replace(':', 'x')
        F = dict(hb=r['hud'], mm=r['mmap'], side=r['side'], pi=r['pi'])
        pics = []
        world = hud.mock(src, None, sw, F, made)
        pics.append(('world', strips(world, sw, '%s: HUD x%.2f (hot bar cell %.1f mm), minimap x%.2f, column x%.2f, portraits x%.2f' % (r['device'], r['hud'], r['hb_mm'], r['mmap'], r['side'], r['pi']))))
        base = hud.mock(src, None, sw, dict(F, hb=1.0, aim=False), made)   # a panel is open: the hot bar is back at factor 1
        bp = out / ('_base_%s.png' % tag); base.save(bp)
        for view in ('inv', 'sheet', 'equip'):
            img, note = ppi.picture(src, view, r['cell'], sw, True, r['bottom'], base)
            pics.append((view, strips(img, sw, '%s: %s' % (r['device'], note))))
        tmp = out / ('_tmp_%s.png' % tag)
        pps.mock_skills(src, str(tmp), sw, str(bp)); pics.append(('skills', strips(Image.open(tmp).convert('RGBA'), sw, '%s: skills_p (scale 1)' % r['device'])))
        pps.mock_craft(src, str(tmp), sw, str(bp)); pics.append(('craft', strips(Image.open(str(tmp).replace('.png', '_experiment.png')).convert('RGBA'), sw, '%s: uiCraft_p (scale 1)' % r['device'])))
        pps.mock_journal(src, str(tmp), sw, str(bp)); pics.append(('journal', strips(Image.open(str(tmp).replace('.png', '_quests.png')).convert('RGBA'), sw, '%s: journal_p (scale 1)' % r['device'])))
        for name, movie, s, bot in (('trade', 'trade_p.swf', 1.0, False), ('container', 'containerInventory_p.swf', 1.0, False), ('reward', 'reward_p.swf', r['reward'], False),
                                    ('splitter', 'itemSplitter_p.swf', r['splitter'], False), ('dialog', 'dialog_p.swf', r['dialog'], True)):
            st = ppt.render_one({'container': 'containerInventory', 'splitter': 'itemSplitter'}.get(name, name), made[movie], None, None, stage_only=True)
            pics.append((name, strips(centred(base, st, s, bot), sw, '%s: %s x%.2f' % (r['device'], movie, s))))
        for p in out.glob('_tmp_%s*.png' % tag): p.unlink()
        bp.unlink()
        for name, img in pics: img.convert('RGB').save(out / ('%s_%s.png' % (tag, name)))
        k = 0.25; tw, th = int(sw * k), int(1080 * k); cols = 4; n = len(pics)
        sheet = Image.new('RGB', (cols * (tw + 8) + 8, ((n + cols - 1) // cols) * (th + 8) + 40), (16, 18, 16))
        d = ImageDraw.Draw(sheet)
        d.text((8, 8), '%s: %.1f mm per 1080 px, aspect %.3f | HUD x%.2f (hot bar cell %.1f mm), minimap x%.2f, column x%.2f, portraits x%.2f | panel cell %d (%.1f mm), %d rows above y %d | dialog x%.2f reward x%.2f splitter x%.2f menu x%.2f tooltip x%.2f' % (
            r['device'], r['mm'], r['aspect'], r['hud'], r['hb_mm'], r['mmap'], r['side'], r['pi'], r['cell'], r['cell_mm'], r['rows'], r['bottom'], r['dialog'], r['reward'], r['splitter'], r['menu'], r['tooltip']), fill=(240, 230, 200))
        for i, (name, img) in enumerate(pics):
            sheet.paste(img.convert('RGB').resize((tw, th), Image.LANCZOS), (8 + (i % cols) * (tw + 8), 32 + (i // cols) * (th + 8)))
        p = out / ('%s_all.png' % tag); sheet.save(p); print('%s: %d pictures' % (p, n))


def main():
    a = sys.argv[1:]
    if a == ['table']: print(text(table())); return
    if len(a) == 3 and a[0] == 'render': render(a[1], a[2]); return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
