# DOS2 iPad IPA: разведка (14.11.2026)

Источник: `<ports>/divinity-original-sin-2_3.6.72.ipa` (com.larian.dos2ipad 3.6.72), распакован в `work/ipa/` (без локализаций,
кроме English/Russian); GUI обеих версий вытащен в `work/ipa/extract/{ipad_gui,sw_gui,ipad_platform}`. Из IPA ничего не запускалось. Разбор
статический: python + capstone, свои скрипты (вне дерева порта). Сравнение со Switch 3.6.117 (`work/romfs/Data`, `work/exefs-update/main.raw`).
Адреса iPad: VA Mach-O (база 0x100000000, file offset = VA − база). Адреса Switch: смещения в `main.raw`.

## 0. Коротко
- Паки iPad — тот же LSPK v13 (хвост 40 байт), `tools/lspk.py` читает их без правок.
- В IPA лежит **полный набор тач-UI**: 49 `*_t.swf` (+ `touchLibrary_t`, `uiElements_t`, `LSClasses_t*`) в `Data/Game.pak`. `Data/Gui.pak` —
  только 17 предконвертированных `.iggy` (контроллерные `_c` + общие), не тач.
- Движок iPad имеет **отдельные C++-классы тач-UI** (`UITouchPartyInventory`, `UITouchCraft` … 14 штук) и тач-устройство ввода `touchscr`;
  в Switch main их нет. Зато в Switch main есть весь **PC-путь мыши** (курсоры, PC-фильмы `hotBar.swf`…, `keyboard_1.json`, Flash mouse events),
  и найдена та же функция инъекции сырого ввода, что зовёт iOS-слой (`0x847690`).
- Рендер iPad: Metal с явными load/store по правилу «грузить только то, что писали в этом кадре», ленивый clear, отложенный store глубины;
  своего набора проходов нет — те же стадии, что на Switch, плюс несколько слитых/новых шейдеров (`CombinePPUI`, деинтерливинг SAO).
- Шейдеры материалов — только **AIR-биткод** (`Materials.metallib`, 15 693 функции, `air64-apple-ios13.5`); исходников MSL материалов в IPA нет.
  Исходники `_Metal.metal` движка (180) и CommunityMods (564) **байт-в-байт те же, что уже лежат в паках Switch**.
- Данные: IPA — 2.8 ГБ «минимального» набора, остальное у Apple (On-Demand Resources, 34 пака, в IPA нет). Текстуры iPad — KTX **ASTC 8×8**.

## 1. UI
### 1.1 Что лежит где
| | iPad | Switch |
|---|---|---|
| PC-фильмы (мышь, без суффикса) | `Game.pak`, все | **есть все** в `Game.pak` (`hotBar.swf`, `partyInventory.swf` …) |
| контроллерные `_c` | есть (+ iggy в `Gui.pak`) | есть (+ `buttonLayoutNX_c`, `consoleHintsNX_c`) |
| тач `_t` | **61 файл**: areaInteract, arenaResult, book, characterAssign, characterCreation, characterSheet, chatLog, combatLog, combatTurn, connectionMenu, containerInventory, contextMenu, dialog, equipmentPanel, examine, formation, gameMenu, giftBags*, hotBar, installScreen, itemSplitter, journal, journal_csp, loadingScreen, mainMenu, minimap, mouseIcon, msgBox, notification, optionsInput, optionsSettings, overhead, partyInventory, playerInfo, pyramid, reward, **rightSideButtons_t**, saveLoad, serverlist, **skillFireHelper_t**, skills, startTurnRequest, statusConsole, subtitles, textDisplay, tooltip, **touchLibrary_t**, trade, tutorialBox, uiCraft, uiElements_t(.swc), userProfile, waypoints, worldTooltip, LSClasses_t(_CN/_JA) | нет |
Общие с Switch файлы: 97 идентичны, 67 отличаются (мелкие правки; `connectivity_c` 1.1 МБ против 0.23, шрифты крупнее). Подписи путей в коде:
iPad main содержит жёсткие строки `GUI/hotBar_t.swf` … (`0x101d413d6` и соседи), Switch main — `GUI/hotBar.swf` и `_c`-варианты.

