# DOS2: видимые элементы для пальца — там, где у iPad кнопка на экране, а у нас только пад / радиальное меню (15.11.2026, ветка `dos2-vis`)

Правило хозяина 15.11: «Где у iPad на экране есть нажимаемый элемент, а у нас только кнопка пада или радиальное меню — так нельзя. Либо
рисуем нажимаемый элемент прямо в UI игры на нужном месте и вешаем на него функцию кнопки, либо берём с iPad. Нельзя вешать функцию на тап,
если нажимать не на что. Накладка с кнопками Switch — не элемент». Значит вердикт **C** из `TOUCH_UI_AUDIT.md` (хост: область → кнопка пада) и
тап по подсказке кнопки недостаточны везде, где у iPad есть видимый элемент.

Всё ниже — статика (SWF / ABC / `main.raw`), на телефоне не запускалось.

## 0. Способы

- **G — пересадка** (`tools/dos2_swf_graft.py`): рисунок элемента iPad (DefineSprite + его DefineShape / DefineBitsLossless / текст, id
  переназначены) вставляется в наш `_c`-фильм на место, где он стоит у iPad; нажатие вешается правкой ABC на ту же функцию, что делает пад
  (EI того же фильма, либо EI хоста). Классы iPad не переносятся: элемент — обычный MovieClip, кадр состояния заморожен.
- **O — отдельный фильм iPad** поверх нашего HUD (как `rightSideButtons_t`): нужен объект UI, а класса `UITouch*` в Switch нет → создавать
  объект чужого зарегистрированного типа с подменой пути (как `hle_dos2_touchui_hud.c`) и ловить его EI на хосте.
- **P — весь экран `_t`**: только когда пересаживать пришлось бы почти всё (контракт `_t` ≠ `_c`, `TOUCH_UI_AUDIT.md` §0 п. 7).
- **B** — элемент у нас уже нарисован, но не слушает мышь: правка ABC без рисунка (это не нарушает правило: нажимать есть на что).

Выбор для колонки кнопок: **G в `bottomBar_c`**, не O. Причины: фильм HUD 1920×1080 на весь кадр и уже в списке маршрутизации мыши (тип 0x3b,
группа `hud`); O требует создать объект типа без своего класса и держать его жизнь на хосте (создание / уничтожение с набором игрока),
а `rightSideButtons_t` ждёт от движка `setButton(id, подсказка)` — без класса iPad колонка была бы пустой.

## 1. Таблица: у iPad элемент есть, в нашем `_c` — нет

Якорь iPad — из матриц SWF (`dos2_swf_graft.py tree`), сцена фильма в скобках. «Функция у нас» — то, что делает пад.

