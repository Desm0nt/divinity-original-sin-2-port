# DOS2: тач-UI с iPad на Switch-движке — план (14.11.2026, ветка `dos2-touchui`)

Решение хозяина: на телефоне основной интерфейс — айпадовский тач-UI; контроллерный (`_c`) — по выбору в меню или когда подключён
геймпад; лучше всего — касание и пад вместе. Режима касания, который выключает пад, не выпускаем.

Разбор статический, на телефоне ничего не запускалось. iPad 3.6.72 (`work/ipa/Payload/…/Divinity Original Sin 2`, адреса = VA Mach-O),
Switch 3.6.117 (`work/exefs-update/main.raw`, main 4979b200, адреса = смещения). Инструменты (в этой ветке, `tools/`):
- `dos2_swf_abc.py` — свой разбор SWF / AVM2 ABC (JPEXS и других декомпиляторов на машине нет): класс документа, его методы (то, что
  движок зовёт через invoke), имена `ExternalInterface.call` с местом вызова; `dump` / `json` / `diff`.
- `dos2_ui_types.py` — таблица типов UI из регистратора: id типа, путь фильма, функция-создатель, размер объекта, конструктор, vtable (iPad и Switch).
- `dos2_ui_sites.py` — все вызовы `CreateUIObject` в Switch main с id типа и пометкой «рядом читается байт режима».

Основа: `IPA_RECON.md`, разделы касания в `DOS2_PORT.md`, шапка `dm/host/hle_dos2_touch.c`, прогоны t1…t5.

## 0. Главное
1. **В IPA лежит полный набор тач-фильмов**: 56 файлов `*_t.swf` в `Data/Game.pak` (`Public/Game/GUI/`), 35.7 МБ. Ни один не отсутствует:
   UI не входит в On-Demand Resources. Уже распакованы в `work/ipa/extract/ipad_gui/`.
2. **У `_t`-фильмов на iPad свои id типов (0x94…0xce), но у 38 фильмов из 52 зарегистрированных — тот же класс движка, что у PC / `_c`-фильма.**
   Отдельных классов только 15 (14 `UITouch*` / `UIRightSideTouchButtons` + второй `playerInfo_t`). Все «общие» классы есть в Switch main.
3. **`_t`-фильм — это доработанный PC-фильм, а не `_c`**: набор методов документа почти совпадает с PC (`mainMenu_t` — те же 54 метода + 1,
   `optionsSettings_t` и `loadingScreen_t` — без отличий), с `_c` расходится сильно (`addBtnHint`, `addOption` … против `addButton` …).
   Значит, классу нужна его PC-ветка, а не контроллерная.
4. **iPad — три режима, не два** (поправка к `DOS2_PORT.md`, проход 4): 0 = клавиатура и мышь, 1 = контроллер, 2 = касание
   (`u32 [0x101ecf458]`). Писатели: `0x10088ac64` → 0, `0x10088adb8` → 2 и **`0x10088a714(mgr, устройство, 1)` → 1** (раздаёт пады 3/4
   игрокам). Слой ввода (`0x100c0054c`) и UI (`0x1012f1ee8`) переключают режим по классу последнего использованного устройства.
   Каждое место создания UI выбирает id так: режим 2 → `_t`, 1 → `_c`, иначе PC (`0x1012e85b8`: `cmp w8,#2` → тип 0xa4 `mainMenu_t`).
   То есть на iPad с геймпадом: нажал на пад — получил `_c`-UI, коснулся экрана — `_t`-UI. Одновременно оба набора не живут.
5. **В Switch-сборке PC-ветки создания UI вырезаны** (поправка к проходу 3): главное меню создаётся как тип 0x48 `mainMenu_c` без проверки
   режима (`0x10c68e0`), сборщик набора игрока `0x10c06d0` при байте режима 1 создаёт `_c`-панели, при 0 — только общий хвост
   (тултипы, overhead, уведомления); вызова `CreateUIObject` с типами PC-HUD (0x28 `hotBar`, 0x1e `minimap`, 0x74, 0x77 …) в main нет вовсе.
   Это и есть причина прогона t4 (PC-режим, а меню `_c`). PC-режим «как есть» на Switch = игра без HUD. Тупик подтверждён статикой.
   При этом **классы PC-UI в main есть** (создатели зарегистрированы, vtable на месте) — их просто никто не создаёт.
6. Отсюда рабочая схема: **хост сам ведёт тач-набор** — подменяет путь фильма в записях создателей (данные, не код) и сам зовёт
   `CreateUIObject` для типов, которые Switch-сборщик не создаёт. Ввод уже есть: `_t`-фильмы получают касание как Flash-мышь
   (`touchscreen_1.json`: `FlashLeftMouse ← touchscr:touch`), прокрутку пальцем делают сами (`LS_Classes.touchVerticalScroller` на
   MOUSE_DOWN / MOVE / UP) — это ровно то, что даёт `hle_dos2_touch.c` после прохода 6.

## 1. Инвентарь
Столбцы: размер; id типа на iPad; класс движка на iPad; что есть в `Game.pak` Switch под тем же именем; «новые методы» — сколько методов
документа нет у PC-фильма Switch (у фильмов без PC-пары — все); «invoke только iPad» — из них те, чьё имя есть строкой в iPad main и нет в
Switch main (движок Switch их не пошлёт); «EI нет в Switch main» — имена `ExternalInterface.call`, которых нет строкой в Switch main
(обработчика нет; `didUse2FingerScrolling` опущен — его нет в Switch main, он у 26 фильмов, это отметка для обучения). Полные списки — `dos2_swf_abc.py dump`.

| фильм `_t` | КБ | id iPad | класс iPad | Switch | новые методы | invoke только iPad | EI нет в Switch main |
|---|---|---|---|---|---|---|---|
| areaInteract | 135 | 0xa6 | **свой: UITouchAreaInteract** | _c | 24 | — | itemSelected |
| arenaResult | 290 | 0xc9 | общий с arenaResult 0x7d | PC | 0 | — | — |
| book | 324 | 0xac | общий с book 0x2 | PC | 0 | — | loadresource |
| characterAssign | 633 | 0xb5 | общий с characterAssign 0x34, characterAssign_c 0x5c | PC + _c | 2 | — | — |
| characterCreation | 4963 | 0xab | общий с characterCreation 0x3, characterCreation_c 0x4 | PC + _c | 6 | — | — |
| characterSheet | 1169 | 0x9d | **свой: UITouchCharactersheet** | PC | 12 | addFastTooltipToElementsById setEquipmentVisibilityButtonState setPlusButtonFastTooltip setTalentPlusButtonFastTooltip | switchDollEquipmentShowing |
| chatLog | 71 | 0xc2 | общий с chatLog 0x6 | PC | 4 | resetChatMaxBottomY setChatMaxBottomY setHasNewMessageFlag | InputOut InputOver |
| combatLog | 60 | 0xce | общий с combatLog 0x7 | PC + _c | 1 | resizeByType | InputOut InputOver |
| combatTurn | 42 | 0x8 | тот же id, что PC (combatTurn) | PC | 0 | — | positionAnimationFinished registeranchorId |
| connectionMenu | 658 | 0xad | общий с connectionMenu 0x21, connectivity_c 0x22 | PC | 1 | — | — |
| containerInventory | 91 | 0xb7 / 0xb8 | общий с containerInventory 0x9, GM/containerInventoryGM 0x8f; общий с containerInventory 0x25 | PC | 3 | setIsMultipleTooltipMode | getItemList hasBusySlotByItem slotDown |
| contextMenu | 16 | 0x9f / 0xa0 | общий с contextMenu 0xa, contextMenu_c 0xc; общий с contextMenu 0xb, contextMenu_c 0x60 | PC + _c | 3 | setHasItemToDrag | closeAndDragItem |
| dialog | 125 | 0xa2 | общий с dialog 0xe | PC + _c | 1 | addFastTooltipToElementsById | — |
| equipmentPanel | 196 | 0x9b | **свой: UITouchPanelEquipmentDoll** | _c | 61 | addFastTooltipToElementsById setDraggingState setEquipmentVisibility setInventoryTitleText setIsMultipleTooltipMode | hasBusySlotByItem hideFade showFade slotDown |
| examine | 1035 | 0xb4 | общий с examine_c 0x43, examine 0x68 | PC + _c | 1 | — | slotDown |
| formation | 347 | 0xb6 | общий с formation 0x82, formation_c 0x87 | PC + _c | 0 | — | — |
| gameMenu | 711 | 0xca | общий с gameMenu 0x13, gameMenu_c 0x4d | PC + _c | 1 | — | registeranchorId |
| giftBagsContent | 226 | 0xc0 | общий с giftBagContent 0x93 | — | 14 | — | — |
| giftBagsMenu | 1379 | 0xbd | общий с giftBagsMenu 0x92 | PC | 0 | — | — |
| hotBar | 300 | 0x97 | **свой: UITouchBottomBar** | PC | 11 | addFastTooltipToElementsById setCancelActionBtnVisible setDropItemWidgetVisibility setDropWindgetEnabled setHasNewMessageFlag | cancelActiveAction didToggleDeckSwitcher dropItem selectSkillSet slotUpEnd |
| installScreen | 4377 | 0xcb | общий с installScreen_c 0x50 | _c | 28 | — | — |
| itemSplitter | 71 | 0xa9 | общий с itemSplitter 0x15, itemSplitter_c 0x55 | PC + _c | 1 | — | — |
| journal | 2819 | — | не регистрируется (journal_t: в коде только journal_csp_t) | PC + _c | 1 | — | questPressed |
| journal_csp | 509 | 0x95 | **свой: UITouchJournal** | PC | 10 | addFastTooltipToElementsById setAvtivityButtonLabels setCustomSelectedQuest setInactiveFilterLabel setMapHelpLabels setMapScaleDiff setOnMapButtonLabel setTrackButtonLabels | — |
| loadingScreen | 37 | 0x17 | тот же id, что PC (loadingScreen) | PC | 0 | — | — |
| LSClasses | 34 | — | библиотека (не тип UI) | PC | 0 | — | — |
| mainMenu | 2241 | 0xa4 / 0xa5 | общий с mainMenu 0x1c; общий с mainMenu 0x32 | PC + _c | 1 | setGiftBagsButtonLabel | menuButtonOver |
| minimap | 468 | 0x94 | **свой: UITouchMinimap** | PC + _c | 13 | setArenaModeFlag setBtnActive setMenuBtnInfo setMenuInfoVisible setMenuVisible | diplomacyPressed openMap |
| mouseIcon | 8 | 0xa1 | общий с mouseIcon 0x1f | PC | 0 | — | — |
| msgBox | 743 | 0xae / 0xaf | общий с msgBox 0x1d, msgBox_c 0x4b; общий с msgBox 0x12, msgBox_c 0x4c | PC + _c | 4 | resetMaxBottomY setMaxBottomY | inputText |
| notification | 239 | 0xcd | общий с notification 0x24 | PC | 0 | — | registeranchorId showNotificationDetails |
| optionsInput | 459 | 0xc7 | общий с optionsInput 0xd | PC | 4 | addTouchGesture addTouchGestureDivider addTouchGestureHeader resetGestureDescriptionList | — |
| optionsSettings | 300 | 0xc3 / 0xc4 / 0xc5 / 0xc6 | общий с optionsSettings 0x1, optionsSettings_c 0x58; общий с optionsSettings 0x11, optionsSettings_c 0x59; общий с optionsSettings 0x23, optionsSettings_c 0x5a; общий с optionsSettings 0x2d, optionsSettings_c 0x5b | PC + _c | 0 | — | menuButtonOver selectorID |
| overhead | 55 | 0xbb | общий с overhead 0x5 | PC | 1 | — | clickedAction |
| partyInventory | 786 | 0x9c | **свой: UITouchPartyInventory** | PC + _c | 13 | addFastTooltipToElementsById addSetWaresModeTitle setEquipmentButtonState setIsMultipleTooltipMode | hasBusySlotByItem hideToolip openCombineSorter slotDown switchDollVisibility switchItemWaresState switchWaresModeEnabled |
| playerInfo | 119 | 0x96 / 0xa8 | **свой: (own creator, class not named)**; **свой: UITouchObjectCharacterSwitcher** | PC | 3 | showPlayersStatuses | hidetooltip |
| pyramid | 86 | 0xc1 | общий с pyramid 0x81, pyramid_c 0x86 | PC + _c | 8 | setConfirmButtonLabel | hideAllWindows |
| reward | 105 | 0xbc | **свой: UITouchRewardPanel** | PC + _c | 6 | addDraggingItemToInventory setIsMultipleTooltipMode | hasBusySlotByItem slotDown |
| rightSideButtons | 920 | 0x99 | **свой: UIRightSideTouchButtons** | — | 13 | setNewJournalEntriesMarkerVisible setRolledIn | rightBarBtnClicked toggleSidePanel |
| saveLoad | 949 | 0xba | общий с saveLoad 0x27, saveLoad_c 0x4a | PC + _c | 1 | — | selectCharTab |
| serverlist | 3124 | 0xb2 / 0xb3 | общий с serverlist 0x18; общий с serverlist 0x1a, serverlist_c 0x1b | PC + _c | 5 | resetChatMaxBottomY setChatMaxBottomY | InputOut InputOver dropDownChanged userOut userOver userPressed |
| skillFireHelper | 27 | 0xcc | **свой: UITouchMagnifier** | — | 9 | showMagnifyingGlass | actionPressed |
| skills | 395 | 0x9e | **свой: UITouchSkillTree** | PC | 11 | addFastTooltipToElementsById setMemSlotsStats | — |
| startTurnRequest | 69 | 0xc8 | общий с startTurnRequest_c 0x90, startTurnRequest 0x91 | PC + _c | 0 | — | registeranchorId |
| statusConsole | 328 | 0x98 | общий с statusConsole 0x75 | PC | 2 | setCharacterBarsVisible | animDone |
| subtitles | 8 | 0x20 | тот же id, что PC (subtitles) | PC | 0 | — | — |
| textDisplay | 42 | 0xaa | общий с textDisplay 0x2b | PC | 2 | moveWorldTooltip | — |
| tooltip | 40 | 0xbf | общий с tooltip 0x2c | PC | 1 | — | — |
| touchLibrary | 29 | — | библиотека (не тип UI) | — | 1 | — | — |
| trade | 317 | 0xa3 | **свой: UITouchTrade** | PC + _c | 7 | addFastTooltipToElementsById setIsMultipleTooltipMode setPlayerFilterName setTotalText setTraderFilterName switchMode | getItemList hasBusySlotByItem openContextWindow outItem slotDown |
| tutorialBox | 71 | 0xb9 | общий с tutorialBox 0x37, tutorialBox_c 0x5e | PC + _c | 0 | — | fadeOutNonModalPointerDone |
| uiCraft | 616 | 0xa7 | **свой: UITouchCraft** | PC | 8 | addFastTooltipToElementsById addItemSlotTooltips setCharacterGoldWeight setIsMultipleTooltipMode | clearRuneTarget hasBusySlotByItem openCombineSorter slotDown startDraggingFromSlot startDraggingRuneFromSlot |
| uiElements | 14 | — | библиотека (не тип UI) | PC | 0 | — | — |
| userProfile | 827 | 0xb1 | общий с userProfile 0x33 | PC | 0 | — | ClosePressed SelectProfile |
| waypoints | 856 | 0xb0 | общий с waypoints 0x2f, waypoints_c 0x4e | PC + _c | 1 | — | registeranchorId selectedWaypoint |
| worldTooltip | 4 | 0xbe | общий с worldTooltip 0x30 | PC | 1 | — | — |

