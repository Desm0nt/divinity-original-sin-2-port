# DOS2: аудит касания — каждый экран iPad (`_t`) против нашего консольного (`_c`) (15.11.2026, ветка `dos2-uiaudit`)

Вопрос хозяина: всё ли, что на iPad делается пальцем, можно вызвать пальцем в нашем консольном UI; где нельзя — придётся ли переносить
экран с iPad. Только статика (ABC фильмов + строки `main`), на телефоне ничего не запускалось.

Данные: iPad — `work/ipa/extract/ipad_gui/*_t.swf` (56 файлов из `Game.pak` IPA); наши — `work/ipa/extract/sw_gui/*.swf` (это `Game.pak`
Switch: проверено по размерам `lspk.py list`). Инструмент: `tools/dos2_ui_audit.py` (новый): слушатели событий каждого класса, привязка
тач-кнопок (`callbackFunction`, `callbackFunctionName`), до какого `ExternalInterface.call` (EI) доходит обработчик, пометка `[STUB]`
(обработчик пустой), `pair` — разница EI с числом аргументов, `dead` — EI, которых нет в `main` Switch.
`python -I tools/dos2_ui_audit.py all <dir> <out>` даёт по файлу на фильм — полные списки там, здесь выводы.

Вердикты по функции: **A** — уже нажимается в `_c` (код мыши есть и рабочий); **B** — добавляется правкой ABC `_c` (элемент на экране
есть, нет обработки мыши); **C** — добавляется хостом (EI / invoke / нажатие кнопки пада: тап по области → R/L и т. п.);
**D** — в `_c` нет, нужен фильм `_t` или пересадка элемента.

## 0. Главное

1. **Экранов с вердиктом D, из-за которых пришлось бы переносить фильм iPad, нет.** Всё, что iPad делает своими элементами и чего в `_c`
   нет как элемента (колонка кнопок справа, кнопки у миникарты, переключатель наборов хотбара, кнопка «отмена действия», фильтры журнала
   боя), у нас есть как кнопка пада или пункт радиального меню — то есть закрывается C (тап → `dmPadBtn`) или нашей шестерёнкой.
   Чисто айпадовские вещи без аналога (лупа `skillFireHelper_t`, кнопка действия над головой `overhead_t`, тап по уведомлению о новом
   навыке, «отложить квест») игре не нужны: движок Switch их и не поддерживает (EI нет в `main`).
2. **Создание персонажа (пример хозяина): стрелки в `_c` есть, они нарисованы, но не слушают мышь.** `optionSelection` /
   `appearanceOptionSelection` держат `arrowL_mc` / `arrowR_mc`, шапка — `leftBtn_mc` / `rightBtn_mc` и вкладки шагов, панель —
   `LB_mc` / `RB_mc`, строки атрибутов и способностей — `min_mc` / `plus_mc`, навыки — `check_mc`, пол — `female_mc` / `male_mc`,
   броня / шлем — `armor_mc` / `helmet_mc`. Движку оба фильма шлют **одни и те же EI с теми же аргументами** (`selectOption/3`,
   `plusAttribute/1`, `minAttribute/1`, `plusAbility/2`, `minAbility/2`, `toggleTalent/1`, `toggleTag/2`, `setInstrument/1`,
   `setGender/1`, `setArmourState/1`, `startGame/0`). Значит это **B: одна правка ABC** (как `mainMenu_d`), порт `_t` не нужен. На iPad,
   кстати, выпадающих списков в создании персонажа нет: там те же стрелки ◄ ► (`LB_mc` / `RB_mc` — `touchSimpleButton`) и списки с
   прокруткой пальцем.
3. **В половине игровых `_c`-фильмов касание уже написано Larian** (не PC-мышь, а именно тач-слой консольной версии: попадание по
   `mouseX/mouseY`, таймеры двойного нажатия и удержания, EI `openedFromTouch` / `closedFromTouch`): хотбар, инвентарь, экипировка,
   характеристики, навыки, ремесло, диалог, контейнер, портреты, контекстное меню, путевые точки, разделитель, обучение. Им нужно только
   довести мышь (фильтр `[mgr+0x170]`) и защита «тап ≠ A» — это уже есть в `TOUCH_CONSOLE_PLAN.md` (способ Ф).
4. **Совсем без кода мыши** (нужна правка B): создание персонажа, торговля, журнал/карта, награда, «обыскать область», радиальный выбор
   панели, пауза, сохранения (список), настройки (строки и стрелки селекторов), окно сообщения, осмотр, сортировка, управление отрядом,
   формация, пирамиды, сетевые экраны.
5. **Поправка к `TOUCH_CONSOLE_PLAN.md` §2:** у `msgBox_c` подсказки-кнопки *не* рабочие — слушатели `MOUSE_DOWN → onButtonDown`,
   `copyBtn::onDown`, `PasteBtn::onDown` пустые (`[STUB]`, тело 3–7 байт). То же у `mainMenu_c` (`Menu_button`, `redButton`) — это и видел
   прогон. `msgBox_d` нужна обёртка подсказок, как остальным. `saveLoad_c::PlayerTab` слушает мышь, но его EI `selectCharTab` в `main`
   Switch нет — вкладки персонажей мышью мертвы.
6. **Общий рецепт для B** (не зависит от того, есть ли у действия свой EI): тап → фильм сам выбирает элемент под пальцем тем же методом,
   что и стрелки пада (`list.selectMC(el)` / `select(i)` — движок получает обычное уведомление о выборе) → EI `dmPadBtn(A)` (или X / Y /
   LB / RB по смыслу). Это важно: часть действий `_c` движок делает сам по событию пада, а EI фильма для них нет в `main`
   (`itemSendToBarter` в торговле, `itemSelected` в «обыскать область») — вызывать такие EI из правки бесполезно.
7. **Почему порт `_t` дорогой везде, кроме меню:** класс Switch в режиме контроллера шлёт контракт `_c`. Пример — создание персонажа:
   у `_c` 14 методов документа, которых нет в `_t` (`addAbility/7`, `setBtnHints`, `setAnchor`, `setAmountOfPlayers`, `INTSetPanelPos`,
   `showTooltip` …), `_t` ждёт 10 других (`listItemUpdateMinPlusStates/4`, `setBackButtonVisible` …), `nextStep` / `previousStep` у `_t`
   с 1 аргументом против 0. Прогон TU-05 это и показал («не на весь экран, имена-заглушки, ничего не меняется»). Для игровых панелей
   (`hotBar_t`, `partyInventory_t`, `trade_t`, `uiCraft_t` …) на iPad свой класс движка `UITouch*`, которого в Switch нет: 3–7 EI на
   фильм без обработчика + 4–10 invoke, которых Switch не пошлёт (таблица в `TOUCH_UI_PLAN.md` §1).