| # | функция | элемент iPad (фильм, символ, место) | наш фильм | функция у нас | решение | состояние |
|---|---|---|---|---|---|---|
| 1 | открыть инвентарь / персонаж / экипировку / навыки / ремесло / журнал | `rightSideButtons_t` (1440×1080, якорь `right`): `cmnTabButton` 74×74 + `ico_inventory` / `ico_charsheet` / `ico_skills` / `ico_craft` / `ico_journal`, колонка x 1348, y 422, шаг 80; `toggleBtnSidePanel` (шлем) 1316,310 | радиальное `panelSelect_c` (0x53) | EI `openPanel(id)` → `main+0x1218580` → `0x121a140`; id 0 инвентарь, 1 персонаж, 2 навыки, 3 ремесло, 4 журнал, 5 экипировка | **G** в `bottomBar_c` | **сделано** (`bottomBar_d`, §3) |
| 2 | карта | там же: `ico_map` (id 2 у iPad) | вкладка журнала `journal_c` | LB/RB внутри журнала | G: кнопка колонки (`ico_map`, ячейка под журналом) → `dmPadBtn(27)` = «−» (`ToggleMap`) | **сделано** (`bottomBar_d`, TOUCH_CONSOLE_PLAN §10) |
| 3 | меню игры | `rightSideButtons_t` `btnOptions` 1348,20 | — (кнопка «+») | `gameMenu_c` по «+» | **G**: `btnOptions` iPad в `bottomBar_d` (1480,20, левее миникарты) → `dmPadBtn(28)` = «+» (⚙ — не элемент игры, правило 15.11) | **сделано** (TOUCH_CONSOLE_PLAN §10) |
| 4 | ряд хотбара − / + | `hotBar_t` (1500×1080): `btnSkillSetLeftFrames_66` / `bntSkillSetRightFrames_62` в `deckSwitcher_46`; `btnDeckSwitcher` у левого края панели | `bottomBar_c` | `IE UIHotBarPrev/Next` → EI `prevHotbar` / `nextHotbar` | **G** в `bottomBar_c` (стрелки слева и справа от слотов) | **сделано** |
| 5 | отмена действия | `statusConsole_t` (1920×1080) `cancelBtn` (круг 68) 1161,933; `hotBar_t` `btnCancelAction` | `bottomBar_c` | B пада (`cancelPressed` только при активной панели) | **G**: `cancelBtn` справа от слотов → `dmPadBtn(1)` = B | **сделано** |
| 6 | конец хода | `statusConsole_t` `endTurnBtn` 207×44, 1271,996 (в `fightButtons_mc`) | `bottomBar_c` (`holdEndTurnHint_mc`, удержание) | EI `endTurn` (по завершении анимации удержания) | **G**: плашка `endTurnBtn` над полосами здоровья, видна при `setActiveInCombat(true)` → EI `endTurn` | **сделано** (подпись — «End turn» из фильма iPad, не локализована) |
| 7 | бежать / ждать | `statusConsole_t` `fleeBtn` / `waitBtn` 146×44 | `statusConsole` (0x75) — если создаётся в режиме пада, кнопки уже есть (A) | EI `FleePressed` / `GuardPressed` | проверить прогоном; иначе **G** в `bottomBar_c` рядом с «конец хода» (EI чужого класса → через хост, как `dmPanel`) | не сделано |
| 8 | подтвердить действие (навык по цели) | `statusConsole_t` `confirmActionBtn` (круг 118) | — | A пада | **G** в `bottomBar_c` → `dmPadBtn(A)`; показывать только в режиме прицеливания (признак не найден) | не сделано |
| 9 | миникарта: пинг, тактическая камера, закрепить камеру, быстрое сохранение, подписи предметов, поле зрения врагов, поиск | `minimap_t`: кнопки вокруг карты (`pingButton`, `tacticalCamera`, `lockCamera`, `quickSaveBtn`, `worldTooltips`, `enemySight`, `activeSearch`) → `buttonPressed/2` | `minimap_c` (357×500, без слушателей) | кнопки пада: такт. камера, подсветка (удержание), сохранение — в паузе | **G** в `minimap_c` (тип 0x3c добавить в маршрутизацию): рисунки кнопок iPad → `dmPadBtn(код)`; тап по самой карте → журнал/карта | не сделано |
| 10 | выбрать сектор радиального меню | — (у iPad меню нет) | `panelSelect_c` | стик + A | не нужно: колонка (№1) заменяет меню; само меню остаётся паду | — |
| 11 | инвентарь: сортировка | `partyInventory_t` `sort_mc` → `openCombineSorter` | `partyInventory_c` (подсказка кнопки) | кнопка пада → `sortBy_c` | **G**: `sort_mc` в шапку инвентаря → `dmPadBtn(кнопка сортировки)` | частично: в `partyInventory_d` / `inventorySkillPanel_d` есть кнопки «меню предмета» (X) и «выбор» (RS), §5; отдельной кнопки сортировки нет (у пада её тоже нет в событиях фильма) |
| 12 | инвентарь: «товары» (пометить на продажу) | `partyInventory_t` переключатель `waresBtn` → `switchWaresModeEnabled` | `partyInventory_c` | `UIMarkWares` / `UIToggleMultiselection` | **G** рисунок переключателя → `dmPadBtn` | **сделано** кнопкой «выбор» (RS = `UIMarkWares` / `UIToggleMultiselection`), §5 |
| 13 | инвентарь / экипировка: сравнить, сменить персонажа | портреты отряда в `partyInventory_t`; сравнение — долгим нажатием | `partyInventory_c`, `equipmentPanel_c` | `UISwitchLeft/Right`, `UICompareItems` | **G**: стрелки ◄ ► у имени персонажа (рисунок `LB_mc` / `RB_mc` создания персонажа iPad) → `dmPadBtn(LB/RB)` | сравнить — **сделано** (кнопка RS, §5); смена персонажа (правый стик ◄ ►) — не сделано: у `dmPadBtn` нет кода стика |
| 14 | экипировка: шлем вкл / выкл | `characterSheet_t` кнопка `helmet_mc` → `setHelmetOption` | `equipmentPanel_c` | `UIToggleHelmet` → `toggleHelmetVisibility` | **G** `helmet_mc` → EI `toggleHelmetVisibility` (тот же класс) | **сделано** (`equipmentPanel_d`, рисунок `SwitchHelmetVisibilityButton` → RS = `UIToggleHelmet`), §5 |
| 15 | лист персонажа: главные вкладки | `characterSheet_t` `StatsTabButton` → `selectedTab` | `statsPanel_c` (вкладки нарисованы в шапке?) | `UITabPrev/Next` | если вкладки нарисованы — **B** (тап по вкладке); иначе **G** `StatsTabButton` | **сделано** стрелками ◄ ► (LB / RB) в `statsPanel_d`, §5; свои вкладки: слушатель `CLICK` сцены в `statsTabHeader_22` есть, но по прогону tw2 не переключают — не разобрано |
| 16 | торговля: баланс, подтвердить, отдать хлам, очистить | `trade_t`: `balanceBtn`, `tradeBtn`, `clearOffers`, `offerJunkBtn2` | `trade_c`: `balanceButtonContainer_mc`, `holdToAcceptHint_mc` (нарисованы, без мыши) | `UITradeBalance` → `balance/0`, удержание A → `confirmOffer/0`, `offerJunk/0` | **B** по своим элементам (`balance/0`, `confirmOffer/0` — EI того же класса); «хлам» — **G** `offerJunkBtn2` | **сделано** в `trade_d`: баланс (X), подтвердить (EI `confirmOffer`), окно / сравнить (RS), закрыть, §5; «хлам» — не сделано (кнопка iPad без подписи) |
| 17 | торговля: ремонт, опознание | `trade_t` `repairClicked` / `identifyClicked` (кнопки) | `trade_c` — пункты контекстного меню (X) | `showActionMenuItem/3` | контекстное меню по долгому нажатию (A в `contextMenu_c`) — элемент появляется; кнопки не нужны | есть другим путём |
| 18 | ремесло: «Соединить» / «Создать», количество ± | `uiCraft_t`: `combineBtn`, `craftBtn`, `plus_mc` / `minus_mc` | `craftPanel_c` (подсказка кнопки) | событие пада `Combine` → EI `combine` / `craftRecipe/2` | **G** `combineBtn` / `craftBtn` в `craftPanel_c` → EI того же класса | **сделано** в `craftPanel_d` кнопкой «✓» (A = `Combine` / `UIAccept`), §5; количество ± — не сделано |
| 19 | журнал / карта: вкладки, «на карте», метки | `journal_csp_t`: `JournalTab2`, `showQuestOnMap`, легенда | `journal_c` (вкладки нарисованы, без мыши) | LB/RB, Y | вкладки **B** (нарисованы); «на карте» **G** кнопки iPad → `dmPadBtn(Y)`; опыт с `journal_csp.swf` (аудит §7) | **сделано** в `journal_d`: стрелки вкладок (LB / RB), «✓» (A: показать на карте / отслеживать), инфо (Y), закрыть, §5; карта (кнопка «−») и удаление метки (X) — не сделано; `journal_csp` (тип 0x8c) не тронут |
| 20 | создание персонажа: старт, назад, голос | `characterCreation_t`: `start_mc`, `back_mc`, `voiceOriginBtn_mc` | `characterCreation_d` (подсказки кнопок внизу) | `startGame/0`, `mainMenu/0`, `playOriginStory/0` | **G** `start_mc` / `back_mc` в `characterCreation_d` → EI того же класса | не сделано (следующее) |
| 21 | настройки: вкладки, применить / ОК / отмена | `optionsSettings_t`: вкладки-кнопки, `apply_mc`, `ok_mc`, `cancel_mc` | `optionsSettings_d` (подсказки) | LB/RB, A → `acceptPressed`, X → `pressedDefaults` | вкладки — если нарисованы, **B**; кнопки **G** → EI того же класса | не сделано |
| 22 | сохранения: загрузить / сохранить / удалить / новая | `saveLoad_t`: кнопки действия, `delete_mc`, `newSave_mc` | `saveLoad_d` (подсказки) | A, `UIDelete` → `deleteSave` | **G** кнопок iPad → EI того же класса (`loadPressed` / `savePressed` / `deleteSave`) | не сделано |
| 23 | окно сообщения: Да / Нет | `msgBox_t` `btnYes` / `btnNo` | `msgBox_c`: подсказки A/B (в `msgBox_d` нажимаются) | `ButtonPressed(n)` | подсказки `msgBox_c` — собственные кнопки Larian с мышью (`addBtnHint` MOUSE_DOWN), но это значки пада → **G** `btnYes` / `btnNo` → EI `ButtonPressed` | не сделано |
| 24 | пауза: пункты | `gameMenu_t` `touchSimpleButton` | `gameMenu_c` (пункты нарисованы) | A | **B** (пункты есть на экране) | не сделано (аудит: 1 ч) |
| 25 | награда: принять | `reward_t` `acceptBtn` | `reward_c` `holdAcceptBtn_mc` (нарисован) | удержание A | **B** | **сделано** (`reward_d`: «✓» → EI `acceptClicked`), §5; не проверено, принимает ли движок без выбора награды |
| 26 | контейнер: взять всё, закрыть | `containerInventory_t` | `containerInventory` — кнопки есть и с мышью | — | A (уже) | есть |
| 27 | диалог: торговля, «перестать слушать» | `dialog_t` `trade_mc`, `stopListen_mc` | `dialog_c` (подсказки) | кнопки пада | **G** двух кнопок → `dmPadBtn` | не сделано |
| 28 | подсказки-кнопки «(A) … (B) …» во всех меню | у iPad их нет: на их месте настоящие кнопки (строки 16–23) | `_d`-фильмы: тап по подсказке | — | считать временным; заменяется строками 16–23 | — |

