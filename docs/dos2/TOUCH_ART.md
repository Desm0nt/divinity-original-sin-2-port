# DOS2: the touch UI's own art (16.11.2026, branch `dos2-touchart`)

Owner 16.11: TouchUI.pak is built from the player's Switch dump alone; the release (repository and APK) carries no Larian art - neither the
iPad build's graphics nor the Switch's, and no game movie. Every iPad graphic the touch passes used is replaced:
- **png** - our own drawing (Codex image generation, 16.11; picked from 3 variants each, then a palette grade toward the original's colour
  statistics: mean / spread per channel of the opaque pixels, no pixel taken), `assets/touchui/<name>.png`, 2x the on-screen size;
- **sw** - a symbol of the player's own Switch `Game.pak`, grafted at setup time, ONLY where the iPad's element IS that drawing (proof below);
- **vec** - geometry drawn in code (`tools/dos2_touchart.py`).
Rules for the art: indistinguishable at a glance (size, layout, palette, frame style, line weight), meaning clear at once, not a copy (drawn
from scratch; no tracing, pasting or recolouring of the originals). Code: `tools/dos2_touchart.py` (PNG reader / writer without Pillow,
DefineBitsLossless2 + bitmap-filled DefineShape, polygon shapes, a `$Title` text field), used by `dos2_hud_touch.py`, `dos2_column_touch.py`,
`dos2_panel_touch.py`. No pass reads an iPad file any more; `setup` ignores an `.ipa`. `tools/dos2_swf_render.py` renders SWF characters
(frame 1) to PNG for comparison sheets.

## Inventory (what TouchUI.pak took from the iPad files, pack 4c / GOOD-last)
| element | movie / instance | iPad source (movie: symbol, size) | now | art |
|---|---|---|---|---|
| options | bottomBar_d `dmVisOpt_mc` | rightSideButtons_t `btnOptions` 74x74 | png | `plate_round_copper` + `icon_menu` |
| tactical camera | bottomBar_d `dmVisTac_mc` | minimap_t `tacticalCamera_mc` (plate 67 + `ico_tacticalCamera` 52x54) | png | `plate_round_copper` + `icon_tactical` |
| hot bar row arrows | bottomBar_d `dmVisPrev/Next_mc` | hotBar_t `btnSkillSetLeft/RightFrames` (vector, 88x85) | vec | `ARROW_LEFT`, flat (150,141,120) |
| cancel | bottomBar_d `dmVisCancel_mc` | statusConsole_t `cancelBtn` 69x69 (5 layers) | png | `plate_round_steel` + `glyph_cancel` |
| end turn | bottomBar_d `dmVisEnd_mc` | statusConsole_t `endTurnBtn` 207x44 + label | png | `plate_endturn` + own DefineEditText (`$Title`, "End turn") |
| sneak / weapon plates | bottomBar_d `dmVisSneak/Destr_mc` | cancelBtn plate (4 layers) | png | `plate_round_steel` |
| sneak / weapon icons | same | iPad none (4b: Switch atlas `Action_Sneak` / `Action_Sheath`; 4a: iPad possessIco / combatIcon) | png | `icon_sneak`, `icon_sheath` (the iPad has no such button; the atlas is not used) |
| (unused copies) | bottomBar_d | rightSideButtons_t `cmnTabButtonBg_9`, `btnSidePanelBg_6` copied but never placed | - | dropped |
| character | dmColumn_d `dmVis1_mc` | rightSideButtons_t `btnSidePanelBg_6` 105x105 | png | `btn_character` |
| column plate | dmColumn_d `dmVis0/2..5_mc`, map | rightSideButtons_t `cmnTabButtonBg_9` 74x74 | png | `plate_square_copper` |
| column icons | dmColumn_d | rightSideButtons_t `ico_charsheet/inventory/skills/craft/journal/map` 59x54 | png | `ico_*`, `icon_map` |
| fold arrows | dmColumn_d `dmVisFoldR/L_mc` | hotBar_t arrows (vector) | vec | `ARROW_LEFT` x0.58 |
| close | panels `dmPanClose_mc` | uiElements_t `mcBtnCloseTouch` 50x50 | png | `pan_close` |
| tab prev / next | panels `dmPanPrev/Next_mc` | characterSheet_t `selectorBtnLeft/Right` 32x39 | **sw** | Switch characterSheet.swf `selectorBtnLeft/Right` |
| helmet | equipmentPanel_d | equipmentPanel_t `SwitchHelmetVisibilityButton` 71x71 | png | `pan_helmet` |
| item menu | panels `dmPanList_mc` | partyInventory_t `sortButton` 64x64 | png | `pan_list` (an actions-list icon: the button opens the item menu) |
| info | panels `dmPanInfo_mc` | uiCraft_t `searchBtn` | **sw** | Switch uiCraft.swf `searchBtn` |
| multi | panels `dmPanMulti_mc` | skills_t `filterAllBtn` 67x67 | png | `pan_multi` (stacked cards + check) |
| plus / minus | statsPanel_d | characterSheet_t `IncreaseAttriburePlusButton` / uiCraft_t `minusBtn` 34x34 | png | `pan_plus`, `pan_minus` |
| ok | panels `dmPanOk_mc` | partyInventory_t `okBtn` 78x30 | **sw** | Switch partyInventory.swf `okBtn` |
| balance | trade_d | trade_t `balanceBtn3` 64x83 | png | `pan_balance` (Switch trade.swf `balanceBtn_27` is a different drawing: 57x90) |
minimap_d took the iPad tactical camera only with `--mm-tac` (pack 4b); that path is gone from the builder.