## 1. Сводная таблица (все 56 `_t`)

«Мышь у нас»: **полн.** — рабочий тач-код на основных элементах; **част.** — на части; **нет**; **загл.** — слушатели есть, но пустые.
Часы — агентские, на правку и статическую проверку; у каждого экрана ещё прогон на телефоне (ведёт главный агент).

| # | iPad `_t` | наш фильм (тип Switch) | мышь у нас | вердикт по функциям | что делать | ч |
|---|---|---|---|---|---|---|
| 1 | characterCreation | characterCreation_c (0x4) | почти нет (вращение куклы) | B всё; C — шаги LB/RB, «Старт», «Назад» (подсказки) | `_c` + правка | 3.5 |
| 2 | hotBar | bottomBar_c (0x3b) | полн. | A слоты/перетаскивание/раскрыть; C — ряды хотбара, конец хода, отмена | `_c` + Ф | 1 |
| 3 | playerInfo | playerInfo (0x3d, общий с PC) | полн. | A всё | `_c` + Ф | 0.3 |
| 4 | minimap | minimap_c (0x3c) | нет | B тап = карта; C — кнопки (пинг, такт. камера, быстрое сохр. …) | `_c` + 1 слушатель + шестерёнка | 0.5 |
| 5 | statusConsole | statusConsole (0x75) / подсказки `bottomBar_c` | полн. (бежать / ждать) | A, если фильм создаётся в режиме пада (не проверено); иначе C | `_c` | 0.3 |
| 6 | rightSideButtons | panelSelect_c (0x53, радиальное) | нет | B сектор → `openPanel/1`; либо C — кнопки шестерёнки | `_c` + правка | 1 |
| 7 | skillFireHelper | — | — | D, но не нужен (лупа; EI `actionPressed` нет в `main`) | не делать | 0 |
| 8 | partyInventory | partyInventory_c (0x8e) | полн. | A тап/двойной/удержание/перетаскивание/вкладки; C — сортировка, «товары», сравнение | `_c` + Ф | 1 |
| 9 | equipmentPanel | equipmentPanel_c (0x40) | полн. | A кукла и сетка; C — шлем, сравнение | `_c` + Ф | 0.5 |
| 10 | characterSheet | statsPanel_c (0x3f) | полн. | A строки, «+ / −», подвкладки; C — главные вкладки LB/RB | `_c` + Ф | 0.5 |
| 11 | skills | inventorySkillPanel_c (0x3e) | част. | A навыки (тап/двойной), вкладки; B — сетка предметов этой панели | `_c` + Ф + правка | 1 |
| 12 | trade | trade_c (0x49) | нет | B сетки, предложения, фильтры; C — баланс, подтвердить, «отдать хлам» | `_c` + правка | 2.5 |
| 13 | containerInventory | containerInventory (0x9/0x25, общий с PC) | полн. | A всё (ячейки, «взять всё», закрыть) | как есть + Ф | 0.3 |
| 14 | reward | reward_c (0x89) | нет | B выбор награды; C — принять (удержание A) | `_c` + правка | 0.7 |
| 15 | areaInteract | areaInteract_c (0x44) | нет | B строка списка + A; C — контекст | `_c` + правка | 0.7 |
| 16 | dialog | dialog_c (0x42, 0x65) | полн., но по-своему | A: провести = выбор / прокрутка, отпустить = выполнить; B — тап прямо по ответу | `_c` + Ф + правка | 1 |
| 17 | contextMenu | contextMenu_c (0xc, 0x60) | полн. | A всё | `_c` + Ф | 0.2 |
| 18 | journal_csp | journal_c (0x46) | нет | B вкладки, списки; C — карта (перетаскивание, зум, метки) | `_c` + правка (или опыт с `journal_csp`) | 3 |
| 19 | journal | — | — | на iPad не регистрируется (мёртвый файл) | — | 0 |
| 20 | waypoints | waypoints_c (0x4e) | полн. | A тап = перейти (`clickedWaypoint`) | `_c` + Ф | 0.2 |
| 21 | uiCraft | craftPanel_c (0x54) | част. | A вкладки, ячейки опыта, руны, прокрутка рецептов; B — выбор рецепта; C — «Соединить» | `_c` + Ф + правка | 1.5 |
| 22 | examine | examine_c (0x43) | нет (только чтение) | C закрыть / прокрутка | `_c` | 0.2 |
| 23 | tooltip | tooltip (0x2c) | — (показ) | C: короткое долгое нажатие → `UIShowTooltip` | хост | 0.5 |
| 24 | worldTooltip | worldTooltip (0x30, общий) | полн. | A тап по подписи предмета (`tooltipClicked`) | как есть + Ф | 0.1 |
| 25 | itemSplitter | itemSplitter_c (0x55) | полн. | A ползунок, принять / отмена | `_c` + Ф | 0.2 |
| 26 | gameMenu | gameMenu_c (0x4d) | нет | B (тот же `Menu_button` / список, что в `mainMenu_d`) | `_c` + правка | 1 |
| 27 | optionsSettings | optionsSettings_c (0x58–0x5b) | част. (ползунок, полоса) | B строка, стрелки `left_mc` / `right_mc`; C — вкладки, «применить» | `_c` + правка | 1.5 |
| 28 | optionsInput | buttonLayoutNX_c (0x5f, картинка) | — | нет функций (список жестов iPad) | — | 0 |
| 29 | saveLoad | saveLoad_c (0x4a) | вкладки (EI мёртв) | B запись списка; C — загрузить / удалить (подсказки) | `_c` + правка | 1 |
| 30 | msgBox | msgBox_c (0x4b, 0x4c) | загл. | B опции; C — A/B/X/Y по подсказкам; A — поле ввода | `_c` + правка | 0.7 |
| 31 | mainMenu | mainMenu_c (0x48, 0x57) | загл. | сделано (`mainMenu_d`) | — | 0 |
| 32 | tutorialBox | tutorialBox_c (0x5e) | полн. | A тап = закрыть / «не показывать» | `_c` + Ф | 0.2 |
| 33 | characterAssign | characterAssign_c (0x5c) | нет | C / B (сетевая игра) | позже | 0.7 |
| 34 | connectionMenu | connectivity_c (0x22) | част. (флажок) | C / B (сетевая игра) | позже | 0.5 |
| 35 | serverlist | serverlist_c (0x19, 0x1b) | част. (запись, обновить) | C / B (лобби) | позже | 1 |
| 36 | formation | formation_c (0x87) | нет | C; перетаскивание по точкам — B | позже | 0.5 |
| 37 | pyramid | pyramid_c (0x86) | нет | B строка + A | `_c` + правка | 0.3 |
| 38 | book | book (0x2, тот же фильм) | полн. | A углы страниц, закрыть | как есть + Ф | 0.1 |
| 39 | arenaResult | arenaResult (0x7d, PC) | полн. | A кнопки (арена) | как есть | 0.1 |
| 40 | chatLog | chatLog (0x6, PC; в режиме пада не создаётся) | — | чат на Switch не нужен | — | 0 |
| 41 | combatLog | combatLog_c (0x41) | нет элементов | фильтры iPad — не нужны | — | 0 |
| 42 | combatTurn | combatTurn (0x8, тот же) | полн. | A тап по портрету = камера на персонажа | как есть + Ф | 0.1 |
| 43 | giftBagsMenu | giftBagsMenu (0x92) | загл. | B / C (пад) | позже | 0.3 |
| 44 | giftBagsContent | giftBagContent (0x93) | загл. | B / C (пад) | позже | 0.3 |
| 45 | installScreen | installScreen_c (0x50) | — | ввода нет | — | 0 |
| 46 | loadingScreen | loadingScreen (0x17) | — | ввода нет | — | 0 |
| 47 | notification | notification (0x24) | — | тап по «новый навык» — нет в `main`, не нужен | — | 0 |
| 48 | overhead | overhead (0x5) | — | кнопка действия над головой — нет в `main`, не нужна | — | 0 |
| 49 | startTurnRequest | startTurnRequest_c (0x90) | нет | C: удержание пальца = удержание A | хост | 0.2 |
| 50 | textDisplay | textDisplay (0x2b) | — | ввода нет | — | 0 |
| 51 | subtitles | subtitles (0x20) | — | ввода нет | — | 0 |
| 52 | mouseIcon | mouseIcon (0x1f) | — | ввода нет | — | 0 |
| 53 | userProfile | userProfile (0x33, PC) | полн. | профили — у Switch системные учётки | — | 0 |
| 54–56 | LSClasses, touchLibrary, uiElements | — | — | библиотеки, не экраны | — | 0 |