Итог по правилу: закрыты №1, 4, 5, 6 (ядро HUD); следующими по пользе идут №20 (старт / назад в создании персонажа), №9 (миникарта), №18, №16, №22–23.
Порядок величины: G одной–трёх кнопок в фильм, где EI того же класса, ≈ 0.5–1 ч агента (инструмент готов); EI чужого класса — + обработчик хоста
(образец `dmPanel`). Настоящий ограничитель — прогон на телефоне: места кнопок взяты из матриц, а не увидены.

## 2. Инструмент пересадки — `tools/dos2_swf_graft.py`

Копирует дерево символа из одного SWF в другой на уровне тегов: спрайт → всё, что ставит его шкала; фигура (DefineShape 1–4, морф 1–2) →
растры заливок и линий (в т. ч. стили записей StateNewStyles); DefineEditText / DefineText → шрифты; DefineButton2 → состояния; записи
ImportAssets2; «висящие» теги (CSMTextSettings, DefineScalingGrid, DefineFontAlignZones, DefineFontName). Id переназначаются на свободные.
Свои теги цели не переписываются: новые определения вставляются перед первым управляющим тегом корня, PlaceObject2 экземпляров — перед
DoABC первого кадра (глубина = наибольшая занятая + 1, имя экземпляра задаётся).

