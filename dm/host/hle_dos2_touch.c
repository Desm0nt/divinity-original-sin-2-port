// [DOS2] Touch as mouse, the iPad way (DM_DOS2_TOUCH, default 0 until checked on the phone; docs/dos2/DOS2_PORT.md "Касание как мышь",
// docs/dos2/IPA_RECON.md 2.x). On the iPad build (3.6.72) LSMTLView turns touches into sendLeftMouseDown / sendXYMotion ... which call
// ls::InputManager::InjectInput (iPad 0x1008872ac) with a 20-byte record; the Switch main has the same function, used by its pad poll.
// main = main.raw of update v786432, build 4979b200, offsets = main.raw:
//   0x847690  InjectInput(mgr, const rec *r, int): copies the record into the manager's ObjectSet (+0x278 array, +0x280 size/cap,
//             stride 0x14), no lock; the frame's input update reads the set later. Record (all little endian):
//               +0x00 u32 raw = device << 16 | id     +0x04 float value[2]     +0x0c u8 pressed     +0x0d..0x0f copied
//               +0x10 u8 (InjectInput writes 0 on the Switch; the iPad copies its third argument there: 1 for mouse motion)
//   0x8a7530  the NX pad poll (x0 = NX input object, vtable main+0x29764e0); it takes mgr from the global main+0x2bb8960 (0x8a77dc) and
//             calls InjectInput for the buttons (0x8a8478 press: value {1,1}, pressed 1; 0x8a84b8 release: value 0, pressed 0; id from the
//             table main+0x1f58f4c, device = u16 [obj+0xe0]), the triggers (0x8a8640 / 0x8a8694) and the sticks (0xbdf4e0 -> 0xbdf65c ...).
//   raw ids: the name table at main+0x2974b10 (pointers, relocated) gives id = index - 2 (checked against the pad table: a = 0x8c,
//             leftstick_xneg = 0x82 as the stick code passes). Mouse (the iPad passes device 1 = mouse with the same ids: motion 0x79):
//               left 0x74  middle 0x75  right 0x76  x1 0x77  x2 0x78  motion 0x79  motion_xneg/ypos/xpos/yneg 0x7a..0x7d
//               wheel_xpos 0x7e  wheel_xneg 0x7f  wheel_ypos 0x80  wheel_yneg 0x81    touch_tap 0x9b  touch_hold 0x9c
//               touch_pinch_in 0x9d  touch_pinch_out 0x9e  touch_rotate 0x9f  touch_flick 0xa0  touch_press 0xa1
//   Bindings the Switch loads: PlayerProfiles/Default/inputconfig.json (0xa68d14, always): FlashLeftMouse / Action1 <- mouse:left,
//   FlashRightMouse / ContextMenu <- mouse:right, FlashMouseMove <- mouse:motion, FlashScrollUp/Down <- wheel_ypos/yneg; and
//   Eoc/controller_1.json: CameraZoomIn/Out <- c:nx_touch_pinch_in/out (+ right stick). Eoc/keyboard_1.json (wheel zoom, mouse camera) is
//   NOT loaded on the Switch (its path is not in main), so pinch = the controller's pinch raw ids on the pad's device, not the wheel.
//   The UI cursor (found 14.11, pass 2): the iPad stores the position apart from the record (sendXYMotion -> 0x1003c42a8:
//   *(float2 *)[0x101ecf428] = position in window pixels, then InjectInput(motion, {1,1}, 1)). The Switch twin is the pointer global
//   main+0x2bb88e8: the NX input init 0x8a8e94..0x8a8eb4 allocates a 12-byte cursor {float2 pos = 0, u8 +8 = 1} and stores it there (its
//   struct: +8/+0x10 0, +0x30 = 1); 33 readers, no writer at all on NX (the OS layer that writes it on PC / iPad is not in the build), so it
//   stays at 0,0. Readers that matter: the Flash event dispatch 0xd23800 (a UI object's vtable slot, main+0x29c3bd8): event 0x58 =
//   FlashMouseMove -> player vt+0x190 (Iggy mouse position) with the cursor, events 1/2/3 = FlashLeft/Right/MiddleMouse -> 0xd27700 (Iggy
//   button down/up, reads the cursor at 0xd27714), 0x59.. arrow / enter / page keys, 0x67/0x68 FlashScrollUp/Down -> 0xd27680; the UI
//   manager (global main+0x2bbb230) 0xae83e0 (drag of a UI element to cursor + offset), 0xaee2c0 (drag start), 0xae3600 (activate: mouse
//   position pushed to the movie). So DM_DOS2_TOUCH writes the position into [main+0x2bb88e8] before each motion / button (the iPad way)
//   and sends motion with {1,1}; DM_DOS2_TOUCH_CURSOR=<offset> overrides the float2 address, =0 turns the write off.
//   Controller mode (DM_DOS2_TOUCH_UI): the movies get "isControllerMode" (0x1209224, a Flash bool) = (the player's input record +0x2c0 == 1);
//   +0x2c0 is zeroed by the record's constructor (0xe616e4) and set only by SetInputDevice 0x12a04a0 (0x12a04b8 str w1,[x0,#0x2c0], tail-called
//   from 0x129f400, the "input device changed" handler). DM_DOS2_TOUCH_UI=1 patches that store to str wzr: the device stays 0 (not
//   controller), so movies are told isControllerMode = false. Hypothesis: on PC 0 = keyboard and mouse (the UI type table 0x10a9670 also
//   registers both msgBox.swf (type 0x10) and msgBox_c.swf (0x12), the choice is made where the UI is created); not checked on the phone.
// The hook: a 16-byte jump over the poll's prologue (4 instructions, none PC-relative) + trampoline, as hle_dos2_native.c; after the
// original poll returns (same thread, so no race with the pad's own unlocked InjectInput) the queue is drained into InjectInput.
// Tap crash (phone, pack 2a, 14.11): SIGSEGV addr 0x8 at main+0x84586c on the first mouse record. ProcessInputEvent 0x845350 (mgr x0, record;
// called from the manager's update 0xb06a40 for the +0x278 queue, 0xb07e94) looks the record's device up in the bindings hash (+0x88/+0x90;
// device 1 = mouse IS there: Default/inputconfig.json binds mouse:left / motion), walks its bindings, and when one matched stores the raw state
// {float2 value, u8 pressed} at [[mgr+0x18] + device*8] + id*12 (0x84585c..0x84588c; a null per-device entry is skipped by cbz, a null ARRAY is
// not). [mgr+0x18] is the per-device raw-state array: on PC the keyboard / mouse devices fill it; the NX build creates none, so it is null
// (the pad never reaches that store: its device is not in that hash). The array is used only there and at 0x8453a8 (keyboard modifiers,
// ids 0x6b..0x72) - the only two "device*8" lookups (sbfx #13,#19) in main. Fix: before the first record, if [mgr+0x18] is null the host
// gives it a zeroed 64-pointer array and the mouse device a 256 x 12-byte state block (entries for other devices stay null = skipped);
// DM_DOS2_TOUCH_STATE=0 does not allocate and instead turns touch off with one red line (also when the site words differ).
// Gestures (state machine on the game thread, fed by the front end's touches outside the pad's controls, normalized 0..1 to the view):
//   one finger: cursor follows; quick tap = left down + up at the spot; drag past the slop = left down, move, left up (drag items, select);
//   hold still DM_DOS2_TOUCH_HOLD_MS (500) = right click (context menu); two fingers: pinch = camera zoom (pinch in / out pulses on the
//   pad's device), two-finger vertical drag = wheel (UI scroll lists).
// Switches:
//   DM_DOS2_TOUCH=1           on (the gear "Касание как мышь (эксперимент)"); 0 / unset = no hook, the front end keeps touches to the pad
//   DM_DOS2_TOUCH_LOG=1       log the first 64 distinct raw values InjectInput sees (device / id of the pad, our mouse records): phone step 1
//   DM_DOS2_TOUCH_DEV=<n>     mouse device (default 1, as the iPad)
//   DM_DOS2_TOUCH_MOTION=one  motion value {1,1} as the iPad (default) | abs = window pixel position | delta = pixel delta
//   DM_DOS2_TOUCH_CURSOR=<hex> main offset of a float2 to write the position to instead of the cursor [main+0x2bb88e8]; 0 = no write
//   DM_DOS2_TOUCH_STATE=0     do not give the input manager its per-device state array: touch turns itself off (red line) instead
//   DM_DOS2_TOUCH_UI=1        SetInputDevice main+0x12a04b8 stores 0 (superseded by DM_DOS2_TOUCH_PCUI, see pass 3: not the mode; kept for A/B only)
//   DM_DOS2_TOUCH_WIN=<W>x<H> window size for the mapping (default: the size hle_dos2_wide.c set, else 1600x900)
//   DM_DOS2_TOUCH_HOLD_MS=<ms> long press for the right click (default 500)
// Pass 3 (14.11, branch dos2-touch2; docs/dos2/DOS2_PORT.md "Касание: режим UI и фильтр устройств"). The phone showed "touch does nothing or
// works wrong" with the records above. Static findings:
//   The controller mode is ONE global byte, main+0x2bb8918 (the NX input init stores 1 there: 0x8a8eb8 strb w19,[x8,#0x30], x8 = main+0x2bb88e8,
//   right after the cursor pointer; 670 ldrb readers, no other writer: the PC code that flips it (ToggleInputMode in keyboard_1.json) is not in
//   the build). 1 = controller UI, 0 = keyboard and mouse UI. The per-player UI set is made by 0x10c06d0: it reads the byte (0x10c07ac), stores
//   it in the player's record +0x2b0 (hash node +0x2c0, what isControllerMode 0x1209208 compares with 1) and with 1 creates the `_c` types
//   (bottomBar_c 0x3b, contextMenu_c 0xc, minimap_c 0x3c, combatLog_c 0x41 ...), else (0x10c0b30) the PC types (worldTooltip 0x30, overhead 5 ...). The type
//   ids are the PC ones (creator map [UI manager main+0x2bbb230]+0xd8, registered by the function around 0x10a9670: the id of a block belongs
//   to the path BEFORE it: msgBox.swf = 0x1d, msgBox_c.swf = 0x4b, hotBar.swf = 0x28); CreateUIObject = 0xaebbf0(mgr, layer, type, flags, x4,
//   player), 93 callers. So DM_DOS2_TOUCH_UI (SetInputDevice 0x12a04a0) was the wrong place: that +0x2c0 is a field of a UI object, not the mode.
//   The event filter (0x844014, the manager's dispatch to listeners; two copies, 0x84407c.. and 0x844234..): the event's device (u16) gives a
//   class: 0 / 1 (keyboard / mouse) = 0, 3..6 (pads) = 1, else -1; a listener gets the event when its flags (+4) have bit 4, or the class is 0 and
//   byte [mgr+0x170] != 0 ("controller allows keyboard and mouse"), or the class == the mode byte, or the class is -1, or its mask (+0x68) has
//   the class bit. In controller mode (the Switch) mouse records therefore reach only the listeners that ask for them: the rest of the game
//   never sees our clicks. In PC mode the pad's records are dropped the same way (as on the PC: no pad in keyboard mode).
//   Bindings: 0xc05ab0 loads Default/inputconfig.json (mouse + keys of the engine: Flash*, Action1, ContextMenu) and Eoc/controller_1.json for
//   the four players, whatever the mode; Eoc/keyboard_1.json (the PC game bindings: CameraZoomIn/Out <- wheel, ToggleInGameMenu / UICancel <-
//   escape, CameraRotateLeft/Right <- delete / end ...) is in Game.pak but its path is not in main.
//   Cursor space: FlashMouseMove -> player vt+0x190 = 0xd26a60(x, y): x -= max(player +0x10, 0), y -= max(+0x14, 0) (the movie's position in the
//   window), IggyMakeEventMouseMove (0x3ec970), dispatch 0x3ec290; the button 0xd27700 sends the cursor as it is. So the cursor is in WINDOW
//   pixels of the window the game runs now: docked (2400x1080 on the test phone) or handheld (1920x864), not a virtual 1920x1080. The old code
//   always took the docked size; now the size is read from the game's window object ([[main+0x2bbb260]+0x40][0] +0x18 / +0x1c u32, written by
//   Window::SetSize 0x93d21c; the reader 0xc0b50c) whenever a touch arrives.
// New switches (all need DM_DOS2_TOUCH=1; defaults off):
//   DM_DOS2_TOUCH_KBM=2       as 1, but only while no player UI set exists (main menu, loading); in game the byte is 0 again. What DM_DOS2_TOUCHUI sets:
//                             run tu03 crashed on a tap on a party portrait with =1 (the tooltip path, see hle_dos2_touchui.c "tooltip guard").
//                             TU-04: in game the byte is 1 again while a substituted `_t` window with modal bits is visible (pause menu, message
//                             box, save / load, options opened in game; dm_dos2_touchui_poll), 0 when none is; every open / close is logged
//   DM_DOS2_TOUCH_KBM=1       controller UI kept; byte [mgr+0x170] = 1 (rewritten each poll): mouse records pass the filter to every listener
//   DM_DOS2_TOUCH_PCUI=1      PC mode from the start: 0x8a8eb8 stores 0 in the mode byte -> PC movies (hotBar.swf ...), mouse native; the pad's
//                             records are filtered away by the game (on-screen pad dead, as a PC in keyboard mode). Also: the bindings path
//                             string main+0x1ebfc35 ".../controller_1.json" becomes ".../keyboard_1.json" (DM_DOS2_TOUCH_PCUI_KEYS=0 keeps the
//                             pad's file), pinch = mouse wheel (CameraZoomIn/Out), a third finger down = escape (ToggleInGameMenu / UICancel)
//   DM_DOS2_TOUCH_LIVE=1      pass 4 below: the UI follows the last used device (touch = PC UI, pad = controller UI), rebuilt in game; =2 menu only
//   DM_DBG=1                  (with DM_DOS2_TOUCH=1) the first 64 records with the touch position, the window and the cursor read back, and a
//                             summary line every 600 polls with activity: records per kind, cursor, pad device, mode byte, [mgr+0x170], window
// Pass 4 (14.11, branch dos2-touch3; docs/dos2/DOS2_PORT.md "Касание: живое переключение"): DM_DOS2_TOUCH_LIVE, the PC build's switch between the
// keyboard-and-mouse UI and the controller UI by the last used device, rebuilt for touch vs pad. Static findings (iPad 3.6.72 VA / Switch main.raw):
//   iPad: the mode is a u32 at 0x101ecf458 (struct 0x101ecf428 + 0x30, the cursor pointer's struct as on the Switch; 744 readers; the code writes 0
//   and 2 only, 2 = touch). Writers: 0x10088ac64(mgr, force) -> 0 and 0x10088adb8(mgr, force) -> 2: both write [mgr+0x228] = -1, the mode, call
//   0x10088a368(mgr, player 0, device 0 / 0xc) (per-player device class [mgr+0x228 + player*4]: device 0/1 -> 0, 3..6 -> 1, 0xc -> 2), reset the
//   devices of the lists mgr+0xa8.. (0x100889b70) and end in the listener invoker 0x10088aadc(mgr+0x1d0, slot 0x48, virtual): every input
//   listener's vtable slot 9 = "input mode changed". The UI's work is 0x1012f1f58(ui, now): now = 0 posts itself to a job queue (lambdas
//   0x1012fed0c / 0x1012fed7c), now = 1 does it: 0x1012dfbf8(ui, 1) destroys the mode's UI objects, then ONLY in game state 0x1c (0x13 / 7: destroy
//   only, the state's own code makes the new UI) it recreates the shared objects (types 0x63 / 0x64) and runs the per-player set builder
//   0x1012dfea4 for every player of the player manager (lambda 0x1012fedf8, flags 1), then three refreshes (0x1012eae08(ui,0), 0x1012f2874,
//   0x1012eb390(ui,0)).
//   Switch: the per-player class code, the invoker call and the handler are not in the build (no `mov w1,#0x48; mov w2,#1` pair, one writer of the
//   byte). What remains of the same class (the player UI manager, global pointer main+0x3298140):
//     0x10c4580(ui)  create all: shared 0x63 / 0x64 (0x10c2840) + 0x10c06d0(ui, id, 0x20000000) for every player ([main+0x2bc0510] hash +0x94/+0x98,
//                    lambda 0x10c4720); one caller, the game start 0x104daa0 (x0 = [main+0x3298140] at 0x104da98)
//     0x10c4730(ui)  destroy all: shared (0x10c41f0), per player 0x10bc250 + 0x10bc750(ui, id, 0) (71 UI objects each), clears the per-player hash
//                    ui+0xf0 (0x10c25e0); one caller, the game unload 0xd4006c (x0 = [main+0x3298140] at 0xd40064)
//     0x10bb080 / 0x10bafe0 (thunks 0x10bba70 / 0x10bba50): player removed / added = destroy (0x10bc250, 0x10bc630, 0x10bc750(.., 1)) / create
//                    (0x10c06d0(.., 1)) for one player: the game itself rebuilds a player's UI set in the middle of a session
//   The per-player hash ui+0xf0: +4 u32 buckets, +8 bucket array, node +0 next, +8 u16 player id; node +0x2c0 = the mode the set was built for.
// DM_DOS2_TOUCH_LIVE=1 (default 0; needs DM_DOS2_TOUCH=1): the first touch after pad use switches to the PC UI, the first pad input (a press, or a
//   stick / trigger past half) after touch use switches back; not sooner than DM_DOS2_TOUCH_LIVE_MS (1000) after the last switch, and to the pad only
//   with no finger down. The switch runs on the game thread in the pad poll hook, the iPad's order: the mode byte main+0x2bb8918, then - only when
//   the per-player hash has records (= in game, the Switch's stand-in for the iPad's "state 0x1c") - 0x10c4730 and 0x10c4580. Without records
//   (main menu, loading) only the byte changes: the UI made later reads it. The pad stays the way back: in PC mode the game filters its records
//   (0x844014), the host sees them in InjectInput during the poll and flips the mode, the next press works (as a PC: pad press = controller UI).
//   DM_DOS2_TOUCH_LIVE=2: the minimal variant, never rebuilds in game: the switch waits until the per-player hash is empty (menu / loading).
//   The bindings file stays Eoc/controller_1.json (no keyboard_1.json in live mode: the pad must keep its bindings), so in PC mode pinch = wheel
//   scrolls lists (FlashScrollUp/Down) but there is no wheel camera zoom; a pad input brings the controller UI and its camera back.
//   Not done: the iPad's three refreshes after the rebuild (Switch twins not matched), the per-player device class, UI outside this manager (main
//   menu, message boxes already on screen keep the look they were made with).
// Pass 5 (14.11, branch dos2-touch4; docs/dos2/DOS2_PORT.md "Касание: левый клик, движение, щипок"). Phone, pack 2l: records arrive, the cursor reads
// back right, yet left clicks do nothing (controller UI + KBM and PC UI alike), the long-press right click works, pinch does nothing. Static findings:
//   The Flash route. UIManager::OnInputEvent 0xae83e0 (mgr = [main+0x2bbb230]) walks the UI objects top down and calls 0xaea7b0(mgr, object, event,
//   pass, ..) twice; that one needs object flags (+0x14) bit 1, the per-player input bit (5 + player, event +4), and for events 1..3 (FlashLeft /
//   Right / MiddleMouse) the byte [mgr+0x10b] != 0 (0xaea8ac; the constructor 0xae7430 stores 1, an input listener 0xc03ab0 toggles it on event
//   0xd6), then UIObject vt+0xa8 (0xae38b0) -> FlashPlayer vt+0x30 0xd23420 -> vt+0x38 0xd23800(event, 0): FlashMouseMove (0x58, pressed) -> vt+0x190
//   0xd26a60 = IggyMakeEventMouseMove(cursor - movie origin), 1..3 -> 0xd27700 = Iggy button down / up with the cursor. The other call of
//   0xd23800 (0xaea844, flag 1 = an upper object took the event) skips the motion: that is the `w2 & 1` bit. So the hypothesis "the Flash event for
//   motion is dropped in 0xd23800" is rejected; what breaks is earlier, in the input manager:
//   1. The raw-state array is an engine Array at mgr+0x18 {buffer, +0x20 capacity, +0x24 size}. ProcessInputEvent 0x845350 compares a record with
//      the STORED state of the binding's inputs (0x84566c: device < [mgr+0x24], then [[mgr+0x18] + device*8] != null, else "stored = 0";
//      0x845740 fcmp stored, new: equal = no event; stored pressed and new released = the release path 0x8463d0). The writer 0x84585c has no size
//      check. Pass 1 gave the manager a buffer (the tap crash) but left the size 0: the state was written and never read back. So a press
//      (0 -> 1) made an event and a RELEASE (0 == 0) made none: FlashLeftMouse never went up, a Flash button needs down + up; the right click acts
//      on the press (ContextMenu), which is why it alone worked. The same bounds-then-null checks are in the manager's update (0xb06f80,
//      0xb06fec, 0xb071d8, 0xb07c08, 0xb07610) and at 0x84609c / 0x846104, all null checked: a size of 64 with null entries for the pads changes
//      nothing for them.
//   2. Motion. The iPad's InjectInput copies its third argument into the record's +0x10 (1 for motion); ProcessInputEvent (0x845498 ldrb
//      [rec+0x10], 0x845f94..0x84620c) then appends the raw id to the list mgr+0x218, and the next update (0xb07e40 -> 0xb075b4..0xb075e0) feeds a
//      {raw, value 0, released} record for every id of that list before the new queue: an automatic release. That is how a constant {1,1}
//      motion makes a FlashMouseMove every frame. The Switch's InjectInput stores 0 there (0x84771c strb wzr,[x8,#0x10]), the consumer is intact.
//      Without it (and with the state live) only the first motion ever is a change. DM_DOS2_TOUCH sets +0x10 = 1 in the queued motion record.
//   3. Pinch. controller_1.json binds CameraZoomIn/Out to "c:rightstick_yneg/ypos" and "c:nx_touch_pinch_in/out"; main has no raw name
//      "nx_touch_pinch_*" (the table main+0x2974b10 ends at item11 0xac; only touch_pinch_in/out 0x9d/0x9e exist), so that half of the binding
//      resolves to nothing and our 0x9d/0x9e pulses had no binding. The zoom of the controller UI is the right stick: the pad poll's stick code
//      0xbdf4e0 sends {|axis| * sensitivity / 30, |axis|}, pressed = past the dead zone, on the pad device with rightstick_yneg 0x89 (zoom in) /
//      rightstick_ypos 0x87 (zoom out), and only while the stick is off centre (an idle stick sends nothing, so it does not fight ours).
// Switches of pass 5 (all need DM_DOS2_TOUCH=1):
//   DM_DOS2_TOUCH_DEVN=<n>    size of the raw-state array [mgr+0x24] the host sets with its buffer (default 64 = the buffer; 0 = leave 0, the pass 1..4
//                             behaviour: no release events for mouse / keyboard records)
//   DM_DOS2_TOUCH_AUTOREL=1   motion records get +0x10 = 1 in the queue (the iPad's automatic release, default); 2 = an explicit released motion
//                             record after every motion instead; 0 = neither (pass 1..4)
//   DM_DOS2_TOUCH_UIMOUSE=0   do not hold the UI manager's "mouse buttons reach Flash" byte [[main+0x2bbb230]+0x10b] at 1 (default: held, logged when it was 0)
//   DM_DOS2_TOUCH_PINCH=stick controller UI pinch = right stick y on the pad device, held DM_DOS2_TOUCH_PINCH_FRAMES (8) polls per step, value
//                             DM_DOS2_TOUCH_PINCH_V (1.0) (default) | touch = the old touch_pinch 0x9d / 0x9e pulses
//   DM_DBG=1                  also hooks 0xd26a60 (Iggy mouse move) and 0xd27700 (Iggy button): the summary line counts what reached Iggy and where
// Pass 6 (14.11, same branch; phone run t3, pack 2m, PC UI): "any click hangs the menu". What the log showed:
//   - the size check of pass 5 failed on the phone: main+0x845670 is `sbfx x12, x18, ..` and the loader rewrites instructions that use x18 (the
//     platform register on Android), so that word never matches main.raw. The check now skips it (words 0x84566c, 0x845674, 0x845678, 0x84567c..).
//   - the raw-state array is NOT absent on NX (pass 1 was wrong): the game makes it when the pad's device registers ([mgr+0x24] = 4 in the second
//     launch, where the first touch came after "pad device = 3"). In the first launch the touch came earlier, the host gave its own calloc buffer
//     with size 0, and the game then replaced it: our blocks for devices 0 / 1 were gone, g_state_ok stayed 1, nothing was stored and no button
//     ever went up again (the log has only "code 1" in that launch: every press stuck down = the dead menu).
//     Now: the array is checked on every poll with touches; while it is null or shorter than the mouse device the touches are dropped (one line),
//     the blocks for devices 0 / 1 are (re)added whenever they are missing, and only if the game still has no array DM_DOS2_TOUCH_STATE_WAIT
//     (300) polls after the first dropped touch does the host make one (engine allocator 0x83cc00 when its call site matches, capacity 64, size
//     mouse + 1).
//   - coordinates: the Iggy hook prints the argument of 0xd26a60 BEFORE it subtracts the movie origin, so "cursor 552,544" was simply the touch of
//     that moment; window px are right. The movie is 1920 wide at +240 in the 2400 window (origin 240,0): the move is sent in movie space, the
//     button event carries the raw window cursor (0xd27714), the game's own asymmetry (a PC ultrawide has the same origin), so Iggy most likely
//     ignores it. DM_DOS2_TOUCH_BTNXY=1 sends the button with the cursor minus the movie origin, for an A/B run only (default off).
//   - in the second launch a click was complete (down handled 1 by the menu movie, up delivered) and the movie stopped answering afterwards; a
//     release reaches every movie twice (two released events per record). Not explained by statics: DM_DOS2_TOUCH_UPONCE=1 drops the second "up"
//     to the same movie (default off); the summary now prints the UI manager's capture state ([uimgr+0x10d], [uimgr+0x218]).
// Pass 7 (15.11, branch dos2-tc2; docs/dos2/TOUCH_CONSOLE_PLAN.md section 6): per-movie routing and the world's first step. Static only.
//   With hle_dos2_touchui_route.c's hook in, KBM=2 keeps [mgr+0x170] = 1 (in menus and in game) and that hook decides per UI object who gets the mouse.
//   DM_DOS2_TOUCH_WORLD=1 (default 0; needs the routing hook and a player UI set = in game, no menu / panel / dialog on screen): a finger down sends
//     the left press AT ONCE (not on release); the UI's own answer says who took it (main+0xaea8d4, dm_dos2_route_taken). Taken = a UI gesture as
//     before (drag, release = click). Not taken = the press is released and the finger is the WORLD's: held longer than 150 ms or moved = virtual
//     left stick (vector from the screen point DM_DOS2_TOUCH_WORLD_C "0.5,0.52" = the character under the camera, to the finger; the controller's
//     movement is camera relative, so no picking is needed), a short tap = walk towards the point for distance * DM_DOS2_TOUCH_WORLD_WALK_MS (1400
//     ms per screen height) and then pad A (the controller's "use the highlighted object"). Two fingers: pinch = zoom (as before), horizontal
//     two-finger drag = camera rotation (right stick x through the npad, DM_DOS2_TOUCH_ROT_SWAP=1 flips), vertical = wheel.
//   DM_DOS2_TOUCH_WORLD=2: no "who took it" test: fixed HUD zones instead (bottom 20 %, left 13 %, top right 20 % x 32 %) keep the mouse
//     gestures, a finger anywhere else is the world's and sends no mouse record at all (for the A/B run if the UI's answer proves unreliable).
//   The sticks go through hle_hid.c fill (dm_dos2_touch_pad_axes), only when the pad's own stick is at rest.
// Pass 9 (16.11, branch dos2-touch5; docs/dos2/TOUCH_CONSOLE_PLAN.md section 15), owner's two asks:
//   1. One-finger swipe = camera rotation (DM_DOS2_TOUCH_SWIPE_ROT=0: the old rule). A world finger that moves past the slop BEFORE the hold
//      time (g_world_hold_ms) is a swipe: its horizontal motion is the right stick x in steps (dt_swipe_step, the two-finger rotation's sign
//      rule), no walk, no tap, the PC picking window closes. Held still = the hold as before (walk after the finger on the floor, Actions...
//      on an object); moved after the walk started = the walk goes on. The rules are pure (hle_dos2_touch_gesture.h, host test
//      dm/host/tests/dos2_touch_gesture_test.c). The Java pad sends a finger here only off its buttons / stick zones, so the stick rules stay.
//   2. Tap places a moved object (DM_DOS2_TOUCH_PLACE=0: off). hle_dos2_touch_world.c sees the item placement mode (the controller runs the
//      item-move task, type 10); then a world tap = the cursor (the PC picking) at the finger, picking window held open, pad A pressed
//      through the npad pulse = what a pad player does with the cursor there (dt_place_tap / dt_w3_poll phase 3). No Action1 of ours.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "dm_input.h"   // DM_BTN_* (also included further down; pack 3y uses DM_BTN_X before that)
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "hle_dos2_touch_gesture.h"
#include "hle_dos2_uimode.h"