Наши экраны без пары на iPad: `sortBy_c` (0x4f), `partyManagement_c` (0x52), `mods_c` (0x67: вкладки — A, остальное пад) — кода мыши нет,
B «строка + A» по общему рецепту, ≈1.5 ч на все.

**Итог: порт с iPad не нужен ни для одного экрана.** Объём: игровой цикл одного игрока без пада ≈ 23 ч агента (создание персонажа 3.5,
группа «уже есть тач» 5, пауза / сообщение / сохранения / настройки 4.2, диалог 1, торговля 2.5, журнал 3, награда и «обыскать» 1.4,
радиальное 1, общий помощник правки и жесты хоста 3.5, остаток — мелочи); всё вместе с сетевыми и редкими экранами ≈ 33 ч. Телефон: 8–10
прогонов (по группе экранов на прогон) — это и есть настоящий ограничитель.

## 2. Создание персонажа — `characterCreation_t` против `characterCreation_c`

**iPad** (`events`: Tap, ShortLongPress, Touch2Fingers, CameraZoom, HideVirtualKeyboard):

| функция | элемент и жест iPad | EI |
|---|---|---|
| происхождение / раса / внешность (каждый селектор) | `optionSelection_23`: кнопки `LB_mc` / `RB_mc` (`touchSimpleButton`, Tap) | `selectOption/3` |
| пол | `femaleBtn_mc` / `maleBtn_mc` | `setGender/1` |
| показ брони / шлема | `armourBtn_mc` / `helmetBtn_mc` | `setArmourState/1` |
| шаг вперёд / назад | кнопки панели (`nextPanel` / `prevPanel`) | `nextStep/1`, `previousStep/1` |
| прыжок на вкладку шага | вкладка в шапке | `setPanel/1` |
| класс: правка | `editBtn_mc` | `editClass/1` |
| атрибуты, способности | кнопки `+` / `−` в строке | `plusAttribute/1`, `minAttribute/1`, `plusAbility/2`, `minAbility/2` |
| навыки | перетаскивание в слот, двойной тап | `startDragging/2`, `dragactivate/1`, `cancelDragging/0`, `useSkill/2` |
| таланты | тап по строке | `toggleTalent/1` |
| теги | двойной тап, `+` / `−` | `toggleTag/2` |
| инструмент | тап | `setInstrument/1` |
| имя | поле ввода + экранная клавиатура | `handleTextInput/2` |
| вращение / приближение куклы | перетаскивание по экрану, щипок | `rotateCharacter/0`, `zoomOut` |
| голос происхождения | `voiceOriginBtn_mc` | `playOriginStory/0`, `stopOriginStory/0` |
| старт, назад в меню | `start_mc`, `back_mc` | `startGame/0`, `mainMenu/0` |
| пригласить игрока | пустой портрет | `openConnectivity/0` |
| подсказки | короткое долгое нажатие по строке | `show*Tooltip/6–7` |

**Наш `_c`** (`events`: UIAccept, UIBack, UICancel, UICreationTabNext/Prev, UIUp/Down/Left/Right, UIEditCharacter, UIShowInfo, UIStartGame,
UIToggleEquipment, UITooltipUp/Down). Вся работа — в `delegateDown(событие)` панелей; мышь слушает только сцена: `MOUSE_DOWN` →
`rotateCharacter`, `DOUBLE_CLICK` → `toggleZoom` (этого EI нет в `main`: мёртв), поле имени — двойной клик.