Отличие тач-UI от PC: собственные панели (`rightSideButtons_t` — боковая колонка кнопок, `skillFireHelper_t` — подтверждение навыка,
`touchLibrary_t` — общая библиотека жестов), колода навыков в hotBar (`didToggleDeckSwitcher`, `selectSkillSet`), перетаскивание из слотов
(`startDraggingFromSlot`, `closeAndDragItem`, `dropItem`), режим «витрины» в инвентаре (`switchWaresModeEnabled`, `switchItemWaresState`),
2-пальцевый скролл с подсказкой (`didUse2FingerScrolling`). От контроллерного UI отличается полностью (`_c` — радиальные меню и фокус-навигация).

### 1.2 Связь UI ↔ движок
Iggy + `ExternalInterface.call("имя", …)`; движок сравнивает имена строками (424 из 497 имён Switch-фильмов найдены как C-строки в Switch main;
остальные, вероятно, склеены линкером в хвосты длинных строк). Разбор ABC всех swf: тач-фильмы зовут 331 имя, из них **33 не используются ни одним
Switch-фильмом**; 25 из них есть строкой в iPad main и **нет** в Switch main (= на Switch не обработаются):
`cancelActiveAction clearRuneTarget clickedAction closeAndDragItem didToggleDeckSwitcher didUse2FingerScrolling dropItem hasBusySlotByItem
hideAllWindows hideFade openCombineSorter openContextWindow openMap rightBarBtnClicked selectSkillSet showFade showNotificationDetails
startDraggingFromSlot startDraggingRuneFromSlot switchDollEquipmentShowing switchDollVisibility switchItemWaresState switchWaresModeEnabled
toggleSidePanel` (+ `actionPressed`, `hideToolip` — опечатки/мёртвые, их нет и в iPad main).
Обратное направление (движок → фильм): iPad добавляет `setTouchScreenMode`, `addTouchGesture*`; `setControllerMode`/`isControllerMode`/`setInputDevice`
есть в обоих. Главное: на iPad за тач-фильмами стоят **свои классы UI в движке** (строки-имена типов, только iPad): `UITouchAreaInteract
UITouchBottomBar UITouchCharactersheet UITouchCraft UITouchJournal UITouchMagnifier UITouchMinimap UITouchObjectCharacterSwitcher
UITouchPanelEquipmentDoll UITouchPartyInventory UITouchRewardPanel UITouchSkillTree UITouchTrade UIRightSideTouchButtons`. Без них тач-фильмы
получат данные PC-классов (имена invoke в основном те же), но новые панели/жесты мертвы.
Фильмы, у которых все EI-имена есть в Switch main (пригодны «как есть»): characterAssign_t, connectionMenu_t, formation_t, installScreen_t,
itemSplitter_t, loadingScreen_t, mouseIcon_t, subtitles_t, textDisplay_t, tooltip_t, worldTooltip_t, LSClasses_t, uiElements_t, touchLibrary_t.

## 2. Ввод
### 2.1 iPad (ObjC-имена сохранены, C++ RTTI вырезан)
`LSMTLView` (ObjC, `UIView` + Metal layer): `touchesBegan/Moved/Ended/Cancelled` `0x100b85120/0x100b853fc/0x100b856f8/0x100b85874`,
распознаватели `onTap/onDoubleTap/onTap2..5Fingers/onLongPress(2/3Fingers)/onShortLongPress/onPinch/onRotate/onSwipe/onPan`
(`0x100b85e14…0x100b86e8c`), `UIShortTapGestureRecognizer`, курсор/мышь `setupMouse/captureCursor/warpCursor/setCursorPositionX:y:/
updateMousePosition:name:` (`0x100b83e2c…0x100b85cd4`, GCMouse — iPad-мышь). Все жесты сводятся к `sendLeftMouseDown/Up`, `sendRightMouseDown/Up`,
`sendTouchDown/Up`, `sendXYMotion` (`0x100b859d0…`), которые зовут **`0x1008872ac` = ls::InputManager::InjectInput(mgr, InjectInputData*, 0)**:
запись 20 байт `{u32 raw = (device<<16)|id, float2 value, u8 pressed, …}` в `ObjectSet<InjectInputData>` под SRW-замком. Касание = `raw 0x000C00C1`
(устройство 0xC = `touchscr`), левая кнопка = `0x0001xxxx` (устройство 1 = мышь). Раскладка `ls::InputManager` видна в ObjC type encoding (строка
`^{InputManager=…InjectInputData…InputDevice[13]…}`). Привязки: `GamePlatform.pak/PlayerProfiles/iOS/Eoc/touchscreen_1.json`
(`FlashLeftMouse ← touchscr:touch`, `CameraMove ← pan`, `CameraZoom ← pinch`, `ContextMenu ← longpress`, `TacticalCamera ← swipe 3f`,
`ShowWorldTooltips ← tap2f`, `ShowSneakCones ← tap3f`), то есть UI получает тач **как Flash-мышь**.