Итого 56 файлов, 35 727 539 байт (34.1 МиБ). Не `_t`, но нужны: ничего — импорты тач-фильмов: `fonts_en.swf` (51 фильм),
`consoleHints_c.swf` (9), `LSClasses.swf` (7), `uiElements.swf` (3), `texture_lib.swf`, `tooltipHelper(_kb).swf`,
`containerInventory_lib.swf` — всё это есть в `Game.pak` Switch; `uiElements_t.swf` (8 фильмов, 15 КБ) — берётся из IPA.
`journal_t.swf` (2.9 МБ) в коде iPad не регистрируется (журнал — `journal_csp_t`), не нужен. Реально нужный объём ≈ 32.8 МБ.

Чего на Switch нет совсем: `rightSideButtons` (боковая колонка кнопок), `skillFireHelper` (лупа / подтверждение навыка),
`giftBagsContent` под этим именем (на Switch `giftBagContent.swf`), `touchLibrary`. PC-пары нет, есть только `_c`: `areaInteract`,
`equipmentPanel`, `installScreen`. Всего в `Game.pak` Switch 117 swf (PC и `_c`), классы типов — `tools/dos2_ui_types.py sw`.

## 2. Движок iPad
### 2.1 Таблица типов
Регистратор `0x101336d00…0x10133fd40`: на каждый тип — запись создателя `{функция, std::string путь}` (0x18 байт) и
`0x1004af75c(менеджер, id, создатель)`. Создатель: `operator new(размер)` + переход в конструктор. 173 записи; `_t` идут блоком
после `startTurnRequest` (id 0x94…0xce), три фильма подменены прямо под PC-id: `combatTurn_t` (8), `loadingScreen_t` (0x17),
`subtitles_t` (0x20). Общий создатель = общий класс: `mainMenu_t` 0xa4 / 0xa5 ↔ `mainMenu` 0x1c / 0x32, `msgBox_t` 0xae / 0xaf ↔
`msgBox` 0x1d / 0x12 и `msgBox_c` 0x4b / 0x4c, `gameMenu_t` 0xca ↔ `gameMenu` 0x13 и `gameMenu_c` 0x4d, `saveLoad_t` 0xba,
`optionsSettings_t` 0xc3…0xc6, `characterCreation_t` 0xab, `dialog_t` 0xa2, `contextMenu_t` 0x9f / 0xa0, `examine_t` 0xb4 и т. д.

### 2.2 Отдельные классы тач-UI
| класс (строка-имя) | id | фильм | создатель | размер | конструктор | базовый конструктор | vtable | EI, которых нет в Switch main |
|---|---|---|---|---|---|---|---|---|
| UITouchJournal | 0x95 | journal_csp_t.swf | `0x10134105c` | 0x320 | встроен | — | — | — |
| UITouchMinimap | 0x94 | minimap_t.swf | `0x101341124` | 0x4b0 | `0x1018fc040` | `0x10192a26c` | `0x101e5aa38` `0x101e5a1b0` | diplomacyPressed openMap |
| UIRightSideTouchButtons | 0x99 | rightSideButtons_t.swf | `0x10134114c` | 0x150 | `0x10179aa48` | `0x1004a8724` | `0x101e54f50` `0x101e50660` | rightBarBtnClicked toggleSidePanel |
| ? (playerInfo_t, свой создатель) | 0x96 | playerInfo_t.swf | `0x101341174` | 0x200 | встроен | — | — | hidetooltip |
| UITouchBottomBar | 0x97 | hotBar_t.swf | `0x1013411e8` | 0x310 | `0x1018c6368` | `0x1017d32ac` | `0x101df42b8` `0x101e58d30` | cancelActiveAction didToggleDeckSwitcher dropItem selectSkillSet slotUpEnd |
| UITouchPanelEquipmentDoll | 0x9b | equipmentPanel_t.swf | `0x101341210` | 0x2e0 | `0x1018a7104` | `0x10180f42c` | `0x101e57a38` `0x101e571b8` | hasBusySlotByItem hideFade showFade slotDown |
| UITouchPartyInventory | 0x9c | partyInventory_t.swf | `0x101341238` | 0x3d0 | `0x1015a91f0` | `0x1013a3fc0` | `0x101e41378` `0x101e411d0` | hasBusySlotByItem hideToolip openCombineSorter slotDown switchDollVisibility switchItemWaresState switchWaresModeEnabled |
| UITouchCharactersheet | 0x9d | characterSheet_t.swf | `0x101341260` | 0x2d0 | `0x1013693cc` | `0x1004a8724` | `0x101e54f50` `0x101e30018` | switchDollEquipmentShowing |
| UITouchSkillTree | 0x9e | skills_t.swf | `0x101341288` | 0x2f0 | встроен | — | — | — |
| UITouchCraft | 0xa7 | uiCraft_t.swf | `0x1013412d4` | 0x470 | встроен | — | — | clearRuneTarget hasBusySlotByItem openCombineSorter slotDown startDraggingFromSlot startDraggingRuneFromSlot |
| UITouchTrade | 0xa3 | trade_t.swf | `0x1013413cc` | 0x3b0 | `0x10164fe60` | `0x10151b608` | `0x101e48380` `0x101e481a0` | getItemList hasBusySlotByItem openContextWindow outItem slotDown |
| UITouchObjectCharacterSwitcher | 0xa8 | playerInfo_t.swf | `0x1013413f4` | 0x190 | `0x101717124` | `0x1004a8724` | `0x101e54f50` `0x101e4c880` | hidetooltip |
| UITouchAreaInteract | 0xa6 | areaInteract_t.swf | `0x10134141c` | 0x270 | встроен | — | — | itemSelected |
| UITouchRewardPanel | 0xbc | reward_t.swf | `0x1013414a8` | 0x280 | встроен | — | — | hasBusySlotByItem slotDown |
| UITouchMagnifier | 0xcc | skillFireHelper_t.swf | `0x10134152c` | 0x160 | `0x101927de0` | `0x1004a8724` | `0x101e54f50` `0x101df42b8` | actionPressed |

- Имена классов привязаны по ссылкам на строки (`UITouchBottomBar` ← `0x1018cb4d8` и т. д., рядом с конструктором класса).
- Наследование видно по базовому конструктору: `UITouchPartyInventory` — наследник PC-инвентаря (`0x1013a3fc0`, рядом с конструктором
  `partyInventory` `0x1013a419c`); `UITouchTrade` — та же база, что `trade` и `trade_c` (`0x10151b608`); `UITouchMinimap` — база
  `minimap` (`0x10192a26c`); `UITouchPanelEquipmentDoll` — база контроллерных панелей (`0x10180f42c`, как `equipmentPanel_c`,
  `statsPanel_c`, `craftPanel_c`); `UITouchBottomBar` — своя база `0x1017d32ac`; остальные — прямо от `UIObject` (`0x1004a8724`).
  То есть тач-классы — это PC / `_c`-классы с добавками: перетаскивание из слотов, режим «витрины», колода навыков, боковая панель.
- Что каждый делает сверх PC-класса — видно по контракту фильма (столбцы «invoke только iPad» и «EI нет в Switch main» в §1): новые
  обработчики (25 имён на все фильмы: `cancelActiveAction clearRuneTarget clickedAction closeAndDragItem didToggleDeckSwitcher dropItem
  hasBusySlotByItem hideAllWindows hideFade openCombineSorter openContextWindow openMap rightBarBtnClicked selectSkillSet showFade
  showNotificationDetails startDraggingFromSlot startDraggingRuneFromSlot switchDollEquipmentShowing switchDollVisibility
  switchItemWaresState switchWaresModeEnabled toggleSidePanel` …) и новые invoke (`setCancelActionBtnVisible`,
  `setDropItemWidgetVisibility`, `setIsMultipleTooltipMode`, `addFastTooltipToElementsById`, `setRolledIn`, `showMagnifyingGlass` …).
- **Не сделано:** функции-обработчики EI внутри этих классов не разобраны (у каждого класса свой `OnFunctionCalled` со сравнением строк);
  это работа подзадач, где класс переписывается (§5, TU-09+).

### 2.3 Выбор `_t` / `_c` / PC и геймпад
- Режим `u32 [0x101ecf458]` (структура `0x101ecf428` + 0x30; на Switch близнец — байт `main+0x2bb8918`). Места создания UI сравнивают
  его с 2 и с 1. Пример — главное меню `0x1012e85b8…0x1012e85f0`: режим 2 → `CreateUIObject(…, слой 0x19, тип 0xa4, флаги 0x20021, 0x80, игрок)`.
- Смена режима на ходу: `0x1012f1ee8` (UI) и `0x100c0054c` (слой ввода; срабатывает, только если у объекта UI стоит бит 3 флагов,
  нет модального окна и т. п.) зовут: 0 → `0x1015c4910` (→ `0x10088ac64`), 2 → `0x10088adb8`, 1 → `0x10088a714`. Все три: запись режима,
  класс устройства игроков (`0x10088a368`), рассылка слушателям слота 0x48 «режим ввода сменился», дальше — пересборка UI
  `0x1012f1f58` (описана в `DOS2_PORT.md`, проход 4).
- Геймпад на iPad есть: `GCController` (`gameControllerDidConnect:`, обработчики `GCExtendedGamepad`). Нажатие на паде → режим 1 →
  `_c`-набор; касание → режим 2 → `_t`-набор. `0x10088a714` при уже стоящем режиме 1 ничего не делает.
- Внутри одного фильма оба ввода живут вместе только в меню: `mainMenu_t`, `gameMenu_t`, `msgBox_t`, `saveLoad_t`,
  `optionsSettings_t` … слушают и мышь, и события `IE UIUp / UIDown / UIAccept / UICancel` (как PC-фильмы — стрелки клавиатуры);
  у части есть `setControllerMode` / `isController` (8 + 3 фильма) и импорт `consoleHints_c.swf` — подсказки кнопок.