## Proof for the kept Switch symbols (content, not bytes)
Rendered both (tools/dos2_swf_render.py), scaled to the same height as the panel pass does (128 px), over the same background:
| element | iPad | Switch | mean abs diff | SSIM | bitmaps |
|---|---|---|---|---|---|
| tab prev | characterSheet_t selectorBtnLeft 32x39 | characterSheet.swf selectorBtnLeft 32x39 | 0.00 / 255 | 1.0000 | 3 of 3 pixel-equal |
| tab next | characterSheet_t selectorBtnRight | characterSheet.swf selectorBtnRight | 0.00 | 1.0000 | 3 of 3 pixel-equal |
| ok | partyInventory_t okBtn 78x32 | partyInventory.swf okBtn 78x32 | 0.00 | 1.0000 | 4 of 4 pixel-equal |
| info | uiCraft_t searchBtn | uiCraft.swf searchBtn | 8.44 | 0.878 | 3 of 3 pixel-equal (frame 40x28 x2 + glass 16x16); the iPad's shape draws the glass ~20 % larger |
Close / plus / minus / balance / column icons have Switch look-alikes (uiElements ButtonCrossIconmc 43x43, uiCraft plusBtn 19x19,
trade balanceBtn_27, panelSelect_c iconDeselected 80x80 frames) but no equal bitmap and other sizes: drawn by us. The sneak / sheathe atlas of the
iPad build is not in the IPA's local data (on-demand resources), and the iPad has no such buttons: drawn by us.

## Checks (16.11, pack `packs/5a/TouchUI.pak`, built with no iPad path)
- `dos2_hud_touch.py check`, `dos2_column_touch.py check`, `dos2_panel_touch.py check`: all pass (geometry as before: options 1480,20..1554,94,
  tactical under it, cancel / sneak / weapon centres on y 963, 81.4 px; the column's right side on stage x 308; panel rows 64 px high).
- `dos2_touchui_pak.py hints`: 24 movies = GOOD-last's 24, self-check "every movie of the host tables k_def + k_hint is in the package".
- ExternalInterface / invoke names per movie (dmPadBtn, dmPanel, dmColumnPoll, dmHudRects, addWatermarkText, setActiveInCombat ...): the same set as
  GOOD-last in all 24 movies.
- Bitmap scan: GOOD-last holds 103 iPad-only bitmaps, 5a none; 36 bitmaps of 5a are in neither edition (ours).
- `dos2_touchui_check.py main.raw packs/5a/TouchUI.pak`: all sites match; DM_DOS2_TOUCHUI_SET=hints served.
Not run on a phone.