- Классы AS3 источника не переносятся (классу iPad нужны его библиотека и класс движка). Поэтому спрайты по умолчанию **замораживаются**:
  шкала заменяется списком показа одного кадра (кадр 1 или `frames={id: n}`), иначе многокадровая кнопка без `stop()` мигала бы состояниями.
- `Graft.sprite([...])` собирает новый однокадровый спрайт из пересаженных символов (плашка + значок), `bounds()` даёт рамку для центровки.
- Экземпляр в ABC берётся через `getChildByName(имя)` (корень — динамический `MainTimeline`), см. `tools/dos2_hud_touch.py`.
- Проверки: `check()` — все исходные теги цели на месте байт в байт и по порядку (DoABC сравнивается только позицией), id не
  повторяются, каждое использование после определения, повторное сжатие и чтение дают то же; `selftest` — дерево, скопированное туда и
  отображённое обратно, совпадает с тегами источника байт в байт. Прогон разбора по всем 290 фильмам iPad и Switch: 36 319 символов,
  29 810 записей размещения — ни одной ссылки на неопределённый id, ни одного исключения.
- Особенности здешних фильмов: `ImportAssets2` (`texture_lib.swf`, `fonts_en.swf`) идут с числом 0 — ресурсы берутся по имени класса
  (`PlaceObject3` с именем класса, шрифт текста — класс `$…`), импортированных id нет; ветка «импорт с id» поэтому на живых данных не проверена.
  `PlaceObject3` с `HasImage + HasCharacter` имени класса не несёт (вопреки тексту спецификации) — учтено.