### 2.2 Switch main
- Есть: PC-курсоры (`CursorArrow…`, `Cursors/*.cur`), `UICursorInfo`, `UIMouseIcon`, `FlashLeftMouse/FlashMouseMove`, события `MOUSE_DOWN…`,
  `keyboard_1.json` + `inputconfig.json` (PC-раскладка в `GamePlatform.pak`), `setControllerMode`, имена сырого ввода `touch_tap…touch_press`
  (общий enum, наследие). Нет: устройства `touchscr`/`TouchScreen`, `UITouch*`, `setTouchScreenMode`, тач-распознавателей.
- **Мышь не опрашивается.** `nn::hid::GetMouseState`/`GetKeyboardState` вызываются ровно один раз — из конструктора `0xbdf360` (снимок в объект
  +0x08 клавиатура, +0x38 мышь; хвостовой вызов `0xbdf3e8`), конструктор зовёт `0x8a6fe8` (объект NX-ввода, vtable `0x29764e0`). Покадровый опрос
  `0x8a7d00…0x8a8a00` читает только Npad. HLE в `src/dm/host/hle_hid.c:1063` (`h_hid_MouseState`, нули) здесь ничего не решает.
- **Та же точка инъекции найдена:** `0x847690` = InjectInput Switch (тот же 20-байтный формат: `ldr w21,[x1]; ldur x8,[x1,#4]; ldrb [x1,#0xc]`,
  массив `+0x278/+0x280`, шаг 0x14, без замка). Опрос пада зовёт её 6 раз (`0x8a8478`, `0x8a84b8` …) с `raw = id | (device = [x27+0xe0]) << 16`.

### 2.3 Вывод: touch-as-mouse (как в PoE) — реально, точки хука
1. Из потока ввода хоста (тот же поток, что опрос пада, иначе гонка — у Switch-версии замка нет) звать `0x847690(mgr, {(1<<16)|id_left, {1,1}, 1})`;
   `mgr` = x0 вызова у `0x8a8478` (взять из живого регистра хуком на `0x847690`), id кнопок/движения — из таблицы имён сырого ввода (`mouse:left`,
   `mouse:motion` в `inputconfig.json`).
2. **Неизвестно (блокер №1):** откуда Switch-движок берёт координату курсора для UI-пикинга (на iOS — `setCursorPositionX:y:` → оконный слой). Нужен
   поиск потребителя `UICursorInfo`/`setCursorPosition` в Switch main (Ghidra, ~1–2 ч).
3. **Блокер №2:** переключится ли UI на PC-фильмы (`hotBar.swf`) при вводе мыши или режим контроллера на NX зашит (`setControllerMode`). Проверить
   опытом: инъекция + лог загрузки `GUI/*.swf` (хост уже видит открытия файлов).
4. Подмена тач-фильмов: положить `_t.swf` под PC-именами в оверрайд-пак (движок грузит фиксированные пути). Работает только для 14 «совместимых»
   (§1.2) и частично для остальных; полноценный тач-UI = переписать 14 классов `UITouch*` + 25 callback'ов на стороне движка — это большой объём.