### 2.4 Как касание доходит до фильма
`LSMTLView` (жесты UIKit) → `sendLeftMouseDown/Up`, `sendXYMotion`, `sendTouchDown/Up` → `InjectInput 0x1008872ac`; привязки
`PlayerProfiles/iOS/Eoc/touchscreen_1.json` превращают `touchscr:touch` в `FlashLeftMouse`, жесты — в `CameraMove / CameraZoom /
ContextMenu …` (`IPA_RECON.md` §2.1). Фильм получает обычные события мыши Iggy; жесты прокрутки и перетаскивания собирает сам AS3
(`LS_Classes.touchHelpers`, `touchVerticalScroller`, `touchScrollList`, `touchCustomEventType`). Отдельного «тач-канала» в фильм нет,
кроме `setTouchScreenMode` и списка жестов для экрана настроек (`addTouchGesture*` в `optionsInput_t`).

## 3. Движок Switch
- Таблица типов: регистратор `0x10a7000…0x10b3a00` (не с `0x10a9670`, как считалось: это середина), 146 записей, вставка в хеш
  встроена. Хеш создателей в менеджере UI `[main+0x2bbb230]`: `+0xd8` u32 число корзин, `+0xe0` массив корзин, `+0xe8` число записей;
  узел `{+0 следующий, +8 u32 id, +0x10 создатель}`; создатель `{+0 функция, +8 std::string}` — строка в «длинной» форме
  `{+8 u32 ёмкость|1, +0xc u32 длина, +0x10 char*}` (короткая форма — до 14 символов, пути GUI длиннее). **Путь фильма — данные:
  хост может заменить указатель и длину, не трогая код.**
- id типов те же, что на iPad (0…0x93); типов 0x94+ нет. Общие создатели: `mainMenu` 0x1c и `mainMenu_c` 0x48 — `0x10b56e0`;
  `mainMenu` 0x32 и `mainMenu_c` 0x57 — `0x10b6990`; `msgBox`/`msgBox_c` 0x12/0x4c — `0x10b4e60`, 0x1d/0x4b — `0x10b57b0`;
  `gameMenu`/`_c` 0x13/0x4d — `0x10b4f50`; `saveLoad`/`_c` 0x27/0x4a — `0x10b6000`; `characterCreation`/`_c` 3/4 — `0x10b68c0`;
  `contextMenu`/`_c`, `tutorialBox`/`_c`, `waypoints`/`_c`, `pyramid`/`_c`, `formation`/`_c`, `reward`/`_c`, `examine`/`_c`,
  `characterAssign`/`_c`, `startTurnRequest`/`_c`, `connectionMenu`/`connectivity_c`, `serverlist`/`_c` — тоже по одному классу.
  Раздельные классы PC и `_c`: `hotBar` 0x28 (`0x10b60d0`, 0x2a0) / `bottomBar_c` 0x3b; `minimap` 0x1e / `minimap_c` 0x3c;
  `trade` 0x2e / `trade_c` 0x49; `journal` 0x16 / `journal_c` 0x46 (+ `journal_csp` 0x8c — тот же класс, что `journal_c`);
  `partyInventory` 0x74 / `partyInventory_c` 0x8e; `characterSheet` 0x77 / `statsPanel_c` 0x3f; `skills` 0x29 /
  `inventorySkillPanel_c` 0x3e; `uiCraft` 0x66 / `craftPanel_c` 0x54; `optionsSettings` (4 класса) / `optionsSettings_c` 0x5a, 0x5b.
- Места создания (`dos2_ui_sites.py`, 93 вызова `0xaebbf0`): сборщик набора игрока `0x10c06d0` — `_c`-блок `0x10c0814…0x10c0b28`
  (0x3b, 0xc, 0x60, 0x3c, 0x41, 0x4d, 0x43, 0x40, 0x54, 0x3e, 0x3f, 0x52, 0x53, 0x56, 0x5e, 0x3d, 0x4b, 0x44) под байтом режима и
  общий хвост `0x10c0b30…` (0x30, 5, 0xf, 0, 0x24, 0x25, 0x10, 9, 0x46); главное меню `0x10c68f0` (0x48) и `0x1180340` (0x57);
  окна сообщений 0x4b / 0x4c без проверки и 0x12 с проверкой; создание персонажа — 0x4. Часть мест выбирает id через `csel` по байту
  режима (14 мест: настройки, сохранения, и т. п.) — там PC-ветка жива.

### 3.1 Что каким классом можно вести
| группа | фильмы `_t` | класс на Switch | что нужно |
|---|---|---|---|
| А. Тот же класс, контракт PC ≈ `_t` | mainMenu, gameMenu, msgBox, saveLoad, optionsSettings, optionsInput, loadingScreen, subtitles, userProfile, characterCreation, characterAssign, connectionMenu, serverlist, tutorialBox, waypoints, book, dialog, contextMenu, examine, formation, pyramid, itemSplitter, textDisplay, tooltip, worldTooltip, mouseIcon, notification, overhead, combatTurn, combatLog, chatLog, statusConsole, startTurnRequest, arenaResult, giftBagsMenu, giftBagsContent, containerInventory, installScreen (38) | есть | подмена пути; класс должен идти по PC-ветке (байт режима 0 в момент работы) либо, для фильмов-надмножеств (`mainMenu_t` содержит все методы, которые шлёт и `_c`-ветка, кроме `addBtnHint / setProfile / setPressToJoin …`), годится и режим 1. Недостающие EI (`closeUI`, `menuButtonOver`, `inputFocus` …) — мелкие заглушки |
| Б. Свой класс на iPad, на Switch есть PC-родитель | hotBar (PC `hotBar` 0x28), partyInventory (0x74), characterSheet (0x77), skills (0x29), uiCraft (0x66), trade (0x2e), minimap (0x1e), journal_csp (PC `journal` 0x16), reward (0x88), playerInfo (0x26 / 0x3d) | PC-класс есть, но Switch его не создаёт | хост создаёт объект PC-типа с путём `_t`; базовые данные придут (имена invoke общие: у `hotBar_t` 28 из 39 методов — как у PC), новое мертво до заглушек: 5–8 EI и 4–10 invoke на фильм |
| В. Свой класс, родителя на Switch нет или он `_c` | rightSideButtons, skillFireHelper, equipmentPanel (база `_c`), areaInteract (на Switch только `_c`-класс), второй playerInfo (переключатель персонажей) | нет | хост-класс с нуля: `rightSideButtons` простой (объект 0x150, 13 методов, EI `rightBarBtnClicked`, `toggleSidePanel` → открыть панель), `skillFireHelper` — лупа (можно не делать), `equipmentPanel_t` / `areaInteract_t` — пробовать `_c`-классом, иначе откат на `_c`-фильм |

Проверено только по именам (методы документа ↔ строки main; EI ↔ строки main). Совпадение имени не гарантирует совпадения аргументов:
у `_t` и PC число параметров общих методов сверено скриптом для главного меню (совпало), для остальных — в подзадачах.

## 4. Архитектура
Общие кирпичи (нужны любому варианту):
- **К1. Подмена пути** в записи создателя (хеш `uimgr+0xd8`): таблица `id → GUI/x_t.swf`, откат на исходный указатель. Данные, без кода.
- **К2. Пак с фильмами**: игра открывает паки перечислением каталога (`[res] first open rom:/Data/*.pak` подряд по алфавиту в логах
  t4) — лишний `Data/TouchUI.pak` (LSPK v13, без сжатия) с `Public/Game/GUI/*_t.swf` должен смонтироваться сам. Писателя LSPK в
  `tools/lspk.py` нет — написать (TU-01). Запасной путь — добавить записи в копию `Game.pak` (146 МБ, на телефон не толкать).
- **К3. Режим**: байт `main+0x2bb8918` + пересборка набора (`0x10c4730` / `0x10c4580`, уже за `DM_DOS2_TOUCH_LIVE`) + фильтр
  устройств `0x844014` (`[mgr+0x170]`, уже за `DM_DOS2_TOUCH_KBM`).
- **К4. Перехват EI и invoke** (нужен для Б и В): одна точка на входе обработчика вызовов из фильма (у `UIObject` — виртуальный
  `OnFunctionCalled(имя, аргументы)`; найти через обратный вызов Iggy в `FlashPlayer`, vtable `main+0x29c3bd8`) и вызов
  `FlashPlayer::Invoke` с хоста. Адреса на Switch ещё не найдены (TU-06).

| | суть | плюсы | риски | цена |
|---|---|---|---|---|
| **A. `_t`-фильмы на существующих классах + заглушки** | К1 + К2 для группы А в обоих режимах; HUD касания = PC-классы (группа Б), которые создаёт хост; недостающие EI — заглушки через К4 | код движка почти не трогается; меню готовы быстро; каждая панель включается отдельно | PC-классы на NX ни разу не исполнялись; хосту надо повторить PC-ветку сборщика (слои, флаги — списать с iPad `0x1012dfea4`); новые панели (боковые кнопки, перетаскивание) не работают без заглушек; в режиме 0 игра режет пад (см. ниже) | меню 8–12 агент-ч; HUD 25–40 агент-ч; 6–10 прогонов на телефоне |
| **B. Хост-реализация 15 классов `UITouch*`** | полный перенос поведения iPad: свои объекты (0x150…0x4b0 байт), свои vtable, все обработчики | точная копия iPad | объём (15 классов, сотни обработчиков, внутренние структуры инвентаря / навыков / торговли надо знать по смещениям Switch); каждый класс — отдельный риск падений | 90–160 агент-ч, 20+ прогонов |
| **C. Гибрид (рекомендуется)** | A как основа; из B — только то, без чего тач-HUD неполон: `rightSideButtons` (простой), заглушки 25 EI по мере надобности, перетаскивание — последним; чего нет — откат панели на PC-фильм (мышиный, мелкий, но рабочий) или на `_c` | играбельно по шагам, каждый шаг проверяется отдельно, откат на любой панели | те же, что у A, но ограничены панелью | 45–70 агент-ч до «всё основное пальцем», 10–14 прогонов |

**Пад и касание вместе.**
- Меню (группа А, фильмы слушают и мышь, и `IE UI*`): байт режима остаётся 1, пад работает как сейчас, касание проходит через
  `[mgr+0x170] = 1`. Оба ввода одновременно, без переключений. Это первый срез.
- В игре — модель iPad: набор следует за последним устройством. Касание экрана вне экранного пада → тач-набор (режим 0 + фильмы `_t`,
  HUD создаёт хост); нажатие кнопки или стик пада (экранного или BT) → `_c`-набор (режим 1). Пад не выключается никогда: первое же
  нажатие возвращает контроллерный UI; хост повторяет отброшенную игрой запись после переключения, чтобы нажатие не терялось.
  Пункт меню гира: «Интерфейс: авто (по последнему вводу) / всегда контроллерный / всегда касание»; при подключённом BT-геймпаде
  стартовый режим — контроллерный.
- Полностью одновременная работа в игре (стик ведёт героя при тач-HUD) упирается в фильтр `0x844014`: в режиме 0 события пада
  получают только слушатели с битом 4 или с битом класса в маске `+0x68`. Опыт на потом: выставить бит класса 1 в маске слушателей
  игры — тогда пад жив и при тач-HUD. Не обещать, пока не проверено (670 читателей байта режима).

## 5. Подзадачи (≈60 минут агента каждая)
Мастер-переключатель `DM_DOS2_TOUCHUI` (0 = выкл, по умолчанию; 1 = вкл; требует `DM_DOS2_TOUCH=1`). Всё, что пишет в данные игры,
сверяется пословно (без инструкций с `x18`), при несовпадении — красная строка и пропуск. Код — новый файл `dm/host/hle_dos2_touchui.c`
(+ вызов из перехвата опроса пада в `hle_dos2_touch.c`). «Толкать» = файлы Larian из IPA, только с разрешения хозяина.