| функция | чем в `_c` | EI | элемент на экране | вердикт |
|---|---|---|---|---|
| селекторы | ←/→ на выбранной строке (`selector::delegateDown`) | `selectOption/3` — тот же | `arrowL_mc`, `arrowR_mc`, строка | **B**: тап по строке = выбрать, по половине / стрелке = `previousEl` / `nextEl` + тот же EI |
| пол | `UIShowInfo` | `setGender/1` — тот же | `female_mc`, `male_mc` | **B** (или C: подсказка) |
| броня / шлем | `UIToggleEquipment` | `setArmourState/1` — тот же | `armor_mc`, `helmet_mc` | **B** / C |
| шаги | LB / RB | `nextStep/0`, `previousStep/0` | `leftBtn_mc`, `rightBtn_mc`, вкладки | **C** (тап по подсказке → `dmPadBtn`, уже есть в `characterCreation_d`); тап по вкладке — B (N раз `nextStep`) |
| пресет класса | ←/→ | `nextPreset/0`, `prevPreset/0` | `classSelection_mc` | **B** |
| правка класса, её вкладки | A; LB / RB панели | `canDoClassEdit/1`, `editClass/1`, `nextClassEdit/1`, `prevClassEdit/1` | `classEditTab`, `LB_mc`, `RB_mc` | **B** |
| атрибуты, способности | ↑/↓ выбор, ←/→ | те же четыре EI | `min_mc`, `plus_mc` | **B** (образец есть: так сделан `statsPanel_c`, `fn#313`) |
| навыки | A на строке | `toggleSkill/1` (вместо перетаскивания) | `check_mc`, строка | **B** |
| таланты, теги | A | `toggleTalent/1`, `toggleTag/2` — те же | строка | **B** |
| инструмент | A | `setInstrument/1` — тот же | иконка | **B** |
| имя | A на поле → системная клавиатура | `editName/1`, `handleTextInput/2` | `inputField` | **B** (тап = то же, что A) |
| вращение куклы | — | `rotateCharacter/0` | — | **A** (слушатель сцены уже есть) |
| приближение | — | `toggleZoom` мёртв | — | **C** (кнопка пада, если нужна) |
| голос, старт, назад | кнопки пада | `playOriginStory`, `startGame`, `mainMenu` | подсказки | **C** (подсказки) |
| подсказки по строке | выбор строки | `request*Tooltip/1–2` | — | приходит само при выборе строки тапом |

Рекомендация: **оставить `_c`, правка ABC** (`characterCreation_d`, поверх уже готовой обёртки подсказок): один слушатель `MOUSE_UP` сцены,
попадание по элементам текущей панели (`CCPanel_mc` → панель шага → `contentHolder_mc`), ~10 видов элементов, действия — штатные методы
(`previousEl` / `nextEl`, `delegateDown("IE UILeft")` на выбранной строке и т. п.). 3.5 ч. Порт `_t`: контракт расходится (§0 п. 7),
прогон уже показал поломку — не делать.

### 2.1. Сделано: `characterCreation_d` (ветка `dos2-cc`, `tools/dos2_cc_touch.py`) — только статика, на телефоне не запускалось

`characterCreation_d.swf` = `characterCreation_c.swf` (Switch `Game.pak`) + два слоя в ABC, вид консольный байт в байт (меняется только DoABC):
слой подсказок (`tools/dos2_hint_touch.py`, как раньше) и слой тача — 15 методов в `characterCreation_c_fla.MainTimeline`. Идёт в набор `hints`
(`tools/dos2_touchui_pak.py hints`, он по умолчанию с `dos2-tc2`), подменяется для типа 0x04 той же строкой `k_hint`, что и прежний фильм
«только подсказки»; тип и ветка класса остаются консольными (`_c`). Правок хоста нет.

Принцип: нажатие ищет элемент под пальцем по геометрии, переносит на него штатное выделение (`listDisplay.selectMC` — то же, чем кончаются
↑/↓ пада) и притеняет нажатое (alpha 0.6); отпускание без сдвига (30 px) запускает **сам путь пада**: исходные `onEventDown(i)` + `onEventUp(i)`
с индексом того события, которое послал бы пад. Своих вызовов EI у тача нет — каждое действие = код консольного фильма и его же EI.

| что нажать | событие пада | итог (EI исходного фильма) |
|---|---|---|
| `leftBtn_mc` / `rightBtn_mc` в шапке; вкладка шага левее / правее текущей | `UICreationTabPrev` / `Next` | `previousStep` / `nextStep` (вкладка — один шаг в её сторону) |
| поле имени | `UIEditCharacter` | `inputField.selectElement`, `editName(true)` (системная клавиатура, как от пада); нажатие мимо поля при вводе = `deselectElement` → `handleTextInput/2` |
| строка селектора (происхождение, раса, внешность): левая часть / `arrowL_mc`, правая / `arrowR_mc`; середина — только выбор строки | `UILeft` / `UIRight` | `selectOption/3` |
| `female_mc` / `male_mc` (неактивный) | `UIToggleEquipment` | `setGender/1` |
| `armor_mc` / `helmet_mc` | `UIToggleEquipment` | `toggleState` → `setArmourState/1` (консольный цикл из трёх состояний) |
| название класса: слева / справа от него | `UILeft` / `UIRight` | `prevPreset` / `nextPreset` (сначала выход из списка правок, как ↑ пада: `canDoClassEdit(false)`) |
| строка правки класса: первый тап — выбор, тап по выбранной — вход | `UIAccept` | `canDoClassEdit(true)`, `editClass/1` |
| панели правки: `LB_mc` / `RB_mc`, вкладка левее / правее текущей | `UICreationTabPrev` / `Next` | `prevClassEdit/1` / `nextClassEdit/1` |
| атрибуты: `plus_mc` / `min_mc` строки (тап по строке — выбор) | `UIRight` / `UILeft` | `plusAttribute/1` / `minAttribute/1` |
| способности (боевые, мирные): `plus_mc` / `min_mc` | `UIRight` / `UILeft` | `plusAbility/2` / `minAbility/2` |
| навык (строка) | `UIAccept` | `toggleSkill/1` |
| талант / тег (строка основного списка; расовые — только выбор) | `UIAccept` | `toggleTalent/1` / `toggleTag/2` |
| инструмент | `UIAccept` | `setInstrument/1` |
| старт, назад, лор, подсказка, голос | подсказки кнопок внизу | `dmPadBtn` → кнопка пада (слой подсказок) |
| кукла | — | штатный слушатель сцены → `rotateCharacter` (не тронут) |

Списки (способности, навыки, таланты, теги) листаются перетаскиванием: вертикальный сдвиг от 14 px, начатый на панели, двигает
`scrollList.m_scrollbar_mc.scrollTo`; перетаскивание не считается тапом. Ползунок полосы прокрутки и так имеет код мыши.

Двойное срабатывание: `onEventDown` / `onEventUp` обёрнуты второй раз — `IE UIAccept` в пределах 700 мс от события кнопки мыши (и от
движения, когда кнопка уже была) глотается; когда A всё же прошёл, его время запоминается и отпускание в пределах 700 мс после него ничего
не делает. Тапы зовут исходные тела (`tuEvDownOrig` / `tuEvUpOrig`), поэтому ни эта защита, ни защита слоя подсказок (400 мс), ни защита хоста
(`hle_dos2_touchui_route.c`: `UIAccept` в 600 мс после тапа без нажатой A) их не съедают — `UIAccept` от движка тачу не нужен. A от подсказки
хост жмёт через 750 мс — проходит. Путь пада байт в байт: ни одно исходное тело метода не изменено.