## 3. Рендер (Metal-бэкенд iPad)
- Начало прохода `≈0x100b2ec00…0x100b2f428`: до 8 цветовых вложений; **цвет: store всегда Store(1)**; load = **Clear(2)**, если на текстуре висит
  отложенный clear (флаг `+0x58`/маска слоёв `+0x60`, ставится запросом очистки), иначе **Load(1) только если `tex[+0x5c] == frame[+0x930]`**
  (писали в этом кадре), **иначе DontCare(0)**; после — `tex[+0x5c]=0`. Глубина/стенсил: load тем же правилом, **store = Unknown(4)** на старте,
  в конце, если никто не запросил, → DontCare (`0x100b2f314`, `0x100b2f368`); запрос «нужна глубина дальше» — хелперы `0x100b2c3d0/0x100b2c43c`
  (`setDepthStoreAction:1` на энкодере). Отдельный clear-проход только если флаг не съеден проходом: `0x100b24900` (Clear+Store, пустой энкодер).
  Синхронизация между энкодерами — `MTLFence` (`updateFence/waitForFence`), ресурсы в `MTLHeap` (`newHeapWithDescriptor` ×5).
  `setStorageMode:` с константой 2 (Private); Memoryless (3) явно не встречен; imageblock/tile shaders/`parallelRenderCommandEncoder` не используются.
- Список стадий тот же, что у Switch (Shadow/Deferred Buffer/Deferred Lighting/SSAO/Reflection/PostProcess…). Различия в наборе движковых шейдеров
  (`Core.metallib` 166 функций против `EngineShaders.pak` 149):
  только iPad — **`CombinePPUI`** (пост + UI в одном проходе), `CompressRG11B10F`, `CompressBC4`, `PPSAO_Deinterleave/Reinterleave`,
  `Orthographic_*` (каскады/туман для орто-камеры), `LightVolumeVS/PS`, `DeferredPoint/SpotLightVS`, `DirectionalShadowVS`;
  только Switch — `CB_Resolve` (шахматка), `DownsampleDepth*`, `BilateralUpsample`, `CrossBilateralUpsample`, `CalculateBilateralOffsets`,
  `DeferredDirectionalWithSSAO`, `PPFXAA`, `CombineUI*` (4 варианта), VSM, каскады 512/768 и 3×. Строки только iPad: `Clustered Lighting`,
  `LightVolume Apply/Blur`, `AOTexture`, `fBloomActive/fFogActive/fLightVolumeActive`, `uniformsFog/uniformsTonemap` (параметры одного пост-шейдера);
  только Switch: `Downsample Depth`, `Resolve Offscreen Particles`, `Light Shadows`, `Ambient Occlusion`.
  Итого: iPad = clustered-only освещение (направленный свет внутри кластерного прохода), без шахматки и полуразрешённых апскейлов, пост+UI одним
  проходом, SAO с деинтерливингом. Это и есть «слитые проходы» статей; каких-то новых «тайловых» механизмов Metal (tile memory) нет.
- Материалы: в iPad добавлен вариант **`_SHA`** (отдельный шейдер теневого прохода) у всех 1815 материалов; у Switch тени идут через `_DEP`.

## 4. Шейдеры
- `Materials.metallib` 99.8 МБ, MTLB v1.2.4, 15 693 функции `Materials_<путь>_<SK|ST|STI>_<DEF|DEP|FOR|SHA>_Metal_{vertexMain,fragmentMain}`,
  каждая — LLVM bitcode в обёртке `0x0B17C0DE`, триплет `air64-apple-ios13.5.0`; `Core.metallib` 1 МБ, 173 функции, `ios14.1`.
- Путь в SPIR-V: готового инструмента нет. `llvm-dis` в установленном LLVM 21 нет; Apple-биткод версионно несовместим с апстримом, а SPIRV-LLVM-Translator
  понимает OpenCL-IR, не графику с `air.*`-интринсиками (сэмплеры, varyings, буферные аргументы). `metal-shaderconverter` — только DXIL → metallib.
  Свой AIR → SPIR-V = десятки агент-часов + риск на каждом виде интринсика; не стоит.