| № | что | файлы / переключатели | проверка на хосте | проверка на телефоне | толкать |
|---|---|---|---|---|---|
| TU-01 | Писатель LSPK v13 и сборка `TouchUI.pak` из списка фильмов | `tools/dos2_touchui_pak.py` (наборы `menu1`, `menu`, `hud`, `all`) | чтение обратно `tools/lspk.py`, сравнение CRC с `ipad_gui` | — | — |
| TU-02 | Подмена пути создателя: обход хеша `uimgr+0xd8`, таблица `id → путь`, лог | `hle_dos2_touchui.c`; `DM_DOS2_TOUCHUI=1`, `DM_DOS2_TOUCHUI_MAP=48:mainMenu_t,57:mainMenu_t` (отладочная своя таблица), `DM_DOS2_TOUCHUI_LOG=1` (дамп всей таблицы типов) | сверка 6 мест (вставка в хеш `0x10ab690`, чтение пути создателем, `0x10c68e0`); самотест раскладки строки на копии main.raw (`tools/dos2_ui_types.py sw` даёт эталон id → путь) | — | — |
| TU-03 | **Первый срез: главное меню пальцем, пад жив.** Типы 0x48 и 0x57 → `GUI/mainMenu_t.swf`, байт режима 1, `[mgr+0x170] = 1` автоматически | `DM_DOS2_TOUCHUI=1` (включает `_KBM`); галка гира «Тач-интерфейс (эксперимент)» | — | в логе `[touchui] type 0x48 -> GUI/mainMenu_t.swf`, открытие пака; меню — айпадовское; тап по кнопкам работает, стрелки / A пада работают; нет пака — меню прежнее (`_c`), красная строка | `TouchUI.pak` набор `menu1`: `mainMenu_t.swf` 2 294 909 Б (2.2 МиБ) |
| TU-04 | Остальные меню группы А под `_c`- и PC-id: msgBox (0x4b, 0x4c, 0x12, 0x1d), gameMenu (0x4d), saveLoad (0x4a, 0x27), optionsSettings (0x5a, 0x5b + PC), optionsInput, loadingScreen, userProfile, tutorialBox, characterCreation (4). Сверка числа аргументов общих методов `_t` ↔ `_c` скриптом; где `_c`-ветка класса шлёт другое (`addOption` против `addButton`) — пометить «нужна PC-ветка» | `tools/dos2_swf_abc.py diff`; `DM_DOS2_TOUCHUI_SET=menu` | таблица совместимости в этом файле | каждое меню: открыть, тап, пад; список сломанных | набор `menu`: ≈15.5 МБ (12 фильмов) + `uiElements_t.swf` 15 КБ |
| TU-05 | PC-ветка для меню, которым она нужна: на время жизни окна (создание → закрытие) байт режима 0, потом назад; либо перевод invoke | `DM_DOS2_TOUCHUI_PCWIN=1` | — | окно сообщения с кнопками, настройки, сохранения | — |
| TU-06 | Найти на Switch: вход обработчика EI (`OnFunctionCalled`) и `FlashPlayer::Invoke` / `SetValue`; сделать перехват с таблицей имён и лог неизвестных имён | Ghidra headless на своей копии проекта; `DM_DOS2_TOUCHUI_EILOG=1` | сверка адресов | в логе имена EI из `mainMenu_t` (`buttonPressed`, `PlaySound`, `didUse2FingerScrolling`) | — |
| TU-07 | Тач-набор игрока, шаг 1: хост создаёт HUD в режиме 0 — `hotBar_t` (класс 0x28), `minimap_t` (0x1e), `playerInfo_t` (0x26), `statusConsole_t`, `combatLog_t`; слои и флаги списать с тач-ветки iPad `0x1012dfea4`; уничтожение вместе с набором | `DM_DOS2_TOUCHUI_SET=hud1`, `DM_DOS2_TOUCH_LIVE=1` | — | после тапа в игре: панель навыков, миникарта, портреты; тап по навыку, по земле; нажатие пада возвращает `_c`-набор; 5 переключений подряд | набор `hud`: hotBar 307 КБ, minimap 479, playerInfo 122, statusConsole 336, combatLog 62, combatTurn 43 |
| TU-08 | Повтор первой записи пада после переключения; запрет переключения в диалоге / ролике / загрузке; пункт гира «Интерфейс: авто / контроллер / касание», BT-пад = старт в контроллерном | `frontend-common` (сказать сессиям DOOM и Eternal), `DM_DOS2_TOUCHUI_MODE=auto/pad/touch` | — | переключения в бою, в диалоге, с открытым окном | — |
| TU-09 | `rightSideButtons_t`: хост-класс (создание, `setRolledIn`, метки) + EI `rightBarBtnClicked` / `toggleSidePanel` → открытие инвентаря / листа / навыков / журнала / карты событиями ввода | `DM_DOS2_TOUCHUI_SET=hud2` | — | боковая колонка, каждая кнопка открывает свою панель | 943 КБ |
| TU-10 | Панели: `partyInventory_t` (0x74), `characterSheet_t` (0x77), `skills_t` (0x29), `journal_csp_t` (0x16), `containerInventory_t`, `examine_t`, `contextMenu_t`, `tooltip_t` — создание PC-классом, список мёртвых функций | — | — | открыть каждую, прокрутка, тап по предмету, контекстное меню удержанием | ≈4.1 МБ |
| TU-11 | Заглушки EI для инвентаря и панели навыков: `slotDown / slotUpEnd`, `selectSkillSet`, `didToggleDeckSwitcher`, `cancelActiveAction`, `switchWaresModeEnabled`, `hideFade / showFade`, `openMap` | таблица в `hle_dos2_touchui.c` | — | колода навыков, отмена действия | — |
| TU-12 | `trade_t`, `uiCraft_t`, `reward_t`, `dialog_t`, `equipmentPanel_t`, `areaInteract_t`; где не работает — откат панели на `_c` | `DM_DOS2_TOUCHUI_DENY=<id,…>` (список типов без подмены) | — | торговля, крафт, диалог, награда | ≈1.5 МБ |
| TU-13 | Перетаскивание (`startDraggingFromSlot`, `dropItem`, `closeAndDragItem`, `setDraggingState`) — разбор обработчиков iPad и перенос | — | — | перенос предмета между слотами и на панель | — |
| TU-14 | Опыт «пад жив при тач-HUD»: бит класса пада в маске слушателей (`+0x68`) | `DM_DOS2_TOUCHUI_PADMASK=1` | — | стик ведёт героя при тач-HUD | — |
| TU-15 | Запись в `docs/port-kb` (тема UI / ввод, MATRIX), раздел в `DOS2_PORT.md`, перенос в общий фронт | — | — | — | — |

Порядок: TU-01 → TU-02 → TU-03 (срез) → TU-04 / TU-06 параллельно → TU-05 → TU-07 → TU-08 → TU-09…TU-13. Прогоны на телефоне — по
одному на срез, с разрешения хозяина; перед каждым — копия `files/save`; после сбора логов — force-stop.

**Файлы на телефон** (всё — файлы Larian из IPA хозяина, в APK не кладём, в репозиторий не кладём):
| набор | состав | размер |
|---|---|---|
| `menu1` (TU-03) | `mainMenu_t.swf` | 2.2 МиБ |
| `menu` (TU-04) | msgBox, gameMenu, saveLoad, optionsSettings, optionsInput, loadingScreen, userProfile, characterCreation, connectionMenu, serverlist, tutorialBox + `uiElements_t` (без mainMenu: хозяин оставляет `_c`, 15.11) | 12.61 МиБ |
| `all` | все нужные `_t` без `journal_t` | ≈31.3 МиБ |

## 6. Что не доказано
- Что лишний пак в `Data/` монтируется: статикой подтверждено (§7, TU-01: `FindFiles "*.pak"` без списка имён), на телефоне — первой строкой лога TU-03.
- Что `mainMenu_t` под типом 0x48 в режиме 1 покажет кнопки: набор методов сошёлся, но `_c`-ветка класса дополнительно зовёт
  `addBtnHint / setProfile / setPressToJoin / showLoadingIcon`, которых в `_t` нет (ожидается: вызовы просто не находят метод).
  Если меню пустое — TU-05 (PC-ветка на время жизни окна).
- Что PC-классы HUD на NX рабочие (создатели и vtable есть, код ни разу не исполнялся).
- Аргументы общих методов сверены только для главного меню.
- Обработчики EI внутри `UITouch*` не разбирались.

## 7. TU-01…TU-03: что сделано (14.11, ветка `dos2-tu01`, на телефоне не запускалось)
### TU-01 — пак
- `tools/dos2_touchui_pak.py <набор>`: читает `_t`-фильмы прямо из `Game.pak` IPA (`--src` — другой пак или папка), пишет
  `../work/touchui/<набор>/TouchUI.pak`, читает его обратно через `tools/lspk.py` и сравнивает каждый файл побайтно. Наборы: `menu1`,
  `menu`, `hud`, `all`.
- Формат списан с `Game.pak` Switch, а не «без сжатия», как стояло в плане: файлы по имени без учёта регистра, с нуля, выравнивание 64,
  каждая запись LZ4 (флаги 0x42), crc = CRC-32 байтов на диске, список = u32 + один блок LZ4, заголовок 40 байт (версия 13, части 1,
  флаги 0, приоритет 0). Упакованный `mainMenu_t.swf` получился того же размера, что в паке iPad (2 274 306 Б). `--store` —
  вариант без сжатия, только для сравнения.
- **Монтирование.** Движок сам берёт любой `*.pak`: `FindFiles(каталог, "*.pak")` (`0x8125d0`) зовётся для `Data` (`0x82fea4`),
  `Data/Localization` (`0x82fedc`), языка (`0x82ff1c`), `0x82ff48`, `MinData` (`0x82ff80`); каждое имя проходит только проверку
  расширения `0x828ea0`, списка известных паков нет (в `Data` уже лежит пак из одного файла — `GamePlatform.pak`). Хосту ничего
  добавлять не пришлось: индекс RomFS (`DM_FS_ROM_INDEX`, `hle_fs.c`) обходит папку RomFS при монтировании, файл в `romfs/Data`
  виден как остальные. Оверлей (`DM_ROMFS_OVERLAY`) тоже сработал бы, но с ним индекс выключается (каждый поиск снова через FUSE),
  поэтому пак кладётся в саму папку RomFS. Отдельные файлы вне пака (`Data/Public/Game/GUI/x.swf`) не разбирались и не используются.
- `.iggy`: в `Game.pak` Switch у 8 фильмов (в том числе `mainMenu_c`) есть готовые `.iggy` / `.iggytex`, у остальных 135 — только
  `.swf`, то есть движок умеет грузить `.swf` напрямую; у `mainMenu_t` пары `.iggy` нет — пойдёт как большинство. Как именно движок
  выбирает `.iggy` вместо `.swf`, не разбиралось.

| набор | файл | размер | SHA-256 |
|---|---|---|---|
| `menu1` | `work/touchui/menu1/TouchUI.pak` (1 фильм) | 2 274 467 Б (2.17 МиБ) | `e3d1ca75534fd1230200dd6c9e0bef46397cd14a789e48fe0caef7ae2cae58eb` |
| `menu` | `work/touchui/menu/TouchUI.pak` (12 меню + `uiElements_t`) | 15 493 055 Б (14.78 МиБ) | `a3e9dc6243dcfe64ad7c8db8e311d67e4305e3df29c97367ee7264fa0c2b8fb9` |

### TU-02 — подмена пути (`dm/host/hle_dos2_touchui.c`)
- Раскладка подтверждена разбором: создатель — блок 0x18 `{функция, строка}`; строка движка в длинной форме `{u32 размер|1, u32 длина,
  char *}`; `CreateUIObject 0xaebbf0` ищет узел по `тип % [mgr+0xd8]` и зовёт `функция(&путь)`. Для 18 символов `GUI/mainMenu_c.swf`
  регистратор выделяет 32 байта (`(длина + 16) & ~15`), поэтому `_c` → `_t` пишется **прямо в буфер движка** (чужих указателей в игре
  нет); если новый путь длиннее выделенного — меняются указатель и длина на буфер хоста.
- Одна точка в коде: первые 4 инструкции `CreateUIObject` (без PC-относительных) → прыжок на хост, трамплин как в `hle_dos2_touch.c`.
  При первом вызове (регистратор уже отработал) правятся пути; дальше каждый вызов подменённого типа пишется в лог с полученным
  хэндлом. Создание не удалось (хэндл без битов `0x3fffff00000000`) → типу возвращается старый путь и объект создаётся заново
  (`_c`-меню), красная строка.
- Сверка перед любым изменением — 13 мест (69 слов) + место Iggy (7 слов, только для строки лога), ни одно не использует `x18`; плюс
  два литерала (`GUI/mainMenu_c.swf` `main+0x1ef529b`, `*.pak` `main+0x1e8671a`). То же вне телефона:
  `python tools/dos2_touchui_check.py <main.raw> [TouchUI.pak]` (берёт смещения и слова из исходника хоста, сверяет с `main.raw`,
  проверяет `x18`, типы 0x48 / 0x57 по `dos2_ui_types.py`) — проходит.
- Хост читает список файлов `TouchUI.pak` сам (LSPK + LZ4 с проверкой границ): тип подменяется, только если фильм в паке есть.
  Нет пака — одна красная строка, ничего не меняется, меню прежние.
- Переключатели: `DM_DOS2_TOUCHUI=1`; `DM_DOS2_TOUCHUI_MAP=48:mainMenu_t,57:mainMenu_t` (своя таблица, id в hex); `DM_DOS2_TOUCHUI_LOG=1`
  (вся таблица типов + каждый `CreateUIObject`, первые 400); `DM_DOS2_TOUCHUI_PAK=<имя>`; `DM_DOS2_TOUCHUI_NOCHECK=1`.