enum { DT_POLL = 0x8a7530, DT_INJECT = 0x847690, DT_CALL = 0x8a8478, DT_MGR_LD = 0x8a77dc, DT_MGR = 0x2bb8960, DT_CURSOR = 0x2bb88e8,
       DT_CUR_INIT = 0x8a8e94, DT_CUR_READ = 0xd2392c, DT_SETDEV = 0x12a04a0 };
static const uint32_t k_cur_init[9] = {0x321e07e0 /* mov w0,#0xc */, 0x97fe4f5a /* bl alloc */, 0xf900001f, 0x39002013 /* strb w19,[x0,#8] */,
    0x90011888 /* adrp x8,main+0x2bb8000 */, 0x9123a108 /* add x8,x8,#0x8e8 */, 0xa9417bfd, 0xa900fd1f, 0xf9000100 /* str x0,[x8]: the cursor */};
static const uint32_t k_cur_read[7] = {0xb000f4a8 /* adrp x8,main+0x2bb8000 */, 0xf9447508 /* ldr x8,[x8,#0x8e8] */, 0x2d400500 /* ldp s0,s1,[x8] */,
    0xf9400008, 0x1e380022, 0xf940c908 /* ldr x8,[x8,#0x190] */, 0x1e380001};   // 0xd2392c: FlashMouseMove -> Iggy mouse position
enum { DT_MODE = 0x2bb8918, DT_MODE_INIT = 0x8a8eb8, DT_FILTER = 0x84407c, DT_FILTER2 = 0x8440a0, DT_MGR_KBM = 0x170, DT_WIN_RD = 0xc0b50c,
       DT_WIN_SET = 0x93d21c, DT_WINMGR = 0x2bbb260, DT_UISET = 0x10c07ac, DT_BIND_STR = 0x1ebfc35, DT_BIND_REF = 0xc05ad4 };
static const uint32_t k_mode_init = 0x3900c113;   // 0x8a8eb8 strb w19,[x8,#0x30]: the mode byte main+0x2bb8918 = 1 (x8 = main+0x2bb88e8, k_cur_init)
static const uint32_t k_filter[2] = {0x3945c2ab /* ldrb w11,[x21,#0x170] */, 0x7100095f /* cmp w10,#2 */};
static const uint32_t k_filter2[6] = {0x7100015f /* cmp w10,#0 */, 0x7a400964 /* ccmp w11,#0,#4,eq */, 0x54000161 /* b.ne deliver */,
    0x90011bab /* adrp x11,main+0x2bb8000 */, 0x3964616b /* ldrb w11,[x11,#0x918] */, 0x6b0b015f /* cmp w10,w11 */};
static const uint32_t k_win_rd[5] = {0x9000fd95 /* adrp x21,main+0x2bbb000 */, 0xf94132a9 /* ldr x9,[x21,#0x260] */, 0xb9805268, 0xf9402129 /* ldr x9,[x9,#0x40] */,
    0xf8687934 /* ldr x20,[x9,x8,lsl #3]: the window */};
static const uint32_t k_win_set = 0x29030801;     // 0x93d21c stp w1,w2,[x0,#0x18]: Window::SetSize
static const uint32_t k_uiset[2] = {0x39646118 /* ldrb w24,[x8,#0x918] */, 0xf9406ec8};   // 0x10c07ac: the per-player UI set reads the mode byte
static const uint32_t k_bind_ref[2] = {0xd00095c1 /* adrp x1,main+0x1ebf000 */, 0x9130d421 /* add x1,x1,#0xc35 */};
static const char k_bind_pad[] = "PlayerProfiles/Eoc/controller_1.json", k_bind_keys[] = "PlayerProfiles/Eoc/keyboard_1.json";
static int g_dbg, g_kbm, g_pcui, g_mode_ok, g_win_ok, g_win_fixed;
// live switch (pass 4): the player UI manager's create all / destroy all and the places that prove the global and the hash layout
enum { DT_UI_THIS = 0x3298140, DT_UI_CREATE = 0x10c4580, DT_UI_DESTROY = 0x10c4730, DT_UI_CREATE_CALL = 0x104da98, DT_UI_DESTROY_CALL = 0xd40064,
       DT_UI_HASH = 0x10c4748, DT_UI_WALK = 0x10c47c4, DT_UI_SET = 0x10c06d0, DT_UI_SET_L = 0x10c4720, DT_UIMGR = 0x2bbb230, DT_UI_HASH_OFF = 0xf0 };
static const uint32_t k_ui_create[6] = {0xd101c3ff /* sub sp,sp,#0x70 */, 0xa90457f6, 0xa9054ff4, 0xa9067bfd, 0x910183fd, 0xaa0003f3 /* mov x19,x0 */};
static const uint32_t k_ui_destroy[6] = {0xd10103ff /* sub sp,sp,#0x40 */, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd, 0xaa0003f3, 0x97fffeab /* bl 0x10c41f0: shared */};
static const uint32_t k_ui_create_call[3] = {0xf0011248 /* adrp x8,main+0x3298000 */, 0xf940a100 /* ldr x0,[x8,#0x140] */, 0x9401dab8 /* bl 0x10c4580 */};
static const uint32_t k_ui_destroy_call[3] = {0x90012ac8 /* adrp x8,main+0x3298000 */, 0xf940a100 /* ldr x0,[x8,#0x140] */, 0x940e11b1 /* bl 0x10c4730 */};
static const uint32_t k_ui_hash[5] = {0x9103c274 /* add x20,x19,#0xf0: the per-player hash */, 0x910003e1, 0xf90003f3, 0xaa1403e0, 0x94000016 /* bl 0x10c47b0: destroy each */};
static const uint32_t k_ui_walk[3] = {0xb9400408 /* ldr w8,[x0,#4]: buckets */, 0x340003c8, 0xf940040b /* ldr x11,[x0,#8]: bucket array */};
static const uint32_t k_ui_set[6] = {0xd101c3ff, 0xa9016ffc, 0xa90267fa, 0xa9035ff8, 0xa90457f6, 0xa9054ff4};   // 0x10c06d0: the per-player set builder
static const uint32_t k_ui_set_l[4] = {0xf9400400 /* ldr x0,[x0,#8] */, 0x79400021 /* ldrh w1,[x1] */, 0x320303e2 /* mov w2,#0x20000000 */, 0x17ffefe9 /* b 0x10c06d0 */};
static int g_live, g_live_ms = 1000, g_in_poll, g_pad_act, g_live_wait;
// UI mode switch (hle_dos2_uimode.c): g_live_ok = the live switch's sites matched and the InjectInput hook is in (also without DM_DOS2_TOUCH_LIVE,
// so a change to TOUCH in game can still bring the controller UI back on a pad press); g_live_ui = UI mode TOUCH runs that switch now (the =1
// variant); g_bind_ok / g_bind_keys / g_keys_pref = the bindings path string checked / now keyboard_1.json / wanted in TOUCH (DM_DOS2_TOUCH_PCUI_KEYS)
static int g_live_ok, g_live_ui, g_bind_ok, g_bind_keys, g_keys_pref = 1;
static int dt_live(void) { return g_live ? g_live : g_live_ui; }   // the live switch's variant now: DM_DOS2_TOUCH_LIVE for the run, else the UI mode's
static unsigned g_live_n;
static uint64_t g_live_t;
static unsigned g_n[6], g_nlog, g_nlog_motion, g_polls, g_n_last;   // records: 0 motion 1 left 2 right 3 wheel 4 pinch 5 key
static float g_tx, g_ty;   // the last touch, 0..1
enum { DT_STATE = 0x84585c, DT_MGR_DEVS = 0x18 };
static const uint32_t k_state[6] = {0xb94002e8 /* ldr w8,[x23] */, 0xf9400c6a /* ldr x10,[x3,#0x18] */, 0x934d7d09 /* sbfx x9,x8,#13,#19 */,
    0x927df129, 0xf8696949 /* ldr x9,[x10,x9] */, 0xb4000109 /* cbz x9: null entry skipped */};
static int g_state_ok, g_state_alloc = 1;
static const uint32_t k_setdev[8] = {0xb40006e2 /* cbz x2 */, 0xd10103ff, 0xa90157f6, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd,
    0xb902c001 /* +0x18 str w1,[x0,#0x2c0] */, 0xb940d848};
typedef uint64_t (*dt_fn_fwd)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
// pass 5: the queue InjectInput fills, the automatic release, the raw-state array's size, the UI manager's mouse byte, Iggy (debug hooks)
enum { DT_Q_FLAG = 0x84771c, DT_Q_BUF = 0x8476ec, DT_REC_FLAG = 0x845498, DT_AUTOREL = 0xb075bc, DT_AUTOREL2 = 0xb075d4, DT_STATE_RD = 0x84566c,
       DT_STATE_RD2 = 0x84567c, DT_UIM_BTN = 0xaea8ac, DT_UIM_CTOR = 0xae7424, DT_IGGY_MOVE = 0xd26a60, DT_IGGY_BTN = 0xd27700, DT_STICK = 0xbdf654,
       DT_MGR_Q = 0x278, DT_MGR_QN = 0x280, DT_MGR_DEVN = 0x24, DT_UIM_MOUSE = 0x10b, DT_DEVS = 64 };
static const uint32_t k_q_flag[2] = {0x3900411f /* strb wzr,[x8,#0x10]: InjectInput clears the record's flag */, 0xf9414268 /* ldr x8,[x19,#0x280] */};
static const uint32_t k_q_buf = 0xf9413e69;        // 0x8476ec ldr x9,[x19,#0x278]: the queue's buffer (stride 0x14)
static const uint32_t k_rec_flag[2] = {0x394042e8 /* ldrb w8,[x23,#0x10] */, 0x350057c8 /* cbnz w8, 0x845f94: raw id -> the release list mgr+0x218 */};
static const uint32_t k_autorel[2] = {0xf9410ea8 /* ldr x8,[x21,#0x218] */, 0xb8766908 /* ldr w8,[x8,x22] */};
static const uint32_t k_autorel2[4] = {0x3900211f /* pressed 0 */, 0xf900011f /* value 0 */, 0x390263ff, 0x97f4f75c /* bl 0x845350 ProcessInputEvent */};
static const uint32_t k_state_rd[4] = {0xb940246f /* ldr w15,[x3,#0x24]: the array's size */, 0x93507e4c /* sbfx x12,x18,#16,#16: device */, 0xeb0f019f, 0x54000782 /* b.hs: stored = 0 */};
static const uint32_t k_state_rd2[3] = {0xf9400c6d /* ldr x13,[x3,#0x18] */, 0xf86c79ac, 0xb400072c /* cbz: null entry = stored 0 */};
static const uint32_t k_uim_btn[2] = {0x39442eaa /* ldrb w10,[x21,#0x10b] */, 0x3400160a /* cbz: events 1..3 not sent to Flash */};
static const uint32_t k_uim_ctor[4] = {0x320803e8 /* mov w8,#0x1000000 */, 0x320003e9, 0x79021a89, 0xb9010a88 /* str w8,[x20,#0x108]: +0x10b = 1 */};
static const uint32_t k_iggy_move[4] = {0xd10143ff /* sub sp,sp,#0x50 */, 0xf9001bf3, 0xa9047bfd, 0x910103fd};
static const uint32_t k_iggy_btn[4] = {0xd10183ff /* sub sp,sp,#0x60 */, 0xf9001bf5, 0xa9044ff4, 0xa9057bfd};
static const uint32_t k_stick[3] = {0x390033e8 /* pressed */, 0x2d00abe0 /* stp s0,s10,[sp,#4]: value {scaled, |axis|} */, 0x97f1a00d /* bl 0x847690 */};
static int g_devn = DT_DEVS, g_devn_ok, g_autorel = 1, g_autorel_ok, g_uimouse = 1, g_uimouse_ok, g_pinch_stick = 1, g_pinch_frames = 8, g_motion_in_drain;
static float g_pinch_v = 1.0f;
static unsigned g_n_flag, g_n_rel, g_iggy_move_n, g_iggy_btn_n, g_iggy_btn_code, g_iggy_btn_flag;
static int g_iggy_x, g_iggy_y, g_iggy_ox, g_iggy_oy;
static dt_fn_fwd g_iggy_move_orig, g_iggy_btn_orig;
enum { DT_ALLOC_CALL = 0xd234c8, DT_ALLOC = 0x83cc00, DT_MGR_DEVCAP = 0x20, DT_UIM_CAPT = 0x10d, DT_UIM_CAPT_OBJ = 0x218 };
static const uint32_t k_alloc_call[3] = {0x52800500 /* mov w0,#0x28 */, 0xaa1f03e1 /* mov x1,xzr */, 0x97ec65cc /* bl 0x83cc00: the engine's allocator */};
// pack 3f (owner, run t3e: "press A to start" needed ~5 taps and ~7 s): on the title screen no pad has registered yet, the game has no raw-state
// array and the old default waited 300 polls (~5.5 s) dropping every touch. The game replacing our array later is handled (blocks re-added),
// so the host now makes it at the first touch (DM_DOS2_TOUCH_STATE_WAIT=<polls> brings the wait back).
static int g_alloc_ok, g_state_wait = 0, g_state_waited, g_btnxy, g_uponce;
static unsigned g_n_dropped, g_n_readd, g_n_updrop;
enum { ID_KEY_ESCAPE = 1, DEV_KEYS = 0 };   // raw names table: return 0, escape 1, backspace 2, tab 3 ... (keyboard = device 0)
enum { ID_LEFT = 0x74, ID_RIGHT = 0x76, ID_MOTION = 0x79, ID_WHEEL_YPOS = 0x80, ID_WHEEL_YNEG = 0x81, ID_PINCH_IN = 0x9d, ID_PINCH_OUT = 0x9e,
       ID_RSTICK_YPOS = 0x87 /* CameraZoomOut */, ID_RSTICK_YNEG = 0x89 /* CameraZoomIn */ };
static const uint32_t k_poll[4] = {0x6db733ed /* stp d13,d12,[sp,#-0x90]! */, 0x6d012beb, 0x6d0223e9, 0xa9036ffc};
static const uint32_t k_inject[8] = {0xd10143ff /* sub sp,sp,#0x50 */, 0xf9000bf7, 0xa90257f6, 0xa9034ff4, 0xa9047bfd, 0x910103fd,
                                     0xf8404028 /* ldur x8,[x1,#4] */, 0x39403029 /* ldrb w9,[x1,#0xc] */};
static const uint32_t k_call = 0x97fe7c86;   // 0x8a8478 bl 0x847690
static const uint32_t k_mgr[3] = {0xb0011888 /* adrp x8,main+0x2bb8000 */, 0xf944b108 /* ldr x8,[x8,#0x960] */, 0xf90033e8 /* str x8,[sp,#0x60] */};

typedef struct { uint32_t raw; float v[2]; uint8_t pressed, pad[3], flag, pad2[3]; } dt_rec;
_Static_assert(sizeof(dt_rec) == 20, "InjectInputData is 20 bytes");
typedef uint64_t (*dt_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef void (*dt_inject_fn)(void *mgr, const dt_rec *r, uint64_t from_os);
static dt_fn g_poll_orig, g_inject_orig;
static dt_inject_fn g_inject;
static module_t *g_main;
static int g_on, g_log, g_dev = 1, g_motion = 1, g_hold_ms = 500, g_cursor_ptr;
static int g_ui_press_ms = 200;   // dos2-touch6: a still UI finger gets its left press after this long (DM_DOS2_TOUCH_UI_PRESS_MS, 0 = only on the release / a move)

static uint32_t g_cursor_off;
static float g_win_w = 1600, g_win_h = 900;

// ---- the front end's events (any thread) ----
enum { EV_DOWN, EV_MOVE, EV_UP, EV_CANCEL };
typedef struct { uint8_t ph, id; float x, y; uint64_t t; } dt_ev;
static dt_ev g_q[256];
static unsigned g_qh, g_qt;
static pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER;
static uint64_t dt_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000ull + (uint64_t)t.tv_nsec / 1000000ull; }

int dm_dos2_touch_on(void);
int dm_dos2_touch_on(void) { return g_on && g_poll_orig; }
void dm_dos2_touch_event(int phase, int id, float nx, float ny);   // jni_android.c nativeDos2Touch: phase 0 down / 1 move / 2 up / 3 cancel
void dm_dos2_touch_event(int phase, int id, float nx, float ny) {
    if (!dm_dos2_touch_on() || phase < 0 || phase > 3) return;
    { int dm_dos2_ui_touches(void); if (phase != EV_UP && phase != EV_CANCEL && !dm_dos2_ui_touches()) return; }   // UI mode SWITCH: no mouse records (a finger's up still ends its gesture)
    pthread_mutex_lock(&g_qm);
    if (g_qt - g_qh < 256) {
        if (phase == EV_MOVE && g_qt != g_qh) {   // coalesce moves of the same finger: the game reads once a frame
            dt_ev *l = &g_q[(g_qt - 1) & 255];
            if (l->ph == EV_MOVE && l->id == (uint8_t)id) { l->x = nx; l->y = ny; l->t = dt_ms(); pthread_mutex_unlock(&g_qm); return; }
        }
        g_q[g_qt++ & 255] = (dt_ev){(uint8_t)phase, (uint8_t)id, nx, ny, dt_ms()};
    }
    pthread_mutex_unlock(&g_qm);
}

// ---- injection (game thread) ----
static void *dt_mgr(void) { return *(void *volatile *)(g_main->base + DT_MGR); }
static float g_cx = -1, g_cy = -1;   // cursor, window pixels
static const float *dt_cursor_at(void) {
    if (g_cursor_off) return (const float *)(g_main->base + g_cursor_off);
    return g_cursor_ptr ? *(float *volatile *)(g_main->base + DT_CURSOR) : NULL;
}
static void dt_send(void *mgr, uint32_t dev, uint32_t id, float a, float b, int pressed) {
    dt_rec r; memset(&r, 0, sizeof r);
    r.raw = dev << 16 | id; r.v[0] = a; r.v[1] = b; r.pressed = (uint8_t)pressed;
    g_inject(mgr, &r, id == ID_MOTION);
    int flagged = 0;
    if (id == ID_MOTION && pressed && g_autorel == 1 && g_autorel_ok) {   // the iPad's InjectInput(.., 1): +0x10 = 1 in the queued record = released by the next update
        const uint64_t qn = (*(volatile uint64_t *)((uint8_t *)mgr + DT_MGR_QN) >> 24) & 0xffffff;
        uint8_t *q = *(uint8_t *volatile *)((uint8_t *)mgr + DT_MGR_Q);
        if (q && qn && !memcmp(q + (qn - 1) * 0x14, &r, 13)) { q[(qn - 1) * 0x14 + 0x10] = 1; flagged = 1; g_n_flag++; }
    }
    const int k = id == ID_MOTION ? 0 : id == ID_LEFT ? 1 : id == ID_RIGHT ? 2 : id == ID_WHEEL_YPOS || id == ID_WHEEL_YNEG ? 3 :
                  id == ID_PINCH_IN || id == ID_PINCH_OUT || (dev != DEV_KEYS && (id == ID_RSTICK_YPOS || id == ID_RSTICK_YNEG)) ? 4 : 5;
    g_n[k]++;
    if (g_dbg && g_nlog < 64 && (k || g_nlog_motion < 16)) {   // DM_DBG: gesture -> record, with what the game will read
        const float *c = dt_cursor_at();
        g_nlog++; g_nlog_motion += !k;
        LOG("[touch] #%u %s%s dev %u id %#x value %.0f,%.0f | touch %.3f,%.3f -> window %.0fx%.0f px %.0f,%.0f | cursor read back %.1f,%.1f | mode %u kbm %u",
            g_nlog, k == 0 ? "motion" : k == 1 ? "left" : k == 2 ? "right" : k == 3 ? "wheel" : k == 4 ? "pinch" : "key",
            k ? pressed ? " down" : " up" : !pressed ? " released" : flagged ? " (+0x10 = 1: auto release)" : "", dev, id,
            (double)a, (double)b, (double)g_tx, (double)g_ty, (double)g_win_w, (double)g_win_h, (double)g_cx, (double)g_cy, c ? (double)c[0] : -1., c ? (double)c[1] : -1.,
            g_mode_ok ? (unsigned)*(volatile uint8_t *)(g_main->base + DT_MODE) : 255u, (unsigned)*((volatile uint8_t *)mgr + DT_MGR_KBM));
    }
}
static void dt_cursor(float x, float y) {   // where the UI reads the mouse: [main+0x2bb88e8] -> float2 (or DM_DOS2_TOUCH_CURSOR)
    float *p = NULL;
    if (g_cursor_off) p = (float *)(g_main->base + g_cursor_off);
    else if (g_cursor_ptr) p = *(float *volatile *)(g_main->base + DT_CURSOR);
    if (p) { p[0] = x; p[1] = y; }
}
static void dt_move(void *mgr, float x, float y) {
    float dx = g_cx < 0 ? 0 : x - g_cx, dy = g_cy < 0 ? 0 : y - g_cy;
    g_cx = x; g_cy = y;
    dt_cursor(x, y);
    // a second motion in one poll: the automatic release comes only with the next update, so release by hand first (else "no change" = no event)
    if (g_autorel == 1 && g_autorel_ok && g_motion_in_drain) { dt_send(mgr, (uint32_t)g_dev, ID_MOTION, 0, 0, 0); g_n_rel++; }
    g_motion_in_drain++;
    if (g_motion == 1) dt_send(mgr, (uint32_t)g_dev, ID_MOTION, 1, 1, 1);
    else if (g_motion == 2) dt_send(mgr, (uint32_t)g_dev, ID_MOTION, dx, dy, 1);
    else dt_send(mgr, (uint32_t)g_dev, ID_MOTION, x, y, 1);
    if (g_autorel == 2) { dt_send(mgr, (uint32_t)g_dev, ID_MOTION, 0, 0, 0); g_n_rel++; }   // explicit release right behind the press
}
void dm_dos2_route_left(int down); int dm_dos2_route_on(void); int dm_dos2_route_game(void); int dm_dos2_route_taken(uint32_t *type); int dm_dos2_route_world_ok(uint32_t *type);   // hle_dos2_touchui_route.c
void dm_dos2_route_summary(void);
void dm_dos2_touch_world_hover(int on); void dm_dos2_touch_world_left(int kind, int down); int dm_dos2_touch_world_on(void); void dm_dos2_touch_world_summary(void);   // hle_dos2_touch_world.c
static int g_w3, g_w3_kind = 1;   // DM_DOS2_TOUCH_WORLD=3: PC world clicks; the kind of the next left record: 1 = probe / UI gesture, 2 = the world click
static void dt_button(void *mgr, uint32_t id, int down) { if (g_cx >= 0) dt_cursor(g_cx, g_cy); if (id == ID_LEFT) { dm_dos2_route_left(down); if (g_w3) dm_dos2_touch_world_left(g_w3_kind, down); }
    dt_send(mgr, (uint32_t)g_dev, id, down ? 1 : 0, down ? 1 : 0, down); }

