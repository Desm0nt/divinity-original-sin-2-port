"""[DOS2] dmTouch: screen verbs - what a gesture MEANS on an element of a screen, the same in the hybrid and in touch (docs/dos2/TOUCH_UNIFIED.md
3.3 / 3.6, the iPad's table). A movie map (tools/dos2_touchmap/<movie>.py) gives every element a role of its screen and an action for every
meaning of that role; tools/dos2_touchkit.py refuses a map that leaves a meaning out, so the two modes cannot drift apart unseen.

Gestures (the host's one recogniser, hle_dos2_touch_kit.h): tap, dtap (double tap), slong (short long press 350 ms), long (long press 600 ms),
hold (a long press held: down at 600 ms, up at the release), scroll (one-finger drag on a scroll element). A gesture left out of a role means
nothing there (no event action; the host may still send the native mouse when the element is native)."""

SCREEN = {}

# the right side / the button column (our type 0x8d: dmSide_p in touch, dmColumn_d in the hybrid; step M2 pilot)
SCREEN['side'] = {
    'panel':  {'tap': 'open'},      # a panel button (character, inventory, skills, crafting, journal, equipment)
    'map':    {'tap': 'open'},      # the map
    'menu':   {'tap': 'open'},      # options / the pause menu
    'toggle': {'tap': 'toggle'},    # the helmet / the fold arrows: the column shown / folded inside the movie
}

# the rest of section 3.6 - written here so the next steps (M3..M7) only add movie maps
SCREEN['inventory'] = {
    'cell':  {'tap': 'select', 'dtap': 'use', 'slong': 'tip', 'long': 'menu'},
    'tab':   {'tap': 'tab'},
    'close': {'tap': 'close'},
}
SCREEN['list'] = {   # menus, saves, waypoints, dialog answers
    'row':    {'tap': 'select', 'dtap': 'act', 'slong': 'tip'},
    'button': {'tap': 'act'},
}
SCREEN['hold'] = {   # tutorial "hold A", end turn "hold B", "hold A" of trade / reward
    'hold': {'hold': 'hold'},
}