### TU-03 — главное меню
- По умолчанию при `DM_DOS2_TOUCHUI=1`: типы 0x48 и 0x57 → `GUI/mainMenu_t.swf`. Байт режима не трогается (1, контроллер).
- `hle_dos2_touchui.c` ставится перед `hle_dos2_touch.c` и сам включает `DM_DOS2_TOUCH=1` и `DM_DOS2_TOUCH_KBM=1` (фильтр `[mgr+0x170] = 1`);
  `DM_DOS2_TOUCH_KBM=0` в `dm/env` оставляет фильтр выключенным (для сравнения).
- Фронт: `DmActivity.dos2TouchUiEnv()` после `nativeStart` — при `DM_DOS2_TOUCHUI` касания вне кнопок и стиков экранного пада идут в
  игру, как с галкой гира «Касание как мышь»; кнопки и стики пада шлют ввод пада как раньше. Строки в гире нет.
- **Методы, которых нет в `_t`.** `tools/dos2_swf_abc.py diff mainMenu_c mainMenu_t`: только в `_c` — `addBtnHint`, `allowChange`,
  `clearBtnHints`, `hideHintsAndCopyright`, `setPressToJoin`, `setProfile`, `setProfileP2`, `showDifficultyIcon`, `showLoadingIcon`;
  только в `_t` — `addProfileBox`, `setGiftBagsButtonLabel`; общих 53. Вызов отсутствующего метода безопасен: `IggyPlayerCallMethodRS`
  (Iggy 1.2.56) для «не найдено» пишет предупреждение `No method %n found in IggyPlayerCallMethodRS` и возвращает ошибку 0x1f8
  (`0x3d0370…0x3d03e8`; «не функция» — 0x1f7), без вызова и без падения. Как обёртка движка обходится с этой ошибкой, не разбиралось.
  Что `_c`-ветка класса при этом покажет кнопки — по-прежнему не доказано (§6): подсказки кнопок и профиль просто не появятся.
- EI только у `_t`: `GiftBagPressed closeUI didUse2FingerScrolling hideTooltip menuButtonOver showItemTooltip showStatusTooltip showTooltip`
  — обработчика в классе может не быть (TU-06 покажет имена в логе).

### Прогон на телефоне (делает главный агент, с разрешения хозяина)
1. Копия `files/save`. APK этой ветки: `<dos2-port>/wt-tu01/work/DOS2-port-debug.apk` (22 553 296 Б).
2. Файл Larian (разрешение хозяина): `<dos2-port>/work/touchui/menu1/TouchUI.pak`, 2 274 467 Б →
   `/sdcard/Download/DivinityOriginalSin2Mobile/romfs/Data/TouchUI.pak` (рядом с `Game.pak`). Если RomFS лежит в папке приложения
   (`boot: RomFS from the app directory` в логе) — в её `romfs/Data/`.
3. `dm/env` (`/sdcard/Download/DivinityOriginalSin2Mobile/dm/env`): `DM_DBG=1` и `DM_DOS2_TOUCHUI=1`. Убрать `DM_DOS2_TOUCH_PCUI`,
   `DM_DOS2_TOUCH_LIVE`, `DM_DOS2_TOUCH_UI`, если остались. Одной командой: `tools/run_dos2.sh tu03 90 DM_DOS2_TOUCHUI=1`.
4. В логе по порядку:
   - `[fs] rom index … files` — файлов на один больше, чем раньше;
   - `[touchui] package …/romfs/Data/TouchUI.pak: 1 file(s), 2274467 B`;
   - `[touchui] DM_DOS2_TOUCHUI=1: CreateUIObject main+0xaebbf0 hooked … 0x48 -> GUI/mainMenu_t.swf, 0x57 -> GUI/mainMenu_t.swf`;
   - `[touchui] DM_DOS2_TOUCH=1 (set here), DM_DOS2_TOUCH_KBM=1 (set here)` и строки `[touch] …` как при галке гира;
   - `[res] first open rom:/Data/TouchUI.pak` и `[touchui] the game opened rom:/Data/TouchUI.pak: the package is mounted` — игра
     открыла пак. **Нет этих строк — движок пак не взял** (тогда запасной путь: записи в копии `Game.pak`);
   - `[touchui] type 0x48 -> GUI/mainMenu_t.swf (was GUI/mainMenu_c.swf; … written in place)`, то же для 0x57, `2 of 2 type(s) substituted`;
   - `[touchui] CreateUIObject type 0x48 = GUI/mainMenu_t.swf (layer 0x13 flags 0x20021 …) -> handle …: created`;
   - `[touchui] movie load: the game reads Public/Game/GUI/mainMenu_t.swf from TouchUI.pak` — движок читает фильм.
   - Красные строки `【红】[touchui]`: `no TouchUI.pak` (файл не там), `main+… differs` (не та сборка), `FAILED` + `is back to
     GUI/mainMenu_c.swf` (объект не создался, откат сработал).
5. На экране: главное меню — айпадовское. Проверить: тап по пункту меню; стрелки / A / B экранного пада; «Настройки» откроют ещё
   `_c`-окно — это TU-04. Пустое меню без кнопок = нужен TU-05 (PC-ветка на время жизни окна).
6. Force-stop после сбора логов. Набор `menu` (14.78 МиБ) толкать только к TU-04, вместе с `DM_DOS2_TOUCHUI_MAP`.

Риски среза: (1) пак с новым именем движок может не принять по причине, которой в статике не видно (тогда нет строки `first open`);
(2) `_c`-ветка класса может не заполнить `_t`-меню (сверены только имена методов и число аргументов); (3) перехват `CreateUIObject` —
новая точка в коде на 93 вызова, ставится только при `DM_DOS2_TOUCHUI` / `_LOG`; (4) «загрузка фильма» в логе — это чтение из пака:
ошибку разбора SWF внутри Iggy хост не видит (на экране — пустой слой), а откат на `_c` срабатывает только когда не создался сам объект.

### Прогон tu03 (15.11, pack 2p) и исправления
- Пак движок взял сам (`the game opened rom:/Data/TouchUI.pak`). Подмена не сработала (`0 of 2`): в таблице лежит не литерал, а полный путь
  `rom:/Data/Public/Game/GUI/mainMenu_c.swf` (его делает `0x10b4670`), а хост требовал начало `GUI/`. Теперь сравнивается и заменяется
  хвост от последнего `GUI/`, приставка остаётся движка; длина та же — запись на месте.
- Падение `sig=11 addr=0x1e4 pc=main+0x1121220` при тапе по портрету в игре: `0x10a6920` (отложенный тултип объекта UI, зовётся из
  обновления объектов) заканчивается `bl 0x10a6fc0; … b 0x1120b50` / `b 0x1121150`; `0x10a6fc0` возвращает объект UI тултипа
  (`0x110e1f0`, хэндл `[[main+0x3298140]+0x88]`) или NULL, когда его нет (PC-тултип в контроллерном режиме не создаётся), и оба хвоста
  пишут в него без проверки. Без мыши на Switch туда не попасть. Хост ставит недостающую проверку на вход обеих функций (у каждой
  один вызывающий): `x0 == 0` → возврат, строка `[touchui] tooltip guard: … call skipped`. Ставится при любом `DM_DOS2_TOUCH=1`
  (`DM_DOS2_TOUCH_TIPGUARD=0` — выключить), 7 мест сверяются пословно.
- `DM_DOS2_TOUCHUI` теперь включает `DM_DOS2_TOUCH_KBM=2`: фильтр `[mgr+0x170]` открыт, только пока нет набора UI игрока (меню,
  загрузка); в игре он снова 0 — контроллерный HUD событий мыши не получает, пока нет его `_t`-панелей (TU-07+). Следствие: внутриигровое
  меню паузы пальцем пока не работает (пад работает). `DM_DOS2_TOUCH_KBM=1` в `dm/env` — прежнее поведение для опытов.
- Проверка в логе: `[touchui] type 0x48 -> rom:/Data/Public/Game/GUI/mainMenu_t.swf (was GUI/mainMenu_c.swf; … written in place)`,
  `2 of 2 type(s) substituted`, `CreateUIObject type 0x48 = GUI/mainMenu_t.swf … created`, `movie load: the game reads
  Public/Game/GUI/mainMenu_t.swf`, `[touchui] tooltip guard: main+0x1120b50 and main+0x1121150 return when …`,
  `[touch] DM_DOS2_TOUCH_KBM=2: …` и при входе в игру `[touch] DM_DOS2_TOUCH_KBM=2: [mgr+0x170] -> 0 (in game …)`.

## 8. TU-04 / TU-05: остальные меню группы А и PC-ветка (15.11, ветка `dos2-tu04`, на телефоне не запускалось)
Решение хозяина (15.11): **главное меню остаётся консольным `mainMenu_c`** (дизайн нравится, касание для него — отдельно). В карте по
умолчанию его нет; `DM_DOS2_TOUCHUI_MAP=48:mainMenu_t,57:mainMenu_t` — для опытов, пак `menu1` для этого остаётся.

### Что подменяется (`DM_DOS2_TOUCHUI=1`, таблица `k_def` в `hle_dos2_touchui.c`)
| фильм `_t` | PC-id (путь подменяется) | `_c`-id → PC-близнец того же класса (TU-05) |
|---|---|---|
| msgBox | 0x1d, 0x12 | 0x4b → 0x1d, 0x4c → 0x12 |
| gameMenu (пауза) | 0x13 и **0x4d как есть** (`_t` близок к обоим контрактам, как mainMenu) | — |
| saveLoad | 0x27 | 0x4a → 0x27 |
| optionsSettings | 0x01, 0x11, 0x23, 0x2d | 0x58 → 0x01, 0x59 → 0x11, 0x5a → 0x23, 0x5b → 0x2d |
| optionsInput / loadingScreen / userProfile / connectionMenu | 0x0d / 0x17 / 0x33 / 0x21 | — (у Switch только PC-id) |
| characterCreation | 0x03 | 0x04 → 0x03 |
| serverlist | 0x18, 0x1a | 0x1b → 0x1a |
| tutorialBox | 0x37 | 0x5e → 0x37 |

Хвост пути каждого типа сверяется с ожидаемым (`GUI/msgBox_c.swf` …), подмена — только если фильм есть в паке. `connectivity_c` 0x22 не трогается
(другой фильм под тем же классом). `DM_DOS2_TOUCHUI_DENY=4,3` — оставить типы как есть (например, создание персонажа, 4.9 МБ).

### Почему PC-ветка и как она сделана (TU-05)
- `tools/dos2_swf_abc.py diff`: у msgBox, saveLoad, optionsSettings, tutorialBox, characterCreation `_t` = контракт PC (`addButton / addYesButton`,
  `addSave / addLoad`, нет `addMenuCheckbox / addMenuSlider …`), с `_c` расходится (`addOption`, `addNewSave`, `addMenuButton …`): на `_c`-ветке
  класса такое окно было бы пустым. gameMenu_t близок к обоим.
- Ветку класс берёт **по своему типу**: главное меню создаёт `mods_c` 0x67, если `[obj+0xf0] == 0x48` (`0x118039c`), настройки `_c` 0x5b — если
  0x57 (`0x11a2f98`); ввод msgBox идёт по контроллерному пути при `режим && [obj+0x178]` (`0x1117ffc`, `0x11182f0`). `_c`- и PC-id этих меню —
  один создатель (= один класс), проверено скриптом и на старте хоста.
- Поэтому TU-05 = **подмена типа при создании, а не глобального байта**: игра просит `_c`-id → хук `CreateUIObject` создаёт PC-близнеца того же
  класса (его путь уже `_t`). Тип объекта — PC, дочерние окна идут за ним, байт режима остаётся 1, пад работает. Не создался близнец → тип
  создаётся как был (`_c`-фильм) и больше не подменяется, красная строка. `DM_DOS2_TOUCHUI_PCWIN=0` — `_c`-id этих меню не трогать.
- Вариант плана «байт режима 0 на время жизни окна» — `DM_DOS2_TOUCHUI_PCWIN=2` (как 1 + `main+0x2bb8918 = 0`, пока на экране окно-близнец,
  потом 1). Только для сравнения: в режиме 0 фильтр `0x844014` режет пад у большинства слушателей.
- «Время жизни». Большинство окон создаётся один раз и живёт скрытым (набор игрока делает gameMenu_c, msgBox_c, tutorialBox_c при загрузке,
  лог tu04), поэтому для хоста жизнь окна = видимость. Флаги объекта `+0x14`: SetFlags `0xae411c` (бит 3 = видим: хвост показывает Flash-плеер
  и добавляет модальные биты 0x1e00 в `[uimgr+0x220]` только при бите 3), ClearFlags `0xae3f20`; тип `+0xf0` (`0xaebc5c`). Хэндл → объект:
  `[[uimgr+0x10] + (h & 0xffffffff)*8]` ниже `[uimgr+0x1c]` (`0xaebfe8`), уничтожение пишет туда NULL (`0xaebba0`). Хост запоминает хэндлы
  созданных подменённых окон и на каждом опросе пада читает тип и флаги через слот (только чтение; 5 мест сверяются пословно).

