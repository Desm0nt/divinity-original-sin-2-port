# Touch interface art

Original art made for this port (drawn new in the style of the game's interface; no file of the game or of any other edition
of it was copied, traced or recoloured). It is drawn into the console interface movies by `tools/dos2_hud_touch.py`,
`tools/dos2_column_touch.py` and `tools/dos2_panel_touch.py` (through `tools/dos2_touchart.py`) when setup builds
`TouchUI.pak` from your own Switch files. Sizes are 2x the on-screen size; `python tools/dos2_touchart.py check` lists them.
Inventory and choices: `docs/dos2/TOUCH_ART.md`.

Touch HUD, 23.11.2026 (`tools/dos2_pchud_touch.py`): `icon_chat`, `icon_combatlog` (the hot bar's chat / combat log plates), `icon_save`
(the minimap's quick save), `icon_view`, `icon_labels`, `icon_cones`, `icon_camlock`, `icon_info` (the minimap's view menu) - generated
new with Qwen Image 3 (one 3x3 sheet of pale silver glyphs on flat green, styled after our own `ico_*` column icons), keyed and cut by
code, 112 px; `icon_tactical_s` = our `icon_tactical` recoloured to the same silver. No game or iPad file was used for them.

Touch character sheet / inventory, 23.11.2026 (`tools/dos2_ppanel_inv.py`): `gen_damask` (a seamless dark wine damask tile: the window
body), `gen_panel` (a carved dark iron frame round a plain dark inside, cut into 9 pieces: the HP box, the tab tray, the stat strip, the
filter row, the doll column's rim), `gen_ico_all`, `gen_ico_sort`, `gen_ico_sortby`, `gen_ico_weight` (the inventory's
filter row) - generated new with Qwen Image 3 (text prompts; the icon sheets guided by a contact sheet of our own icons above), keyed,
cut and tiled by code (`tools/dos2_sheet_art.py`; the raw outputs and prompts: `work/uimodes/sheetart/gen/`, not needed by the setup). No game
or iPad file was used for them.