Проверка (`python -I tools/dos2_cc_touch.py check <Switch Game.pak> [characterCreation_d.swf]`): все теги кроме DoABC и все 931 исходное тело
идентичны, пул констант только дополнен, верификатор стека / областей по 954 телам, контракт `_c` → `_d` без потерь (124 метода / слота корня,
39 имён EI из тех же тел; добавлен только `dmPadBtn`), каждое имя, которое читает тач, есть в исходном фильме, каждое действие тапа — EI
исходного фильма. `tools/dos2_abc.py roundtrip` / `verify`, `tools/dos2_swf_abc.py diff` — чисто.

Что не проверено и может потребовать правки после прогона: геометрия (точки привязки стрелок / кнопок неизвестны, зоны взяты с запасом
30–70 px), приход `mouseMove` при зажатой кнопке (без него тапы работают, перетаскивание списка — нет), вкладки шага прыгают на один шаг, а
не на выбранный (EI `setPanel` в `main` Switch не искали).

Шаги на телефоне (пак `hints` + APK этой ветки, `DM_DOS2_TOUCHUI=1`, без пада):
1. Главное меню → «Новая игра» → экран создания. В логе (`DM_DBG=1`) первое чтение `characterCreation_d.swf`.
2. Происхождение: тап по правой / левой стрелке первой строки — меняется персонаж; тап по значку пола — пол; тап по середине другой строки —
   только подсветка.
3. Тап по правой кнопке шапки (RB) — следующий шаг; по левой — назад. Внешность: стрелки строк; значок брони / шлема — цикл.
4. Класс: стрелки у названия — пресет; тап по строке правки дважды — вход в правку; там «+» / «−» атрибутов и способностей, строки навыков,
   `LB` / `RB` панели; протянуть список способностей / навыков пальцем.
5. Таланты, теги: тап по строке ставит / снимает; инструмент — тап.
6. Имя: тап по имени в шапке — клавиатура; ввести, тап мимо поля — имя принято.
7. Подсказка «Старт» внизу — игра стартует. По дороге смотреть: ни один тап не срабатывает дважды и не жмёт «A» на выбранной строке.
Если тач мешает: `DM_DOS2_TOUCHUI_DENY=4` — штатный `characterCreation_c` (только пад, без тач-подсказок), остальное как было.

## 3. HUD

### Хотбар — `hotBar_t` против `bottomBar_c`
| функция | iPad | наш `_c` | вердикт |
|---|---|---|---|
| использовать слот | Tap по `action` → `useAction/1` | `slotsHolder` `CLICK` → `selectSlot` + `SlotPressed/2`; пад: ←/→ + A | **A** |
| перетащить слот | LongPress → `startDraggingAction`, `slotUpEnd` | таймер удержания → `startDragging`; отпустить → `stopDragging` | **A** |
| подсказка навыка | ShortLongPress → `showSkillTooltip/8` | `SlotHover` при выборе; пад `UITooltipUp/Down` | **A** / C |
| раскрыть / свернуть панель | — (всегда видна) | `MOUSE_UP` сцены → `openedFromTouch` / `closedFromTouch` (в `main` нет, сама панель раскрывается) | **A** |
| ряды хотбара | переключатель наборов `deckSwitcher` → `selectSkillSet/1`, замок `setHotbarLocked` | пад `UIHotBarNext/Prev` → `nextHotbar` / `prevHotbar` | **C** (тап по подсказке / свайп по панели → `dmPadBtn`) |
| кнопки панелей (инвентарь, навыки …) | `hotBarButton` → `hotbarBtnPressed/1` | радиальное `panelSelect_c` | C / B (см. ниже) |
| конец хода | `statusConsole_t` | `UIEndTurn` (удержание) → `endTurn/0` | **C** |
| отмена действия | `cancelBtn_mc` → `cancelActiveAction` | B пада → `cancelPressed/0` | **C** (жест «Назад» уже = B) |
| бросить предмет на землю | область `dropItemArea` → `dropItem` | контекстное меню предмета | есть другим путём |
| чат, журнал боя | кнопки | нет (на Switch не нужны) | — |

Оставить `_c`: открыть фильтр + защита «тап ≠ A» (`bottomBar_d`), 1 ч. `hotBar_t` — свой класс `UITouchBottomBar`, 5 EI без обработчика в
Switch (`cancelActiveAction`, `didToggleDeckSwitcher`, `dropItem`, `selectSkillSet`, `slotUpEnd`), 5 invoke только iPad.

### Портреты — `playerInfo_t` против `playerInfo`
Фильмы почти одинаковые (общий с PC). Нажатие → `charSel`, двойное → `centerCamOnCharacter`, перетаскивание → `piAddToGroupUnder` /
`piDetachUnder` … (связать / развязать), правый клик → `openCharInventory`, наведение на статус → подсказка. Всё **A**. У iPad отличие одно:
подсказка по нажатию (`showCharTooltip` на `MOUSE_DOWN`), у нас — по наведению (придёт при касании). 0.3 ч.

### Миникарта — `minimap_t` против `minimap_c`
iPad: тап по карте → `openMap` (нет в `main` Switch) / `minimapPing/2`; кнопки `pingButton`, `homestead`, `tacticalCamera`, `lockCamera`,
`quickSaveBtn`, `worldTooltips`, `enemySight`, `activeSearch`, `diplomacy` → `buttonPressed/2` и др.; щипок — зум. У нас `minimap_c` — только
показ, ни одного слушателя, кнопок нет. **B**: один слушатель «тап по `map_mc` → `dmPadBtn(карта)`»; остальное — **C**: это кнопки пада
(тактическая камера, подсветка предметов, быстрое сохранение — в меню паузы), выносятся в шестерёнку. 0.5 ч. Переносить `minimap_t`
незачем: свой класс `UITouchMinimap`, 5 invoke только iPad.

### Конец хода / бежать / ждать — `statusConsole_t` против `statusConsole`
iPad: кнопки с прямыми EI `EndButtonPressed`, `FleePressed`, `GuardPressed`, `ConfirmActionPressed`, `cancelActiveAction`. Наш
`statusConsole.swf` (PC, тип 0x75): `fleeButton` / `waitButton` слушают мышь (`FleePressed`, `GuardPressed`) — **A**, если этот фильм
создаётся в режиме пада (в списке сборщика набора игрока `0x10c06d0` типа 0x75 нет — проверить прогоном по журналу EI). Иначе **C**:
конец хода и «ждать» — кнопки пада, подсказка `endTurnHint_mc` в `bottomBar_c` → обёртка подсказок.