### Фильтр мыши в игре (KBM=2)
`[mgr+0x170] = 1` вне игры (как было) и **в игре, пока видно подменённое окно с модальными битами** (пауза, сообщение, сохранения, настройки);
как только таких нет — снова 0, `_c`-HUD мыши не получает. Обучающее окно не модальное — его не считает (`DM_DOS2_TOUCHUI_FILTER=1` — любое
видимое подменённое окно).

### Строки лога (`DM_DBG=1`)
- `[touchui] type 0x1d -> rom:/Data/Public/Game/GUI/msgBox_t.swf (was GUI/msgBox.swf; …)` для каждого PC-id;
  `[touchui] type 0x4b (GUI/msgBox_c.swf) will be created as 0x1d -> GUI/msgBox_t.swf (PC branch of the same class …)`;
  `[touchui] N of M type(s) substituted, K `_c` type(s) created as their PC twin (DM_DOS2_TOUCHUI_PCWIN=1)`.
- `[touchui] CreateUIObject type 0x4b as its PC twin 0x1d = GUI/msgBox_t.swf (layer … flags …) -> handle …: created`.
- `[touchui] window type 0x1d made for GUI/msgBox_t.swf (…, handle …): SHOWN (flags …, modal)` / `hidden` / `destroyed`.
- `[touch] DM_DOS2_TOUCH_KBM=2: [mgr+0x170] -> 1 (OPEN: a touch menu is on screen in game …)` / `-> 0 (CLOSED: …)`.
- PCWIN=2: `[touchui] DM_DOS2_TOUCHUI_PCWIN=2: mode byte main+0x2bb8918 1 -> 0 …` / `-> 1 …`.

### Проверка на хосте
`tools/dos2_touchui_check.py ../work/exefs-update/main.raw ../work/touchui/menu/TouchUI.pak` — все места (в том числе 6 новых) совпадают, x18 нет;
все 27 типов таблицы имеют ожидаемый хвост, у 10 пар `_c` → PC общий создатель; 11 фильмов таблицы есть в паке. `source_structure_test.py` PASS,
NDK rc=0, APK подписан (`DM_KEYSTORE`).

| файл | размер | SHA-256 |
|---|---|---|
| `work/touchui/menu/TouchUI.pak` (11 меню + `uiElements_t`, без mainMenu_t) | 13 218 662 Б (12.61 МиБ) | `12f81afa3e9572f9ab425b831ec9cf6ac023d103bf42aaeba774473c60284768` |
| `wt-tu04/work/DOS2-port-debug.apk` | 22 565 584 Б | `725af0953cccc941c2738256d6f6989c8afb9b8e6742a3ff1f675d5883fb6c3e` |

### Прогон на телефоне (главный агент, с разрешения хозяина)
1. Копия `files/save`. APK: `<dos2-port>/wt-tu04/work/DOS2-port-debug.apk`.
2. Файл Larian (12.61 МиБ, разрешение хозяина): `<dos2-port>/work/touchui/menu/TouchUI.pak` →
   `/sdcard/Download/DivinityOriginalSin2Mobile/romfs/Data/TouchUI.pak` (заменяет пак `menu1`).
3. `dm/env`: `DM_DBG=1`, `DM_DOS2_TOUCHUI=1` (PCWIN=1 по умолчанию). Без `DM_DOS2_TOUCHUI_MAP`, `DM_DOS2_TOUCH_KBM`, `_PCUI`, `_LIVE`.
4. Что тапать: главное меню — консольное (как просил хозяин); «Настройки» из главного меню (окно `_t`, вкладки, ползунок, галка), назад;
   «Загрузить» (список сохранений `_t`, прокрутка пальцем, назад — не загружать, если не нужно); войти в игру; пауза (пад «+» / Start) — меню
   паузы `_t`, тап по пунктам; из паузы «Сохранить» (окно `_t`, можно отменить); из паузы «Выход в главное меню» → окно сообщения
   (Да / Нет пальцем и падом). Каждое окно — и пальцем, и падом. После закрытия паузы тап по портрету / HUD ничего не делает (фильтр закрыт).
5. В логе: строки выше; `【红】` — `has …, expected …` (другая сборка), `PC twin … not substituted / another creator`, `FAILED`, `created as itself again`.
6. Если окно пустое / без кнопок — `DM_DOS2_TOUCHUI_PCWIN=2` (байт режима 0, пока окно на экране) или `DM_DOS2_TOUCHUI_DENY=<id>` для этого окна.
   Падение при загрузке игры — сначала `DM_DOS2_TOUCHUI_DENY=5e,37` (обучающее окно создаётся в наборе игрока), потом `PCWIN=0`.
7. Force-stop после сбора логов.

Риски: (1) PC-классы этих окон на NX раньше не исполнялись (ветка PC в Switch-сборке жива только по коду, `csel`-места), падение возможно при
первом показе; (2) часть класса может читать байт режима при заполнении (тогда окно `_t` на PC-типе всё равно получит `_c`-вызовы — проверит
PCWIN=2); (3) поиск «по типу» в коде игры (`cmp [obj+0xf0], #0x4b`) после подмены типа не найдёт окно — на экране это «окно не закрывается
/ не открывается второй раз»; (4) видимость = бит 3 по разбору SetFlags, на телефоне не подтверждена — без него фильтр в игре просто не откроется.

## 8. TU-06 / TU-07: что сделано (15.11, ветка `dos2-tu06`, на телефоне не запускалось)
### TU-06 — вызовы фильм ↔ движок (`dm/host/hle_dos2_touchui_ei.c`, `DM_DOS2_TOUCHUI_EI`)
Адреса Switch (main 4979b200, смещения `main.raw`):
- `FlashPlayer` (Iggy): vtable `main+0x29c3ba0`. Объект: `+0x30` плеер Iggy, `+0x48 / +0x50` таблица имён invoke (записи по 16 байт
  `{быстрое имя Iggy, const char *имя}`), `+0xa0` очередь invoke, `+0xd0` аргументы EI (по 0x40), `+0xe8 / +0xf0` записи EI (по 0x18),
  `+0xf8` слушатель (объект UI), `+0x130` буфер строк.
- **EI, фильм → движок.** `0xd20a30` регистрирует обратные вызовы Iggy: `0xd1f650` (предупреждения), **`0xd1f8c0` — обратный вызов
  `ExternalInterface`** (`0x3ec900`), `0xd1fd70` (custom draw). `0xd1f8c0` находит `FlashPlayer` по плееру и зовёт **`0xd279c0`**
  (`FlashPlayer`, вызов `{char *имя, +8 длина, +0x10 число аргументов, +0x18 аргументы по 0x20}`) — тот только **ставит вызов в очередь**.
  Очередь разбирает **`0xd25d90`** (vt+0x130, прямых вызовов нет): для каждой записи `слушатель->vt[0](слушатель, имя, число, аргументы)`.
  `vt[0]` объекта UI = **`UIObject::OnFunctionCalled`**; базовая реализация `0xae2780` (vtable `main+0x2999a18`, слот 0: `closeUI`,
  `inputFocus`, `inputFocusLost`, `UIAssert`, `PlaySound`, `setMcSize`, `registerAnchorId`, `setAnchor`, `setPosition` … через `strcmp`),
  игровая база `0x10a5750` зовёт её; у каждого класса своя цепочка сравнений впереди. Незнакомое имя проваливается до конца без действия.
- **Invoke, движок → фильм.** vt+0x58…+0x88 = `Invoke0…Invoke6` (`0xd23d10 0xd23f60 0xd241c0 0xd24430 0xd246c0 0xd24960 0xd24c20`:
  `(this, id, InvokeDataValue *a1…a6)`), vt+0x90 `0xd24ef0` = `InvokeArgs(this, id, args, n)`; зовутся только через vtable. Каждый берёт
  замок `+0x60`, создаёт запись 0x28 (`0xd286e0`) и кладёт в очередь `+0xa0`; фильм зовётся позже (`0xd26030` →
  `IggyPlayerCallMethodRS 0x3ebca0`).
- `InvokeDataValue` на Switch — 0x40 байт: `+0` тип (3 bool `+0x28`, 4 число `+0x38`, 5 строка `+8`, 6 широкая строка `+0x18`).
- **Не найдено:** `SetValue` / `GetValue` (кандидаты по вызовам Iggy: vt+0xe0 `0xd25900`, vt+0xf8 `0xd25ae0`; vt+0x110 `0xd25ba0` ставит
  rotation / scaleX / scaleY). Перехвата нет.

Что делает хост при `DM_DOS2_TOUCHUI_EI=1` (9 перехватов прологов с трамплином, 17 мест сверяются пословно, 5 слотов vtable — в
`tools/dos2_touchui_check.py`):
- `0xd25d90`: хост сам проходит записи так же, как игра, пишет каждый вызов в лог, сначала спрашивает свою таблицу обработчиков, иначе
  зовёт `OnFunctionCalled` игры; потом обнуляет счётчик записей и отдаёт оригиналу только уборку.
- `Invoke0…6`, `InvokeArgs`: только лог (имя из таблицы плеера по id), затем оригинал.
- Лог `[ei]`: первая встреча пары (тип UI, имя) — строка с аргументами; дальше на 10, 100, 1000… вызовах; полная таблица со счётчиками —
  через `DM_DOS2_TOUCHUI_EI_DUMP_S` (20) секунд без новых имён и при подъёме тач-HUD.
- Обработчики хоста: `dm_dos2_touchui_ei_register(имя, fn, ctx)` (`hle_dos2_touchui_ei.h`), `fn` возвращает 1 = вызов забран. Встроено:
  29 имён, которых нет строкой в Switch main (`rightBarBtnClicked`, `slotUpEnd`, `selectSkillSet`, `openMap`, `didUse2FingerScrolling` …),
  забирает заглушка-счётчик. `DM_DOS2_TOUCHUI_EI=2` — только лог, без заглушек. `dm_dos2_touchui_invoke(player, имя, args, n)` — вызов
  метода фильма с хоста через `InvokeArgs` (только имена, которые класс сам зарегистрировал).

### TU-07 — тач-HUD, шаг 1 (`dm/host/hle_dos2_touchui_hud.c`, `DM_DOS2_TOUCHUI_HUD`)
- Тач-ветка сборщика iPad `0x1012dfea4` (режим 2, `0x1012e0268`; создания с `0x1012e0b48`), `CreateUIObject(mgr, слой, тип, флаги |
  флаги сборщика, 0x80, игрок)`:

| фильм | id iPad | слой | флаги | класс на Switch (тип) |
|---|---|---|---|---|
| statusConsole_t | 0x98 | 6 | 0x201e0 | statusConsole 0x75 |
| minimap_t | 0x94 | 2 | 0x221e0 | minimap 0x1e |
| playerInfo_t | 0x96 | 0xf | 0x201e0 | playerInfo 0x26 |
| hotBar_t | 0x97 | 0x10 | 0x201e0 | hotBar 0x28 |
| combatLog_t | 0xce | 0xa | 0x201e0 | combatLog 7 |
| rightSideButtons_t | 0x99 | 0x12 | 0x201e0 | нет (TU-09) |

  Ещё в тач-ветке: journal_csp_t 0x95 слой 0xf; переключатель персонажей 0xa8 слой 0x16; лупа 0xcc слой 0, флаги 0x261e0; панели
  0x9b…0x9e, 0xa7 слой 0xa (TU-10+). PC-ветка iPad для сравнения: statusConsole 6, hotBar 0xf, minimap 2, combatLog 0xa, playerInfo 7 —
  флаги 0x201e0. У тач-объектов свои ячейки записи игрока (`+0x2b8…+0x338`), которых в записи Switch нет.
- Сборщик Switch `0x10c06d0` после создания только кладёт хэндл в ячейку (`str x0,[x22,#…]`), никаких Show — объекты живут сами.
- Хост: перехват **create-all `0x10c4580`** (после: при байте режима 0 создаёт для каждой записи игрока типы 0x75, 0x1e, 0x26, 0x28, 7 с
  путями `_t`, слоями и флагами из таблицы, флаги `| 0x20000000`) и **destroy-all `0x10c4730`** (до: уничтожает свои объекты так же, как
  игра уничтожает ячейку — `0x10bc7dc…0x10bc864`: `obj->vt[0xf0/8]()`, `DestroyUIObject 0xaeba10(mgr, &хэндл)`, сообщение
  `{main+0x2999ec0, 1, obj}` → `0xa47030([main+0x2bc0530])`). Хэндлы хранит хост. Пути создателей правятся при первом создании (хвост
  после `GUI/`), фильма нет в паке — тип пропускается с красной строкой.