// gesture state
static int dt_pc(void);
enum { G_IDLE, G_PENDING, G_DRAG, G_HELD, G_TWO, G_WPROBE /* world: left is down, who took it? */, G_WTAP /* the same, finger already up */, G_WORLD /* the finger moves the character */,
       G_MAP /* map view: the finger pans the map (left stick) */, G_RADIAL /* dos2-touch7: a ZL / ZR wheel is open, the finger points its item (right stick) */ };
static struct { int st, f0, f1; float sx, sy, x0, y0, x1, y1, d0, wy, wmx, lx, ly; uint64_t t0; uint32_t pulse; int pulse_frames, wact, wmoved, wplace; } g;   // wact: 1 walk stick, 2 Actions..., 3 swipe rotation; wplace: a placement-mode finger (no mouse press)
// ---- world gestures (pass 7): virtual sticks for hle_hid.c, the tap = walk + A ----
#include <stdatomic.h>
static int g_world, g_world_walk_ms = 1400, g_world_probe_ms = 250, g_ui_ok, g_in_game, g_rot_swap, g_swipe_rot = 1, g_place = 1;   // pass 9: DM_DOS2_TOUCH_SWIPE_ROT / _PLACE
static unsigned g_n_swipe, g_n_place;
static float g_world_cx = 0.5f, g_world_cy = 0.52f;
static _Atomic int g_w_lx, g_w_ly, g_w_rx, g_w_ry;   // g_w_ry: map pinch (map view section)
// dos2-touch7: the ZL / ZR wheels (the radial section below)
static int g_rad_cfg = 1, g_rad_lstick = 1, g_rad_ms = 120;
static _Atomic int g_rad_live;            // UI type of the open wheel (0x53 / 0x52), 0 = none (npad thread reads it)
static _Atomic int g_rad_rx, g_rad_ry;    // the finger's right stick (0,0 = none)
static _Atomic uint64_t g_rad_choose_at;  // npad thread: from then on the held ZL / ZR is let go for the game (0 = no choose asked)
static uint64_t g_rad_sup;                // npad thread only: the trigger mask held back (dt_radial_release)
static uint64_t g_rad_clear_at;           // game thread: when the finger's stick goes back to rest after a choose
static float g_rad_cx, g_rad_cy;          // the open wheel's centre, window px
static unsigned g_n_rad_open, g_n_rad_pick, g_n_rad_lst;
static uint64_t g_w_ry_until;
static uint64_t g_w_until, g_w_rx_until;   // game thread: when a timed stick ends (0 = not timed)
static _Atomic uint64_t g_a_ms;            // when the npad last had A down
static unsigned g_n_wtap, g_n_wstick, g_n_wui;
void dm_dos2_touch_pad_axes(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);   // hle_hid.c fill(), after the pad's own sticks
void dm_dos2_touch_pad_axes(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    const int x = atomic_load(&g_w_lx), y = atomic_load(&g_w_ly), r = atomic_load(&g_w_rx), q = atomic_load(&g_w_ry);
    if (atomic_load(&g_rad_live)) {   // dos2-touch7: an open ZL / ZR wheel: the pad's right stick, else the finger, else the pad's left stick points it
        const int fx = atomic_load(&g_rad_rx), fy = atomic_load(&g_rad_ry);
        const int64_t r2 = (int64_t)*rx * *rx + (int64_t)*ry * *ry;
        if ((fx || fy) && r2 <= 6000ll * 6000ll) { *rx = fx; *ry = fy; *lx = *ly = 0; }
        else if (g_rad_lstick && dt_radial_lstick(lx, ly, rx, ry, 6000)) g_n_rad_lst++;
    }
    if ((x || y) && !(*lx || *ly)) { *lx = x; *ly = y; }
    if (r && !*rx) *rx = r;
    if (q && !*ry) *ry = q;   // map pinch (map view section)
}
uint64_t dm_dos2_touch_a_ms(void);
uint64_t dm_dos2_touch_a_ms(void) { return atomic_load(&g_a_ms); }
int dm_dos2_touch_in_game(void);
int dm_dos2_touch_in_game(void) { return g_in_game; }
// [DOS2] pack 3d (owner: "a tap on a menu item must win over the floating stick"): what the UI shows, for the Java pad (jni_android.c
// nativeDos2UiState, any thread). Made on the game thread in dt_drain (every pad poll) from hle_dos2_touchui_route.c's walk of the visible UI
// objects, so the Java side never reads game memory. bit 0 = a menu / modal / panel / dialog / context menu (any non-HUD UI that takes taps) is
// visible, bit 1 = in game (a player UI set exists), bit 2 = known (route hook in and the UI set readable; 0 = the pad keeps its old rule)
static _Atomic int g_ui_state;
int dm_dos2_touch_ui_state(void);
int dm_dos2_touch_ui_state(void) { return g_on && g_poll_orig ? atomic_load(&g_ui_state) : 0; }
static void dt_world_stick(float x, float y, float force) {   // window px -> left stick; force > 0 = at least that tilt
    const float dx = x - g_world_cx * g_win_w, dy = y - g_world_cy * g_win_h, r = hypotf(dx, dy), dead = g_win_h * 0.04f, full = g_win_h * 0.16f;
    if (r < 1 || (r < dead && force <= 0)) { atomic_store(&g_w_lx, 0); atomic_store(&g_w_ly, 0); return; }
    float mag = (r - dead) / (full - dead); if (mag > 1) mag = 1; if (mag < 0.4f) mag = 0.4f; if (mag < force) mag = force;
    atomic_store(&g_w_lx, (int)(dx / r * mag * 32767.f)); atomic_store(&g_w_ly, (int)(-dy / r * mag * 32767.f));   // npad y: up is positive
}
static void dt_world_stop(void) { atomic_store(&g_w_lx, 0); atomic_store(&g_w_ly, 0); g_w_until = 0; }
// pass 9: one-finger swipe = camera rotation (right stick x held 140 ms per step, like the two-finger drag); g.wmx / g.wy = the last step's point
// owner 16.11: the one-finger swipe camera only where the finger landed in the right 60 % of the screen; the left 40 % (where the left
// stick lives) keeps the old rule: a move past the slop = the walk stick. DM_DOS2_TOUCH_SWIPE_X0 (0..1, default 0.40) moves the edge.
static float g_swipe_x0 = 0.40f;
static int dt_swipe_zone(void) { return g_swipe_rot && g.sx >= g_swipe_x0 * g_win_w; }   // g.sx: where the finger landed
static void dt_swipe_start(float x, float y) {
    g.wact = 3; g.wmx = g.sx; g.wy = g.sy; dt_world_stop(); g_n_swipe++;
    if (g_dbg) { static unsigned n; if (n++ < 100) LOG("[touch] world: one-finger SWIPE from %.0f,%.0f (moved %.0f px within %llu ms): camera rotation, no walk / tap", (double)g.sx, (double)g.sy,
                                                       (double)hypotf(x - g.sx, y - g.sy), (unsigned long long)(dt_ms() - g.t0)); }
}
static void dt_swipe_move(float x, float y) {
    int reset = 0;
    const int dir = dt_swipe_step(x - g.wmx, y - g.wy, g_win_h * 0.02f, &reset);
    if (dir) { atomic_store(&g_w_rx, dt_swipe_rx(dir, g_rot_swap)); g_w_rx_until = dt_ms() + 140; g.wmx = x; g.wy = y; }
    else if (reset) { g.wmx = x; g.wy = y; }
}
static void dt_swipe_end(void) { atomic_store(&g_w_rx, 0); g_w_rx_until = 0; }
int dm_dos2_touch_pad_pulse(uint64_t mask, const char *why, int delay_ms);
static void dt_world_tap(float x, float y) {   // a short tap on the world: walk towards the point, then A (the controller's "use what is highlighted")
    const float d = hypotf(x - g_world_cx * g_win_w, y - g_world_cy * g_win_h) / g_win_h;
    int ms = d < 0.06f ? 0 : (int)(d * (float)g_world_walk_ms); if (ms > 2500) ms = 2500;
    if (ms) { dt_world_stick(x, y, 1.0f); g_w_until = dt_ms() + (uint64_t)ms; }
    const int a = dm_dos2_touch_pad_pulse(1u << 0 /* DM_BTN_A */, "world tap: walk + A", ms + 60);
    g_n_wtap++;
    if (g_dbg || g_n_wtap <= 4) LOG("[touch] world tap at %.0f,%.0f (%.2f screen heights from the character point): left stick %d,%d for %d ms, then pad A%s", (double)x, (double)y, (double)d,
                                    atomic_load(&g_w_lx), atomic_load(&g_w_ly), ms, a ? "" : " (NOT pressed: a pulse is running)");
}
static int dt_world_zone_ui(float nx, float ny) { return ny > 0.80f || (nx < 0.13f) || (nx > 0.80f && ny < 0.32f); }   // DM_DOS2_TOUCH_WORLD=2: hot bar, portraits, minimap
// ---- pass 8 (DM_DOS2_TOUCH_WORLD=3, hle_dos2_touch_world.c): a short tap = the PC's left click on the world ----
// The finger's point is the UI cursor from the moment it lands (the PC picking window is open: outline / path preview under the finger). A tap:
// the picking has had the point for at least g_w3_settle polls, then left down, two polls later left up (both marked "world click"), the window
// stays open g_w3_after_ms more. DM_DOS2_TOUCH_WORLD_CONFIRM=1: the first tap only previews (window kept 4 s), a second tap on the same spot clicks.
static int g_w3_confirm, g_w3_after_ms = 300, g_w3_settle = 2, g_world_hold_ms = 150, g_ctx_hold = 2;   // g_ctx_hold: DM_DOS2_TOUCH_CTX_HOLD=0 off, 1 pad X (pack 3y), 2 PC right click (pack 3z, default)
static struct { int ph, polls, ptype; float x, y, px, py; uint64_t hover_until, prev_until, place_t; } w3;   // ptype: the target mode's task type at the tap (10 / aim)   // ph: 1 / 2 = PC click down / up, 3 = placement tap (pass 9)
static unsigned g_n_w3click, g_n_w3prev;
int dm_dos2_touch_world_placing(void); void dm_dos2_touch_world_kinds_clear(void); int dm_dos2_touch_world_run_type(void);   // hle_dos2_touch_world.c
int dm_dos2_touch_world_place_mode(void); void dm_dos2_touch_world_place_at(void); int dm_dos2_touch_world_place_ready(void); int dm_dos2_touch_world_place_failed(void);
void dm_dos2_touch_world_place_hold(int ms); int dm_dos2_touch_world_place_preview(void); int dm_dos2_touch_world_place_type(void);
// pass 9 / dos2-place: the item placement mode (Actions... -> Move). DM_DOS2_TOUCH_PLACE=1 (default, hle_dos2_touch_world.c point 5): the pad's
// selector (what the item follows) goes to the ground point under the finger - no PC picking window, the item-move preview stays - and once
// the preview is the item-move task again dt_w3_poll presses the pad's A (Action1 <- c:a), the pad player's "place it here".
// DM_DOS2_TOUCH_PLACE=2: the first try (point 4): the PC picking at the finger + A (it hit the moved item itself, phone 16.11).
static void dt_place_tap(void *mgr, float x, float y) {
    const uint64_t now = dt_ms();
    const int how = dm_dos2_touch_world_place_mode();
    w3.prev_until = 0; w3.x = x; w3.y = y; w3.ph = 3; w3.polls = 0; w3.place_t = now; g_n_place++;
    w3.ptype = dm_dos2_touch_world_place_type(); if (w3.ptype < 0) w3.ptype = DT_TASK_ITEMMOVE;
    if (how == 2) dt_move(mgr, x, y);
    else { dt_cursor(x, y); g_cx = x; g_cy = y; }   // place1.log: only the UI cursor (the ray probe of point 5 (a) reads it), no mouse record
    if (how == 2) { if (w3.hover_until < now + 700) w3.hover_until = now + 700; }
    else { w3.hover_until = 0; dm_dos2_touch_world_hover(0); dm_dos2_touch_world_place_at(); }
    { static unsigned n; if (g_dbg || n++ < 20) LOG("[touch] world tap at %.0f,%.0f: %s (%s, preview task type %d): %s", (double)x, (double)y,
                                                    w3.ptype == DT_TASK_ITEMMOVE ? "PLACE" : "AIM", w3.ptype == DT_TASK_ITEMMOVE ? "item placement mode" : "throw / aim mode",
                                                    dm_dos2_touch_world_place_preview(), how == 2 ? "PC picking at the finger, pad A in 2 polls (DM_DOS2_TOUCH_PLACE=2)" :
                                                    "the pad's selector to the ground under the finger, then pad A once the mode's preview is back"); }
}
static void dt_w3_tap(void *mgr, float x, float y) {
    const uint64_t now = dt_ms();
    g_n_wtap++;
    if (g_place && dm_dos2_touch_world_placing()) { dt_place_tap(mgr, x, y); return; }
    if (g_w3_confirm && !(w3.prev_until && now < w3.prev_until && hypotf(x - w3.px, y - w3.py) < g_win_h * 0.07f)) {   // first tap: preview only
        w3.px = x; w3.py = y; w3.prev_until = w3.hover_until = now + 4000; g_n_w3prev++;
        dt_move(mgr, x, y);
        if (g_dbg) LOG("[touch] world tap at %.0f,%.0f: PREVIEW (DM_DOS2_TOUCH_WORLD_CONFIRM=1): tap the same spot again within 4 s to act", (double)x, (double)y);
        return;
    }
    w3.prev_until = 0; w3.x = x; w3.y = y; w3.ph = 1; w3.polls = 0; g_n_w3click++;
    dt_move(mgr, x, y);
    if (g_dbg) LOG("[touch] world tap at %.0f,%.0f: PC click (left down after %d poll(s) with the cursor there, up two polls later; running task type %d)", (double)x, (double)y, g_w3_settle,
                   dm_dos2_touch_world_run_type());
}
static void dt_w3_poll(void *mgr) {   // every pad poll
    const uint64_t now = dt_ms();
    if (w3.ph == 1 && ++w3.polls >= g_w3_settle) { dt_move(mgr, w3.x, w3.y); g_w3_kind = 2; dt_button(mgr, ID_LEFT, 1); g_w3_kind = 1; w3.ph = 2; w3.polls = 0; }
    else if (w3.ph == 3 && ++w3.polls >= g_w3_settle) {   // placement tap -> pad A with the pad's target at the finger
        const int how = dm_dos2_touch_world_place_mode(), pv = dm_dos2_touch_world_place_preview();
        const int ready = how == 2 || (dm_dos2_touch_world_place_ready() && (pv == w3.ptype || now - w3.place_t > 500));   // point 5: preview 10 (aim: its type) back (or give it 0.5 s)
        if (how != 2 && dm_dos2_touch_world_place_failed()) { w3.ph = 0; dm_dos2_touch_world_place_hold(0); LOG("[touch] placement tap: no ground point under the finger: nothing pressed"); }
        else if (ready) {
            if (how == 2) dt_move(mgr, w3.x, w3.y);
            dm_dos2_touch_world_kinds_clear();   // the pad's Action1 must pass tw_input untouched (not taken for our probe press)
            if (dm_dos2_touch_pad_pulse(DM_BTN_A, "placement tap: pad A at the finger", 0)) {
                w3.ph = 0;
                if (how == 2) { if (w3.hover_until < now + 500) w3.hover_until = now + 500; }   // the press is read by the npad ~120 ms later: target still at the finger
                else dm_dos2_touch_world_place_hold(400);
                { static unsigned n; if (g_dbg || n++ < 20) LOG("[touch] placement tap: pad A pressed %llu ms after the tap (preview task type %d), target held 400 / 500 ms more",
                                                                (unsigned long long)(now - w3.place_t), pv); }
            } else if (now - w3.place_t > 900) { w3.ph = 0; if (how != 2) dm_dos2_touch_world_place_hold(0); LOG("[touch] placement tap: pad A NOT pressed (a pulse kept running)"); }
        } else if (now - w3.place_t > 1200) { w3.ph = 0; dm_dos2_touch_world_place_hold(0); LOG("[touch] placement tap: given up after 1.2 s (point %s, preview task type %d)",
                                                                                                 dm_dos2_touch_world_place_ready() ? "known" : "not known", pv); }
    }
    else if (w3.ph == 2 && ++w3.polls >= 2) { g_w3_kind = 2; dt_button(mgr, ID_LEFT, 0); g_w3_kind = 1; w3.ph = 0; if (w3.hover_until < now + (uint64_t)g_w3_after_ms) w3.hover_until = now + (uint64_t)g_w3_after_ms; }
    if (w3.hover_until && now >= w3.hover_until) w3.hover_until = 0;
}
static void dt_w3_sync(int st, int wact) {   // the PC picking window: a world finger that is not a stick yet, a click in flight, the time after it / a preview
    // dos2-place: never in the item placement mode (DM_DOS2_TOUCH_PLACE=1): the PC picking there kills the item-move preview; the tap moves the selector
    if (g_place && dm_dos2_touch_world_place_mode() == 1 && (dm_dos2_touch_world_placing() || w3.ph == 3)) { dm_dos2_touch_world_hover(w3.hover_until != 0); return; }
    dm_dos2_touch_world_hover(st == G_WPROBE || st == G_WTAP || (st == G_WORLD && !wact) || w3.ph || w3.hover_until);
}
static int dt_world_now(void) {   // may a new finger be the world's
    uint32_t ty = 0;
    if (!g_world || !g_in_game || (g_world == 1 && !dm_dos2_route_game())) return 0;
    if (!dm_dos2_route_world_ok(&ty)) { static uint32_t last = ~0u; if (g_dbg && last != ty) { last = ty; LOG("[touch] world gestures paused: a menu / panel / dialog / modal window is on screen (UI type %#x)", ty); } return 0; }
    return 1;
}
// ---- map view (owner 15.11, run tw4 on pack 2z: "the map can't be dragged by finger"; the iPad's journal_csp_t drags it, pinches, taps markers) ----
// The console map is journal_csp.swf (UI type 0x8c: tw4 "world gestures paused ... UI type 0x8c" after '-'), opened by the pad's '-' (controller_1.json
// ToggleMap <- c:back; also bottomBar_d's map button and the minimap tap = dmPadBtn(27)). Its controls are the pad's: UIMapUp/Down/Left/Right <- the
// left stick, UIMapZoomIn/Out <- the right stick y, A on a marker. The host has no view of the movie's tab, so "the map is up" = a '-' press opened it
// (every '-' the npad sees flips the flag, hle_hid.c fill -> dm_dos2_touch_pad_pump) and journal_csp (or journal_c 0x46) is visible; the flag drops
// once neither is visible. While it is up: drag = left stick against the finger's motion (the map follows the finger); a short tap is first a
// normal click for the UI (journal_csp_d's close / tab / legend buttons), and only when no journal movie answers it with an ExternalInterface call
// within DM_DOS2_TOUCH_MAP_WAIT_MS (220; hle_dos2_touchui_ei.c dm_dos2_ei_journal_reacted: a name quiet before the click) is it a map tap = left
// stick towards the point for a time proportional to its distance from the map's centre (the map cursor), then A (run tw6: the X moved the cursor);
// pinch = the right stick y of the npad held for a time proportional to the change of the finger distance (DM_DOS2_TOUCH_MAP_PINCH_MS per screen
// height, default 2500; tw6: the pulse pinch of the world was too weak for the map).
// Minimap (tw6: a tap on minimap_d did not open the map): a world tap whose left press no UI object took, inside the minimap's area (top right,
// minimap_c 357x500 anchored topRight, its map_mc 43..355 x 21..447 at the window height / 1080; DM_DOS2_TOUCH_MINIMAP=x0,y0,x1,y1 in 0..1 of the
// window), is the pad's '-' instead of a world click.
// Switches: DM_DOS2_TOUCH_MAP=0 off, =2 whenever journal_csp / journal_c is visible (any tab); DM_DOS2_TOUCH_MAP_C=x,y centre (0..1, default 0.5,0.5);
// DM_DOS2_TOUCH_MAP_TAP_MS (ms of full stick per screen height, default 900); DM_DOS2_TOUCH_MAP_DRAG (gain, default 1.0; negative = reversed).
static int g_map_cfg = 1, g_map_live, g_map_flag, g_map_tap_ms = 900, g_fmap, g_map_wait_ms = 220, g_map_pinch_ms = 2500;
static float g_mtap_x, g_mtap_y, g_mm[4] = {-1, -1, -1, -1};
static uint64_t g_mtap_t;   // a map tap waits for the UI's answer (0 = none)
int dm_dos2_ei_journal_reacted(uint64_t since, uint64_t quiet_ms);   // hle_dos2_touchui_ei.c
static float g_map_cx = 0.5f, g_map_cy = 0.5f, g_map_drag = 1.0f;
static uint64_t g_map_t;
static _Atomic int g_map_toggles;
static unsigned g_map_polls, g_n_mdrag, g_n_mtap;
int dm_dos2_route_visible(uint32_t want);   // hle_dos2_touchui_route.c
static void dt_map_update(void) {   // game thread, every few polls
    if (!g_map_cfg) { g_map_live = 0; return; }
    const uint64_t now = dt_ms();
    const int t = atomic_exchange(&g_map_toggles, 0);
    if (t & 1) { g_map_flag = !g_map_flag; g_map_t = now; if (g_dbg) LOG("[touch] map: pad '-' -> map flag %d", g_map_flag); }
    const int a = dm_dos2_route_visible(0x8c), b = dm_dos2_route_visible(0x46), vis = a > 0 || b > 0;
    if (a < 0) { g_map_live = 0; return; }
    if (!vis && g_map_flag && now - g_map_t > 1500) { g_map_flag = 0; if (g_dbg) LOG("[touch] map: journal_csp / journal_c gone: map flag 0"); }
    const int live = g_in_game && vis && (g_map_flag || g_map_cfg == 2);
    if (live != g_map_live) { static unsigned n; if (g_dbg || n++ < 8) LOG("[touch] map gestures %s (journal_csp %d, journal_c %d, flag %d)", live ? "ON: drag = pan, pinch = zoom, tap = marker" : "off", a, b, g_map_flag); }
    g_map_live = live;
}
static void dt_map_tap(float x, float y) {   // the map cursor (its centre) towards the point, then A
    const float dx = x - g_map_cx * g_win_w, dy = y - g_map_cy * g_win_h, r = hypotf(dx, dy), d = r / g_win_h;
    int ms = d < 0.03f ? 0 : (int)(d * (float)g_map_tap_ms); if (ms > 2500) ms = 2500;
    if (ms) { atomic_store(&g_w_lx, (int)(dx / r * 32767.f)); atomic_store(&g_w_ly, (int)(-dy / r * 32767.f)); g_w_until = dt_ms() + (uint64_t)ms; }
    const int ok = dm_dos2_touch_pad_pulse(1u << 0 /* DM_BTN_A */, "map tap: cursor + A", ms + 80);
    g_n_mtap++;
    if (g_dbg || g_n_mtap <= 4) LOG("[touch] map tap at %.0f,%.0f: left stick %d,%d for %d ms, then pad A%s", (double)x, (double)y, atomic_load(&g_w_lx), atomic_load(&g_w_ly), ms, ok ? "" : " (NOT pressed: a pulse is running)");
}
static void dt_map_drag(float x, float y) {   // the finger moved from g.lx,g.ly: stick against it (the map follows the finger), held 70 ms past the last motion
    const float k = g_win_h * 0.012f, dx = x - g.lx, dy = y - g.ly;
    float sx = -dx / k * g_map_drag, sy = dy / k * g_map_drag;   // npad y: up is positive; finger down = the view goes up
    const float m = fmaxf(fabsf(sx), fabsf(sy));
    if (m > 1) { sx /= m; sy /= m; }
    atomic_store(&g_w_lx, (int)(sx * 32767.f)); atomic_store(&g_w_ly, (int)(sy * 32767.f)); g_w_until = dt_ms() + 70;
}
static unsigned g_n_mui, g_n_mini;
static int dt_minimap_hit(float x, float y) {   // window px inside the minimap's map (see the map view section)
    if (!g_map_cfg) return 0;
    if (g_mm[0] >= 0) return x >= g_mm[0] * g_win_w && x <= g_mm[2] * g_win_w && y >= g_mm[1] * g_win_h && y <= g_mm[3] * g_win_h;
    const float s = g_win_h / 1080.f;
    return x >= g_win_w - (357 - 43) * s && x <= g_win_w - 2 * s && y >= 21 * s && y <= 447 * s;
}
static void dt_minimap_tap(float x, float y) {
    const int ok = dm_dos2_touch_pad_pulse(1u << 11 /* DM_BTN_MINUS */, "minimap tap: '-' = ToggleMap", 0);
    g_n_mini++;
    if (g_dbg || g_n_mini <= 6) LOG("[touch] minimap tap at %.0f,%.0f (no UI object took the press): pad '-' (open the map)%s", (double)x, (double)y, ok ? "" : " (NOT pressed: a pulse is running)");
}
static int dt_hud_hit(float x, float y);    // inside a live HUD rect a movie reported (below, after the rects)
static int dt_hint_tap(float x, float y);   // a world tap on a button hint a movie reported ("hint<code>" HUD rect): that pad button (below, after the rects)
static int dt_hold_hint(float x, float y);  // dos2-touch6: the pad code of a "hold<code>" HUD rect under the finger (0 = none), below
static uint64_t dt_btn_of_hint(int code);   // LS_Classes.controllerHelper BTN_* -> npad button (below)
static void dt_pad_hold(uint64_t mask, const char *why);   // dos2-touch6: a pad button held while the finger stays down (0 = let go), below
// ---- ZL / ZR wheels by touch (branch dos2-touch7, owner 18.11: "the ZL / ZR menus select with the right stick, a phone has none") ----
// panelSelect_c (0x53) and partyManagement_c (0x52): what they are, how the engine feeds them and the rules: hle_dos2_touch_gesture.h
// (dt_radial_*, tested in dm/host/tests/dos2_touch_gesture_test.c). While one is visible (hle_dos2_touchui_route.c dm_dos2_route_player):
//   - a finger down / moving on the screen = the right stick pointing from the wheel's centre to the finger (past DT_RAD_DEAD of the window
//     height): the wheel highlights the item under the finger (drag over the wheel = the highlight follows);
//   - the finger lifted past the dead radius = choose that item: the stick stays ~DM_DOS2_TOUCH_RADIAL_MS (120) so the wheel has seen it, then
//     the held ZL / ZR is let go FOR THE GAME (its release is what chooses: openPanel / setActiveChar) and kept let go until the player lets it go;
//     lifted inside the dead radius = nothing (the pad's way: let the trigger go yourself);
//   - the pad's LEFT stick points the wheel too while its right stick rests (on-screen / hardware pad; the character does not walk meanwhile).
// The pad's right stick wins over both (they only fill a resting stick). Switches: DM_DOS2_TOUCH_RADIAL=0 off (touch and left stick);
// DM_DOS2_TOUCH_RADIAL_LSTICK=0 = no left stick; DM_DOS2_TOUCH_RADIAL_MS=<ms> the choose delay.
#define DT_RAD_DEAD 0.07f   // of the window height: the wheel's centre (no item); the wheels' radius is 250 / 180 stage px of 1080
const void *dm_dos2_route_player(uint32_t want);   // hle_dos2_touchui_route.c
static void dt_radial_update(void) {   // game thread, every poll while a wheel may be open (cheap: one walk of the UI objects)
    if (!g_rad_cfg) return;
    static const struct { uint32_t type; float sw, sh, wx, wy; const char *name; } k[2] = {{0x53, 1200, 1080, 600, 470, "panelSelect_c (panels)"},
                                                                                         {0x52, 1920, 1080, 960, 470, "partyManagement_c (party)"}};
    int live = 0;
    for (int i = 0; i < 2 && !live; i++) {
        const void *pl = dm_dos2_route_player(k[i].type);
        if (!pl) continue;
        live = (int)k[i].type;
        float cx, cy;
        if (!dt_radial_centre((const int32_t *)((const uint8_t *)pl + 0x10), k[i].sw, k[i].sh, k[i].wx, k[i].wy, &cx, &cy)) { cx = g_win_w / 2; cy = g_win_h * (470.f / 1080.f); }
        g_rad_cx = cx; g_rad_cy = cy;
        if (atomic_load(&g_rad_live) != live) { g_n_rad_open++; if (g_dbg || g_n_rad_open <= 8) LOG("[touch] radial: %s (UI type %#x) is open, centre %.0f,%.0f (window px): a finger on it = the right stick "
            "towards the finger, lifted = choose (ZL / ZR let go for the game)%s", k[i].name, k[i].type, (double)cx, (double)cy, g_rad_lstick ? "; the pad's left stick points it too" : ""); }
    }
    if (!live && atomic_load(&g_rad_live)) {
        if (g.st == G_RADIAL) g.st = G_IDLE;
        atomic_store(&g_rad_rx, 0); atomic_store(&g_rad_ry, 0); g_rad_clear_at = 0;
        if (g_dbg) LOG("[touch] radial: closed");
    }
    atomic_store(&g_rad_live, live);
    if (g_rad_clear_at && dt_ms() >= g_rad_clear_at) { g_rad_clear_at = 0; atomic_store(&g_rad_rx, 0); atomic_store(&g_rad_ry, 0); }
}
static void dt_radial_finger(float x, float y) {   // the finger points the wheel
    int sx, sy;
    dt_radial_stick(x, y, g_rad_cx, g_rad_cy, DT_RAD_DEAD * g_win_h, &sx, &sy);
    atomic_store(&g_rad_rx, sx); atomic_store(&g_rad_ry, sy); g_rad_clear_at = 0;
}
static void dt_radial_up(float x, float y, int up) {
    int sx, sy;
    if (up && dt_radial_stick(x, y, g_rad_cx, g_rad_cy, DT_RAD_DEAD * g_win_h, &sx, &sy)) {
        atomic_store(&g_rad_rx, sx); atomic_store(&g_rad_ry, sy);
        const uint64_t now = dt_ms();
        atomic_store(&g_rad_choose_at, now + (uint64_t)g_rad_ms); g_rad_clear_at = now + (uint64_t)g_rad_ms + 150;
        g_n_rad_pick++;
        if (g_dbg || g_n_rad_pick <= 12) LOG("[touch] radial: finger up at %.0f,%.0f (wheel centre %.0f,%.0f, direction %.0f deg): choose - right stick %d,%d, the held ZL / ZR let go for the game in %d ms",
                                            (double)x, (double)y, (double)g_rad_cx, (double)g_rad_cy, (double)(dt_radial_angle(sx, sy) * 57.29578f), sx, sy, g_rad_ms);
    } else { atomic_store(&g_rad_rx, 0); atomic_store(&g_rad_ry, 0); g_rad_clear_at = 0; }
}
static int g_pad_dev = -1;
static float px_x(float nx) { return nx * g_win_w; }
static float px_y(float ny) { return ny * g_win_h; }