- Команды: `list`, `tree <id|символ> [кадр]`, `graft <src> <символ>[@кадр] <dst> <out> [--place x,y[,имя]]`, `selftest`, `png <id>` (посмотреть растр).

## 3. Сделано: `bottomBar_d` — колонка панелей, ряды хотбара, отмена, конец хода

`bottomBar_d.swf` = `bottomBar_c.swf` (Switch `Game.pak`) + 79 тегов определений из трёх фильмов iPad + 10 экземпляров на корне + 5 методов
в `bottomBar_c_fla.MainTimeline` (`tools/dos2_hud_touch.py`; 148 726 → 324 954 Б). Тип 0x3b, строка в `k_hint`
(`hle_dos2_touchui.c`), набор пака `hints` (нужен исходник iPad: `--src`, по умолчанию `work/ipa/Payload/*/Data/Game.pak`).

| экземпляр | рисунок iPad | место (сцена 1920×1080) | нажатие |
|---|---|---|---|
| `dmVis1_mc` персонаж | шлем `btnSidePanelBg_6` (105 → 74 px) | 1828,422 | EI `dmPanel(1)` |
| `dmVis5_mc` экипировка | `cmnTabButtonBg_9` + `ico_charsheet` (доспех) | 1828,502 | `dmPanel(5)` |
| `dmVis0_mc` инвентарь | плашка + `ico_inventory` | 1828,582 | `dmPanel(0)` |
| `dmVis2_mc` навыки | плашка + `ico_skills` | 1828,662 | `dmPanel(2)` |
| `dmVis3_mc` ремесло | плашка + `ico_craft` | 1828,742 | `dmPanel(3)` |
| `dmVis4_mc` журнал | плашка + `ico_journal` | 1828,822 | `dmPanel(4)` |
| `dmVisPrev_mc` / `dmVisNext_mc` | стрелки наборов `hotBar_t`, ×0.8 | центры 351,963 и 1571,963 (слоты: 406..1516, 926..1000) | `PlaySound` + EI `prevHotbar` / `nextHotbar` (свой класс) |
| `dmVisCancel_mc` | `cancelBtn` `statusConsole_t` | центр 1650,963 | EI `dmPadBtn(1)` = B |
| `dmVisEnd_mc` | `endTurnBtn` `statusConsole_t` (с подписью) | 1310,826..1517,870 | EI `endTurn` (свой класс); видна только при `setActiveInCombat(true)` |

Колонка стоит там же, где у iPad: правый край, y 422, шаг 80 (у iPad сцена 1440 с якорем `right` → +480 по x).

ABC: `onEventInit` и `setActiveInCombat` перенаправлены на обёртки (исходные тела — `dmVisInitOrig` / `dmVisCombatOrig`), `dmVisHook` ставит
на экземпляры код действия и слушатель `mouseDown`; нажатие притеняет кнопку (alpha 0.6), отпускание сцены на той же кнопке
(`hitTestPoint`) выполняет действие. Ни одно исходное тело метода и ни один исходный тег не изменены.

Хост — `dm/host/hle_dos2_touchui_vis.c`: обработчик EI `dmPanel(id)` находит объект `panelSelect_c` (тип 0x53, vtable `main+0x29e9e98`;
создаётся для каждого игрока сборщиком набора `0x10c0a38`, живёт скрытым) и зовёт его `vt[0]` с `("openPanel", 1, {число id})` — ровно тот
вход, которым пользуется радиальное меню по A (`closeMenu` → EI `openPanel` → `0x1218580` → `0x121a140` → `0x10c0360(игрок, вид, 1, персонаж)`),
в том же потоке (слив EI). Четыре участка кода и таблица id сверяются при старте и в `tools/dos2_touchui_check.py` (`VS_*`). Мышь до
`bottomBar` уже доходит (группа `hud` маршрутизации). Пад и радиальное меню работают как раньше.