- Переключение — существующий `DM_DOS2_TOUCH_LIVE=1` (включается сам): касание → байт режима 0 + пересборка, ввод пада → 1 + пересборка.
  Добавлен шлюз `dm_dos2_touchui_hud_gate` (одна строка в `dt_live_switch`): только в игре (есть записи игроков), только в состоянии игры
  `0x13` и пока на экране нет видимого (`флаги +0x14`, бит 3) dialog 0xe / dialog_c 0x42, 0x65 / loadingScreen 0x17 / characterCreation 3, 4.
  Состояние = `u32 [[[[main+0x3298150]+0x30]]+0x10]` (сборщик читает его в `0x10c0764`); самые сравниваемые id в main — 0x13 (14 мест),
  7 (12), 0x1c (5), 0x11 (3) = Running / Menu / GameMasterPause / Paused перечисления PC. **Что 0x13 = «идёт игра» — вывод из статики,
  не проверено**: если шлюз отказывает всегда, в логе `[hud] switch … refused: game state (game state 0x…)` — поправить
  `DM_DOS2_TOUCHUI_HUD_STATES=<hex,…>` или `any`. Отказ касанию — отбрасывается (следующее касание спросит снова); отказ паду —
  повторяется каждый опрос. Вне игры байт режима хостом в 0 не ставится; при выгрузке набора самой игрой с поднятым тач-набором байт
  возвращается в 1 (меню и загрузка — контроллерные, как в прогоне tu04).
- Пад хост не выключает никогда. В тач-наборе записи пада отбрасывает сама игра (`0x844014`), хост видит их в опросе и переключает обратно
  (первое нажатие тратится на переключение — повтор записи остался за TU-08; пад при тач-HUD — TU-14).
- Переключатели: `DM_DOS2_TOUCHUI_HUD=1`; `DM_DOS2_TOUCHUI_HUD_SET=28,1e` (только эти типы — искать ломающий);
  `DM_DOS2_TOUCHUI_HUD_PC=1` (PC-фильмы `hotBar.swf` … вместо `_t`, для сравнения); `_STATES`, `_BLOCK=<hex,…>`.
- Общий код тронут тремя местами: `main.c` (2 строки установки), `hle_dos2_touch.c` (шлюз в `dt_live_switch`), `hle_dos2_touchui.c`
  (`dm_dos2_touchui_pak_has`).

**Пак `hud`**: `<dos2-port>/work/touchui/hud/TouchUI.pak`, 19 фильмов (набор `menu` + hotBar, minimap, playerInfo,
statusConsole, combatLog, combatTurn), 16 848 386 Б (16.07 МиБ), SHA-256 `913a11b78b04944a3e7f5d44c61e5e1e06d42b1c7bf2524945412cc10cdcba3d`,
чтение обратно — все файлы равны. Импорты HUD-фильмов (`fonts_en.swf`, `uiElements.swf`, `LSClasses.swf`) есть в `Game.pak` Switch.
Сверх набора `menu` — 1 355 331 Б.

### Прогон на телефоне (главный агент, с разрешения хозяина; копия `files/save` до запуска)
1. APK `<dos2-port>/wt-tu06/work/DOS2-port-debug.apk` (22 590 160 Б); пак `hud` → `romfs/Data/TouchUI.pak` (файл Larian, 16.07 МиБ).
2. Шаг А, только лог: `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_TOUCHUI_EI=1`. Ждать: `[ei] DM_DOS2_TOUCHUI_EI=1: 9 of 9 hooks`, в меню
   `[ei] EI <- type 0x48 mainMenu_t.swf: buttonPressed(…)`, `PlaySound`, `[ei] invoke -> … addMenuButton(…)`, таблица `[ei] table (quiet)`.
   Меню и игра должны вести себя как в tu04.
3. Шаг Б, HUD: добавить `DM_DOS2_TOUCHUI_HUD=1`. В логе: `[hud] DM_DOS2_TOUCHUI_HUD=1: create-all … hooked`, `[touch] DM_DOS2_TOUCH_LIVE=1`.
   В игре коснуться экрана вне пада: `[hud] switch #1 to the touch set (game state 0x13 …)`, `[touch] live #1: touch -> PC …`,
   `[hud] type 0x28 -> GUI/hotBar_t.swf …` ×5, `[hud] CreateUIObject type 0x28 = GUI/hotBar_t.swf … -> handle …` ×5,
   `[hud] touch HUD up: 5 object(s) for 1 player(s)`, `[touchui] movie load: the game reads Public/Game/GUI/hotBar_t.swf`. Нажать кнопку пада:
   `[hud] switch #2 to the controller set`, `[hud] touch HUD down (switch): 5 of 5`. Пять переключений подряд; затем диалог с NPC —
   `[hud] switch … refused: a blocking UI is on screen`.
4. Падение при подъёме — сузить `DM_DOS2_TOUCHUI_HUD_SET` по одному типу; пустые панели — сравнить с `DM_DOS2_TOUCHUI_HUD_PC=1` и смотреть
   таблицу `[ei]` (какие invoke класс шлёт, что фильм не нашёл). Force-stop после сбора логов.

Риски: (1) PC-классы HUD на NX не исполнялись ни разу — падение при создании или первом обновлении возможно; (2) хэндлы не лежат в
ячейках записи игрока — код игры, который ищет HUD через запись (скрыть в диалоге / ролике), их не найдёт: HUD может остаться поверх;
(3) id состояния 0x13 и типы «блокирующих» UI — из статики; (4) сообщение `0xa47030` при уничтожении повторено по образцу, смысл его не
разобран; (5) игрок, добавленный посреди сессии (`0x10bafe0`), тач-HUD не получит до следующей пересборки; (6) в режиме 0 окна,
создаваемые с выбором id по байту режима, станут PC-фильмами (пока TU-04/05 не подставит `_t`); (7) перехват `0xd25d90` меняет порядок
«разбор — уборка» на «хост разбирает, оригинал убирает» — при `DM_DOS2_TOUCHUI_EI=0` его нет.

## 9. Вариант D (только главное меню): консольный вид + настоящее касание — сделано (15.11, ветка `dos2-tud`, на телефоне не запускалось)
Решение хозяина 15.11: главное меню остаётся нашим консольным (`mainMenu_c`) по ВИДУ, поведение — полный тач: тап по пункту = нажатие
именно этого пункта (наведение / нажатое состояние, действие пункта), как в портах D3 / TL2 / PoE; пад работает тоже. Всё остальное —
айпадовское, по плану выше. Не реализовано, только факты и маршрут:
- `mainMenu_c.swf` (`tools/dos2_swf_abc.py dump`): пункт — класс `Menu_button`; `Menu_button::selectElement` шлёт EI `buttonOver`,
  `Menu_button::buttonFunction` — EI `buttonPressed`; список — `LS_Classes.listDisplay / scrollList` (`selectMC`, `next`, `previous`);
  корень: `moveCursor/1`, `setSelection/1`, `executeSelected/0`, `onEventUp/Down` на `IE UIUp / UIDown / UIAccept / UICancel`. В пуле
  констант фильма есть `MouseEvent`, `mouseDown`, `mouseUp`, `rollOver`, `mouseOver` (у `larCombo` / `scrollbar` точно; вешает ли слушателей
  сам `Menu_button` — инструмент не показывает, не разобрано). Наблюдение с телефона «клик подтверждает только текущий пункт» сходится с
  гипотезой, что нажатие ведёт в `executeSelected` текущего выделения, но место в AS3 не найдено — это первое, что надо разобрать.
- У `mainMenu_c` на Switch есть готовые `mainMenu_c.iggy / .iggytex`; подставной фильм под новым именем пойдёт как `.swf` (как `_t`).
- Маршрут (б), предпочтительный: правка ABC `mainMenu_c` — в `Menu_button` слушатели `ROLL_OVER` → `selectElement`, `MOUSE_DOWN / UP` →
  нажатое состояние + `buttonFunction` (его же EI `buttonPressed`, класс движка не меняется), файл `mainMenu_ct.swf` в `TouchUI.pak`,
  подстановка пути типов 0x48 / 0x57 уже работает. Нужен патчер ABC в `tools/` (сейчас `dos2_swf_abc.py` только читает).
- Маршрут (в): `mainMenu_t` уже делает всё это (его `Menu_button` с мышью, EI те же) — перенести в него графику `_c` (символы / кадры)
  дороже и требует сборки SWF; брать, только если (б) упрётся в различия классов.
- Проверка маршрута без телефона невозможна; на телефоне `DM_DOS2_TOUCHUI_EI=1` покажет, какие EI приходят от `mainMenu_c` на тап сейчас
  (`buttonPressed` с каким id, был ли `buttonOver`).

## 10. Прогон tu05a (15.11, pack 2s): «тапы не работают нигде» — причина и исправление (`dm/host/hle_dos2_touchui_ie.c`)
Лог `work/logs/tu05a.log`, окружение `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_TOUCHUI_EI=1`. Что видно:
- Главное меню в этом прогоне — `mainMenu_c.swf` (тип 0x48 не подставляется по решению хозяина): тап там не работает по известной причине
  (вариант D, §9, не сделан). Это не регрессия перехватов EI: `buttonOver(0)`, `buttonPressed(1)` от пада в логе есть.
- Пауза (`gameMenu_t`, тип 0x4d) и окна `msgBox_t`: фильтр `[mgr+0x170]` открывался, движения мыши доходили до Iggy (счётчик 107 → 136 → 147
  при открытой паузе, начало фильма 240,0), кнопки тоже (1542 → 1917 → 2005) — а EI от фильма нет.
- **Причина — в AS3 айпадовских фильмов, не в доставке мыши и не в `setControllerMode`** (у `gameMenu_t` такого метода нет вовсе).
  Кнопка `LS_Classes.touchSimpleButton` на `MOUSE_DOWN / MOUSE_UP` только меняет вид (нажата / отпущена). Действие висит на своём событии
  `touchCustomEventType.TAP` (`onTapHandle` → `callbackFunction` → EI `buttonPressed`), а рассылает его сам фильм из обработчика
  **события ввода**: `MainTimeline::onEventUp("IE Tap")` → `touchHelpers.dispatchCustomMouseEvent(root, TAP, true)` (объект под
  `stage.mouseX / mouseY`); `onEventDown("IE Tap")` запоминает тап мимо меню (`isFadeTapDowned` → отпускание закрывает меню).
  «Tap» — событие ввода движка iPad (жест в `touchscreen_1.json`). В массиве `events` его объявляют **38 из 56** `_t`-фильмов;
  ещё `IE ShortLongPress` — 18, `IE LongPress` — 9, `IE DoubleTap` — 7, `IE Touch2Fingers` — 24. Ни одного из этих имён нет строкой в
  Switch main: движок Switch их не знает и не шлёт. У `mainMenu_t` в `events` только `UIUp / UIDown / UIAccept / UICancel`, кнопки берут
  мышь — поэтому в tu04 оно работало, а остальные фильмы — нет. (Поправка к §2.4: «отдельного тач-канала в фильм нет» — неверно.)
- Как движок связывает события ввода с фильмом (`FlashPlayer`): `0xd26ae0` (vt+0x160) читает `events[i]` через
  `[this+0x108]->vt[0xa8/8](obj, "events", 5, &value, i)` — **это и есть GetValue** (объект значений плеера `+0x108`, не слот самого
  `FlashPlayer`; поправка к §8), ищет имя без `IE ` в именах событий менеджера ввода и кладёт узел `{событие, индекс}` в хеш плеера
  (`+0x110 / +0x114 / +0x118`); неизвестное имя пропускает. `0xd23420` (vt+0x30, `OnInputEvent`) по хешу сразу зовёт
  `onEventDown / onEventUp(индекс, игрок, значение)` фильма.

Исправление (включено вместе с `DM_DOS2_TOUCHUI=1`, `DM_DOS2_TOUCHUI_IE=0` — выключить):
- перехват `0xd26ae0`: после движка хост так же проходит `events` и для `IE Tap`, `IE LongPress`, `IE ShortLongPress` добавляет узлы со
  своими id (0x7f01…0x7f03, аллокатор движка `0x83cc00`);
- `hle_dos2_touch.c` сообщает о жесте (тап; долгое нажатие 500 мс), через `DM_DOS2_TOUCHUI_IE_DELAY` (2) опроса — когда движение и кнопка
  этого касания уже дошли до Iggy — хост зовёт `OnInputEvent(id, нажато)` и `(id, отпущено)` у каждого видимого объекта UI (флаги `+0x14`
  биты 3 и 5), у плеера которого есть узел. Не шлётся, если мышь сейчас до фильмов не доходит (`[mgr+0x170] = 0` в контроллерном режиме);
- 12 мест сверяются пословно (`tools/dos2_touchui_check.py`); отладка: `[ie] type … events […]: N touch event(s) wired`,
  `[ie] IE Tap down/up -> N movie(s): gameMenu_t.swf(0x4d)=…`; строка `[touch] Iggy button` теперь с именем фильма и пишется весь
  прогон для левой кнопки у целевого фильма (в tu05a обрывалась на 96).