### Колонка кнопок — `rightSideButtons_t` против `panelSelect_c`
iPad: постоянная колонка (инвентарь, персонаж, навыки, журнал, карта, ремесло, меню) → `rightBarBtnClicked/1`, `toggleSidePanel` — обоих EI
в `main` Switch нет, класса `UIRightSideTouchButtons` тоже. У нас — радиальное меню по кнопке пада, выбор стиком, A → `openPanel/1`
(обработчик есть). **B**: сектор под пальцем → выбрать + `openPanel`; открывается оно кнопкой шестерёнки (**C**). 1 ч.

### Лупа — `skillFireHelper_t`
Увеличение под пальцем при наведении навыка, EI `actionPressed` (нет в `main`). Аналога нет и не нужен.

## 4. Инвентарь, экипировка, персонаж, навыки

### `partyInventory_t` против `partyInventory_c`
| функция | iPad (`touchInventoryClass`) | наш (`LS_Classes.larInventory`) | вердикт |
|---|---|---|---|
| выбрать предмет | Tap → `clickItem/2`, `slotOver` | `mouseDown` → `selectItem` (+ `slotOver/3`) | **A** |
| использовать / надеть | DoubleTap → `useItem/1` | `mouseClick` дважды → `onDoubleClickCallback` → `cellInteraction/1` | **A** |
| контекстное меню | ShortLongPress → `openContextMenu/3–4` | таймер удержания → `showActionMenuItem/5` | **A** |
| перетаскивание | `slotDown`, `startDragging`, `stopDragging`, на портрет — `stopDragOnChar` | `mouseMove` → `startDragging/2`, `mouseUp` → `stopDragging/6` | **A** |
| вкладки | `Tab` | `tabBarMCInv` `CLICK` → `selectTab` | **A** |
| прокрутка | 2 пальца / палец (`touchVerticalScroller`) | полоса прокрутки мышью, колесо | **A** / C (два пальца = колесо, уже есть) |
| сортировка | `sort_mc` → `openCombineSorter` | кнопка пада → `sortBy_c` | **C** |
| «товары» | переключатель → `switchWaresModeEnabled`, `switchItemWaresState` | `UIMarkWares`, `UIToggleMultiselection` | **C** |
| сравнить, другой персонаж | — / портреты | `UICompareItems`, `UISwitchLeft/Right` | **C** |
| кукла вкл / выкл | `switchDollVisibility` | отдельная панель экипировки | — |

Оставить `_c` (Ф). `partyInventory_t`: класс `UITouchPartyInventory`, 7 EI без обработчика, 4 invoke только iPad.

### `equipmentPanel_t` против `equipmentPanel_c`
Кукла: iPad Tap → `slotDollOver` / подсказка, DoubleTap → `selectDollSlot`, LongPress → `showDollActionMenuItem`; у нас `equipDoll`
`CLICK` → `selectDollSlot` (+ `slotDollOver`), удержание → `showDollActionMenuItem`, `MOUSE_DOWN` мимо слота → `rotateCharacter`. Сетка
предметов для слота: iPad Tap / DoubleTap → `selectItem`; у нас `EqInv` `CLICK` → `selectItem`, удержание → `showActionMenuItem/4`. Все EI
общие (16 совпадают). Шлем: iPad `setHelmetOption/1`, у нас `UIToggleHelmet` → `toggleHelmetVisibility` — **C**. Остальное **A**.

### `characterSheet_t` против `statsPanel_c`
iPad: строки с `plus_mc` / `minus_mc` (прямые EI `plusStat`, `plusAbility`, `plusTalent`, `minus…`), вкладки `StatsTabButton` →
`selectedTab`, портреты отряда → `selectCharacter`, выпадающие списки (`larCombo`), переименование, перетаскивание навыка. У нас: `CLICK`
по строке атрибута / способности / таланта = выбрать (`selectStat`, `selectAbility`, `selectTalent`), попадание в `plus_mc` / `min_mc` =
`addPoint` / `removePoint` (EI `addPoints…` / `removePoints…` — в `main` есть), заголовок подвкладок — тап слева / справа =
`previousSubTab` / `nextSubTab`, перетаскивание = прокрутка. Всё **A**; главные вкладки (характеристики / экипировка / навыки —
`UITabPrev/Next`) — **C**. EI у фильмов разные (4 общих из 24 / 15): перенос `characterSheet_t` = чужой класс `UITouchCharactersheet`.

### `skills_t` против `inventorySkillPanel_c`
iPad: список навыков + «память»: перетаскивание в ячейки памяти (`startDragging`, `dragActivate`), фильтры школ (`exclusiveFilter`,
`showAllFilters`), сброс (`resetMemory`), забыть. У нас: `skillMiddle` `CLICK` → `selectSkill/1`, двойной → `useSkill/1` (выучить /
забыть / на хотбар — как A пада), перетаскивание → `startDragging` / `stopDragging/6`, вкладки `CLICK` — **A**. Половина «предметы» этой
панели (`inventoryMiddle`) слушателей мыши не имеет — **B** по образцу `partyInventory_c` (класс `larInventory` тот же, не подключены
три слушателя). Фильтры школ — вкладки. 1 ч.

## 5. Торговля, контейнер, награда, «обыскать область»

### `trade_t` против `trade_c`
iPad (`touchItemInventoryInteractor`): Tap — выбрать, DoubleTap → `itemClick/2` (в сделку / обратно), перетаскивание между сетками,
ShortLongPress — контекст (`openContextWindow`), портреты → `selectCharacter`, кнопки `clearOffers`, `repairClicked`, `identifyClicked`,
баланс. У нас в `trade_c` **ни одного слушателя мыши**: две сетки + предложения, стик + A (`itemClick/3`; `itemSendToBarter` в `main` нет —
действие делает движок по A), LB/RB — фильтры (`changedItemFilter`), `UITradeSwitchWindow`, `UITradeBalance` → `balance/0`, удержание A →
`confirmOffer/0`, удержание → `offerJunk/0`, X — контекст (`showActionMenuItem/3`: там ремонт и опознание). Элементы на экране есть
(`playerInventory_mc`, `traderInventory_mc`, `playerOffer_mc`, `traderOffer_mc`, `LB_mc`, `RB_mc`, `balanceButtonContainer_mc`,
`holdToAcceptHint_mc`). **B**: тап по ячейке = перевести выбор движка на неё (тот же метод, что стик) + `dmPadBtn(A)` на втором тапе;
**C**: баланс / подтвердить / хлам / фильтры — подсказки. 2.5 ч. `trade_t`: класс `UITouchTrade`, 5 EI без обработчика, 6 invoke только
iPad — не переносить.