Выключатели: `DM_DOS2_TOUCHUI_DENY=3b` — штатный `bottomBar_c` (кнопок нет); `DM_DOS2_TOUCHUI_VIS=0` — кнопки колонки нарисованы, но не
действуют; пак без колонки — `dos2_touchui_pak.py hints --no-vis`.

Не проверено (нужен прогон): что экземпляры видны и стоят, где рассчитано (сцена HUD на 20:9; не закрыты ли миникартой / журналом боя);
что нажатие доходит до экземпляра без класса (`mouseDown` на MovieClip из PlaceObject2); подпись «End turn» — шрифт берётся по классу из
`fonts_en.swf`, как у текстов самого `bottomBar_c`, язык не меняется; что `openPanel` у скрытого `panelSelect_c` открывает панель
(по коду обработчик от видимости меню не зависит); что тап по кнопке не запускает ещё и «раскрыть / свернуть хотбар» (`mouseUp` сцены у
`bottomBar_c`: тап мимо слотов при активной панели шлёт `closedFromTouch`, в `main` его нет — эффекта быть не должно).

## 4. Шаги на телефоне

Пак `work/touchui/vis/TouchUI.pak` → `<romfs>/Data/TouchUI.pak`, APK этой ветки, `DM_DOS2_TOUCHUI=1`, `DM_DBG=1`.
1. Лог при старте: `[vis] visible touch elements: ExternalInterface "dmPanel"(…)`, без красных строк `[vis]`; `[touchui]` — тип 0x3b → `bottomBar_d`.
2. Загрузить сохранение. Справа видна колонка из шести кнопок (шлем, доспех, сумка, книга, молот, свиток), по бокам хотбара — стрелки,
   правее — круглая «отмена». Снять скриншот: это главный вопрос прогона (места и перекрытия).
3. Тап по «сумке»: `[route] type 0x3b … DELIVERED`, `[vis] dmPanel(0 Inventory) #1: "openPanel" to the panelSelect_c object …` — открылся
   инвентарь. Закрыть (жест «Назад»). Так же шлем → лист персонажа, доспех → экипировка, книга → навыки, молот → ремесло, свиток → журнал.
4. Стрелки у хотбара: номер ряда меняется (`prevHotbar` / `nextHotbar` в журнале EI при `DM_DOS2_TOUCHUI_EI=1`).
5. Выбрать навык, тап по круглой «отмене»: `[touch] pad button 0x2 pressed … (hint tap, BTN code 1)` — прицеливание снято.
6. Бой: появилась плашка «End turn»; тап в свой ход — ход передан. Вне боя плашки нет.
7. Пад: радиальное меню и все кнопки работают как раньше.
Если колонка мешает или что-то сломалось: `DM_DOS2_TOUCHUI_DENY=3b`.


## 5. Сделано: панели — ряд кнопок iPad в каждом `_c`-фильме панели (ветка `dos2-panels`, 15.11)

Хозяин (прогон tw2, пак 2y): колонка открывает панели, но внутри вкладки переключаются только R/L, B-закрыть / X-список / Y / шлем не нажать, а
подсказки внизу панелей идут бегущей строкой — тап по подсказке там не годится. Решение — как у iPad: видимые кнопки в самой панели.

`tools/dos2_panel_touch.py` (один инструмент, таблица `CFG`): `<панель>_c.swf` → `<панель>_d.swf` = исходный фильм + рисунки кнопок iPad
(пересадка, кадр «обычный») на корне + 4 метода в `MainTimeline` (`dmPanHook` / `dmPanDown` / `dmPanUp`, обёртка `onEventInit`; исходный —
`dmPanInitOrig`). Ряд высотой 64 px прижат к правому краю панели, стоит над её верхней кромкой (на 46 px выше, но не выше сцены; у панелей с
верхом 46 это y 0..64), шаг 8 px. Нажатие притеняет кнопку, отпускание на ней выполняет действие:

- `dmPadBtn(код)` — хост жмёт кнопку пада (`hle_dos2_touch.c`), т. е. панель и её класс движка получают ровно то событие, что от пада
  (`Eoc/controller_1.json`: `UICancel` c:b, `UITabPrev/Next` c:leftshoulder / rightshoulder, `ControllerContextMenu` c:x, `UIShowTooltip` c:y,
  `UIToggleHelmet` / `UICompareItems` / `UIToggleMultiselection` / `UIMarkWares` / `UITradeSwitchWindow` c:rightstick, `UIAddPoints` c:a,
  `UIRemovePoints` / `UITradeBalance` c:x, `Combine` c:a);