static void dt_step(void *mgr, const dt_ev *e) {
    const float x = px_x(e->x), y = px_y(e->y), slop = g_win_h * 0.025f;
    g_tx = e->x; g_ty = e->y;
    switch (e->ph) {
    case EV_DOWN:
        if (g.st == G_IDLE) g_fmap = 0;
        if (g.st == G_IDLE && atomic_load(&g_rad_live)) {   // dos2-touch7: a ZL / ZR wheel is open: the finger points it
            g.f0 = e->id; g.sx = g.lx = x; g.sy = g.ly = y; g.t0 = e->t; g.wact = 0; dt_world_stop(); g.st = G_RADIAL; dt_radial_finger(x, y);
        }
        else if (g.st == G_IDLE && g_map_live) {   // map view: drag = pan; a tap is decided on the release (click first, the map only if no button answered)
            g.f0 = e->id; g.sx = g.lx = x; g.sy = g.ly = y; g.t0 = e->t; g.wact = 0; dt_world_stop(); g_fmap = 1; g_mtap_t = 0;
            dt_move(mgr, x, y); g.st = G_MAP;
        }
        else if (g.st == G_IDLE && dt_world_now() && !(g_world == 2 && dt_world_zone_ui(e->x, e->y))) {   // pass 7: the world's first step
            g.f0 = e->id; g.sx = g.lx = x; g.sy = g.ly = y; g.t0 = e->t; g.wact = 0; g.wmoved = 0; g.wplace = 0; dt_world_stop();
            if (g_world == 2) g.st = G_WORLD;   // zones: no mouse record for a world finger
            else if (g_w3 && g_place && dm_dos2_touch_world_place_mode() == 1 && dm_dos2_touch_world_placing() && !dt_hud_hit(x, y)) {
                // dos2-place, phone place1.log: in the item placement mode the probe's mouse press / release CANCELS the move (preview 10 -> 25 / 8,
                // then running -1) and the picking stops, so the finger is the world's at once: no mouse record at all, only the UI cursor for
                // the ray probe; a tap places (dt_w3_tap -> dt_place_tap), a swipe turns the camera, a hold does nothing. HUD rects keep the UI path.
                dt_cursor(x, y); g_cx = x; g_cy = y; g.wplace = 1; g.st = G_WORLD;
                { static unsigned n; if (g_dbg || n++ < 20) LOG("[touch] placement mode: finger at %.0f,%.0f is the world's without a mouse press (a click cancels the move)", (double)x, (double)y); }
            }
            else { dt_move(mgr, x, y); dt_button(mgr, ID_LEFT, 1); g.st = G_WPROBE; }   // the UI answers who took the press (dt_world_poll)
        }
        else if (g.st == G_IDLE) { g.st = G_PENDING; g.f0 = e->id; g.sx = x; g.sy = y; g.t0 = e->t; dt_move(mgr, x, y); }
        else if ((g.st == G_PENDING || g.st == G_DRAG || g.st == G_WPROBE || g.st == G_WORLD || g.st == G_MAP) && e->id != g.f0) {
            if (g.st == G_DRAG || g.st == G_WPROBE) dt_button(mgr, ID_LEFT, 0);
            if (g.st == G_WORLD && g.wact == 4) dt_pad_hold(0, "a second finger");
            if (g.st == G_WORLD || g.st == G_WPROBE || g.st == G_MAP) { dt_world_stop(); g.x0 = g.lx; g.y0 = g.ly; } else { g.x0 = g_cx; g.y0 = g_cy; }
            g.st = G_TWO; g.f1 = e->id; g.x1 = x; g.y1 = y; g.wmx = (g.x0 + g.x1) / 2;
            g.d0 = hypotf(g.x1 - g.x0, g.y1 - g.y0); g.wy = (g.y0 + g.y1) / 2;
        } else if (g.st == G_TWO && dt_pc() && e->id != g.f0 && e->id != g.f1) {   // PC mode, a third finger: escape (ToggleInGameMenu / UICancel / FlashCancel)
            dt_send(mgr, DEV_KEYS, ID_KEY_ESCAPE, 1, 1, 1); dt_send(mgr, DEV_KEYS, ID_KEY_ESCAPE, 0, 0, 0);
        }
        break;
    case EV_MOVE:
        if (g.st == G_TWO) {
            if (e->id == g.f0) { g.x0 = x; g.y0 = y; } else if (e->id == g.f1) { g.x1 = x; g.y1 = y; } else break;
            float d = hypotf(g.x1 - g.x0, g.y1 - g.y0), my = (g.y0 + g.y1) / 2, step = g_win_h * 0.04f;
            if (g_fmap && fabsf(d - g.d0) > g_win_h * 0.01f) {   // map view: right stick y held in proportion to the distance change (apart = zoom in = stick up)
                const uint64_t now = dt_ms(), add = (uint64_t)(fabsf(d - g.d0) / g_win_h * (float)g_map_pinch_ms);
                const int want = ((d > g.d0) != !!getenv("DM_DOS2_TOUCH_PINCH_SWAP")) ? 32767 : -32767;
                if (atomic_load(&g_w_ry) != want || !g_w_ry_until || g_w_ry_until < now) g_w_ry_until = now;   // direction changed / ran out: start from now
                atomic_store(&g_w_ry, want); g_w_ry_until += add; if (g_w_ry_until > now + 1500) g_w_ry_until = now + 1500;
                g.d0 = d; g.wy = my;
            } else if (g_fmap) {   // map view: no wheel / rotation from two fingers
            } else if (dt_pc() && fabsf(d - g.d0) > step) {   // PC mode: pinch = the wheel (keyboard_1.json CameraZoomIn <- wheel_ypos; lists scroll too)
                uint32_t id = (d > g.d0) != !!getenv("DM_DOS2_TOUCH_PINCH_SWAP") ? ID_WHEEL_YPOS : ID_WHEEL_YNEG;
                dt_button(mgr, id, 1); dt_button(mgr, id, 0); g.d0 = d; g.wy = my;
            } else if (g_pad_dev >= 0 && g_pad_dev < 64 && fabsf(d - g.d0) > step) {   // pinch: the controller's touch_pinch on the pad device (controller_1.json CameraZoomIn/Out)
                const int out = (d > g.d0) != !!getenv("DM_DOS2_TOUCH_PINCH_SWAP");   // fingers apart = zoom in
                const uint32_t id = g_pinch_stick ? (out ? ID_RSTICK_YNEG : ID_RSTICK_YPOS) : (out ? ID_PINCH_OUT : ID_PINCH_IN);
                const uint32_t raw = (uint32_t)g_pad_dev << 16 | id;
                const int held = g.pulse_frames > 0;
                if (held && g.pulse != raw) dt_send(mgr, g.pulse >> 16, g.pulse & 0xffff, 0, 0, 0);   // direction changed: let the other one go
                if (!held || g.pulse != raw) dt_send(mgr, raw >> 16, id, g_pinch_stick ? g_pinch_v : 1, 1, 1);   // the stick code's {scaled, |axis|}, pressed
                g.pulse = raw; g.pulse_frames = g_pinch_stick ? g_pinch_frames : 2; g.d0 = d; g.wy = my;
            } else if (g_world && g_in_game && fabsf((g.x0 + g.x1) / 2 - g.wmx) > step && fabsf((g.x0 + g.x1) / 2 - g.wmx) > fabsf(my - g.wy)) {   // pass 7: horizontal two-finger drag = camera rotation
                const float mx = (g.x0 + g.x1) / 2;
                atomic_store(&g_w_rx, ((mx > g.wmx) != (g_rot_swap != 0)) ? 32767 : -32767); g_w_rx_until = dt_ms() + 140;
                g.wmx = mx; g.wy = my; g.d0 = d;
            } else if (fabsf(my - g.wy) > step) {   // two-finger drag: wheel (FlashScrollUp/Down, inputconfig.json)
                uint32_t id = my < g.wy ? ID_WHEEL_YNEG : ID_WHEEL_YPOS;
                dt_button(mgr, id, 1); dt_button(mgr, id, 0); g.wy = my; g.d0 = d;
            }
        } else if (e->id == g.f0 && g.st == G_RADIAL) {   // dos2-touch7: the highlight follows the finger
            g.lx = x; g.ly = y; dt_radial_finger(x, y);
        } else if (e->id == g.f0 && g.st == G_MAP) {   // map: pan
            if (!g.wact && hypotf(x - g.sx, y - g.sy) > slop) { g.wact = 1; g_n_mdrag++; }
            if (g.wact) dt_map_drag(x, y);
            g.lx = x; g.ly = y;
        } else if (e->id == g.f0 && (g.st == G_WORLD || g.st == G_WPROBE)) {
            g.lx = x; g.ly = y;
            if (g.st == G_WPROBE) { dt_move(mgr, x, y); if (hypotf(x - g.sx, y - g.sy) > slop) g.wmoved = 1; }   // decided when the UI answers (dt_world_poll)
            else {
                if (!g.wact) {   // pass 9: moved past the slop before the hold time = swipe (camera), after it = the walk stick (as before)
                    const int k = dt_gesture_move(hypotf(x - g.sx, y - g.sy), e->t - g.t0, slop, g_world_hold_ms, dt_swipe_zone());
                    if (k == DT_GW_SWIPE) dt_swipe_start(x, y); else if (k == DT_GW_STICK) { g.wact = 1; g_n_wstick++; }
                }
                if (g.wact == 1) dt_world_stick(x, y, 0);   // 2 = the long press opened Actions...
                else if (g.wact == 3) dt_swipe_move(x, y);
            }
        } else if (e->id == g.f0 && g.st != G_IDLE && g.st != G_WTAP) {
            if (g.st == G_PENDING && hypotf(x - g.sx, y - g.sy) > slop) { dt_button(mgr, ID_LEFT, 1); g.st = G_DRAG; }   // press where it started
            dt_move(mgr, x, y);
        }
        break;
    case EV_UP: case EV_CANCEL:
        if (g.st == G_TWO) { if (e->id == g.f0 || e->id == g.f1) { g.st = G_IDLE; if (g.pulse_frames > 1) g.pulse_frames = 1; } break; }   // pinch over: zoom released on the next poll
        if (e->id != g.f0) break;
        if (g.st == G_WPROBE) { g.lx = x; g.ly = y; if (e->ph == EV_UP) { g.st = G_WTAP; break; } dt_button(mgr, ID_LEFT, 0); g.st = G_IDLE; break; }   // up before the UI answered: dt_world_poll ends it
        if (g.st == G_WTAP) break;
        if (g.st == G_RADIAL) { dt_radial_up(x, y, e->ph == EV_UP && atomic_load(&g_rad_live)); g.st = G_IDLE; break; }   // dos2-touch7: lifted on an item = choose it
        if (g.st == G_MAP) {
            dt_world_stop();
            if (!g.wact && e->ph == EV_UP && e->t - g.t0 < 400) {   // a click for the UI now; the map tap only if no journal button answers (dt_world_poll)
                dt_move(mgr, x, y); dt_button(mgr, ID_LEFT, 1); dt_button(mgr, ID_LEFT, 0);
                { void dm_dos2_touchui_ie_gesture(int kind); dm_dos2_touchui_ie_gesture(0); }
                g_mtap_x = x; g_mtap_y = y; g_mtap_t = dt_ms(); if (!g_mtap_t) g_mtap_t = 1;
            }
            g.st = G_IDLE; break;
        }
        if (g.st == G_WORLD && g.wact == 3) { dt_swipe_end(); g.st = G_IDLE; break; }   // pass 9: a swipe ends: no tap
        if (g.st == G_WORLD && g.wact == 4) { dt_pad_hold(0, "finger up"); g.st = G_IDLE; break; }   // dos2-touch6: a held hint lets its button go: no tap
        if (g.st == G_WORLD) { if (!g.wact && e->ph == EV_UP && e->t - g.t0 < 400 && dt_hint_tap(x, y)) {} else if (!g.wact && e->ph == EV_UP && e->t - g.t0 < 400 && dt_minimap_hit(x, y)) dt_minimap_tap(x, y); else if (g_w3 && !g.wact && e->ph == EV_UP) dt_w3_tap(mgr, x, y); else if (!g_w3 && !g.wact && e->ph == EV_UP && e->t - g.t0 < 350) dt_world_tap(x, y); else dt_world_stop(); g.st = G_IDLE; break; }
        if (g.st == G_PENDING && e->ph == EV_UP) { dt_move(mgr, x, y); dt_button(mgr, ID_LEFT, 1); dt_button(mgr, ID_LEFT, 0);
            { void dm_dos2_touchui_ie_gesture(int kind); dm_dos2_touchui_ie_gesture(0); } }   // the iPad movies act on the input event "IE Tap", not on the click (hle_dos2_touchui_ie.c)
        else if (g.st == G_DRAG) { dt_move(mgr, x, y); dt_button(mgr, ID_LEFT, 0); }
        g.st = G_IDLE;
        break;
    }
}
// ---- live switch between the PC UI and the controller UI (DM_DOS2_TOUCH_LIVE; the header's pass 4) ----
static uint64_t dt_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull; }
static int dt_ui_records(const uint8_t *ui) {   // players that have a UI set now (the hash ui+0xf0): 0 = not in game
    const uint32_t nb = *(const volatile uint32_t *)(ui + DT_UI_HASH_OFF + 4);
    void *const volatile *bk = *(void *const volatile *const volatile *)(ui + DT_UI_HASH_OFF + 8);
    int n = 0;
    if (!bk || !nb || nb > 0x10000) return 0;
    for (uint32_t i = 0; i < nb; i++) for (void *const volatile *nd = (void *const volatile *)bk[i]; nd && n < 64; nd = (void *const volatile *)*nd) n++;
    return n;
}
static void dt_live_switch(int pc, const char *why) {
    volatile uint8_t *mode = (volatile uint8_t *)(g_main->base + DT_MODE);
    const uint8_t want = pc ? 0 : 1;
    if (*mode == want) { g_live_wait = 0; return; }
    const uint64_t now = dt_ms();
    if (g_live_t && now - g_live_t < (uint64_t)g_live_ms) return;   // hysteresis
    uint8_t *ui = *(uint8_t *volatile *)(g_main->base + DT_UI_THIS);
    void *uimgr = *(void *volatile *)(g_main->base + DT_UIMGR);
    const int recs = ui ? dt_ui_records(ui) : 0;
    if (dt_live() == 2 && recs) {   // minimal variant: never rebuild in game
        if (g_live_wait != 1 + pc && g_dbg)
            LOG("[touch] live: %s: %s UI wanted, %d player UI set(s) exist: DM_DOS2_TOUCH_LIVE=2 waits for the menu / a load", why, pc ? "PC" : "controller", recs);
        g_live_wait = 1 + pc; return;
    }
    { int dm_dos2_touchui_hud_gate(int to_touch, int records);   // DM_DOS2_TOUCHUI_HUD: only at a safe point (hle_dos2_touchui_hud.c); back to the pad is asked again every poll
      if (!dm_dos2_touchui_hud_gate(pc, recs)) { g_live_wait = pc ? 0 : 1; return; } }
    g_live_wait = 0;
    const uint64_t t0 = dt_us();
    uint64_t t1 = t0, t2 = t0;
    *mode = want;   // the iPad's order: the mode first, then the rebuild
    if (recs && uimgr) {
        ((void (*)(void *))(void *)(g_main->base + DT_UI_DESTROY))(ui); t1 = dt_us();
        ((void (*)(void *))(void *)(g_main->base + DT_UI_CREATE))(ui);  t2 = dt_us();
    }
    g_live_t = now; g_live_n++;
    if (g_dbg || g_live_n <= 4)
        LOG("[touch] live #%u: %s -> %s UI (mode byte main+%#x = %u); %s; destroy %llu us, create %llu us, player UI sets %d -> %d", g_live_n, why,
            pc ? "PC (keyboard and mouse)" : "controller", DT_MODE, want,
            recs && uimgr ? "UI rebuilt: main+0x10c4730 + main+0x10c4580" : "no player UI set (menu / loading): byte only",
            (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1), recs, ui ? dt_ui_records(ui) : 0);
}
static int dt_pc(void) { return dt_live() ? !*(volatile uint8_t *)(g_main->base + DT_MODE) : g_pcui; }   // the PC UI is up now
// ---- the UI mode switch's HUD part (hle_dos2_uimode.c, game thread) ----
static int dt_poke_ro(module_t *m, uint32_t off, const void *src, size_t n);
static int dt_bind(int keys) {   // the bindings path main+0x1ebfc35: Eoc/keyboard_1.json (1) or Eoc/controller_1.json (0); read by main+0xc05ab0 when it loads them
    if (!g_bind_ok) return 0;
    if (keys == g_bind_keys) return 1;
    if (dt_poke_ro(g_main, DT_BIND_STR, keys ? k_bind_keys : k_bind_pad, keys ? sizeof k_bind_keys : sizeof k_bind_pad)) { LOG("【红】[touch] mprotect RW failed: the bindings path is left alone"); return 0; }
    g_bind_keys = keys;
    LOG("[touch] bindings path main+%#x -> %s (loaded by main+0xc05ab0; when the game loads them again was not traced: maybe only at the start)", DT_BIND_STR, keys ? k_bind_keys : k_bind_pad);
    return 1;
}
int dm_dos2_touch_ui_records(void);   // player UI sets now (0 = not in game: main menu / loading), -1 = cannot tell (the hash sites differ)
int dm_dos2_touch_ui_records(void) {
    if (!g_on || !g_ui_ok || !g_main) return -1;
    uint8_t *ui = *(uint8_t *volatile *)(g_main->base + DT_UI_THIS);
    return ui ? dt_ui_records(ui) : 0;
}
// The HUD part of UI mode `mode`: the mode byte (1, or 0 for TOUCH), the bindings path, TOUCH's live pad switch; rebuild = also destroy-all +
// create-all of the player UI manager now (plan 3 B; the hud gate of hle_dos2_touchui_hud.c is asked as by dt_live_switch). 1 done, 0 refused.
int dm_dos2_touch_ui_hud(int mode, int rebuild, uint64_t *us);
int dm_dos2_touch_ui_hud(int mode, int rebuild, uint64_t *us) {
    if (!g_on || !g_main || !ui_mode_valid(mode)) return 0;
    const int pc = mode == UI_TOUCH;
    uint8_t *ui = *(uint8_t *volatile *)(g_main->base + DT_UI_THIS);
    void *uimgr = *(void *volatile *)(g_main->base + DT_UIMGR);
    const int recs = ui && g_ui_ok ? dt_ui_records(ui) : 0;
    if (rebuild) {
        int dm_dos2_touchui_hud_gate(int to_touch, int records);
        if (!g_live_ok || !recs || !uimgr || !dm_dos2_touchui_hud_gate(pc, recs)) return 0;
    }
    if (pc && !g_mode_ok) { LOG("【红】[touch] UI mode touch: main+%#x / +%#x differ: the mode byte is not set (the controller UI stays)", DT_MODE_INIT, DT_UISET); return 0; }
    if (g_mode_ok) *(volatile uint8_t *)(g_main->base + DT_MODE) = (uint8_t)ui_mode_byte(mode);
    dt_bind(pc && g_keys_pref);
    g_live_ui = pc && g_live_ok; g_pcui = pc; g_live_wait = 0; g_live_t = dt_ms();   // the pad's live switch waits its hysteresis from here
    if (rebuild) {
        const uint64_t t0 = dt_us();
        ((void (*)(void *))(void *)(g_main->base + DT_UI_DESTROY))(ui);
        ((void (*)(void *))(void *)(g_main->base + DT_UI_CREATE))(ui);
        if (us) *us = dt_us() - t0;
        LOG("[touch] UI mode %s: player UI sets rebuilt (main+0x10c4730 + main+0x10c4580, mode byte %d), %d -> %d set(s)", ui_mode_name(mode), ui_mode_byte(mode), recs, dt_ui_records(ui));
    }
    return 1;
}