- MSL-исходников материалов в IPA нет. `MSLShaders.pak` (7494) = 7260 `_Metal.bshd` (это 2 строки: имена vertex/fragment-функций в metallib) +
  88 `.lsb`. Исходники `_Metal.metal`: `Engine.pak` 180 и `CommunityMods.pak` 564 — **CRC совпадают со Switch** (те же 744 файла, что уже есть).
  `ShaderCache.pak`: 3970 `.pso` (LSF «GP»: vertexAttribs, blend, pixelFormat, depth/stencil формат на материал×вариант) — каталог реальных
  сочетаний состояния, можно использовать как прогревочный список.
- Ключи материалов: путь `.bshd` совпадает 1:1 (`Public/.../X_ST_DEF_NX.bshd` ↔ `X_ST_DEF_Metal.bshd`): общих 5445 из 5448 Switch (нет только 3
  `FX_MAT_BasicAlphaBlendingParticles_*_C`), iPad добавляет 1815 `_SHA`.

## 5. Данные
- В IPA только «минимальные» паки: `MinData/LevelResources.pak` (1287 текстур, 768 GR2), `MinDataMods` (≈ Origins/Shared без текстур — lsj/lsf/patch),
  `MinDataSharedSounds` (4813 .wem), `MinDataMinimaps`; остальное (34 ODR-пака: textures×3, origins×2, shared×2, sounds, voice, video, extra)
  скачивается с App Store и в IPA отсутствует. Всего 2.8 ГБ против 7.2 ГБ Switch; из 35 960 Switch-текстур в IPA есть 1529.
- Текстуры iPad — KTX с расширением `.dds`, `glInternalFormat 0x93B7` = **ASTC 8×8** (2 bpp), тех же размеров, что Switch (`DFvN`, NVN-тайлинг).
  Для слабых телефонов как опция «меньше VRAM» пригодно только частично (подмножество), а у Switch уже есть `LowTex.pak` (12 893 `_lowtex.dds`).

## 6. Рекомендация (по убыванию отдачи)
1. **Политика load/store как у iPad в нашем Vulkan-слое** (`xbg_game.cpp` [tile], без кода iPad): штамп «RT писан в этом кадре» → loadOp LOAD только
   при совпадении, иначе DONT_CARE; запрос очистки — отложенный флаг, сливается в loadOp CLEAR следующего прохода; depth/stencil store = DONT_CARE,
   если до конца кадра никто не читает (наш парковочный A-B-A даёт нужный look-ahead). Сверить с R3/R4/R7 (`NOLOAD`, `STORE_DEAD`). **4–8 агент-ч** +
   прогон на Mali/PowerVR (ABBA).
2. **Touch-as-mouse на Switch-движке** через `0x847690`: хук + маппинг жестов по `touchscreen_1.json`; сначала опыт на 2 вопроса (§2.3 п.2–3).
   Найти источник координаты курсора — **1–2 агент-ч** (Ghidra headless, свой проект); инъекция + жесты — **4–6 агент-ч**; 1–2 прогона на телефоне
   (с разрешения хозяина). Если UI на NX зашит в контроллерный режим — патч переключателя режима, ещё **2–4 ч**.
3. **Тач-фильмы `_t`** поверх п.2: сначала 14 совместимых (оверрайд-пак под PC-именами) — **2–3 ч**; полные hotBar/inventory/craft/trade/
   rightSideButtons требуют переписать 14 классов `UITouch*` + 25 callback'ов на хосте — **40–80 агент-ч**, только если п.2 даст играбельный PC-UI.
4. **Слитые проходы по образцу iPad** для нативного бэкенда: пост+UI одним проходом (`CombinePPUI`), без шахматки/апскейлов — это изменения
   движка/шейдеров; в текущем хосте реалистично только через уже спроектированный A-B-A / subpass. Как ориентир, не задача.
5. Не делать: AIR → SPIR-V (нет инструмента; исходников материалов нет), подмену текстур на ASTC из IPA (подмножество, нужен свой загрузчик KTX).