- EI своего класса — там, где пад требует удержания: торговля `confirmOffer`, награда `acceptClicked`.

Рисунки (iPad): закрыть — `uiElements_t` `mcBtnCloseTouch` (красный крест); ◄ ► — `characterSheet_t` `selectorBtnLeft/Right`; шлем —
`equipmentPanel_t` `SwitchHelmetVisibilityButton`; «список» (меню предмета) — `partyInventory_t` `sortButton`; «инфо» (лупа) — `uiCraft_t`
`searchBtn`; «выбор» (∞) — `skills_t` `filterAllBtn`; «+» / «−» — `characterSheet_t` `IncreaseAttriburePlusButton` / `uiCraft_t` `minusBtn`;
«✓» — `partyInventory_t` `okBtn`; весы — `trade_t` `balanceBtn3`.

| фильм (тип) | кнопки слева направо → действие |
|---|---|
| `partyInventory_d` (0x8e) | ◄ LB, ► RB (вкладки), инфо Y, список X (меню предмета), выбор RS (мультивыбор / сравнить), закрыть B |
| `equipmentPanel_d` (0x40) | список X, шлем RS (`UIToggleHelmet`), закрыть B |
| `statsPanel_d` (0x3f) | ◄ LB, ► RB, «−» X (`UIRemovePoints`), «+» A (`UIAddPoints`), инфо Y, закрыть B |
| `inventorySkillPanel_d` (0x3e) | ◄ LB, ► RB, «✓» A (`cursorAccept`: выучить / забыть / применить навык, использовать предмет; с `dos2-taps4`), инфо Y, список X, выбор RS (товары / мультивыбор / сравнить), закрыть B |
| `journal_d` (0x46) | ◄ LB, ► RB, «✓» A (на карте / отслеживать), инфо Y, закрыть B |
| `craftPanel_d` (0x54) | ◄ LB, ► RB, инфо Y, список X, «✓» A (`Combine`), закрыть B |
| `trade_d` (0x49) | ◄ LB, ► RB, инфо Y, окно RS (`UITradeSwitchWindow` / сравнить), весы X (`UITradeBalance`), «✓» EI `confirmOffer`, закрыть B |
| `reward_d` (0x89) | «✓» EI `acceptClicked` |
| `examine_d` (0x43) | инфо Y, закрыть B |

**Журнал — это `journal_csp` (0x8c), а не `journal_c` (0x46)** (прогон tw4, пак 2z: `[route] type 0x8c journal_csp.swf … kept away: not in the allow list`;
`journal_d` на телефоне не создаётся). Ветка `dos2-taps4`: в `CFG` панель `journal_csp` (исходник `GUI/journal_csp.swf`, ключ `src`) →
`journal_csp_d.swf`, тот же ряд, что у журнала (◄ LB, ► RB, «✓» A, инфо Y, закрыть B), якорь `journal_mc` (у `journal_csp` он на весь экран
1920×1080 — ряд стоит в правом верхнем углу сцены, x 1472…1920, y 0…64; не перекрывает ли он шапку журнала — не смотрели). Плюс **вкладки тапом**
(`wire_jtabs`, 6 методов `dmJt*`, только со сцены — слушатели элементов на телефоне не срабатывали): нажатие и отпускание ≤ 1,5 с на видимой
вкладке `journal_mc.journalSubTabList` (её `getBounds` + 10 px, живьём) = `journalSubTabList.selectMC(вкладка, false)` — ровно то, чем кончается
next / previous пада (`journalTab.selectElement` → `selectClickedSubTab(…, false, true)` → EI `selectClickedTab` + звук); на карте тап по левой /
правой боковой части `mapSubTabList` (`m_LeftSide` / `m_RightSide`) = `previous()` / `next()` (как `UITabPrev / Next` в `onEventUp`). Штатный
`CLICK` вкладки, если сработает, приходит после `mouseUp` сцены и находит вкладку уже выбранной — двойного шага нет. Слушатели сцены ставит тик
`enterFrame` из обёртки `onEventInit` и снимает себя. Хост: 0x8c в группе `panels` маршрутизации (`hle_dos2_touchui_route.c`), строка
`{0x8c, 0, "journal_csp_d", "GUI/journal_csp.swf"}` в `k_hint` (`dos2_touchui_check.py`: `type 0x8c: GUI/journal_csp.swf -> GUI/journal_csp_d.swf ok`).
Фильм крупный (5,2 МБ), пак растёт на столько же. Строки квестов / карта пальцем — не трогались (у `journal_csp` своя прокрутка
перетаскиванием, теперь мышь до неё доходит). Откат: `DM_DOS2_TOUCHUI_DENY=8c`. На телефоне не запускалось.