// [mgr+0x18] (the per-device raw-state array) must exist before a mouse record reaches ProcessInputEvent (see the tap crash above)
static void *dt_block(void) { return calloc(256, 12); }
static int dt_state(void *mgr) {   // 1 = records may go, 0 = drop the touches of this poll (g_on cleared when it is for good)
    void **volatile *at = (void **volatile *)((uint8_t *)mgr + DT_MGR_DEVS);
    volatile uint32_t *cap = (volatile uint32_t *)((uint8_t *)mgr + DT_MGR_DEVCAP), *n = (volatile uint32_t *)((uint8_t *)mgr + DT_MGR_DEVN);
    void **arr = *at;
    const uint32_t need = (uint32_t)(g_dev > DEV_KEYS ? g_dev : DEV_KEYS) + 1;
    if (g_dev >= DT_DEVS) { LOG("【红】[touch] mouse device %d out of the state array: touch as mouse off", g_dev); g_on = 0; return 0; }
    if (!arr || *n < need) {   // the game makes the array when the pad's device registers: wait for it
        if (!g_state_alloc) { LOG("【红】[touch] no usable per-device state array and DM_DOS2_TOUCH_STATE=0: touch as mouse off"); g_on = 0; return 0; }
        static uint64_t t0; if (!t0) t0 = dt_ms();   // [DOS2] wait by wall time (~16 ms a poll), not by polls with touches: run tw3 (15.11) - on the title screen
        // nobody had touched the pad yet, so the array never came and 300 tapping polls were needed before the host gave one
        if ((g_state_waited++ < g_state_wait && dt_ms() - t0 < (uint64_t)g_state_wait * 16u) || (arr && !g_devn)) {
            if (!g_n_dropped++) LOG("[touch] the game's raw-state array is not ready ([mgr+0x18] %p, size %u, need %u): touches are dropped until the pad's device registers (at most %d polls)",
                                    (void *)arr, *n, need, g_state_wait);
            return 0;
        }
        if (!arr) {
            arr = g_alloc_ok ? ((void **(*)(uint64_t, uint64_t))(void *)(g_main->base + DT_ALLOC))(DT_DEVS * sizeof *arr, 0) : calloc(DT_DEVS, sizeof *arr);
            if (!arr) { LOG("【红】[touch] no memory for the state array: touch as mouse off"); g_on = 0; return 0; }
            memset(arr, 0, DT_DEVS * sizeof *arr);
            *at = arr; *cap = DT_DEVS; *n = need;
            LOG("[touch] the game made no raw-state array in %d polls: the host gave one (%s, capacity %d, size %u)", g_state_wait, g_alloc_ok ? "engine allocator main+0x83cc00" : "calloc", DT_DEVS, need);
        } else if (*cap >= need && g_devn_ok) {
            LOG("[touch] raw-state array size [mgr+%#x] %u -> %u (capacity %u; main+%#x reads the stored state only below the size)", DT_MGR_DEVN, *n, need, *cap, DT_STATE_RD);
            for (uint32_t i = *n; i < need; i++) arr[i] = NULL;
            *n = need;
        } else { if (g_n_dropped++ < 3) LOG("【红】[touch] the game's raw-state array is too short (size %u, capacity %u, need %u): touches dropped", *n, *cap, need); return 0; }
    }
    g_state_waited = 0;
    if (!arr[g_dev] || !arr[DEV_KEYS]) {   // also after the game rebuilt its array
        const int had_m = !!arr[g_dev], had_k = !!arr[DEV_KEYS];
        if (!arr[g_dev] && !(arr[g_dev] = dt_block())) { LOG("【红】[touch] calloc failed: touch as mouse off"); g_on = 0; return 0; }
        if (!arr[DEV_KEYS]) arr[DEV_KEYS] = dt_block();
        if (g_n_readd++ < 8) LOG("[touch] raw-state blocks (256 ids x 12 bytes) given: mouse device %d %s, keyboard device %d %s | array %p size %u capacity %u%s", g_dev, had_m ? "kept" : "new",
                                 DEV_KEYS, had_k ? "kept" : "new", (void *)arr, *n, *cap, g_n_readd > 1 ? " (again: the game replaced its array)" : "");
    }
    g_state_ok = 1;
    return 1;
}
// DM_DBG: what reaches Iggy (FlashPlayer vt+0x190 mouse move, the button sender); same thread as the poll
static uint64_t dt_iggy_move_hook(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    g_iggy_move_n++; g_iggy_x = (int)(int32_t)a1; g_iggy_y = (int)(int32_t)a2;
    if (a0) { g_iggy_ox = *(const int32_t *)(uintptr_t)(a0 + 0x10); g_iggy_oy = *(const int32_t *)(uintptr_t)(a0 + 0x14); }
    return g_iggy_move_orig(a0, a1, a2, a3, a4, a5);
}
static uint64_t dt_iggy_btn_hook(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    static unsigned nlog, nlog1;
    static uint64_t up_who[8]; static unsigned up_code[8], up_at;   // DM_DOS2_TOUCH_UPONCE: the movies that got an "up" since the last "down"
    const unsigned code = (unsigned)a1, flag = (unsigned)a2 & 1;
    g_iggy_btn_n++; g_iggy_btn_code = code; g_iggy_btn_flag = flag;
    int ox = 0, oy = 0;
    if (a0) { ox = *(const int32_t *)(uintptr_t)(a0 + 0x10); oy = *(const int32_t *)(uintptr_t)(a0 + 0x14); if (ox < 0) ox = 0; if (oy < 0) oy = 0; }
    int dropped = 0;
    if (g_uponce) {
        if (code & 1) up_at = 0;   // a down: forget
        else {
            for (unsigned i = 0; i < up_at; i++) if (up_who[i] == a0 && up_code[i] == code) dropped = 1;
            if (!dropped && up_at < 8) { up_who[up_at] = a0; up_code[up_at++] = code; }
        }
    }
    float *c = (float *)(uintptr_t)dt_cursor_at();
    const float sx = c ? c[0] : 0, sy = c ? c[1] : 0;
    uint64_t v = 0;
    if (dropped) g_n_updrop++;
    else {
        if (g_btnxy && c) { c[0] = sx - (float)ox; c[1] = sy - (float)oy; }   // the event's x, y in movie space, as the mouse move
        v = g_iggy_btn_orig(a0, a1, a2, a3, a4, a5);
        if (g_btnxy && c) { c[0] = sx; c[1] = sy; }
    }
    if (g_dbg && (flag ? nlog1 < 16 : nlog < 96 || (code <= 2 && nlog < 6000))) { if (flag) nlog1++; else nlog++;   // the target's left button: the whole run (tu05a ended at 96)
        char mv[48]; uint32_t ty = 0; { void dm_dos2_touchui_movie_of(const void *player, uint32_t *type, char *buf, size_t n); dm_dos2_touchui_movie_of((const void *)(uintptr_t)a0, &ty, mv, sizeof mv); }
        LOG("[touch] Iggy button #%u: movie %#llx %s (type %#x) code %u (1/2 left down/up, 3/4 right, 5/6 middle) %s window cursor %.0f,%.0f movie origin %d,%d%s -> handled %u%s | mouse moves so far %u, last %d,%d (window px)",
            g_iggy_btn_n, (unsigned long long)a0, mv, ty, code, flag ? "flag 1 (not its event)" : "flag 0 (THE TARGET)", (double)sx, (double)sy, ox, oy, g_btnxy ? " (sent minus origin)" : "",
            (unsigned)v & 1, dropped ? " DROPPED (second up)" : "", g_iggy_move_n, g_iggy_x, g_iggy_y); }
    return v;
}
// the window the game runs now (docked or handheld size): the cursor and the movies' positions are in its pixels
static void dt_window(void) {
    if (!g_win_ok || g_win_fixed) return;
    void *volatile *wm = *(void *volatile *volatile *)(g_main->base + DT_WINMGR);
    void *volatile *arr = wm ? (void *volatile *)wm[0x40 / 8] : NULL;
    const uint32_t *w = arr ? (const uint32_t *)arr[0] : NULL;
    if (!w) return;
    const uint32_t ww = w[0x18 / 4], wh = w[0x1c / 4];
    if (ww < 320 || wh < 200 || ww > 8192 || wh > 8192 || ((float)ww == g_win_w && (float)wh == g_win_h)) return;
    LOG("[touch] the game's window is %ux%u (was mapping to %.0fx%.0f): touches map to it now", ww, wh, (double)g_win_w, (double)g_win_h);
    g_win_w = (float)ww; g_win_h = (float)wh; g_cx = g_cy = -1;
}
static void dt_summary(void *mgr) {
    unsigned t = g_n[0] + g_n[1] + g_n[2] + g_n[3] + g_n[4] + g_n[5];
    if (t == g_n_last) return;
    g_n_last = t;
    const float *c = dt_cursor_at();
    LOG("[touch] records: motion %u left %u right %u wheel %u pinch %u key %u | cursor %.1f,%.1f (read back %.1f,%.1f) | window %.0fx%.0f | pad device %d | "
        "mode byte %u (1 controller UI, 0 PC UI) | [mgr+0x170] %u", g_n[0], g_n[1], g_n[2], g_n[3], g_n[4], g_n[5], (double)g_cx, (double)g_cy,
        c ? (double)c[0] : -1., c ? (double)c[1] : -1., (double)g_win_w, (double)g_win_h, g_pad_dev,
        g_mode_ok ? (unsigned)*(volatile uint8_t *)(g_main->base + DT_MODE) : 255u, (unsigned)*((volatile uint8_t *)mgr + DT_MGR_KBM));
    const uint8_t *um = g_uimouse_ok ? *(uint8_t *volatile *)(g_main->base + DT_UIMGR) : NULL;
    void **arr = *(void **volatile *)((uint8_t *)mgr + DT_MGR_DEVS);
    const uint32_t an = *(volatile uint32_t *)((uint8_t *)mgr + DT_MGR_DEVN);
    LOG("[touch] pass 5: motion flagged %u, released by hand %u | raw-state array %p size %u capacity %u, mouse block %p keys block %p, touches dropped %u, blocks re-added %u | "
        "UI mouse byte [uimgr+0x10b] %d, capture [uimgr+0x10d] %d object %#llx | Iggy: mouse moves %u (last %d,%d, movie origin %d,%d), buttons %u (last code %u flag %u), second ups dropped %u%s",
        g_n_flag, g_n_rel, (void *)arr, an, *(volatile uint32_t *)((uint8_t *)mgr + DT_MGR_DEVCAP), arr && an > (uint32_t)g_dev ? arr[g_dev] : NULL, arr && an > DEV_KEYS ? arr[DEV_KEYS] : NULL,
        g_n_dropped, g_n_readd, um ? (int)um[DT_UIM_MOUSE] : -1, um ? (int)um[DT_UIM_CAPT] : -1, um ? (unsigned long long)*(const uint64_t *)(um + DT_UIM_CAPT_OBJ) : 0ull,
        g_iggy_move_n, g_iggy_x, g_iggy_y, g_iggy_ox, g_iggy_oy, g_iggy_btn_n, g_iggy_btn_code, g_iggy_btn_flag, g_n_updrop, g_iggy_move_orig ? "" : " (Iggy hooks off)");
}
// pass 7, every pad poll: the answer to a probing left press, the hold that starts the stick, timed sticks
static void dt_world_poll(void *mgr) {
    const uint64_t now = dt_ms();
    if (g_w_until && now >= g_w_until && g.st != G_WORLD) dt_world_stop();
    if (g_w_rx_until && now >= g_w_rx_until) { atomic_store(&g_w_rx, 0); g_w_rx_until = 0; }
    if (g_w_ry_until && now >= g_w_ry_until) { atomic_store(&g_w_ry, 0); g_w_ry_until = 0; }
    if (g_mtap_t && now - g_mtap_t >= (uint64_t)g_map_wait_ms) {
        const uint64_t t = g_mtap_t; g_mtap_t = 0;
        if (dm_dos2_ei_journal_reacted(t, 500)) { g_n_mui++; if (g_dbg || g_n_mui <= 6) LOG("[touch] map tap at %.0f,%.0f: a journal button answered the click: no map tap", (double)g_mtap_x, (double)g_mtap_y); }
        else if (g_map_live) dt_map_tap(g_mtap_x, g_mtap_y);
    }
    if (g.st == G_WPROBE || g.st == G_WTAP) {
        uint32_t ty = 0; const int r = dm_dos2_route_taken(&ty);
        if (r < 0 && now - g.t0 < (uint64_t)g_world_probe_ms) return;   // the UI has not seen the press yet
        const int was_tap = g.st == G_WTAP;
        if (was_tap) {   // the finger is up already: a click for the UI, or a world tap
            dt_button(mgr, ID_LEFT, 0); g.st = G_IDLE;
            if (r != 1 && dt_swipe_zone() && g.wmoved) { if (g_dbg) LOG("[touch] world: a quick swipe (lifted before the UI answered): no tap"); }   // pass 9
            else if (r == 1) { g_n_wui++; { void dm_dos2_touchui_ie_gesture(int kind); dm_dos2_touchui_ie_gesture(0); } } else if (dt_hint_tap(g.lx, g.ly)) {} else if (dt_minimap_hit(g.lx, g.ly)) dt_minimap_tap(g.lx, g.ly); else if (g_w3) dt_w3_tap(mgr, g.lx, g.ly); else dt_world_tap(g.lx, g.ly);
        } else if (r == 1) { g.st = G_DRAG; g_n_wui++; }   // a UI element has it: the finger drags / clicks as the mouse
        else { dt_button(mgr, ID_LEFT, 0); g.st = G_WORLD;
               if (dt_swipe_zone() && g.wmoved) { const int k = dt_gesture_move(hypotf(g.lx - g.sx, g.ly - g.sy), now - g.t0, g_win_h * 0.025f, g_world_hold_ms, 1);   // pass 9: moved during the probe
                   if (k == DT_GW_SWIPE) { dt_swipe_start(g.lx, g.ly); dt_swipe_move(g.lx, g.ly); } else if (k == DT_GW_STICK) { g.wact = 1; g_n_wstick++; } } }
        if (g_dbg) { static unsigned n; if (n++ < 300) LOG("[touch] world: the left press at %.0f,%.0f was %s (%llu ms after the finger%s; UI type %#x)", (double)g.sx, (double)g.sy,
                                                            r == 1 ? "TAKEN by a UI object: mouse gesture" : r == 0 ? "taken by no UI object: the finger is the world's" : "never seen by the UI (timeout): the world's",
                                                            (unsigned long long)(now - g.t0), was_tap ? ", already lifted" : "", ty); }
    }
    if (g.st == G_WORLD && !g.wact && now - g.t0 >= (uint64_t)g_world_hold_ms) {   // dos2-touch6: a finger held on a "hold<code>" hint (bottomBar_d "Hold (B) to end the turn")
        const int code = dt_hold_hint(g.lx, g.ly); const uint64_t btn = code > 0 ? dt_btn_of_hint(code) : 0;
        if (btn) { char why[48]; snprintf(why, sizeof why, "held hint (hold%d)", code); dt_pad_hold(btn, why); g.wact = 4; dt_world_stop(); }
    }
    int on_obj = 0;
    if (g.st == G_WORLD && !g.wact && !g.wplace && now - g.t0 >= (uint64_t)g_world_hold_ms && g_ctx_hold) {   // pack 3y: a long press ON a character / item = "Actions..." (pad X)
        int dm_dos2_touch_world_obj_under(void); on_obj = dm_dos2_touch_world_obj_under();
        if (on_obj && now - g.t0 >= (uint64_t)(g_hold_ms > g_world_hold_ms ? g_hold_ms : g_world_hold_ms)) {   // the long-press time (DM_DOS2_TOUCH_HOLD_MS, 500)
            if (g_ctx_hold == 1) { if (dm_dos2_touch_pad_pulse(DM_BTN_X, "long press on a character / item: Actions... (ControllerContextMenu)", 0)) g.wact = 2; }
            else {   // pack 3z (default 2): the PC's right click = ContextMenu for the object under the cursor (the finger), run in PC mode by tw_input
                void dm_dos2_touch_world_ctx(void); dm_dos2_touch_world_ctx();
                dt_button(mgr, ID_RIGHT, 1); dt_button(mgr, ID_RIGHT, 0); g.wact = 2;
            }
            static unsigned n; if (g.wact == 2 && n++ < 100) LOG("[touch] world: long press on a character / item at %.0f,%.0f: %s, no walk", (double)g.lx, (double)g.ly,
                                                              g_ctx_hold == 1 ? "pad X = Actions... (pad selection)" : "right click = ContextMenu for the object under the finger (PC mode)");
        }
    }
    if (g.st == G_WORLD && !g.wact && !g.wplace && !on_obj && now - g.t0 >= (uint64_t)g_world_hold_ms) {   // on the floor: the follow-the-finger walk g.wact = 1; g_n_wstick++; dt_world_stick(g.lx, g.ly, 0);
        if (g_dbg) { static unsigned n; if (n++ < 100) LOG("[touch] world: finger held at %.0f,%.0f: left stick %d,%d", (double)g.lx, (double)g.ly, atomic_load(&g_w_lx), atomic_load(&g_w_ly)); } }
}
int dm_dos2_touchui_gate(int in_game, int *why);   // hle_dos2_touchui.c
// ---- pad buttons pressed by the host for the player (console touch, owner 15.11): a tap on a button hint of a console movie, see
// tools/dos2_hint_touch.py. The button goes into the npad state the game reads (hle_hid.c fill -> dm_dos2_touch_pad_pump), held DM_DOS2_TOUCH_PAD_POLLS
// npad reads (default 4) and at least 120 ms - the same path as the on-screen pad's buttons, so the game's own pad poll makes the press and the release records.
// One pulse at a time: a second one inside 250 ms of the first is dropped (one tap may hit two neighbouring hints).
#include <stdatomic.h>
#include "dm_input.h"
#include "hle_dos2_touchui_ei.h"
static _Atomic uint64_t g_pp_mask; static _Atomic int g_pp_left; static int g_pp_polls = 4, g_pp_on;
static uint64_t g_pp_t, g_pp_asked;   // press time (ms; in the future for a delayed press), when it was asked
int dm_dos2_touch_pad_pulse(uint64_t mask, const char *why, int delay_ms);
int dm_dos2_touch_pad_pulse(uint64_t mask, const char *why, int delay_ms) {
    if (!g_pp_on || !mask) return 0;
    const uint64_t now = dt_ms();
    if (atomic_load(&g_pp_left) > 0 || now - g_pp_asked < 250) return 0;
    g_pp_asked = now; g_pp_t = now + (uint64_t)(delay_ms > 0 ? delay_ms : 0); atomic_store(&g_pp_mask, mask); atomic_store(&g_pp_left, g_pp_polls);
    { static unsigned n; if (n++ < 200 || g_dbg) LOG("[touch] pad button %#llx pressed for the player (%s), in %d ms, held %d npad reads", (unsigned long long)mask, why ? why : "", delay_ms, g_pp_polls); }
    return 1;
}
void dm_dos2_touch_pad_pump(uint64_t *buttons);   // hle_hid.c fill(): the thread that reads the npad
static _Atomic uint64_t g_ph_mask; static uint64_t g_ph_t;   // dos2-touch6: a pad button held for a held hint (dt_pad_hold)
static void dt_pad_hold(uint64_t mask, const char *why) {
    const uint64_t was = atomic_exchange(&g_ph_mask, mask), now = dt_ms();
    if (was == mask) return;
    static unsigned n;
    if (mask) { g_ph_t = now; if (n++ < 60 || g_dbg) LOG("[touch] pad button %#llx HELD for the player (%s) until the finger is up", (unsigned long long)mask, why ? why : ""); }
    else if (n++ < 60 || g_dbg) LOG("[touch] pad button %#llx let go after %llu ms (%s)", (unsigned long long)was, (unsigned long long)(now - g_ph_t), why ? why : "");
}
void dm_dos2_touch_pad_pump(uint64_t *buttons) {
    {   // dos2-touch7: a wheel's item chosen by touch: the player's held ZL / ZR is let go for the game (its release chooses) until really let go
        const uint64_t ca = atomic_load(&g_rad_choose_at);
        const int ask = ca && dt_ms() >= ca;
        if (ask) atomic_store(&g_rad_choose_at, 0);
        if (ask || g_rad_sup) {
            const uint64_t was = g_rad_sup;
            *buttons = dt_radial_release(*buttons, ask, DM_BTN_ZL | DM_BTN_ZR, &g_rad_sup);
            if (ask && !g_rad_sup) { static unsigned n; if (n++ < 12) LOG("[touch] radial: choose asked, but no ZL / ZR is held on the pad: nothing to let go (the wheel chooses when its button is let go)"); }
            if (was != g_rad_sup && !g_rad_sup) { static unsigned n; if (g_dbg && n++ < 40) LOG("[touch] radial: the player let ZL / ZR go: the pad's triggers pass again"); }
        }
    }
    *buttons |= atomic_load(&g_ph_mask);
    int left = atomic_load(&g_pp_left);
    const uint64_t now = dt_ms();
    if (left > 0 && now >= g_pp_t) {   // (before g_pp_t: a delayed press, A after a tap: past the movies' "a tap is not accept" guards)
        *buttons |= atomic_load(&g_pp_mask);
        if (left > 1 || now - g_pp_t >= 120) atomic_store(&g_pp_left, left - 1);   // at least g_pp_polls reads AND 120 ms (the npad may be read several times a frame)
    }
    if (*buttons & DM_BTN_A) atomic_store(&g_a_ms, now);   // pass 7: hle_dos2_touchui_route.c tells a real A from the "accept" a tap makes
    { static uint64_t prev; if ((*buttons & DM_BTN_MINUS) && !(prev & DM_BTN_MINUS)) atomic_fetch_add(&g_map_toggles, 1); prev = *buttons; }   // map view: '-' = ToggleMap
}
// LS_Classes.controllerHelper BTN_* (the code a button hint is made with) -> npad button; sticks / directions of sticks have no button
static uint64_t dt_btn_of_hint(int code) {
    switch (code) {
    case 1: return DM_BTN_B; case 2: return DM_BTN_A; case 3: return DM_BTN_X; case 4: return DM_BTN_Y; case 5: return DM_BTN_ZL; case 6: return DM_BTN_ZR;
    case 21: return DM_BTN_UP; case 22: return DM_BTN_DOWN; case 23: return DM_BTN_LEFT; case 24: return DM_BTN_RIGHT;
    case 27: return DM_BTN_MINUS; case 28: return DM_BTN_PLUS; case 29: return DM_BTN_LS; case 30: return DM_BTN_RS; case 31: return DM_BTN_L; case 32: return DM_BTN_R;
    default: return 0;
    }
}
int dm_dos2_touchui_may_press(const void *ui);   // hle_dos2_touchui.c
static int dt_ei_padbtn(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {   // ExternalInterface "dmPadBtn"(code)
    (void)ctx; (void)player; (void)name;
    const int code = n && a && a[0].type == 4 ? (int)a[0].d : 0;
    const uint64_t btn = dt_btn_of_hint(code);
    static unsigned nl;
    if (!btn && code >= 900) {   // a movie's trace (tools/dos2_taps3_touch.py SP_TRACE / HB_TRACE codes, geometry 20000.. 70000 + px): its own budget
        static unsigned nt; if (nt++ < 300) LOG("[touch] dmPadBtn(%d): no pad button for that hint code (movie trace)", code); return 1; }
    if (!btn) { if (nl++ < 40) LOG("[touch] dmPadBtn(%d): no pad button for that hint code", code); return 1; }
    if (!dm_dos2_touchui_may_press(ui)) { if (nl++ < 40) LOG("[touch] dmPadBtn(%d): refused, the window is not on top (a modal menu is over it)", code); return 1; }
    char why[40]; snprintf(why, sizeof why, "hint tap, BTN code %d", code);
    // optional 2nd argument = the delay in ms the movie asks for (the title splash: 60 ms, its close is no guarded accept; pack 3f)
    if (n >= 2 && a[1].type == 4 && a[1].d >= 0 && a[1].d <= 2000) { dm_dos2_touch_pad_pulse(btn, why, (int)a[1].d); return 1; }
    // A = "accept": the console movies swallow an accept that comes within 400 / 700 ms of a mouse event (a click is also "IE UIAccept" on the phone:
    // tools/dos2_hint_touch.py, tools/dos2_mainmenu_touch.py), so the A a hint tap asks for is pressed 750 ms after the tap; the other buttons at once
    dm_dos2_touch_pad_pulse(btn, why, btn == DM_BTN_A ? 750 : 0);
    return 1;
}
// ---- HUD element rects for the floating sticks (pack 3f+, owner 15.11: "binding to the screen edges is bad - 16:9 phone? 4:3 tablet? get the real
// position of the objects in pixels"). bottomBar_d / minimap_d / playerInfo_d (tools/dos2_hudrect_touch.py) call ExternalInterface
// "dmHudRects"("<stageWidth>,<stageHeight>|<id>,x,y,w,h;...") with their elements' getBounds(stage) every ~10 frames (changed) / ~30 frames (keep-alive).
// Stage -> window px: window = origin + stage * scale. origin = the FlashPlayer's +0x10 / +0x14 (the EI `player` IS the FlashPlayer, hle_dos2_touchui_ei.c;
// the same offset Iggy's mouse move subtracts, dt_iggy_btn_hook); scale = min(window w / W, window h / H) with W x H = the movie's authored stage
// (the reported stage size when it is 16:9, else 1920 x 1080, the size of every DOS2 GUI movie): a movie is fitted into the window keeping its
// aspect (origin 240,0 for 1920x1080 in 2400x1080 = scale 1). The log prints both and the centred origin the scale implies, so a phone run shows
// whether the assumption holds (DM_DOS2_HUDRECT_SCALE=<f> forces a scale). Rects are kept per player and forgotten DT_HR_TTL ms after its last
// call (a movie that stops ticking stops blocking the stick). Java reads them normalised to the window: DmActivity.nativeDos2HudRects.
// pack 3k: a movie's place in the window, as the game keeps it in its FlashPlayer: +0x10 x, +0x14 y, +0x18 width, +0x1c height (window px; read
// from the phone's memory 15.11: bottomBar_d 240,0 1920x1080, minimap_d 1936,0 464x650 for a 357x500 stage, playerInfo_d 0,0 2496x2600 for
// 1920x2000 - a uniform fit, scale 1.3 on this 2400x1080 window for the last two). Iggy draws the movie only inside that rect (pack 3i: the
// column moved past x 2160 vanished). Stage -> window: x = X + (W - sw * k) / 2 + stage x * k with k = min(W / sw, H / sh).
static int dt_player_fit(const void *player, float sw, float sh, float *k, float *x0, float *y0) {
    if (!player || sw < 16 || sh < 16) return 0;
    const int32_t *f = (const int32_t *)((const uint8_t *)player + 0x10);
    if (f[2] < 16 || f[3] < 16 || f[2] > 16384 || f[3] > 16384 || f[0] < -16384 || f[0] > 16384 || f[1] < -16384 || f[1] > 16384) return 0;
    const float kk = fminf((float)f[2] / sw, (float)f[3] / sh);
    *k = kk; *x0 = (float)f[0] + ((float)f[2] - sw * kk) / 2; *y0 = (float)f[1] + ((float)f[3] - sh * kk) / 2;
    return 1;
}
static int g_bb_full = -1;   // DM_DOS2_BB_FULL: bottomBar_d drawn over the whole window width (pack 3k experiment, see dm_dos2_touch_stage_edge)
#define DT_HR_MAX 48
#define DT_HR_TTL 5000   // > the movies' keep-alive (~2 s, tools/dos2_hudrect_touch.py KEEP)
// dt_hr: hle_dos2_touch_gesture.h (dos2-touch7: the stage-listener press test shares it with the host test)
static dt_hr g_hr[DT_HR_MAX];
static unsigned g_hr_n;
static pthread_mutex_t g_hr_mu = PTHREAD_MUTEX_INITIALIZER;
static float g_hr_scale;   // DM_DOS2_HUDRECT_SCALE (0 = derive)
static int dt_ei_hudrects(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {
    (void)ctx; (void)ui; (void)name;
    static unsigned nlog;
    char buf[2048];
    const char *s = n && a && (a[0].type == 5 || a[0].type == 6) ? dm_ei_str(&a[0], buf, sizeof buf) : NULL;
    if (!s) return 1;
    float sw = 0, sh = 0;
    if (sscanf(s, "%f,%f|", &sw, &sh) != 2) return 1;
    const char *p = strchr(s, '|'); if (!p) return 1; p++;
    const int st169 = sw > 0 && sh > 0 && fabsf(sw / sh - 16.f / 9.f) < 0.02f;
    const float W = st169 ? sw : 1920.f, H = st169 ? sh : 1080.f;
    const float ww = g_win_w, wh = g_win_h;
    float k = g_hr_scale > 0 ? g_hr_scale : fminf(ww / W, wh / H);
    int ox = 0, oy = 0;
    if (player) { ox = *(const int32_t *)((const uint8_t *)player + 0x10); oy = *(const int32_t *)((const uint8_t *)player + 0x14); }
    { float dk, dx, dy; if (g_hr_scale <= 0 && dt_player_fit(player, sw, sh, &dk, &dx, &dy)) { k = dk; ox = (int)lroundf(dx); oy = (int)lroundf(dy); } }   // pack 3k
    const uint64_t now = dt_ms();
    char mv[48] = ""; uint32_t ty = 0;
    const int lg = g_dbg && nlog < 40;
    if (lg) { void dm_dos2_touchui_movie_of(const void *player, uint32_t *type, char *buf, size_t n); dm_dos2_touchui_movie_of(player, &ty, mv, sizeof mv); }
    char line[600]; size_t ln = 0; line[0] = 0;
    pthread_mutex_lock(&g_hr_mu);
    unsigned w = 0;
    for (unsigned i = 0; i < g_hr_n; i++) if (g_hr[i].player != player && now - g_hr[i].t <= DT_HR_TTL) g_hr[w++] = g_hr[i];   // drop this movie's old set and stale ones
    g_hr_n = w;
    while (*p && g_hr_n < DT_HR_MAX) {
        const char *e = strchr(p, ';'); if (!e) e = p + strlen(p);
        float x, y, rw, rh;
        const char *c = memchr(p, ',', (size_t)(e - p));
        if (c && (size_t)(c - p) < sizeof g_hr[0].id && sscanf(c + 1, "%f,%f,%f,%f", &x, &y, &rw, &rh) == 4 && rw > 0 && rh > 0 && ww > 0 && wh > 0
            && rw * k < ww * 0.8f && rh * k < wh * 0.8f && fabsf(x) < 1e5f && fabsf(y) < 1e5f) {   // pack 3h: a box near the window's size is a hidden layer / sentinel
            dt_hr *r = &g_hr[g_hr_n++];
            r->player = player; r->t = now; memcpy(r->id, p, (size_t)(c - p)); r->id[c - p] = 0;
            r->x0 = (ox + x * k) / ww; r->y0 = (oy + y * k) / wh; r->x1 = (ox + (x + rw) * k) / ww; r->y1 = (oy + (y + rh) * k) / wh;
            if (lg && ln < sizeof line - 64) ln += (size_t)snprintf(line + ln, sizeof line - ln, " %s x %.0f..%.0f y %.0f..%.0f;", r->id, (double)(ox + x * k), (double)(ox + (x + rw) * k), (double)(oy + y * k), (double)(oy + (y + rh) * k));
        }
        p = *e ? e + 1 : e;
    }
    pthread_mutex_unlock(&g_hr_mu);
    if (lg) { nlog++;
        LOG("[touch] HUD rects of %s (type %#x, player %p): stage %.0fx%.0f (used %.0fx%.0f), window %.0fx%.0f, origin %d,%d (a centred fit at this scale: %.0f,%.0f), scale %.3f%s -> window px:%s",
            mv[0] ? mv : "?", ty, player, (double)sw, (double)sh, (double)W, (double)H, (double)ww, (double)wh, ox, oy, (double)((ww - W * k) / 2), (double)((wh - H * k) / 2), (double)k,
            g_hr_scale > 0 ? " (DM_DOS2_HUDRECT_SCALE)" : "", line[0] ? line : " none (nothing visible)"); }
    return 1;
}
// Button hints by their rect (branch dos2-dpadbtns, owner 15.11: "every game function needs an element; make the X / Y hints tappable"): a movie
// reports a hint as HUD rect "hint<BTN code>" (tools/dos2_hudrect_touch.py: overhead_d's "(X) Actions..." under the selected target - type 5 gets
// no mouse at all - and bottomBar_d's "(Y) quick access" under the hot bar). A world tap (a press no UI object took) inside one, grown by
// DT_HINT_SLOP of the window height, presses that pad button (dt_btn_of_hint) instead of the world click. A rect counts DT_HINT_TTL ms after the
// movie's last report (overhead_d reports every 10 frames, keep-alive every 4th: the line comes and goes with the selection). bottomBar_d's Y hint
// also has its own press listener (tools/dos2_hud_touch.py code 17); if that one takes the press the host never gets here, and if both fire the
// pulse guard drops the second one (250 ms).
// Branch dos2-dpad2 (pack 4a: "the tactical camera button does not react"): minimap_d (UI type 0x3c) never gets a mouse BUTTON in Iggy - the
// routing hook delivers its mouse moves, but no DM_DBG run ever logged an "Iggy button #" for it (its map tap only ever worked through the
// dt_minimap_hit fallback below), so its own dmTcDown / dmTcUp listeners can not fire. Its tactical camera button is reported as "btn21": an
// id "btn<code>" is a button that is always on screen, the same tap as a hint but it counts as long as the movie's rects live (DT_HR_TTL;
// minimap_d re-sends unchanged rects only every ~2 s, longer than DT_HINT_TTL).
#define DT_HINT_TTL 1500
#define DT_HINT_SLOP 0.012f
static unsigned g_n_hint;
static int dt_hud_hit(float x, float y) {   // any live HUD rect (+ the hint slop), window px
    if (g_win_w <= 0 || g_win_h <= 0) return 0;
    const float nx = x / g_win_w, ny = y / g_win_h, sx = DT_HINT_SLOP * g_win_h / g_win_w, sy = DT_HINT_SLOP;
    const uint64_t now = dt_ms(); int hit = 0;
    pthread_mutex_lock(&g_hr_mu);
    for (unsigned i = 0; i < g_hr_n && !hit; i++) {
        const dt_hr *r = &g_hr[i];
        if (now - r->t > DT_HR_TTL) continue;
        if (nx >= r->x0 - sx && nx <= r->x1 + sx && ny >= r->y0 - sy && ny <= r->y1 + sy) hit = 1;
    }
    pthread_mutex_unlock(&g_hr_mu);
    return hit;
}
static int dt_hint_tap(float x, float y) {
    if (g_win_w <= 0 || g_win_h <= 0) return 0;
    const float nx = x / g_win_w, ny = y / g_win_h, sx = DT_HINT_SLOP * g_win_h / g_win_w, sy = DT_HINT_SLOP;
    const uint64_t now = dt_ms(); int code = 0; char id[20] = "";
    pthread_mutex_lock(&g_hr_mu);
    for (unsigned i = 0; i < g_hr_n && !code; i++) {
        const dt_hr *r = &g_hr[i];
        const int hint = !strncmp(r->id, "hint", 4), hold = !hint && !strncmp(r->id, "hold", 4), btn = !hint && !hold && !strncmp(r->id, "btn", 3);
        if (!hint && !btn && !hold) continue;   // dos2-touch6: a "hold<code>" rect is also a tap (its button pressed once; a held finger holds it, dt_hold_hint)
        if (now - r->t > (hint ? DT_HINT_TTL : DT_HR_TTL)) continue;
        if (nx >= r->x0 - sx && nx <= r->x1 + sx && ny >= r->y0 - sy && ny <= r->y1 + sy) { code = atoi(r->id + (btn ? 3 : 4)); memcpy(id, r->id, sizeof id); }
    }
    pthread_mutex_unlock(&g_hr_mu);
    const uint64_t btn = code > 0 ? dt_btn_of_hint(code) : 0;
    if (!btn) return 0;
    char why[48]; snprintf(why, sizeof why, "hint tap (%s)", id);
    const int ok = dm_dos2_touch_pad_pulse(btn, why, btn == DM_BTN_A ? 750 : 0);
    g_n_hint++;
    if (g_dbg || g_n_hint <= 8) LOG("[touch] hint tap at %.0f,%.0f on HUD rect %s: pad button of BTN code %d%s", (double)x, (double)y, id, code, ok ? "" : " (NOT pressed: a pulse is running)");
    return 1;
}
// dos2-touch7: hle_dos2_touchui_route.c asks, for a button press a movie with a stage listener (DM_DOS2_TOUCH_STAGE_TYPES, bottomBar_d) answered
// "handled", whether the UI cursor (= the finger) is inside one of that movie's live HUD rects (+ the hint slop): DT_HR_IN / DT_HR_OUT / DT_HR_NONE.
int dm_dos2_touch_stage_press(const void *player, float *x, float *y);
int dm_dos2_touch_stage_press(const void *player, float *x, float *y) {
    const float *c = dt_cursor_at();
    if (!c || !player || g_win_w <= 0 || g_win_h <= 0) return DT_HR_NONE;
    const float cx = c[0], cy = c[1];
    if (x) *x = cx;
    if (y) *y = cy;
    pthread_mutex_lock(&g_hr_mu);
    const int st = dt_hr_state(g_hr, g_hr_n, player, dt_ms(), DT_HR_TTL, cx / g_win_w, cy / g_win_h, DT_HINT_SLOP * g_win_h / g_win_w, DT_HINT_SLOP);
    pthread_mutex_unlock(&g_hr_mu);
    return st;
}
// dos2-touch6 (owner 18.11, in combat: "Hold (B) to end your turn" must be holdable by finger): bottomBar_d reports its end-turn hints as
// "hold<BTN code>" (tools/dos2_hudrect_touch.py). A world finger (its press taken by no UI object) held inside one past the hold time holds
// that pad button until it is lifted (dt_pad_hold) - the pad's own hold, so the movie's hold circle fills and ends the turn as with B.
static int dt_hold_hint(float x, float y) {
    if (g_win_w <= 0 || g_win_h <= 0) return 0;
    const float nx = x / g_win_w, ny = y / g_win_h, sx = DT_HINT_SLOP * g_win_h / g_win_w, sy = DT_HINT_SLOP;
    const uint64_t now = dt_ms(); int code = 0;
    pthread_mutex_lock(&g_hr_mu);
    for (unsigned i = 0; i < g_hr_n && !code; i++) {
        const dt_hr *r = &g_hr[i];
        if (strncmp(r->id, "hold", 4) || now - r->t > DT_HR_TTL) continue;
        if (nx >= r->x0 - sx && nx <= r->x1 + sx && ny >= r->y0 - sy && ny <= r->y1 + sy) code = atoi(r->id + 4);
    }
    pthread_mutex_unlock(&g_hr_mu);
    return code;
}
// minimap_d may be wider than minimap_c's 357x500 stage (tools/dos2_minimap_touch.py MM_DX: the tactical camera button left of the ring; the frame
// grows to the left and the document moves right by the same width). minimap_c's own stage x 0 is then the rect's right side - 357 * k, k = the rect's
// height / 500 (the height is unchanged). For the stock 357 px movie this is the same fit as dt_player_fit(mp, 357, 500).
static int dt_minimap_fit(const void *mp, float *k, float *x0, float *y0) {
    float k0, x00, y00;
    if (!dt_player_fit(mp, 357.f, 500.f, &k0, &x00, &y00)) return 0;
    const int32_t *f = (const int32_t *)((const uint8_t *)mp + 0x10);
    const float kk = (float)f[3] / 500.f;
    if ((float)f[2] < 357.f * kk - 1.f) { *k = k0; *x0 = x00; *y0 = y00; return 1; }   // narrower than minimap_c: the old centred fit
    *k = kk; *x0 = (float)f[0] + (float)f[2] - 357.f * kk; *y0 = (float)f[1];
    return 1;
}
// pack 3h (owner: "the column should sit along the screen edge for everyone"): the window's right edge in a 1920x1080 movie's stage px, by the
// same mapping as the HUD rects (origin + stage * scale): (window w - origin x) / scale. hle_dos2_touchui_vis.c hands it to bottomBar_d
// (dmColEdge), which moves its right-anchored buttons by edge - 1920. 0 = unknown (no player / window).
int dm_dos2_touch_stage_edge(const void *player);
int dm_dos2_touch_stage_edge(const void *player) {
    if (!player || g_win_w <= 0 || g_win_h <= 0) return 0;
    if (g_bb_full < 0) { const char *v = getenv("DM_DOS2_BB_FULL"); g_bb_full = v && *v == '1'; }
    int32_t *f = (int32_t *)((uint8_t *)player + 0x10);
    if (g_bb_full && f[2] > 0 && f[3] > 0 && (f[0] != 0 || f[2] != (int32_t)g_win_w)) {   // pack 3k experiment: the movie's rect = the whole window width
        static unsigned n; if (n++ < 12) LOG("[vis] DM_DOS2_BB_FULL: bottomBar_d rect %d,%d %dx%d -> 0,%d %dx%d (the column may then reach the screen edge)", f[0], f[1], f[2], f[3], f[1], (int)g_win_w, f[3]);
        f[0] = 0; f[2] = (int32_t)g_win_w;
    }
    // pack 3n: the movie's own rect decides. "fixed" (stock): 1920x1080 centred, Iggy clips there -> e = 1920 (nothing moves). "fillVFit"
    // (pack 3l, tools/dos2_hud_touch.set_layout): the rect is the whole window, the stage is fitted by height (k = h / 1080) and starts at the
    // left edge, stage width W / k; the movie centres its 1920 design by moving the document by (W / k - 1920) / 2, so the visible right edge
    // in document px is e = 1920 + (W / k - 1920) / 2.
    if (f[2] < 16 || f[3] < 16) return 1920;
    const float k = (float)f[3] / 1080.f, sw = (float)f[2] / k;
    const float e = sw > 1920.f ? 1920.f + (sw - 1920.f) / 2.f : 1920.f;
    return e < 8000.f ? (int)lroundf(e) : 1920;
}
// pack 3o (owner's sketch: the column's right side on the minimap ring's right side): that line in bottomBar_d document px, from the ring's live
// rect (minimap_d "minimap", tools/dos2_hudrect_touch.py) and bottomBar_d's own rect: window x = X0 + k * (document x + (e - 1920)), clamped to
// the visible edge e. 0 = not known yet (no ring rect).
#define DT_RING_R 308.f
int dm_dos2_touch_col_right(const void *player, int edge);
int dm_dos2_touch_col_right(const void *player, int edge) {
    if (!player || edge < 1920 || g_win_w <= 0) return 0;
    const int32_t *f = (const int32_t *)((const uint8_t *)player + 0x10);
    if (f[2] < 16 || f[3] < 16) return 0;
    // pack 3p (owner, run t3o: "the button column moves left and right a bit"): the ring's live rect also holds the animated direction arrows
    // around it (2148 -> 2121 between two polls). The ring itself is fixed in minimap_c's 357x500 stage: its right side at stage x
    // DT_RING_R (measured on the phone 15.11: window 2336 with the movie at 1936, scale 1.3). Mapped through the minimap movie's own rect.
    float R = -1; const void *mp = NULL; const uint64_t now = dt_ms();
    pthread_mutex_lock(&g_hr_mu);
    for (unsigned i = 0; i < g_hr_n; i++) if (!strcmp(g_hr[i].id, "minimap") && now - g_hr[i].t <= DT_HR_TTL) mp = g_hr[i].player;
    pthread_mutex_unlock(&g_hr_mu);
    { float mk, mx, my; if (mp && dt_minimap_fit(mp, &mk, &mx, &my)) R = mx + DT_RING_R * mk; }
    if (R < 0) return 0;
    const int fill = f[2] >= (int32_t)g_win_w - 1;
    const float k = fill ? (float)f[3] / 1080.f : fminf((float)f[2] / 1920.f, (float)f[3] / 1080.f);
    const float x0 = fill ? (float)f[0] : (float)f[0] + ((float)f[2] - 1920.f * k) / 2.f;
    float c = (R - x0) / k - (float)(edge - 1920);
    if (c > (float)edge) c = (float)edge;
    return c > 960.f ? (int)lroundf(c) : 0;
}
// pack 3s: where the column movie (dmColumn_d, its own 357x830 stage) puts its buttons, from the minimap's REAL rect: right side on the ring's
// right side (minimap stage x DT_RING_R), top just under the ring (minimap stage y DT_RING_B + a 6 px gap). Run t3r: the column class is scaled
// 1.0 (rect 2043,0 357x830) while the minimap is 1.3, so the movie's fixed numbers were 15 px off and over the ring. 0 = minimap not known yet.
#define DT_RING_B 338.f
#define DT_COL_GAP_BTN 0.15f  // column top this many button steps (68 column stage px) under the ring (owner: 1.5 in 3s was too low)
int dm_dos2_touch_col_place(const void *col_player, int *rx, int *ty);
int dm_dos2_touch_col_place(const void *col_player, int *rx, int *ty) {
    const void *mp = NULL; const uint64_t now = dt_ms();
    pthread_mutex_lock(&g_hr_mu);
    for (unsigned i = 0; i < g_hr_n; i++) if (!strcmp(g_hr[i].id, "minimap") && now - g_hr[i].t <= DT_HR_TTL) mp = g_hr[i].player;
    pthread_mutex_unlock(&g_hr_mu);
    float mk, mx, my, ck, cx, cy;
    if (!mp || !dt_minimap_fit(mp, &mk, &mx, &my) || !dt_player_fit(col_player, 357.f, 1080.f, &ck, &cx, &cy)) return 0;
    const float R = mx + DT_RING_R * mk, B = my + DT_RING_B * mk + DT_COL_GAP_BTN * 68.f * ck;   // owner (3r): "a bit lower, 1-2 icons"
    *rx = (int)lroundf((R - cx) / ck); *ty = (int)lroundf((B - cy) / ck);
    return *rx > 0 && *rx < 10000 && *ty >= 0 && *ty < 10000;
}
// pack 3x (owner's sketch): the options button (bottomBar_d dmVisOpt_mc, 74 px) just left of the minimap ring at the top: its left side at
// the minimap movie's left + DT_OPT_L (minimap stage px), in bottomBar_d document px, clamped inside bottomBar's visible 1920 rect.
#define DT_OPT_L 3.f
int dm_dos2_touch_opt_x(const void *bb_player);
int dm_dos2_touch_opt_x(const void *bb_player) {
    const void *mp = NULL; const uint64_t now = dt_ms();
    pthread_mutex_lock(&g_hr_mu);
    for (unsigned i = 0; i < g_hr_n; i++) if (!strcmp(g_hr[i].id, "minimap") && now - g_hr[i].t <= DT_HR_TTL) mp = g_hr[i].player;
    pthread_mutex_unlock(&g_hr_mu);
    float mk, mx, my, bk, bx, by;
    if (!mp || !bb_player || !dt_minimap_fit(mp, &mk, &mx, &my) || !dt_player_fit(bb_player, 1920.f, 1080.f, &bk, &bx, &by)) return 0;
    float x = (mx + DT_OPT_L * mk - bx) / bk;
    if (x > 1920.f - 74.f) x = 1920.f - 74.f;
    return x > 960.f ? (int)lroundf(x) : 0;
}
// jni_android.c nativeDos2HudRects: the live rects, normalised to the window {x0, y0, x1, y1} each; returns the count (<= max)
int dm_dos2_hud_rects(float *out, int max);
int dm_dos2_hud_rects(float *out, int max) {
    if (!out || max <= 0) return 0;
    const uint64_t now = dt_ms();
    int k = 0;
    pthread_mutex_lock(&g_hr_mu);
    for (unsigned i = 0; i < g_hr_n && k < max; i++) {
        if (now - g_hr[i].t > DT_HR_TTL) continue;
        out[k * 4] = g_hr[i].x0; out[k * 4 + 1] = g_hr[i].y0; out[k * 4 + 2] = g_hr[i].x1; out[k * 4 + 3] = g_hr[i].y1; k++;
    }
    pthread_mutex_unlock(&g_hr_mu);
    return k;
}
// pack 3m (owner, runs t3k / t3l: "it crashed on Continue"): a tap on "Continue" 1.6 s after the title splash (the splash A now comes in 60 ms)
// landed while the game was still writing the profile (save commits #12..#21 every ~200 ms) and crashed at main+0x8ce8dc (null array + 0x40).
// The good runs pressed it 10 s+ later, with the save quiet. A pad hardly gets there that fast; a tap calls the item at once. So a main menu
// "buttonPressed" waits for the save: while it was written or committed in the last DT_MM_QUIET_MS the press is held back (logged; tap again).
#define DT_MM_QUIET_MS 1200
// pack 3n (owner: "can such a press just be delayed and run as soon as it is possible?"): a held press is not lost - the tap already selected
// that item (mainMenu_d tuPress -> tuOver = the stock selection), so once the save is quiet dt_drain presses the pad's A = executeSelected ->
// the same buttonPressed, now let through. Forgotten after DT_MM_PEND_MS (the player may have moved on).
#define DT_MM_PEND_MS 8000
static uint64_t g_mm_pend;   // dt_ms() of the held press, 0 = none
static int dt_ei_mm_button(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {   // ExternalInterface "buttonPressed"
    (void)ctx; (void)ui; (void)name;
    uint32_t ty = 0; char mv[48]; { void dm_dos2_touchui_movie_of(const void *player, uint32_t *type, char *buf, size_t n); dm_dos2_touchui_movie_of(player, &ty, mv, sizeof mv); }
    if (ty != 0x48 && ty != 0x57) return 0;   // the main menu only; every other movie's buttonPressed goes on as before
    uint64_t dm_fs_save_quiet_ms(void); const uint64_t q = dm_fs_save_quiet_ms();
    if (q >= DT_MM_QUIET_MS) return 0;
    static unsigned nl; if (nl++ < 40) LOG("[touch] main menu buttonPressed(%d) deferred: the save was busy %llu ms ago (< %d ms; the game crashed on Continue in that window, runs t3k / t3l) - pressed again when quiet",
                                          n && a && a[0].type == 4 ? (int)a[0].d : -1, (unsigned long long)q, DT_MM_QUIET_MS);
    g_mm_pend = dt_ms();
    return 1;
}
static void dt_drain(void) {
    if (g_mm_pend) {   // pack 3n: a main menu press held back while the save was busy -> the pad's A once it is quiet
        uint64_t dm_fs_save_quiet_ms(void); const uint64_t now = dt_ms();
        if (now - g_mm_pend > DT_MM_PEND_MS) { g_mm_pend = 0; LOG("[touch] deferred main menu press dropped: the save stayed busy for %d ms", DT_MM_PEND_MS); }
        else if (dm_fs_save_quiet_ms() >= DT_MM_QUIET_MS && dm_dos2_touch_pad_pulse(DM_BTN_A, "deferred main menu press (save quiet now)", 0)) {
            LOG("[touch] deferred main menu press: A for the selected item, %llu ms after the tap", (unsigned long long)(now - g_mm_pend)); g_mm_pend = 0; }
    }
    void *mgr = dt_mgr();
    if (!mgr || !g_on) return;
    { void dm_dos2_touchui_ie_poll(void); dm_dos2_touchui_ie_poll(); }   // touch input events of earlier polls (hle_dos2_touchui_ie.c)
    { void dm_dos2_uimode_poll(void); dm_dos2_uimode_poll(); }   // the UI mode switch: a request from the gear, the HUD part at the next load (hle_dos2_uimode.c)
    if (g_kbm) {   // controller mode: let keyboard / mouse records through the listener filter (main+0x84407c)
        volatile uint8_t *k = (volatile uint8_t *)mgr + DT_MGR_KBM;
        uint8_t want = 1;
        int why = 0;   // console touch (owner 15.11): hle_dos2_touchui.c dm_dos2_touchui_gate decides; -1 = busy, keep the last state
        if (g_kbm == 2) {   // in game (a player UI set exists) the controller HUD must not get mouse events (phone run tu03: crash on a portrait tap); a click is
                            // also "accept" for a console movie, so a console menu without touch code keeps the filter closed out of game too
            uint8_t *ui = *(uint8_t *volatile *)(g_main->base + DT_UI_THIS);
            const int in_game = ui && dt_ui_records(ui);
            const int open = dm_dos2_touchui_gate(in_game, &why);   // polls the windows (the window log)
            want = open < 0 ? *k : (uint8_t)(open > 0);
            if (dm_dos2_route_on() && (!in_game || dm_dos2_route_game())) { want = 1; why = 5; }   // pass 7: the per-object hook decides who gets the mouse
        }
        if (*k != want && (want || g_kbm == 2)) { static unsigned n; *k = want;
            static const char *const k_why[6] = {"menus (no window watch)", "OPEN: a touch window is on screen, mouse records reach every listener",
                "OPEN: no console menu on screen (splash / loading / press A: a tap is accept)", "CLOSED: a console menu without touch code is on screen (pad, Back = B)",
                "CLOSED: in game, no touch menu on screen: mouse records only to the listeners that ask for them",
                "OPEN: per-movie routing (hle_dos2_touchui_route.c) decides which UI object gets the mouse"};
            if (n++ < 400) LOG("[touch] DM_DOS2_TOUCH_KBM=%d: [mgr+%#x] -> %u (%s)", g_kbm, DT_MGR_KBM, want, g_kbm == 2 ? k_why[why < 0 || why > 5 ? 0 : why] : "mouse records reach every listener in controller mode"); }
    }
    g_motion_in_drain = 0;
    if (g_ui_ok) { uint8_t *ui = *(uint8_t *volatile *)(g_main->base + DT_UI_THIS); g_in_game = ui && dt_ui_records(ui); }
    { uint32_t ty = 0; const int known = g_ui_ok && dm_dos2_route_on(), menu = known && !dm_dos2_route_world_ok(&ty);   // for the Java pad (pack 3d)
      const int st = known ? 4 | (g_in_game ? 2 : 0) | (menu ? 1 : 0) : 0, was = atomic_exchange(&g_ui_state, st);
      if (g_dbg && was != st) { static unsigned n; if (n++ < 200) LOG("[touch] UI state for the pad: %s%s (UI type %#x): floating sticks %s", known ? (menu ? "MENU on screen" : "HUD only") : "unknown",
                                                                    known ? (g_in_game ? ", in game" : ", out of game") : "", ty, !known ? "as before" : menu || !g_in_game ? "OFF (taps go to the menu)" : "on, off the HUD"); } }
    if (g_map_cfg && (++g_map_polls % 6 == 0 || atomic_load(&g_map_toggles))) dt_map_update();   // map view gestures (journal_csp after '-')
    if (g_rad_cfg && g_in_game) dt_radial_update(); else if (atomic_load(&g_rad_live)) { atomic_store(&g_rad_live, 0); atomic_store(&g_rad_rx, 0); atomic_store(&g_rad_ry, 0); if (g.st == G_RADIAL) g.st = G_IDLE; }   // dos2-touch7: ZL / ZR wheels
    if (g_uimouse && g_uimouse_ok) {   // events 1..3 reach Flash only with this byte set (main+0xaea8ac)
        uint8_t *um = *(uint8_t *volatile *)(g_main->base + DT_UIMGR);
        if (um && !um[DT_UIM_MOUSE]) { static unsigned n; um[DT_UIM_MOUSE] = 1; if (n++ < 4) LOG("[touch] UI manager mouse byte [uimgr+%#x] was 0 -> 1 (Flash mouse buttons were off: main+%#x)", DT_UIM_MOUSE, DT_UIM_BTN); }
    }
    if (g_dbg && ++g_polls % 600 == 0) { dt_summary(mgr); dm_dos2_route_summary();
        if (g_w3) { LOG("[touch] world clicks (DM_DOS2_TOUCH_WORLD=3): %u click(s), %u preview tap(s), sequencer phase %d", g_n_w3click, g_n_w3prev, w3.ph); dm_dos2_touch_world_summary(); }
    if (g_world) LOG("[touch] world (DM_DOS2_TOUCH_WORLD=%d): taps %u, stick holds %u, swipes %u, placement taps %u, presses the UI took %u, in game %d, sticks now %d,%d / rx %d", g_world, g_n_wtap,
                         g_n_wstick, g_n_swipe, g_n_place, g_n_wui, g_in_game, atomic_load(&g_w_lx), atomic_load(&g_w_ly), atomic_load(&g_w_rx));
    if (g_rad_cfg && g_n_rad_open) LOG("[touch] radial (ZL / ZR wheels): opened %u, chosen by touch %u, npad reads with the left stick as the right one %u, open now %#x",
                                       g_n_rad_open, g_n_rad_pick, g_n_rad_lst, atomic_load(&g_rad_live)); }
    if (dt_live()) {   // pad input seen in this poll (dt_inject_log): back to the controller UI, with no finger down and no touch waiting
        if (g_pad_act && g.st == G_IDLE && g_qh == g_qt) dt_live_switch(0, "pad input");
        else if (g_live_wait && g.st == G_IDLE && g_qh == g_qt) dt_live_switch(g_live_wait == 2, "deferred");   // DM_DOS2_TOUCH_LIVE=2: once the UI sets are gone
        g_pad_act = 0;
    }
    if (g_qh == g_qt && !g.pulse_frames && g.st != G_PENDING && g.st < G_WPROBE && !g_w_until && !g_w_rx_until && !g_w_ry_until && !g_mtap_t && !w3.ph && !w3.hover_until) { if (g_w3) dt_w3_sync(G_IDLE, 0); return; }
    dt_window();
    if (!dt_state(mgr)) { pthread_mutex_lock(&g_qm); g_qh = g_qt; pthread_mutex_unlock(&g_qm); g.st = G_IDLE; g.pulse_frames = 0; dt_world_stop(); atomic_store(&g_w_rx, 0); g_w_rx_until = 0;
                          if (g_w3) { memset(&w3, 0, sizeof w3); dt_w3_sync(G_IDLE, 0); } return; }
    dt_ev ev[64]; int n = 0;
    pthread_mutex_lock(&g_qm);
    while (g_qh != g_qt && n < 64) ev[n++] = g_q[g_qh++ & 255];
    pthread_mutex_unlock(&g_qm);
    if (g.pulse_frames > 0 && --g.pulse_frames == 0) dt_send(mgr, g.pulse >> 16, g.pulse & 0xffff, 0, 0, 0);   // pinch released
    if (dt_live() && g.st == G_IDLE) for (int i = 0; i < n; i++) if (ev[i].ph == EV_DOWN) { dt_live_switch(1, "touch"); break; }   // before the click is sent
    for (int i = 0; i < n; i++) dt_step(mgr, &ev[i]);
    if (g_world || g_map_live || g_fmap || g_mtap_t || g_w_ry_until) dt_world_poll(mgr);
    if (g_w3) { dt_w3_poll(mgr); dt_w3_sync(g.st, g.wact); }
    // dos2-touch6 (owner 18.11): the console movies' long presses (hot bar tooltip, item menu, equipment, end turn) need the left button DOWN
    // while the finger is: a still finger on a menu / panel got no press at all until it was lifted (a tap = press + release in one poll), and the
    // long press below became a right click no console movie listens to. Controller UI only (the PC UI keeps its right-click context menus);
    // a tap is still press + release on the same spot = the same click.
    if (g.st == G_PENDING && g_ui_press_ms > 0 && !dt_pc() && dt_ms() - g.t0 >= (uint64_t)g_ui_press_ms) {
        dt_button(mgr, ID_LEFT, 1); g.st = G_DRAG;
        if (g_log || (g_dbg && g_nlog < 64)) LOG("[touch] UI finger still %d ms at %.0f,%.0f: left press now (held until the finger is up)", g_ui_press_ms, (double)g_cx, (double)g_cy);
    }
    if (g.st == G_PENDING && dt_ms() - g.t0 >= (uint64_t)g_hold_ms) {   // long press, still: right click (ContextMenu / FlashRightMouse)
        dt_button(mgr, ID_RIGHT, 1); dt_button(mgr, ID_RIGHT, 0); g.st = G_HELD;
        { void dm_dos2_touchui_ie_gesture(int kind); dm_dos2_touchui_ie_gesture(1); }   // "IE ShortLongPress" / "IE LongPress" for the iPad movies
        if (g_log || (g_dbg && g_nlog < 64)) LOG("[touch] long press at %.0f,%.0f: right click", (double)g_cx, (double)g_cy);
    }
}
static uint64_t dt_poll_hook(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    g_in_poll = 1;
    uint64_t v = g_poll_orig(a0, a1, a2, a3, a4, a5);
    g_in_poll = 0;
    if (a0) {   // the pad's device, read every poll (the slot may be unset before a pad attaches: 0xfffe / 0xffff = none)
        int d = *(const uint16_t *)(uintptr_t)(a0 + 0xe0); if (d >= 0xfff0) d = -1;
        if (d != g_pad_dev) { g_pad_dev = d; if (g_log || g_dbg) LOG("[touch] pad device (NX input +0xe0) = %d", d); }
    }
    dt_drain();
    return v;
}
// DM_DOS2_TOUCH_LOG: what reaches InjectInput (the pad's records and ours)
static uint64_t dt_inject_log(uint64_t mgr, uint64_t rec, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    static uint32_t seen[64]; static int n;
    const dt_rec *r = (const dt_rec *)(uintptr_t)rec;
    if (r && dt_live() && g_in_poll && (r->pressed || fabsf(r->v[0]) > 0.5f || fabsf(r->v[1]) > 0.5f)) g_pad_act = 1;   // the pad's own record: the pad is in use
    if (r && g_log && n < 64) {
        int have = 0; for (int i = 0; i < n; i++) if (seen[i] == r->raw) have = 1;
        if (!have) { seen[n++] = r->raw; LOG("[touch] InjectInput raw %#x (device %u id %#x) value %.2f,%.2f pressed %u from_os %u (mgr %#llx)", r->raw, r->raw >> 16,
                                             r->raw & 0xffff, r->v[0], r->v[1], r->pressed, (unsigned)a2 & 0xff, (unsigned long long)mgr); }
    }
    return g_inject_orig(mgr, rec, a2, a3, a4, a5);
}

static int dt_poke_ro(module_t *m, uint32_t off, const void *src, size_t n) {   // read-only data (a string): RW, copy, back to R
    uint8_t *at = m->base + off, *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    size_t len = (size_t)(((uintptr_t)(at + n) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    memcpy(at, src, n);
    mprotect(pg, len, PROT_READ);
    return 0;
}
static int dt_poke(module_t *m, uint32_t off, const uint32_t *w, int n) {
    uint32_t *at = (uint32_t *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    size_t len = (size_t)(((uintptr_t)(at + n) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    memcpy(at, w, 4 * (size_t)n);
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[touch] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + n));
    return 0;
}
static int dt_jump(module_t *m, uint32_t off, void *to) {   // ldr x16, #8; br x16; .quad to
    uint32_t w[4] = {0x58000050, 0xd61f0200, (uint32_t)(uintptr_t)to, (uint32_t)((uint64_t)(uintptr_t)to >> 32)};
    return dt_poke(m, off, w, 4);
}

void dm_dos2_touch_install(void);   // main.c host_preload, after dm_dos2_wide_install (before any guest code runs)
void dm_dos2_touch_install(void) {
    const char *e = getenv("DM_DOS2_TOUCH"), *lg = getenv("DM_DOS2_TOUCH_LOG");
    g_on = e && atoi(e) > 0; g_log = lg && atoi(lg) > 0;
    if (!g_on && !g_log) return;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    module_t *m = g_main;
    if (!m) { LOG("【红】[touch] no main module: touch as mouse off"); g_on = 0; return; }
    if (m->text_end < DT_POLL + 16 || memcmp(m->base + DT_POLL, k_poll, sizeof k_poll) || memcmp(m->base + DT_INJECT, k_inject, sizeof k_inject) ||
        memcmp(m->base + DT_CALL, &k_call, 4) || memcmp(m->base + DT_MGR_LD, k_mgr, sizeof k_mgr) || memcmp(m->base + DT_STATE, k_state, sizeof k_state)) {
        LOG("【红】[touch] main+%#x / +%#x / +%#x differ: not DOS2 update v786432 main (build 4979b200): touch as mouse off", DT_POLL, DT_INJECT, DT_MGR_LD);
        g_on = 0; return; }
    const char *v;
    if ((v = getenv("DM_DOS2_TOUCH_DEV")) && *v) g_dev = atoi(v) & 0xffff;
    if ((v = getenv("DM_DOS2_TOUCH_STATE")) && *v == '0') g_state_alloc = 0;
    if ((v = getenv("DM_DOS2_TOUCH_MOTION"))) g_motion = !strcmp(v, "abs") ? 0 : !strcmp(v, "delta") ? 2 : 1;
    if ((v = getenv("DM_DOS2_TOUCH_HOLD_MS")) && atoi(v) >= 150) g_hold_ms = atoi(v);
    if ((v = getenv("DM_DOS2_TOUCH_UI_PRESS_MS")) && atoi(v) >= 0 && atoi(v) <= 5000) g_ui_press_ms = atoi(v);
    g_cursor_ptr = !memcmp(m->base + DT_CUR_INIT, k_cur_init, sizeof k_cur_init) && !memcmp(m->base + DT_CUR_READ, k_cur_read, sizeof k_cur_read);
    if (!g_cursor_ptr) LOG("【红】[touch] main+%#x / +%#x differ: the UI cursor main+%#x is not written", DT_CUR_INIT, DT_CUR_READ, DT_CURSOR);
    if ((v = getenv("DM_DOS2_TOUCH_CURSOR")) && *v == '0' && !v[1]) { g_cursor_ptr = 0; LOG("[touch] DM_DOS2_TOUCH_CURSOR=0: the UI cursor is not written"); }
    else if (v && *v) {
        unsigned long o = strtoul(v, NULL, 16);
        if (o >= m->text_end && o + 8 <= m->size) g_cursor_off = (uint32_t)o;   // data only, never code
        else LOG("【红】[touch] DM_DOS2_TOUCH_CURSOR=%s is not a data offset of main: not written", v);
    }
    g_win_ok = !memcmp(m->base + DT_WIN_RD, k_win_rd, sizeof k_win_rd) && !memcmp(m->base + DT_WIN_SET, &k_win_set, 4);
    if (!g_win_ok) LOG("【红】[touch] main+%#x / +%#x differ: the game's window size is not read (the docked size is used)", DT_WIN_RD, DT_WIN_SET);
    unsigned ww = 0, wh = 0;
    if ((v = getenv("DM_DOS2_TOUCH_WIN")) && sscanf(v, "%ux%u", &ww, &wh) == 2 && ww >= 320 && wh >= 200) { g_win_w = (float)ww; g_win_h = (float)wh; g_win_fixed = 1; }
    else { void dm_dos2_wide_size(unsigned *w, unsigned *h); dm_dos2_wide_size(&ww, &wh); if (ww && wh) { g_win_w = (float)ww; g_win_h = (float)wh; } }
    g_dbg = g_on && (v = getenv("DM_DBG")) && atoi(v) > 0;
    if ((v = getenv("DM_DOS2_TOUCH_DEVN")) && *v) { g_devn = atoi(v); if (g_devn > DT_DEVS) g_devn = DT_DEVS; if (g_devn < 0) g_devn = 0; }
    if ((v = getenv("DM_DOS2_TOUCH_AUTOREL")) && *v) g_autorel = atoi(v) == 2 ? 2 : atoi(v) > 0;
    if ((v = getenv("DM_DOS2_TOUCH_UIMOUSE")) && *v == '0') g_uimouse = 0;
    if ((v = getenv("DM_DOS2_TOUCH_PINCH")) && !strcmp(v, "touch")) g_pinch_stick = 0;
    if ((v = getenv("DM_DOS2_TOUCH_PINCH_FRAMES")) && atoi(v) >= 2 && atoi(v) <= 120) g_pinch_frames = atoi(v);
    if ((v = getenv("DM_DOS2_TOUCH_PINCH_V")) && atof(v) > 0.05 && atof(v) <= 4.0) g_pinch_v = (float)atof(v);
    // main+0x845670 (k_state_rd[1], sbfx x12,x18,..) uses x18: the loader rewrites such instructions, so that word is not compared
    g_devn_ok = !memcmp(m->base + DT_STATE_RD, &k_state_rd[0], 4) && !memcmp(m->base + DT_STATE_RD + 8, &k_state_rd[2], 8) && !memcmp(m->base + DT_STATE_RD2, k_state_rd2, sizeof k_state_rd2);
    if (!g_devn_ok && g_devn) LOG("【红】[touch] main+%#x / +%#x differ: the raw-state array's size is never set by the host", DT_STATE_RD, DT_STATE_RD2);
    g_alloc_ok = !memcmp(m->base + DT_ALLOC_CALL, k_alloc_call, sizeof k_alloc_call);
    if ((v = getenv("DM_DOS2_TOUCH_STATE_WAIT")) && atoi(v) >= 0) g_state_wait = atoi(v);
    if ((v = getenv("DM_DOS2_TOUCH_BTNXY")) && atoi(v) > 0) g_btnxy = 1;
    if ((v = getenv("DM_DOS2_TOUCH_UPONCE")) && atoi(v) > 0) g_uponce = 1;
    g_autorel_ok = !memcmp(m->base + DT_Q_FLAG, k_q_flag, sizeof k_q_flag) && !memcmp(m->base + DT_Q_BUF, &k_q_buf, 4) && !memcmp(m->base + DT_REC_FLAG, k_rec_flag, sizeof k_rec_flag) &&
                   !memcmp(m->base + DT_AUTOREL, k_autorel, sizeof k_autorel) && !memcmp(m->base + DT_AUTOREL2, k_autorel2, sizeof k_autorel2);
    if (!g_autorel_ok && g_autorel == 1) { g_autorel = 2; LOG("【红】[touch] main+%#x / +%#x / +%#x differ: no automatic release flag, motion is released by an explicit record", DT_Q_FLAG, DT_REC_FLAG, DT_AUTOREL); }
    g_uimouse_ok = !memcmp(m->base + DT_UIM_BTN, k_uim_btn, sizeof k_uim_btn) && !memcmp(m->base + DT_UIM_CTOR, k_uim_ctor, sizeof k_uim_ctor);
    if (!g_uimouse_ok && g_uimouse) LOG("【红】[touch] main+%#x / +%#x differ: the UI manager's mouse byte is not held", DT_UIM_BTN, DT_UIM_CTOR);
    if (g_pinch_stick && memcmp(m->base + DT_STICK, k_stick, sizeof k_stick)) { g_pinch_stick = 0; LOG("【红】[touch] main+%#x differs: pinch stays touch_pinch (no right stick records)", DT_STICK); }
    const int iggy_hooks = (g_dbg || g_btnxy || g_uponce) && !memcmp(m->base + DT_IGGY_MOVE, k_iggy_move, sizeof k_iggy_move) && !memcmp(m->base + DT_IGGY_BTN, k_iggy_btn, sizeof k_iggy_btn);
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOG("【红】[touch] mmap failed: touch as mouse off"); g_on = 0; return; }
    const struct { uint32_t off; const uint32_t *pro; } tr[4] = {{DT_POLL, k_poll}, {DT_INJECT, k_inject}, {DT_IGGY_MOVE, k_iggy_move}, {DT_IGGY_BTN, k_iggy_btn}};
    for (int i = 0; i < 4; i++) {
        uint32_t *s = t + 8 * i; uint64_t rest = (uint64_t)(uintptr_t)(m->base + tr[i].off + 16);
        memcpy(s, tr[i].pro, 16); s[4] = 0x58000050 /* ldr x16,#8 */; s[5] = 0xd61f0200 /* br x16 */; s[6] = (uint32_t)rest; s[7] = (uint32_t)(rest >> 32);
    }
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { LOG("【红】[touch] mprotect RX failed: touch as mouse off"); munmap(t, 0x4000); g_on = 0; return; }
    __builtin___clear_cache((char *)t, (char *)(t + 32));
    g_poll_orig = (dt_fn)(void *)t; g_inject_orig = (dt_fn)(void *)(t + 8);
    if (iggy_hooks) { g_iggy_move_orig = (dt_fn)(void *)(t + 16); g_iggy_btn_orig = (dt_fn)(void *)(t + 24); }
    g_mode_ok = !memcmp(m->base + DT_MODE_INIT, &k_mode_init, 4) && !memcmp(m->base + DT_UISET, k_uiset, sizeof k_uiset) && g_cursor_ptr;   // k_cur_init covers x8 = main+0x2bb88e8
    // the live switch's sites: checked for DM_DOS2_TOUCH_LIVE and, since the UI mode switch (hle_dos2_uimode.c), whenever touch is on - TOUCH can be
    // chosen in game and then needs the pad's way back (red lines only for DM_DOS2_TOUCH_LIVE, as before; the UI mode logs its own)
    const char *live_env = getenv("DM_DOS2_TOUCH_LIVE");
    int dm_dos2_ui_mode(void); const int ui_start = dm_dos2_ui_mode();   // hle_dos2_uimode.c, installed first
    if (g_on) {
        const struct { uint32_t off; const uint32_t *w; size_t n; } ck[] = {{DT_UI_CREATE, k_ui_create, sizeof k_ui_create}, {DT_UI_DESTROY, k_ui_destroy, sizeof k_ui_destroy},
            {DT_UI_CREATE_CALL, k_ui_create_call, sizeof k_ui_create_call}, {DT_UI_DESTROY_CALL, k_ui_destroy_call, sizeof k_ui_destroy_call}, {DT_UI_HASH, k_ui_hash, sizeof k_ui_hash},
            {DT_UI_WALK, k_ui_walk, sizeof k_ui_walk}, {DT_UI_SET, k_ui_set, sizeof k_ui_set}, {DT_UI_SET_L, k_ui_set_l, sizeof k_ui_set_l}};
        int bad = -1;
        for (unsigned i = 0; i < sizeof ck / sizeof *ck; i++) if (memcmp(m->base + ck[i].off, ck[i].w, ck[i].n)) { bad = (int)i; break; }
        const int want_env = live_env && atoi(live_env) > 0;
        if (!g_mode_ok) { if (want_env) LOG("【红】[touch] main+%#x / +%#x differ: DM_DOS2_TOUCH_LIVE not applied", DT_MODE_INIT, DT_UISET); }
        else if (bad >= 0) { if (want_env) LOG("【红】[touch] main+%#x differs (the player UI manager's create / destroy): DM_DOS2_TOUCH_LIVE not applied", ck[bad].off); }
        else {
            g_live_ok = 1;
            if (want_env) g_live = atoi(live_env) >= 2 ? 2 : 1;
            if ((v = getenv("DM_DOS2_TOUCH_LIVE_MS")) && atoi(v) >= 100) g_live_ms = atoi(v);
        }
    }
    if ((v = getenv("DM_DOS2_TOUCH_PCUI_KEYS")) && *v == '0') g_keys_pref = 0;
    g_bind_ok = !memcmp(m->base + DT_BIND_STR, k_bind_pad, sizeof k_bind_pad) && !memcmp(m->base + DT_BIND_REF, k_bind_ref, sizeof k_bind_ref);
    const int hook_inject = g_log || g_live || g_live_ok;   // the log and the live switch (the pad's records during the poll) share one hook
    g_inject = g_log ? (dt_inject_fn)(void *)dt_inject_log : (dt_inject_fn)(void *)(m->base + DT_INJECT);
    if (hook_inject && dt_jump(m, DT_INJECT, (void *)dt_inject_log)) {
        LOG("【红】[touch] mprotect RW failed: no InjectInput hook (no log%s)", g_live ? ", DM_DOS2_TOUCH_LIVE off" : "");
        g_inject = (dt_inject_fn)(void *)(m->base + DT_INJECT); g_log = 0; g_live = 0; g_live_ok = 0; }
    else if ((g_live || g_live_ok) && !g_log) g_inject = (dt_inject_fn)(void *)g_inject_orig;   // our own records skip the hook (through the trampoline)
    if (g_on && dt_jump(m, DT_POLL, (void *)dt_poll_hook)) { LOG("【红】[touch] mprotect RW failed: touch as mouse off"); g_on = 0; g_poll_orig = NULL; }
    if (g_on) LOG("[touch] DM_DOS2_TOUCH=1: pad poll main+%#x hooked, touches -> InjectInput main+%#x (mouse device %d, left %#x right %#x motion %#x "
                  "value %s%s), window %.0fx%.0f, long press %d ms = right click, pinch = camera zoom on the pad device, two-finger drag = wheel",
                  DT_POLL, DT_INJECT, g_dev, ID_LEFT, ID_RIGHT, ID_MOTION, g_motion == 1 ? "{1,1}" : g_motion == 2 ? "delta" : "position",
                  g_cursor_off ? ", position -> DM_DOS2_TOUCH_CURSOR" : g_cursor_ptr ? ", position -> UI cursor [main+0x2bb88e8]" : ", cursor not written", g_win_w, g_win_h, g_hold_ms);
    if (g_on && iggy_hooks && (dt_jump(m, DT_IGGY_MOVE, (void *)dt_iggy_move_hook) || dt_jump(m, DT_IGGY_BTN, (void *)dt_iggy_btn_hook))) {
        LOG("【红】[touch] mprotect RW failed: no Iggy debug hooks"); }   // a half-installed pair still works: each hook has its own trampoline
    else if (g_on && (g_dbg || g_btnxy || g_uponce) && !iggy_hooks) LOG("【红】[touch] main+%#x / +%#x differ: no Iggy debug hooks", DT_IGGY_MOVE, DT_IGGY_BTN);
    if (g_on && (g_btnxy || g_uponce)) LOG("[touch] DM_DOS2_TOUCH_BTNXY=%d (button events with the cursor minus the movie origin) DM_DOS2_TOUCH_UPONCE=%d (second up to a movie dropped)%s",
                                             g_btnxy, g_uponce, g_iggy_btn_orig ? "" : ": NOT applied (no Iggy hooks)");
    if (g_on) LOG("[touch] pass 5: raw-state size %d%s, motion release %s, UI mouse byte %s, controller pinch = %s%s", g_devn, g_devn_ok ? "" : " (not applied)",
                  g_autorel == 1 ? "+0x10 flag (automatic, as the iPad)" : g_autorel == 2 ? "explicit record" : "none", g_uimouse && g_uimouse_ok ? "held at 1" : "left alone",
                  g_pinch_stick ? "right stick y (0x89 in / 0x87 out) on the pad device" : "touch_pinch 0x9d / 0x9e", g_iggy_move_orig ? ", Iggy move / button hooks on (DM_DBG)" : "");
    if (!g_on) { g_live = 0; g_live_ok = 0; }
    { int dm_dos2_touchui_on(void);   // console touch (hle_dos2_touchui.c): pad buttons for the player, ExternalInterface "dmPadBtn" of the `_d` movies
      if (g_on && dm_dos2_touchui_on() && !((v = getenv("DM_DOS2_TOUCHUI_PADBTN")) && *v == '0')) {
          if ((v = getenv("DM_DOS2_TOUCH_PAD_POLLS")) && atoi(v) >= 1 && atoi(v) <= 60) g_pp_polls = atoi(v);
          g_pp_on = dm_dos2_touchui_ei_register("dmPadBtn", dt_ei_padbtn, NULL);
          if (!((v = getenv("DM_DOS2_MM_WAIT_SAVE")) && *v == '0')) dm_dos2_touchui_ei_register("buttonPressed", dt_ei_mm_button, NULL);   // pack 3m (=0 off)
          if ((v = getenv("DM_DOS2_HUDRECT_SCALE")) && atof(v) > 0.1 && atof(v) < 10) g_hr_scale = (float)atof(v);
          if (!((v = getenv("DM_DOS2_HUDRECT")) && *v == '0')) {   // HUD element rects for the floating sticks (tools/dos2_hudrect_touch.py)
              const int ok = dm_dos2_touchui_ei_register("dmHudRects", dt_ei_hudrects, NULL);
              LOG("[touch] HUD rects: ExternalInterface \"dmHudRects\" of bottomBar_d / minimap_d / playerInfo_d -> D3PadView (no floating stick on them)%s (DM_DOS2_HUDRECT=0 off)",
                  ok ? "" : " - NOT registered");
          }
          LOG("[touch] console touch: ExternalInterface \"dmPadBtn\"(BTN code) of a `_d` movie = that pad button, held %d npad reads (hint taps; DM_DOS2_TOUCHUI_PADBTN=0 off)%s",
              g_pp_polls, g_pp_on && dm_dos2_touchui_ei_on() ? "" : " - NOT active: the ExternalInterface hook is off (DM_DOS2_TOUCHUI_EI=0 or its sites differ)");
      } }
    g_ui_ok = !memcmp(m->base + DT_UI_HASH, k_ui_hash, sizeof k_ui_hash) && !memcmp(m->base + DT_UI_WALK, k_ui_walk, sizeof k_ui_walk) &&
              !memcmp(m->base + DT_UI_CREATE_CALL, k_ui_create_call, sizeof k_ui_create_call);   // "in game" = the player UI manager has a set (pass 4)
    if (g_on && (v = getenv("DM_DOS2_TOUCH_WORLD")) && atoi(v) > 0) {
        float cx, cy;
        if (!g_ui_ok) LOG("【红】[touch] main+%#x / +%#x / +%#x differ (the player UI manager's hash): DM_DOS2_TOUCH_WORLD not applied", DT_UI_HASH, DT_UI_WALK, DT_UI_CREATE_CALL);
        else {
            g_world = atoi(v) == 2 ? 2 : 1;
            if (atoi(v) >= 3) {   // pass 8: PC world clicks (hle_dos2_touch_world.c); its hooks missing = plain =1
                g_w3 = dm_dos2_touch_world_on() && dm_dos2_route_on(); g_world_hold_ms = g_w3 ? 300 : 150;
                g_w3_confirm = (v = getenv("DM_DOS2_TOUCH_WORLD_CONFIRM")) && atoi(v) > 0;
                if ((v = getenv("DM_DOS2_TOUCH_WORLD_AFTER_MS")) && atoi(v) >= 0 && atoi(v) <= 5000) g_w3_after_ms = atoi(v);
                if ((v = getenv("DM_DOS2_TOUCH_WORLD_SETTLE")) && atoi(v) >= 1 && atoi(v) <= 30) g_w3_settle = atoi(v);
                LOG("[touch] DM_DOS2_TOUCH_WORLD=3: %s", g_w3 ? "a short tap on the world = the PC's left click at the finger (move / talk / attack / pick up / open); "
                    "the outline and the path preview follow the finger while it is down; hold or drag = left stick as with =1" :
                    "NOT active (the world hooks or the routing hook are off): behaves as DM_DOS2_TOUCH_WORLD=1 (walk + pad A)");
                if (g_w3) LOG("[touch] world click: left down %d poll(s) after the tap, picking window kept %d ms after the release, %s",
                              g_w3_settle, g_w3_after_ms, g_w3_confirm ? "first tap previews, second tap on the same spot acts (DM_DOS2_TOUCH_WORLD_CONFIRM=1)" : "one tap acts (DM_DOS2_TOUCH_WORLD_CONFIRM=1: two taps)");
            }
            if ((v = getenv("DM_DOS2_TOUCH_WORLD_HOLD_MS")) && atoi(v) >= 50 && atoi(v) <= 2000) g_world_hold_ms = atoi(v);
            if ((v = getenv("DM_DOS2_TOUCH_CTX_HOLD")) && *v >= '0' && *v <= '2') g_ctx_hold = *v - '0';
            if (!g_pp_on) g_pp_on = 1;   // the tap's A goes through the same npad pulse as the hint taps
            if ((v = getenv("DM_DOS2_TOUCH_WORLD_WALK_MS")) && atoi(v) >= 0 && atoi(v) <= 10000) g_world_walk_ms = atoi(v);
            if ((v = getenv("DM_DOS2_TOUCH_WORLD_PROBE_MS")) && atoi(v) >= 30 && atoi(v) <= 2000) g_world_probe_ms = atoi(v);
            if ((v = getenv("DM_DOS2_TOUCH_WORLD_C")) && sscanf(v, "%f,%f", &cx, &cy) == 2 && cx > 0 && cx < 1 && cy > 0 && cy < 1) { g_world_cx = cx; g_world_cy = cy; }
            g_rot_swap = (v = getenv("DM_DOS2_TOUCH_ROT_SWAP")) && atoi(v) > 0;
            g_swipe_rot = !((v = getenv("DM_DOS2_TOUCH_SWIPE_ROT")) && *v == '0');
            if ((v = getenv("DM_DOS2_TOUCH_SWIPE_X0")) && *v) { const float f = (float)atof(v); if (f >= 0.f && f <= 1.f) g_swipe_x0 = f; }   // pass 9 (A/B only: =0 old rule)
            g_place = !((v = getenv("DM_DOS2_TOUCH_PLACE")) && *v == '0');
            LOG("[touch] pass 9: one-finger swipe on the world (landed right of x = %.2f W, moved > 2.5 %% of the height within %d ms) = camera rotation %s; item placement taps %s", (double)g_swipe_x0, g_world_hold_ms,
                g_swipe_rot ? "ON (DM_DOS2_TOUCH_SWIPE_ROT=0 off)" : "OFF (DM_DOS2_TOUCH_SWIPE_ROT=0)", !g_place ? "OFF (DM_DOS2_TOUCH_PLACE=0)" : g_w3 ? "ON (DM_DOS2_TOUCH_PLACE=0 off)" :
                "inactive (needs DM_DOS2_TOUCH_WORLD=3)");
            LOG("[touch] DM_DOS2_TOUCH_WORLD=%d: in game with no menu on screen a finger %s is the world's: held / dragged = left stick from the screen point %.2f,%.2f, "
                "a short tap = walk (%d ms per screen height) + pad A; two-finger horizontal drag = camera rotation (right stick x%s)%s", g_world,
                g_world == 1 ? "whose left press no UI object takes (answer waited at most the probe time)" : "outside the HUD zones (bottom 20 %%, left 13 %%, top right)",
                (double)g_world_cx, (double)g_world_cy, g_world_walk_ms, g_rot_swap ? ", swapped" : "",
                g_world == 1 && !dm_dos2_route_on() ? " - NOT active: the routing hook (hle_dos2_touchui_route.c) is off, use DM_DOS2_TOUCH_WORLD=2" : "");
        }
    }
    if (g_on) {   // dos2-touch7: ZL / ZR wheels by touch / left stick (the radial section above)
        if ((v = getenv("DM_DOS2_TOUCH_RADIAL")) && *v == '0') g_rad_cfg = 0;
        if ((v = getenv("DM_DOS2_TOUCH_RADIAL_LSTICK")) && *v == '0') g_rad_lstick = 0;
        if ((v = getenv("DM_DOS2_TOUCH_RADIAL_MS")) && atoi(v) >= 30 && atoi(v) <= 1000) g_rad_ms = atoi(v);
        if (!g_ui_ok || !dm_dos2_route_on()) g_rad_cfg = 0;
        LOG("[touch] ZL / ZR wheels (panelSelect_c 0x53, partyManagement_c 0x52)%s: a finger = the right stick from the wheel's centre to the finger, lifted on an item = "
            "choose (the held trigger let go for the game after %d ms); the pad's left stick points the wheel too%s", g_rad_cfg ? "" : " OFF (DM_DOS2_TOUCH_RADIAL=0, or the routing hook / UI hash is off)",
            g_rad_ms, g_rad_lstick ? "" : " - OFF (DM_DOS2_TOUCH_RADIAL_LSTICK=0)");
    }
    if (g_on) {   // map view gestures (the map view section above)
        float cx, cy;
        if ((v = getenv("DM_DOS2_TOUCH_MAP")) && *v) g_map_cfg = atoi(v) == 2 ? 2 : atoi(v) > 0;
        if (!g_ui_ok || !dm_dos2_route_on()) g_map_cfg = 0;
        if ((v = getenv("DM_DOS2_TOUCH_MAP_C")) && sscanf(v, "%f,%f", &cx, &cy) == 2 && cx > 0 && cx < 1 && cy > 0 && cy < 1) { g_map_cx = cx; g_map_cy = cy; }
        if ((v = getenv("DM_DOS2_TOUCH_MAP_TAP_MS")) && atoi(v) >= 0 && atoi(v) <= 10000) g_map_tap_ms = atoi(v);
        if ((v = getenv("DM_DOS2_TOUCH_MAP_DRAG")) && *v) { const float f = (float)atof(v); if (f != 0 && fabsf(f) <= 10) g_map_drag = f; }
        if ((v = getenv("DM_DOS2_TOUCH_MAP_WAIT_MS")) && atoi(v) >= 50 && atoi(v) <= 2000) g_map_wait_ms = atoi(v);
        if ((v = getenv("DM_DOS2_TOUCH_MAP_PINCH_MS")) && atoi(v) >= 100 && atoi(v) <= 20000) g_map_pinch_ms = atoi(v);
        { float a, b, c, d; if ((v = getenv("DM_DOS2_TOUCH_MINIMAP")) && sscanf(v, "%f,%f,%f,%f", &a, &b, &c, &d) == 4 && a >= 0 && c <= 1 && a < c && b >= 0 && d <= 1 && b < d) { g_mm[0] = a; g_mm[1] = b; g_mm[2] = c; g_mm[3] = d; } }
        if (g_map_cfg && !g_pp_on) g_pp_on = 1;   // the tap's A goes through the npad pulse
        LOG("[touch] map view gestures%s: while journal_csp (0x8c) / journal_c (0x46) is up %s: drag = left stick (gain %.2f), pinch = right stick y (UIMapZoomIn/Out), "
            "tap (no journal button answered in %d ms) = left stick towards the point from %.2f,%.2f (%d ms per screen height) + A; pinch hold %d ms per screen height; "
            "minimap tap fallback = '-'", g_map_cfg ? "" : " OFF (DM_DOS2_TOUCH_MAP=0, or the routing hook / UI hash is off)",
            g_map_cfg == 2 ? "(any tab, DM_DOS2_TOUCH_MAP=2)" : "after a '-' (pad / map button / minimap)", (double)g_map_drag, g_map_wait_ms, (double)g_map_cx, (double)g_map_cy, g_map_tap_ms, g_map_pinch_ms);
    }
    if (g_live) LOG("[touch] DM_DOS2_TOUCH_LIVE=%d: the UI follows the last used device: touch -> PC UI, pad input -> controller UI (mode byte main+%#x, %s; "
                    "not sooner than %d ms after a switch); InjectInput main+%#x hooked for the pad's records", g_live, DT_MODE,
                    g_live == 2 ? "only while no player UI set exists (menu / loading)" : "in game the player UI manager [main+0x3298140] is rebuilt: main+0x10c4730 + main+0x10c4580",
                    g_live_ms, DT_INJECT);
    if (g_on && (v = getenv("DM_DOS2_TOUCH_KBM")) && atoi(v) > 0) {
        if (memcmp(m->base + DT_FILTER, k_filter, sizeof k_filter) || memcmp(m->base + DT_FILTER2, k_filter2, sizeof k_filter2))
            LOG("【红】[touch] main+%#x / +%#x differ: DM_DOS2_TOUCH_KBM not applied", DT_FILTER, DT_FILTER2);
        else if (atoi(v) >= 2 && (memcmp(m->base + DT_UI_HASH, k_ui_hash, sizeof k_ui_hash) || memcmp(m->base + DT_UI_WALK, k_ui_walk, sizeof k_ui_walk) ||
                                  memcmp(m->base + DT_UI_CREATE_CALL, k_ui_create_call, sizeof k_ui_create_call)))
            LOG("【红】[touch] main+%#x / +%#x / +%#x differ (the player UI manager's hash): DM_DOS2_TOUCH_KBM=2 not applied, the filter stays closed", DT_UI_HASH, DT_UI_WALK, DT_UI_CREATE_CALL);
        else if (atoi(v) >= 2) { g_kbm = 2; LOG("[touch] DM_DOS2_TOUCH_KBM=2: [mgr+%#x] = 1 only while no player UI set exists (menus, loading); in game it is 0, the controller HUD gets no mouse events", DT_MGR_KBM); }
        else { g_kbm = 1; LOG("[touch] DM_DOS2_TOUCH_KBM=1: [mgr+%#x] kept at 1 (the listener filter main+%#x lets keyboard / mouse records through in controller mode)", DT_MGR_KBM, DT_FILTER); }
    }
    if (((v = getenv("DM_DOS2_TOUCH_PCUI")) && atoi(v) > 0) || ui_start == UI_TOUCH) {   // UI mode TOUCH at start = this PC start (hle_dos2_uimode.c)
        static const uint32_t strb_wzr = 0x3900c11f;   // strb wzr,[x8,#0x30]
        if (ui_start == UI_TOUCH) {
            g_live_ui = g_on && g_live_ok;
            LOG("[touch] UI mode touch at start: the PC UI from the start (as DM_DOS2_TOUCH_PCUI=1)%s", g_live_ui
                ? "; a pad press brings the controller UI back, a touch the PC UI again (DM_DOS2_TOUCH_LIVE=1's switch)" : " - NO live pad switch (its sites differ or touch is off): the pad has no way back");
        }
        if (!g_on) LOG("【红】[touch] DM_DOS2_TOUCH_PCUI needs DM_DOS2_TOUCH=1 (the PC UI has no pad): not applied");
        else if (!g_mode_ok) LOG("【红】[touch] main+%#x / +%#x differ: DM_DOS2_TOUCH_PCUI not applied", DT_MODE_INIT, DT_UISET);
        else if (dt_poke(m, DT_MODE_INIT, &strb_wzr, 1)) LOG("【红】[touch] mprotect RW failed: DM_DOS2_TOUCH_PCUI not applied");
        else {
            g_pcui = 1;
            LOG("[touch] DM_DOS2_TOUCH_PCUI=1: main+%#x stores 0 in the mode byte main+%#x: keyboard-and-mouse UI (PC movies), the pad's records are "
                "filtered by the game; pinch = wheel, third finger = escape", DT_MODE_INIT, DT_MODE);
            if (g_live) LOG("[touch] DM_DOS2_TOUCH_LIVE: bindings stay Eoc/controller_1.json (the pad needs them after a switch); DM_DOS2_TOUCH_PCUI is only the start mode");
            else if (!g_keys_pref) LOG("[touch] DM_DOS2_TOUCH_PCUI_KEYS=0: bindings stay Eoc/controller_1.json (no wheel zoom, no escape)");
            else if (!g_bind_ok) LOG("【红】[touch] main+%#x / +%#x differ: the bindings path is left alone (no keyboard_1.json)", DT_BIND_STR, DT_BIND_REF);
            else if (dt_poke_ro(m, DT_BIND_STR, k_bind_keys, sizeof k_bind_keys)) LOG("【红】[touch] mprotect RW failed: the bindings path is left alone");
            else {
                g_bind_keys = 1;
                LOG("[touch] bindings path main+%#x: %s -> %s (loaded by main+0xc05ab0 for the four players)", DT_BIND_STR, k_bind_pad, k_bind_keys);
                if (g_live_ui) LOG("[touch] UI mode touch: after a pad press the controller UI runs with the keyboard_1.json bindings (controller_1.json is not loaded): "
                                   "DM_DOS2_TOUCH_PCUI_KEYS=0 keeps controller_1.json (no wheel zoom / escape then)");
            }
        }
    }
    if (g_dbg) LOG("[touch] DM_DBG=1: the first 64 records are logged (touch, window px, cursor read back, mode byte) + a summary every 600 polls with activity");
    const char *ui = getenv("DM_DOS2_TOUCH_UI");
    if (g_on && ui && atoi(ui) > 0) {
        static const uint32_t str_wzr = 0xb902c01f;   // str wzr,[x0,#0x2c0]
        if (memcmp(m->base + DT_SETDEV, k_setdev, sizeof k_setdev)) LOG("【红】[touch] main+%#x differs: DM_DOS2_TOUCH_UI not applied", DT_SETDEV);
        else if (dt_poke(m, DT_SETDEV + 0x18, &str_wzr, 1)) LOG("【红】[touch] mprotect RW failed: DM_DOS2_TOUCH_UI not applied");
        else LOG("[touch] DM_DOS2_TOUCH_UI=1: SetInputDevice main+%#x stores 0 (the player's input device stays not-controller: movies get "
                 "isControllerMode false)", DT_SETDEV + 0x18);
    }
    if (g_log) LOG("[touch] DM_DOS2_TOUCH_LOG=1: InjectInput main+%#x logs the first 64 distinct raw values", DT_INJECT);
}
#endif
