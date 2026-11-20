"""[DOS2] dmTouch map of dmSide_p.swf (UI mode TOUCH, type 0x8d: tools/dos2_pchud_touch.py `side`) - the M2 pilot's touch half.
Elements = the movie's own buttons (SIDE of tools/dos2_pchud_touch.py); actions = the host verbs (dmAct), the helmet = the movie's own fold
(dmBAct(20): the column shown / hidden, as its old press code does)."""

MOVIE = {
    'movie': 'dmSide_p.swf', 'screen': 'side', 'family': 'pc', 'types': [0x8d],
    'class_fn': 0xae2780,   # watermark.swf's class is a plain UIObject: OnFunctionCalled = the base main+0xae2780 (ei: names are checked on its chain)
    'elements': [   # top first: (id, instance path from the root, kind e / w / c, role of the screen, pad px, {meaning: action(s)})
        ('menu', 'dmMenu_mc', 'e', 'menu', 0, {'open': 'host:menu'}),
        ('helm', 'dmHelm_mc', 'e', 'toggle', 0, {'toggle': 'call:.dmBAct(20)'}),
        ('map', 'dmP2_mc', 'e', 'map', 0, {'open': 'host:map'}),
        ('character', 'dmP3_mc', 'e', 'panel', 0, {'open': 'host:panel(1)'}),
        ('inventory', 'dmP4_mc', 'e', 'panel', 0, {'open': 'host:panel(0)'}),
        ('skills', 'dmP5_mc', 'e', 'panel', 0, {'open': 'host:panel(2)'}),
        ('crafting', 'dmP6_mc', 'e', 'panel', 0, {'open': 'host:panel(3)'}),
        ('journal', 'dmP7_mc', 'e', 'panel', 0, {'open': 'host:panel(4)'}),
    ],
}