- Не сделано: `DoubleTap`, `Touch2Fingers` (прокрутка двумя пальцами в `saveLoad_t` / `optionsSettings_t` — у фильмов есть и своя прокрутка
  мышью), порядок между перекрывающимися `_t`-окнами (Tap получает каждое видимое).

Проверка на телефоне (pack со слиянием `dos2-tu06`, пак `hud` уже на месте): то же окружение `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_TOUCHUI_EI=1`.
В логе при старте `[ie] touch input events for the iPad movies: main+0xd26ae0 hooked`, при создании окон
`[ie] type 0x4d gameMenu_t.swf: events [UIUp, UIDown, UIAccept, UICancel, Tap]: 1 touch event(s) wired`. В игре открыть паузу (пад +), тапнуть
по пункту: `[ie] IE Tap down -> … gameMenu_t.swf(0x4d)`, затем `[ei] EI <- type 0x4d gameMenu_t.swf: buttonPressed(…)`; тап мимо меню —
меню закрывается. Потом окно сообщения (выход в меню → «да / нет»), «Загрузить» (`saveLoad_t`), настройки. Главное меню тапом по-прежнему
не работает (вариант D).
Риски: (1) `OnInputEvent` зовётся из перехвата опроса пада, а не из разбора очереди ввода — если Iggy в этот момент занят другим потоком,
вызов вернёт ошибку «re-enter» без действия (в логе `=0` и нет EI); (2) узлы хоста в хеше плеера освобождает движок своим `free` —
выделены его же аллокатором; (3) Tap уходит всем видимым `_t`-фильмам: при окне поверх окна может сработать и нижняя кнопка.

айпадовское, по плану выше.

### Маршрут: правка ABC `mainMenu_c` → `GUI/mainMenu_d.swf` в `TouchUI.pak`
- `tools/dos2_abc.py` — читатель / **писатель** ABC + дизассемблер + мини-ассемблер + разбор тегов SWF. Модель пишется обратно байт в байт
  (`roundtrip`: `mainMenu_c` и `mainMenu_t` — SWF и ABC совпадают), `verify` проверяет каждое тело метода (глубина стека на каждом слиянии,
  scope, локальные, цели переходов, индексы констант): 617 тел `mainMenu_c` и 671 `mainMenu_t` проходят — проверка откалибрована на
  коде компилятора Adobe. Команды: `roundtrip | classes | dis <Класс[::метод]> | verify`.
- `tools/dos2_mainmenu_touch.py build|check <Switch Game.pak | mainMenu_c.swf> [mainMenu_d.swf]` — сам патч и его статическая проверка.
  `tools/dos2_touchui_pak.py menu|hud|all` теперь сам делает `mainMenu_d.swf` из `../work/romfs/Data/Game.pak` (`--sw <путь>`, `--no-d` — без
  него), прогоняет `check` и кладёт фильм в пак.
- Имя `mainMenu_d.swf` той же длины, что `mainMenu_c.swf`: путь создателя пишется на месте (как `_t` в прогоне tu04). Таблица `k_def`
  (`hle_dos2_touchui.c`): 0x48 и 0x57 → `mainMenu_d` **по умолчанию** при `DM_DOS2_TOUCHUI=1`. Тип не меняется → класс движка и его
  `_c`-ветка те же. Выключить: `DM_DOS2_TOUCHUI_DENY=48,57` (штатный `mainMenu_c`); айпадовский: `DM_DOS2_TOUCHUI_MAP=48:mainMenu_t,57:mainMenu_t`.
- У `mainMenu_c` на Switch есть `mainMenu_c.iggy / .iggytex`; `mainMenu_d` пойдёт как `.swf` (как `_t` и 135 фильмов Switch без `.iggy`).
  Не доказано, что вид `.swf` и `.iggy` совпадает точка в точку (шрифты / сжатие текстур) — смотреть на телефоне.

### Что нашлось в `mainMenu_c` (разбор `dis`)
- `Menu_button` (символ 3) — **один текст `label_txt`** (DefineEditText, autoSize CENTER), фона нет. В его `frame1` уже стоят слушатели
  `mouseOver / mouseOut / mouseDown` → `buttonOver / buttonOut / buttonPressed`, но это **пустые заглушки** (3 байта: `getlocal0, pushscope,
  returnvoid`); `buttonReleased` тоже пуст. В `mainMenu_t` те же методы живые (`bg_mc.gotoAndStop`, `mouseUp` → `buttonFunction`). То же у
  `redButton_25` (кнопка подарков): `onOver / onDown / onUp` пустые.
- **Почему клик подтверждал только текущий пункт.** В фильме нет ни одного работающего обработчика мыши (см. выше) и нет глобального
  слушателя (`stage.addEventListener` — только у `scrollbar` / `larCombo`). `buttonFunction` (EI `buttonPressed`) зовётся из одного места:
  `mainMenu_mc_1::executeSelected` ← `MainTimeline::onEventUp` на `IE UIAccept` (и корневой `executeSelected/0`, но строки `executeSelected`
  в `main` нет — движок его не зовёт). Значит клик доходит до фильма как **событие ввода `IE UIAccept`**, а не как мышь. Исключено: хост
  (`hle_dos2_touch.c` шлёт только `mouse:left`, кнопку A не шлёт); обработчик EI класса (`0x117f968 buttonOver` — только подсказка для id
  0x19…0x1c, выделение не запоминает, то есть сам движок «текущий пункт» не нажимает). **Не найдено:** место в движке, где `mouse:left`
  становится `UIAccept` (в `inputconfig.json` `mouse:left` → `FlashLeftMouse`, `Action1`; `UIAccept` ← только `c:a` в `Eoc/controller_1.json`).
  На патч это не влияет: защита стоит на единственном входе — `executeSelected`. Прогон с `DM_DOS2_TOUCHUI_EI=1` покажет порядок событий.
- Раскладка списка: `listDisplay.positionElements` берёт `element.height`, поэтому добавлять в кнопку прозрачный прямоугольник для
  попадания нельзя (изменит шаг списка, то есть вид). Отсюда решение — геометрия вместо попадания в объекты.

### Что добавлено (только класс `mainMenu_c_fla.mainMenu_mc_1`, 9 методов в конец; ни одно исходное тело не изменено)
- `init` и `executeSelected` — имена переведены на новые методы-обёртки, исходные методы остались под именами `tuInitOrig` / `tuExecOrig`.
- `init`: исходный + один раз `stage.addEventListener("mouseMove" | "mouseDown" | "mouseUp")` (так же делает штатный `scrollbar`).
- `tuHit()`: пункт под указателем — `btnList.containerContent_mc.mouseX / mouseY` против каждого видимого `selectable` элемента: строка
  `y − EL_SPACING/2 … y + высота + EL_SPACING/2` (без щелей между пунктами), по горизонтали центр надписи ± (полширины + 120 px).
  Открытый выпадающий список (`combo_mc.m_isOpen`) не трогается. `tuActive()`: меню видно и заставка (`splashScreen_mc`) не видна.
- движение без кнопки → пункт под пальцем становится выделенным штатным `listDisplay.selectMC` (стрелка, текст-описание, EI `buttonOver`,
  звук) — это «наведение» консольного вида; нажатие → выделение + нажатый вид (надпись на 2 px ниже, alpha 0.6); увод пальца с пункта —
  отмена; отпускание на том же пункте → его `buttonFunction()` (EI `buttonPressed(id)`), у выключенного — EI `disabledButtonPressed(id)`.
- **Двойное срабатывание.** `executeSelected` 700 мс после события кнопки мыши (и после движения мыши, если до фильма уже дошло хотя бы
  одно событие кнопки) ничего не делает; когда срабатывает — ставит метку, и отпускание в течение 700 мс после неё второй раз не нажимает
  (другой порядок тех же двух событий). Залипнуть нечему — только время. Если Iggy событий мыши до `stage` не доведёт, защита не
  включится и останется сегодняшнее поведение (клик подтверждает текущий пункт), хуже не станет.
- Пад: `onEventUp / onEventDown`, `moveCursor`, `setSelection`, `setSelectedIndex`, `listDisplay.next / previous / selectMC` — те же
  методы, байт в байт; A идёт через обёртку (не срабатывает только в 700 мс после касания). Заставка по A / клику — как была.

### Проверка без телефона (всё проходит)
`python -I tools/dos2_mainmenu_touch.py check ../work/romfs/Data/Game.pak <mainMenu_d.swf>`: все теги, кроме DoABC, байт в байт (193 тега:
вид тот же); пул констант только дополнен (+26 строк, +17 имён); 617 исходных тел байт в байт, +9 методов; пад-путь привязан к тем же
методам; меняется только один класс (+9 трейтов-методов, без слотов: класс не sealed, состояние — динамические свойства `tuDown / tuItem /
tuY / tuLast / tuFired / tuSeen / tuHooked`); все 626 тел проходят `verify`, у новых `max_stack` / `local_count` равны вычисленным, scope
10..11; фильм читается обратно и переписывается байт в байт; контракт (`dos2_swf_abc.py diff`): EI 13 общих, методов корня 62 общих, различий нет.
`tools/dos2_touchui_check.py ../work/exefs-update/main.raw ../work/touchui/hud/TouchUI.pak` и `source_structure_test.py` — проходят.

| файл | размер | SHA-256 |
|---|---|---|
| `mainMenu_d.swf` (из `mainMenu_c.swf` 2 263 326 Б) | 2 264 045 Б | `efb1aabc83ad0fcfe32f689e45a402904d17be47ef0bfa9c72e495f142e61d27` |
| `work/touchui/hud/TouchUI.pak` (19 фильмов) | 16 817 345 Б (16.04 МиБ) | `f996d4c2b74fdb4aad904a8523a1c74d25ee0c3cf15da507b5f3de6f6c42d0f5` |

### Прогон на телефоне (главный агент, с разрешения хозяина; копия `files/save` до запуска)
1. Положить новый `work/touchui/hud/TouchUI.pak` (16.0 МиБ) в `romfs/Data` вместо прежнего, APK этой ветки; `dm/env`: `DM_DOS2_TOUCHUI=1`,
   `DM_DBG=1`, для первого раза `DM_DOS2_TOUCHUI_EI=1`.
2. Лог при старте: `[touchui] type 0x48 -> rom:/Data/Public/Game/GUI/mainMenu_d.swf (was GUI/mainMenu_c.swf; … written in place)`, то же для
   0x57; `CreateUIObject type 0x48 = GUI/mainMenu_d.swf … created`; `movie load: the game reads Public/Game/GUI/mainMenu_d.swf`. Красная
   строка «создание не удалось → прежний путь» = фильм не загрузился (тогда меню штатное).
3. Вид: меню то же, что `mainMenu_c` (сравнить снимок с прежним: шрифт, фон, стрелка, подсказки кнопок, профиль).
4. Тап по НЕ текущему пункту: стрелка прыгает на него, надпись «проседает», пункт срабатывает (в логе EI `buttonOver <id>`, затем ровно
   один `buttonPressed <id>` с id этого пункта). Тап мимо пунктов — ничего. Палец увести с пункта и отпустить — ничего.
5. Пад: вверх / вниз / A / B как раньше; A сразу после касания (< 0.7 с) может не сработать — это защита.
6. Заставка «нажмите кнопку» — закрывается как раньше; вложенные списки (сложность, «Продолжить / Загрузить») — те же тапы.
7. Если тап ведёт себя как раньше (подтверждает текущий пункт): Iggy не довёл события мыши до `stage` — в логе не будет `buttonOver`
   на касание; тогда следующий шаг — слушатели на самих `Menu_button` (их заглушки уже подписаны) + прозрачная область в `scrollHit_mc`.
8. Откат без пересборки: `DM_DOS2_TOUCHUI_DENY=48,57`.

### Риски / не сделано
- Не проверено на телефоне: доставка `mouseMove / mouseDown / mouseUp` до `stage` в Iggy 1.2.56 Switch (опора — тот же приём у штатного
  `scrollbar` и работающий `mainMenu_t`), `mouseX / mouseY` у контейнера списка, порядок «клик → `IE UIAccept`» против событий мыши (защита
  симметричная, покрывает оба порядка).
- Кнопка подарков (`giftBagsBtn`, `redButton_25`) и подсказки кнопок касанием не нажимаются — только пад (в `_c` у них нет EI; `_t` шлёт
  `GiftBagPressed`, которого класс на `_c`-ветке может не знать).
- Зона попадания считается по надписи: длинные пункты шире, короткие — надпись ± 120 px.
- Окно поверх главного меню (настройки, сообщение, сохранения): если движок отдаёт событие мыши и фильму под ним, тап по окну в месте
  пункта меню нажал бы и пункт. Проверить на телефоне (открыть настройки, тапать там, где под окном пункты меню); если так — в `tuActive()`
  нужен признак «меню перекрыто» от хоста или движка.