Контейнер / добыча (`containerInventory`, 0x09 / 0x25) не трогался: его кнопки «взять всё» / «закрыть» свои и с мышью (строка 26).

Хост: строки `k_hint` для девяти типов (`hle_dos2_touchui.c`), в маршрутизацию мыши добавлены 0x46, 0x49, 0x89, 0x43 (`hle_dos2_touchui_route.c`,
группа `panels`; остальные панели там уже были). Нового обработчика EI нет. Пак: набор `hints` строит их (`--src` — пак iPad; `--no-panels` — без них).
Проверки (`dos2_panel_touch.py check`, вызывается сборкой пака): все исходные теги байт в байт и по порядку, круговая проверка каждой
пересадки, все исходные тела методов (482..931 на фильм) байт в байт, пул констант только дополнен, переименован один метод, ни один не удалён,
ни одна строка не удалена, ABC проверяется, кнопки внутри сцены и не перекрывают друг друга.

Выключатели: `DM_DOS2_TOUCHUI_DENY=<тип>` — штатная `_c`-панель (например `8e,40,3f,3e,46,54,49,89,43` — все); `DM_DOS2_TOUCHUI_PADBTN=0` — кнопки
нарисованы, но не действуют.

Не проверено (нужен прогон): места — ряд стоит над / на верхней кромке панели, по матрицам кадра 1; не закрывает ли он заголовок (награда и
«осмотр» начинаются с y 0, там ряд лежит на шапке панели); рисуется ли то, что за рамкой панели, но внутри сцены; `balanceBtn3` — векторный, вид
не просмотрен; «✓» в ремесле и листе («+») — A жмётся хостом с задержкой 750 мс (защита «тап ≠ A»); награда — принимает ли `acceptClicked` без
`itemClicked`; `journal_csp` (0x8c, если на телефоне создаётся он, а не `journal_c`) — без кнопок.
Осталось: смена персонажа в инвентаре (правый стик ◄ ►), количество в ремесле, «хлам» в торговле, карта из журнала, свои вкладки тапом,
`journal_csp`. **P** (целиком `_t`) нигде не понадобился: пересаживается 1–7 кнопок на экран.

### Шаги на телефоне (панели)

Пак `work/touchui/panels/TouchUI.pak` → `<romfs>/Data/TouchUI.pak`, APK ветки `dos2-panels`, `DM_DOS2_TOUCHUI=1`, `DM_DBG=1`.
1. Лог при старте: `[touchui] … 0x8e -> GUI/partyInventory_d.swf, 0x40 -> …equipmentPanel_d … 0x43 -> …examine_d`, без красных строк.
2. Колонка HUD → инвентарь. Над правым верхним углом панели виден ряд: ◄ ► лупа список ∞ красный крест. Скриншот (главный вопрос — места).
3. Тап ► / ◄: `[route] type 0x8e … DELIVERED`, `[touch] pad button … (hint tap, BTN code 32 / 31)` — вкладка сменилась.
4. Выбрать предмет, тап «список»: открылось меню предмета (`BTN code 3`). Тап лупы: подсказка предмета (`BTN code 4`).
5. Тап красного креста: панель закрылась (`BTN code 1`).
6. Экипировка: тап шлема — шлем скрыт / показан (`BTN code 30`). Лист персонажа: ◄ ►, при свободных очках «+» / «−».
7. Навыки, ремесло («✓» = соединить), журнал (◄ ►, «✓»), торговля (весы = баланс, «✓» = сделка), награда («✓»), осмотр — по таблице выше.
8. Пад: всё как раньше. Если ряд мешает: `DM_DOS2_TOUCHUI_DENY=<тип>`.