### `containerInventory_t` против `containerInventory`
Наш фильм — общий с PC и полностью мышиный: ячейки (`slotDown` / `slotUp`, двойной → `doubleClickItem` = взять), `takeAllButton` →
`takeAll`, `CloseButton`, перетаскивание окна и предметов. **A** целиком.

### `reward_t` против `reward_c`
iPad: Tap / DoubleTap по награде → `itemClicked`, кнопка `acceptBtn` → `acceptClicked`. У нас: сетка стиком, удержание A — принять
(`holdAcceptBtn_mc`); слушателей нет. **B** (тап = выбрать) + **C** (удержание пальца на `holdAcceptBtn_mc` = удержание A). 0.7 ч.

### `areaInteract_t` против `areaInteract_c`
iPad: Tap по строке → `itemSelected` (в `main` Switch нет), LongPress → `showContext`, кнопка закрытия. У нас: список, ↑/↓, A, X —
контекст; EI те же (`itemOver`, `itemOut`, `showContext`, `closeUI`), слушателей нет. **B**: тап по строке = `select` + `dmPadBtn(A)`,
удержание = X. 0.7 ч. `areaInteract_t` шёл бы нашим `_c`-классом (24 новых метода документа) — не стоит.

## 6. Диалог, контекстное меню

### `dialog_t` против `dialog_c`
iPad: `CLICK` по ответу → `selectedId` (+ `QuestionPressed`), двойной — сразу подтвердить, 2 пальца — прокрутка, кнопки `trade_mc` →
`TradeButtonPressed`, `stopListen_mc` → `StopListening`. У нас `Dialog_1`: `MOUSE_MOVE` — провести по тексту = прокрутка
(`startScrollText`), провести по ответам = сдвиг выбора (`moveSelection` → `selectedId`); `MOUSE_UP` = `executeSelected`
(`QuestionPressed`). То есть работает (**A**), но «отпустил палец = выполнить выбранный ответ», а не «тап по нужному ответу». **B**
(1 ч): в `MOUSE_UP` без движения — попадание по строкам ответов, выбрать именно её, затем `executeSelected`. Торговля из диалога и
«перестать слушать» — кнопки пада (**C**, подсказки).

### `contextMenu_t` против `contextMenu_c`
Одинаково: `WEntry` `MOUSE_DOWN` / `MOUSE_UP` → `buttonPressed`, клик мимо → `menuClosed`. **A**. У iPad сверх того — вытащить предмет
из меню перетаскиванием (`closeAndDragItem`, в `main` нет) — не нужно.

## 7. Журнал, карта, путевые точки

### `journal_csp_t` против `journal_c`
iPad: вкладки (`JournalTab2`), строки квестов / диалогов / обучения (`hit_mc` → `select`), кнопки «на карте» (`showQuestOnMap`),
«отложить» (`postponeQuest`), фильтр отложенных; карта — перетаскивание (`mapOffset/2`), щипок, тап по метке (`useWaypoint`,
`trackIcon`), LongPress — поставить флаг (`addFlag`) / меню метки (`showContextMenu/8`), легенда. У нас `journal_c`: слушателей нет;
вкладки LB/RB (`selectClickedTab` — есть и у нас), списки ↑/↓ (`questOpened`, `dialogSelected`), A — отслеживать
(`toggleQuestTracking`), Y — на карте; карта — курсор стиком (`onMapMoveFrame` → `selectedMarker` / `selectedWaypointMarker` /
`selectedCustomMarker`), A на метке (`useWaypoint`, `addFlag`), `UIMapRemoveMarker` → `removeFlag/2`. 15 EI общих.
**B**: вкладки и строки списков тапом; **C**: карта — палец = правый / левый стик (курсор и сдвиг), щипок = зум, тап = A в точке.
3 ч. Дешёвый опыт: в `Game.pak` Switch лежит `journal_csp.swf` (тип 0x8c, **тот же класс**, что `journal_c`) — в нём уже есть `CLICK` по
вкладке (`selectTab`) и прокрутка списков перетаскиванием; контракт отличается на 3 + 1 метода (`addDialog`, `hideCalled`,
`setJournalTitle` / `addQuestInfo`). Подмена пути 0x46 → `journal_csp.swf` (механизм TU-02) может дать вкладки и прокрутку даром — один
прогон. «Отложить квест» и фильтр — только iPad, не нужны. `journal_t.swf` на iPad не используется.

### `waypoints_t` против `waypoints_c`
iPad: Tap — выбрать, DoubleTap / кнопка «Путь» → `clickedWaypoint`. У нас `Waypoint` `MOUSE_DOWN` / `MOUSE_UP` → `clickedWaypoint`
(сразу перейти). **A**.

## 8. Ремесло — `uiCraft_t` против `craftPanel_c`
iPad: рецепты (`Recipe` → `selectRecipe`), категории, количество `+` / `−`, «Создать» (`craftRecipe/2`), опыты — перетаскивание
ингредиентов в ячейки (`startDraggingFromSlot`, `stopDraggingIngredient`), двойной тап — убрать (`clearSlot`), «Соединить» (`combine`),
руны (перетаскивание, `extractRuneFromSlot`), сортировка, смена персонажа. У нас: `TabHeader` `CLICK` (вкладки), `experimentPanel`
`CLICK` → `useItem` / `clearSlot` (предмет в ячейку / из ячейки) — **A**; `runePanel` `CLICK` → `selectRune` / `selectRuneForSlot` /
`selectRuneTarget` — **A**; рецепты — перетаскивание пальцем = прокрутка — **A**, выбор рецепта тапом — **B** (`selectRecipe` есть);
«Соединить» / «Создать» — событие пада `Combine` / A — **C**. 17 EI общих. 1.5 ч. `uiCraft_t`: класс `UITouchCraft`, 6 EI без обработчика.

## 9. Осмотр, подсказки, мир

- `examine_t` против `examine_c`: только чтение; закрыть (жест «Назад» = B), прокрутка (колесо двумя пальцами). **C**, 0.2 ч.
- `tooltip_t` против `tooltip`: показ. На iPad подсказка — по короткому долгому нажатию; у нас — `UIShowTooltip` (кнопка пада) либо само
  при выборе элемента. **C**: «короткое долгое» в `dt_step` → кнопка подсказки. 0.5 ч.
- `worldTooltip_t` против `worldTooltip`: подписи предметов на земле кликаются одинаково (`tooltipClicked`) — **A**; показать подписи —
  кнопка пада (**C**, шестерёнка).
- `overhead_t`: кнопка действия над головой (`clickedAction`) — в `main` Switch нет; не нужно.
- `combatTurn_t` = `combatTurn`: тап по портрету в очереди → `centerCamOnCharacter` — **A**.
- `itemSplitter_t` против `itemSplitter_c`: ползунок (`LSSlider`, мышь) + `CLICK` с попаданием по зонам «принять / отмена» — **A**.

## 10. Меню

| экран | iPad | наш `_c` | вердикт, что делать | ч |
|---|---|---|---|---|
| главное | `Menu_button` мышью → `buttonPressed` | слушатели-заглушки | сделано: `mainMenu_d` | — |
| пауза `gameMenu` | `touchSimpleButton` → `buttonPressed` | список `Menu_button`, слушателей нет | **B**: обобщить `dos2_mainmenu_touch.py` (тот же список) | 1 |
| сообщение `msgBox` | `btnYes` / `btnNo` → `ButtonPressed`, поле ввода | опции списком (`addOption`), A/B/X/Y → `ButtonPressed`; слушатели подсказок и copy / paste — **заглушки**; поле ввода рабочее (`acceptInput`) | **B** опции + **C** подсказки (обёртка в `msgBox_d` нужна) | 0.7 |
| настройки `optionsSettings` | флажок (Tap → `checkBoxID`), выпадающий список (`larCombo` → `comboBoxID`), ползунок, селектор (`selectorID`), «Применить» / «ОК» / «Отмена» | строки: `SelectorMC` (флажок и список — одно: ←/→ → `checkBoxID` / `comboBoxID`, стрелки `left_mc` / `right_mc` нарисованы), `SliderComp` (`LSSlider` слушает мышь → `menuSliderID` — **A**), кнопки, A → `acceptPressed`, X → `pressedDefaults` | **B**: тап по строке = выбрать, по `left_mc` / `right_mc` = ←/→; **C**: вкладки LB/RB и применить — подсказки | 1.5 |
| сохранения `saveLoad` | запись: Tap / двойной → `loadPressed` / `savePressed`, кнопки действия / удалить / новая, вкладки персонажей | список ↑/↓, A, `UIDelete` → `deleteSave`; `PlayerTab` слушает мышь, но `selectCharTab` в `main` нет | **B**: тап по записи = выбрать, повторный = A; **C**: удалить / синхронизация — подсказки | 1 |
| обучение `tutorialBox` | `ok_mc`, `close_mc`, флажок | `CLICK` / `MOUSE_UP` → `cancelNonModal` / `toggleTutorials`, удержание | **A** | 0.2 |
| управление `optionsInput` | список привязок и жестов | на Switch — картинка `buttonLayoutNX_c` | нет функций | — |

Дешёвая альтернатива для msgBox / saveLoad / options уже описана в `TOUCH_UI_PLAN.md` §8 (TU-04/05: `_t`-фильм на PC-близнеце того же
класса). Для этих трёх контракт `_t` = PC, класс общий — это единственное место, где фильм iPad реально встаёт без разбора движка. Но
решение хозяина 15.11 — консольный вид, поэтому в таблице стоит правка `_c`.

## 11. Остальное

- Сетевые: `characterAssign_c`, `connectivity_c`, `serverlist_c` — на iPad мышиные кнопки и выпадающие списки, у нас пад (частично
  мышь: флажок в `connectivity_c`, двойной клик по серверу и «обновить» в `serverlist_c`). **C** (подсказки) сейчас, **B** позже.
- `formation_c`, `pyramid_c`, `sortBy_c`, `partyManagement_c`, `giftBag*` — списки без мыши: общий рецепт B «строка + A».
- `startTurnRequest_c`: удержание A; на iPad — удержание пальца (`IE Touch`). **C**: палец на экране, пока окно видно = A зажата.
- Без ввода: `installScreen`, `loadingScreen`, `textDisplay`, `subtitles`, `mouseIcon`, `notification`, `combatLog_c`.

## 12. Порядок работ (уточнение к `TOUCH_CONSOLE_PLAN.md` §3)

1. Группа «тач уже есть» одним паком и одним прогоном (Ф): `bottomBar_d`, `playerInfo`, `contextMenu_c`, `containerInventory`,
   `partyInventory_c`, `equipmentPanel_c`, `statsPanel_c`, `inventorySkillPanel_c`, `craftPanel_c`, `dialog_c`, `waypoints_c`,
   `itemSplitter_c`, `tutorialBox_c`, `worldTooltip`, `combatTurn`. Прогон отвечает на главный непроверенный вопрос: доходит ли мышь до
   этих фильмов в игре и не срабатывает ли тап как A.
2. Общий помощник правки в `tools/` («тап по элементу списка → штатный выбор → `dmPadBtn`») — 2 ч; на нём: `gameMenu_c`, `msgBox_c`,
   `saveLoad_c`, `optionsSettings_c`, `areaInteract_c`, `reward_c`, `pyramid_c`, `sortBy_c`, `panelSelect_c`.
3. `characterCreation_d` (3.5 ч) — нужен до первого запуска новой игры без пада. **Сделано статикой, §2.1; ждёт прогона.**
4. `trade_c` (2.5 ч), `journal_c` (сначала опыт с `journal_csp.swf`, затем правка / карта, 3 ч), прямой тап по ответу в `dialog_c` (1 ч).
5. Жесты хоста: двойной тап, «короткое долгое» = подсказка, удержание = A для окон с удержанием (1.5 ч).
6. Сетевые и редкие экраны.

## 13. Не доказано

- Всё — по ABC и строкам `main`; ни один вердикт A не подтверждён на телефоне. Тач-слой `_c` написан Larian, но шёл ли он когда-либо в
  сборке Switch — неизвестно (движок Switch тачскрин не читает): возможны места, где обработчик есть, а данные для него движок не шлёт.
- «EI есть в `main`» = имя встречается подстрокой; совпадение числа аргументов с ожиданием движка для путей мыши не сверялось
  (`SlotPressed` из `CLICK` идёт с 2 аргументами, с пада — тоже 2; `stopDragging/6` у `larInventory`).
- Создаётся ли `statusConsole` (0x75) в режиме пада; какой из `journal_c` / `journal_csp` реально на экране.
- Фильмы под модальным окном: получают ли клик (опыт E1 из `TOUCH_CONSOLE_PLAN.md`).
- Геометрия правок B: списки `_c` прокручиваются и масштабируются; попадание брать через `hitTestPoint` самих элементов, как делает
  тач-слой Larian (`getSlotForMousePosition`, `fn#313` в `statsPanel_c`), а не по координатам из головы.
