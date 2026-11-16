# Divinity: Original Sin 2 — Definitive Edition (Switch) → нативный Android: состояние и журнал

Подход тот же, что у DOOM 2016 / Eternal / XC2 / Diablo III / Torchlight II: ARM64-код игры исполняется напрямую, NSO патчится офлайн
(x18/TLS/SVC/cntpct), `nn::` — HLE, NVN → Vulkan, шейдеры офлайн. Дерево — клон `tl2-port/src` (ветка `tl2`, 2159a74), ветка `dos2`;
сборка `DM_GAME=dos2` = ядро DOOM + `DM_GAME_D3` + `DM_GAME_TL2` (общие части TL2) + `DM_GAME_DOS2`. Адресный код Torchlight II
(широкий экран, тапы по меню, иконки, карта, трассировка Pia, poll back-off) — за `DM_TL2_MAIN`, он есть только в сборке `tl2`.

## Вход
| Файл | Что |
|---|---|
| `Divinity Original Sin 2 Definitive Edition\...[010027400CDC6000][v0].nsp` (11.2 ГБ) | база |
| `...[010027400CDC6800][v786432].nsp` (0.9 ГБ) | апдейт v12 (BKTR) |

## Разведка (14.11.2026)
- Движок Larian (Divinity Engine), NVN, PhysX 3.4.2, NEX 4.6.3 / Pia 5.24 (сеть), libcurl. **SDK 9.3.0**, sdk build `230994ac…` —
  тот же, что у Hades в nx2android (`nxnative/dm/games/0100535012974000`, но Hades рисует через Vulkan, не NVN). Профиля DOS2 в nx2android
  (`eceab46`, 14.11) нет.
- ExeFS апдейта: `main` (build-id `4979b200…`, 20.6 МБ сжато, text 29.3 МБ), `sdk`, `subsdk0` — **побайтная копия sdk** (md5 одинаковый,
  у main в NEEDED только `nnSdk.nss`): грузим `main sdk`, subsdk0 не нужен.
- npdm: prio 44, core 0, **SystemResourceSize 0x200000** (2 МБ: VAMM включён, main импортирует `AllocateAddressRegion`,
  `IsVirtualAddressMemoryEnabled`), стек 2 МБ, ProgramId 010027400CDC6000.
- main: 730 импортов, 367 `nn::`/nvn; `nvnBootstrapLoader` + 517 имён NVN через GetProcAddress — **все** есть в `dm/gfx/nvn` (names.txt
  не знал только `nvnWindowSetPresentInterval`, но он реализован).
- Глубина: сразу после инициализации устройства безусловно `nvnDeviceSetDepthMode(1 = NEAR_IS_ZERO)` и `SetWindowOriginMode(1)`
  (main 0x938574 / 0x93858c; слоты указателей main+0x2bbef18 / +0x2bbef10, загрузчик имён около 0x934f00) — как TL2/D3 → `depth_zero`.
- RomFS (база + BKTR, IVFC ок): **47 файлов, 11.36 ГБ** — пакеты Larian LSPK v13 `Data/*.pak` (+ части `_1`, `_2`…: Voice, Video,
  SharedSounds), `Data/Localization/<Язык>/*.pak` (русский есть). Пакеты читает `tools/lspk.py` (заголовок в конце файла, список
  файлов LZ4, данные LZ4/zlib/без сжатия).
- Новый вид BKTR у апдейта: таблица подсекций кончается на смещении таблицы релокаций, а секция длиннее таблиц на 0x4200 —
  `tools/content_reader.py` (копия D3) теперь принимает оба вида.

## Шейдеры
NVN-программы — файлы `.bshd` (6158) плюс 75 стадий GLSLC, вшитых в `main` (0x29779f8…, control 0x838). Полный скан по магии (14.11, `tools/scan_shader_magic.py`: все члены всех паков, кроме звука/видео, и `sdk.raw`) других шейдеров не нашёл. `.bshd` лежат в `EngineShaders.pak` (`Shaders/NX/*.bshd`, 152) и `<материал>_<проход>_NX.bshd` в `Engine.pak` (135),
`Materials.pak` (5448), `CommunityMods.pak` (423). `.bshd` = BE u32 `0x80000000 | размер` + zlib → контейнер `ffffffff 01020118 …`, в нём
по голому GLSLCoutput (магия 0x19866891) на стадию, вершинная первой. Рядом лежат исходники `*_NX.shd` (GLSL 450 + GL_NV_gpu_shader5) —
только справка. `tools/dos2_shader_jobs.py` → `work/shaders-jobs` (формат tl2_shader_jobs.py, тот же ключ, факты depth_zero / compute local):
6158 файлов, 6126 vs+ps, 5 cs, 27 пустых; **12 256 уникальных стадий, 6131 программа, 0 проблем**.
```
python src/tools/dos2_shader_jobs.py work/romfs/Data work/shaders-jobs
XLATE=<ports>/d3-port/work/build/dmsh_xlate_d3/bin/dmsh_xlate.exe SPIRV_OPT=… SPIRV_VAL=… J=11 \
  python src/dm/gfx/tools/dmsh_vk_xc2.py work/shaders-jobs work/shaders-vk
```

## Патч NSO
`PYTHONUTF8=1 DM_TARGET=android python src/dm/tools/patch_nso.py work/exefs-update/<mod> work/patched <mod>` для main и sdk
(без PYTHONUTF8 скрипт падает на китайском print в конце); `equiv.py`: **0 расхождений**, мутации 300/300 на обоих
(main 12 335 мест, 0 гейтов — svc/cntpct в main нет; sdk 11 242 места, 93 гейта).

## HLE: сверка импортов
main минус всё, что берёт хост (`"_Z…"` литералы), минус импорты main TL2: 56 функций, из них библиотечные (os-синхронизация,
LightEvent, ReaderWriterLock, util, crypto, cstd, swkbd-сеттеры, ngc::CountNumbers) идут в нативный sdk. С сервисами — `dm/host/hle_dos2.c`
(по nxnative, MIT): `fs::GetSaveDataSize`, `fs::ExtendSaveData`, `err::ShowError`, `hid::GetActualVibrationValue`, `kpr::GetKeyCodeMap`,
`ngc::ProfanityFilter::MaskProfanityWordsInText`, `nifm::SetLocalNetworkMode`, `account::LoadProfileImage`. `web::ShowWebPageArg` —
уже в hle_doom.c, `audio::*AudioRendererParameter` — hle_doom_audio.c. `fs::IsSaveDataExisting` (hle_tl2.c) сравнивает с id DOS2.
Полный аудит всех nn::-импортов против собранной `swforce` и путей в нативном sdk (14.11, ветка `dos2-hleaudit`): `docs/dos2/HLE_AUDIT.md`
(+ `socket::GetHostEntByName(const char*)` → NULL, `nnosExitProcess` → graceful exit хоста).

## Гейты DM_GAME_DOS2 (относительно TL2)
| Где | DOS2 |
|---|---|
| CMake | `dos2` → DOOM + D3 + TL2 + DOS2; `DM_TL2_MAIN` только у `tl2` |
| hle_svc.c | SystemResourceSize 0x200000 (VAMM вкл.), ProgramId 0x010027400CDC6000 |
| hle_args.c | argv[0] "dos2" |
| hle_tl2_wide/ui/icons/map.c, main.c wide install, hle_hid.c ui pump, jni TL2-натив, vulkan_present.cpp (pillarbox, иконки), xbg_game.cpp splash band, hle_ldn.c Pia, hle_thread.c poll back-off | `DM_TL2_MAIN` (выкл.) |
| front_android.c | пакет по умолчанию `org.dos2.port` |
| Java | `DOS2` ⇒ `TL2` (фронт TL2 пока), папка `Download/DivinityOriginalSin2Mobile`, имя «Divinity Original Sin 2 Mobile»; ресурсы ветки переименованы |

## Сборка и данные на телефоне
```
cmake -S dm/android -B work/build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
  -DCMAKE_MAKE_PROGRAM=C:/Python312/Scripts/ninja.exe -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 -DCMAKE_BUILD_TYPE=Release -DDM_GAME=dos2
GRADLE=<gradle> DM_KEYSTORE=<keystore> powershell -File tools/build_apk.ps1
                                               # -> work/DOS2-port-debug.apk (org.dos2.port, 0.0.1-debug, ключ хозяина)
bash tools/push_phone_dos2.sh                  # dm/out, dm/shaders, turnip27, RomFS -> Download/DivinityOriginalSin2Mobile (APK: DOS2_INSTALL=1)
```
Сабмодуль libadrenotools берётся из локального клона TL2 (`git config submodule.third_party/libadrenotools.url <tl2>/third_party/libadrenotools`).
Телефон (Mi 14 Ultra, 14.11 ~09:20, USB): `/sdcard/Download/DivinityOriginalSin2Mobile/{romfs (47 файлов, 11 ГБ), dm/out (15), drivers/turnip27,
logs, DOS2-port-debug.apk}`; `dm/shaders` — после перевода. APK не установлен и не запускался (ждёт хозяина).

## Дальше
1. Перевод шейдеров → `dm/shaders` на телефон; установка APK, выдача доступа к файлам, первый запуск по Wi-Fi adb.
2. Первый запуск: смотреть `[nvn] 深度模式` (= 1), отказы SetHeapSize / VAMM, сервисы, которых нет (сверка по логу), аудио (FMOD? Wwise —
   проверить), видео Bink (`Bink.bshd` есть — ролики через Bink, не nn::movie).
3. Главное меню с хорошим fps; затем 20:9, тач (у DOS2 на Switch — пад; фронт TL2 пока как есть).

## Первые запуски (14.11.2026, Mi 14 Ultra, Turnip 27)
- r1: телефон заблокирован, поверхности нет — движок не стартовал ("game not started").
- r2 (60 с, `tools/run_dos2.sh`): загрузка без падений. Модули встали, SetHeapSize 3020 МБ с первого раза, VAMM, все 47 пакетов RomFS
  открыты, сохранения коммитятся, глубина «near=0» и origin «upper-left» подтверждены логом, окно NVN 1600x900 (2 текстуры, интервал 2),
  звук: AudioOut 5.1 (даунмикс в стерео) + audren 48 кГц, 6 mix buffers. Первый кадр через 12 с, два провала по 2–3 с на загрузке
  (поток читает пакеты через FUSE), дальше ровные **60 fps** (p50 16.67 мс, CPU кадра ~1.6 мс, GPU ~0.1 мс, ~440 вызовов NVN за кадр) до конца
  прогона. Что было на экране — не проверено (скриншота нет; такой малый GPU — скорее заставка или экран загрузки, а не меню).
  В конце прогона игру остановил run_dos2.sh, после чего хозяин забрал телефон. Лог: `work/logs/r2.log`.
- Дальше: скриншот и проверка, что на экране; если игра стоит на заставке — смотреть ввод (нажать A / +), oe-уведомления и потоки загрузки.

## Чёрный экран → главное меню (14.11.2026, r3–r5, Wi-Fi adb)
- Симптом: звук есть, экран чёрный; в логе `missing shader bundle …: its draws are skipped`, `draws=0` при 10 DrawArrays за кадр.
- Причина 1: 75 стадий GLSLC вшиты в сам `main` (0x29779f8…, control 0x838: логотип, UI Iggy, движковые) — `dos2_shader_jobs.py` теперь
  берёт и их (9 из 13 пропавших ключей).
- Причина 2 (главная): игра **освобождает буфер control сразу после `nvnProgramSetShaders`** (Switch NVN разбирает control внутри вызова),
  а хост читал его лениво при первом draw — первые 8 байт уже были словами аллокатора, ключ каждый запуск новый. Фикс как у Switch:
  `hle_nvn.c nvnProgramSetShaders` копирует control (интерн по содержимому, `DM_NVN_CTRL_COPY=1` по умолчанию, гейт DOS2).
- Диагностика: при промахе хост пишет `dm/missing/<key>.rec` (формат заданий) — `dos2_shader_jobs.py` подхватывает `work/missing`.
- Итог r5: экран загрузки 60 fps, **главное меню** (Story / Continue / Arena / Options / Credits, 3D-фон) ~58–60 fps: 2200 draw, GPU 11.5 мс,
  подача 11 мс; провалы до 8 fps только при первой сборке пайплайнов. Открыто: цветной мусор на крыше замка (текстуры), диалог Steam Cross-save
  (сеть — отвечать «Нет»), 20:9 уже на весь экран (окно 1600x900 растянуто? проверить), язык en-US.


## Сверка с подготовкой прошлых портов (14.11.2026)
- Control NCA: `tools/control_icon.py` → `work/control/{control.nacp, icon_*.dat, Acd_1/}` (Acd_1 — иконка «Nintendo Switch 2 Edition»);
  иконка APK = `dm/android/dos2/icon.png` (локально, в .gitignore). NACP: языки 0x6efb (нет ja / nl / ko / pt-BR, хотя паки Japanese,
  Korean, Portuguesebrazil в RomFS есть), сохранения user 0x1800000 / journal 0x1800000 (хост отвечает 256+256 МБ — безвредно).
- AOC: во входной папке только база и апдейт; main не импортирует `nn::aoc` / `MountAddOnContent`; `SharedDLC*.pak` уже в RomFS — шаг не нужен.
- Модули: subsdk0 = копия sdk, патчатся main + sdk. ASTC для GPU без BC (Mali / PowerVR) — ленивый кэш на устройстве, см.
  «ASTC-кэш без ПК» (текстуры в LSPK `.dds` = упакованные NVN, инструмент D3 знает только CPK).
- Пак 1a = `../packs/DOS2-pack1a-0.0.2-crashfix.apk` (0.0.2-debug, фикс падения после загрузки кампании).

## Редактор Switch-пада как в DOOM / TL2 (14.11.2026, ветка `dos2-pad`; собрано, на телефоне не проверено)
Раскладка по умолчанию — `gamepad` D3PadView (Joy-Con: плавающий левый стик, крестовина под ним, ромб ABXY, правый стик, L/ZL R/ZR,
− ⚙ +). Её редактор теперь тот же, что у раскладок TL2 (раздел «Редактор как в DOOM» в `docs/tl2/TL2_PORT.md`):
- Код: редактор вынесен из `Tl2Touch` в `PadEditor.java` (модель мест, `place()`, загрузка / запись префов, жесты, панель, подсказка);
  `Tl2Touch` (tl2 / tl2_side) и `D3PadView` (gamepad при `GAMEPAD_EDITOR = DmActivity.DOS2`) его используют. У D3 гейт выключен —
  его старый редактор (тянуть группы, «Сбросить / Готово») не тронут. Формат префов TL2 не менялся; ползунок зума и зона хода TL2 — «зонные»
  элементы редактора (`zone`), как раньше: зум скрывается и масштабируется, зона хода только тянется.
- Панель «− + Группами/Раздельно Скрыть/Показать Сброс Отмена Сохранить» (вид DOOM, 30 % высоты по центру), подписи — строки
  `pad_ed_*` (values / values-ru, язык меню через `d3Ui()`); подсказка для пада: «ромб ABXY и крестовина — целиком» / «каждая кнопка отдельно».
- Тянуть контрол пальцем, второй палец — размер (0.5–2.5 × «Размер контролов»), −/+ без выбора — все группы. «Группами» (по умолчанию):
  группы `dpad`, `abxy`, остальные (`lstick rstick l zl r zr minus plus gear`) по одной; «Раздельно» (`d3_split_gamepad`) — каждая кнопка
  сама, крестовина и ромб распадаются на 4. Скин с одной картинкой крестовины рисуется, только пока её 4 кнопки на своих местах; сдвинутые /
  уменьшенные / скрытые — четыре отдельные кнопки.
- «Скрыть»: кнопка (Раздельно) или группа (Группами) не рисуется и не ловит касаний в игре (скрытый стик не хватается и в плавающей зоне), в
  редакторе — блёклая с красной чертой; шестерёнка не скрывается.
- Сохраняется только по «Сохранить»; «Отмена» и Back откатывают. Префы `doom_controls`: `d3_off_gamepad_<группа>` = `fx,fy[,scale]`,
  `d3_btn_gamepad_<имя>` = `dx;dy;scale;hidden` (имена a b x y l r zl zr plus minus up down left right gear lstick rstick).
  Старые `d3_pos_gamepad_<группа>` (абсолютный якорь прежнего редактора) читаются как смещение группы до первого «Сохранить» / «Сброса»
  (FrontMenu), которые их стирают. Прозрачность / размер — общие `d3_opacity` / `d3_scale` (ползунки FrontMenu), размер каждого контрола —
  в редакторе.
- Шестерёнка → «Раскладка управления»: первым «Геймпад Switch (Joy-Con, по умолчанию)», затем две сенсорные TL2 (`tl2`, `tl2_side`).
- Проверить на телефоне: открыть редактор (⚙ → экранный геймпад → «Изменить раскладку…»), тянуть / щипать стик, крестовину, ромб;
  «Раздельно» — тянется одна стрелка / буква; «Скрыть» на правом стике / кнопке → «Сохранить» → в игре её нет, касание там ничего не жмёт;
  «Отмена» / Back откатывают; «Сброс» + «Сохранить» возвращает всё; скин с крестовиной; tl2 / tl2_side — редактор работает как раньше.

## Широкий экран (DM_DOS2_WIDE, по умолчанию вкл., 14.11.2026; статика, на телефоне ещё не проверено)
Размер окна DOS2 у системы не спрашивает (из oe/vi только GetOperationMode / GetPerformanceMode / OpenDefaultDisplay / CreateLayer /
GetNativeWindow): он задан immediates в init рендера. Адреса — `main.raw` апдейта v786432, build 4979b200:
| Где | Что |
|---|---|
| `0x93845c mov w8,#1600` / `0x93846c mov w8,#900` | режим дока `{id 0, 1600, 900}` на стеке (fp−0x70) |
| `0x938474 mov x8,#1` + `0x938478 movk x8,#1280,lsl#32` / `0x938480 mov w8,#720` | портативный режим `{id 1, 1280, 720}` (fp−0x88); id = индекс, который выбирает `0x8a7758`: (GetOperationMode ≠ 1) \| (GetPerformanceMode ≠ 1) |
| `0x938b84 ldp w1,w2,[fp,#-0x6c]` | размер дока → настройки рендера (`+0x14/+0x18`, глобал `main+0x2bb8a60`; там же `+0x34 = 0.9` — масштаб UI / DRS 1440x810) и окно «Window» |
| vtable окна `main+0x2984d40` | SetSize vt+0x48 = `0x93d200` (→ `+0x18/+0x1c`), режимы регистрируются vt+0x60; смена режима `0x8a7788` → устройство vt+0xf8 (resize) |
| `0x939120` → `0x93c260` | swap-текстуры окна из того же описателя (`TextureBuilderSetSize2D`, `nvnWindowBuilderSetTextures` `0x939268`, `nvnWindowInitialize` `0x939288`) |
| `0x90fc50` Viewport::UpdateCameraAspect | камера `+0xa0` (аспект) = ширина окна × отн. ширина вьюпорта / (высота × отн. высота); проекция (vtable камеры `main+0x29949b0`, `0xa4b80c`): 1/tan(fov/2), FOV вертикальный, x делится на аспект ⇒ шире окно = **Hor+** |
| `0xc0b4b0` (16/9f `main+0x1f123bc`, сравнение `0xc0b548`) | единственное включение «ограничить аспект» окна (`+0xd1/+0xd4`, `0x93e350` → прямоугольник `0x93d320`, вьюпорты `0x90fab0`): только если окно **уже 16:9** и байт `main+0x2bb8918` ≠ 0 — тогда игра сама делает 16:9 с полосами внутри окна; шире 16:9 — всё окно |
| `0x110e32c` | масштаб каждого Iggy-мувика = высота прямоугольника окна / высота мувика (`× 0.9`, если `0x2bb8918` = 0); раскладка по якорям движка (`setAnchor` / `registerAnchorId`, код ПК) ⇒ HUD у краёв |
| `0xd1f4e0` | ролики (Bink, `0xd1b7ec/0xd1ba18`) вписываются в свой аспект по центру |

Других мест с размером нет: пара 1600/900 в main одна, пар 1280/720 и 1920/1080 нет (immediates и данные), 16/9f читается только в `0xc0b548`.

`dm/host/hle_dos2_wide.c` (`dm_dos2_wide_install`, из `main.c` сразу после `load_modules`): сверка 16 слов `0x93844c…` (иначе ничего не
меняется), затем 4 immediates режимов. Размеры: шире 16:9 — док W × 900, W = 900 × аспект (кратно 16, ≤ 3200; 2400x1080 → **2000x900**),
портатив 720 × аспект × 720 (1600x720); уже 16:9 — док 1600 × H, H = 1600 / аспект (кратно 16, ≤ 1600; 4:3 → 1600x1200, 16:10 → 1600x1000),
портатив 1280 × (1280 / аспект). Ручки: `DM_DOS2_WIDE` не задан / `1` — по экрану (`front_screen_px`, длинная сторона первой), `=<W>x<H>` —
размер дока (так передаёт Java), `=<W>` — ширина при 900, `=<a>:<b>` — аспект, `=0` — как на Switch. `DM_DOS2_WIDE_TALL=1` — для окна уже 16:9
снять собственные полосы игры (`0xc0b550 b.mi` → `nop`, сверка 9 слов): всё окно 4:3, Hor− (вертикаль та же, по бокам меньше), UI 16:9 на 4:3 —
эксперимент; по умолчанию полосы игры (как на ПК), картинка не сжимается.

Презентер: `present_native` отдаёт буфер окна размером с окно, композитор масштабирует его равномерно в полноэкранный SurfaceView ⇒ окно формы
экрана = 1:1 без растяжения. Тач: у DOS2 на Switch тача нет (нет импорта GetTouchScreenState; мышь инициализируется, хост отвечает «нет мыши»),
все касания — пад фронтенда, поправок не нужно.

Java (`Tl2Gear` / `DmActivity`): «Формат картинки» — 16:9 с полосами / растянуть / широкий (по умолчанию широкий, pref `aspect`, со следующего
запуска) → `DM_DOS2_WIDE=<W>x<H>` (`DmActivity.dos2WideSize`, то же правило) или `0`; подписи режима Switch показывают размер
(`d3RenderSize`, ветка DOS2); строки «растягивать экраны загрузки» у DOS2 нет. `dm/env` перекрывает ⚙ — не держать там `DM_DOS2_WIDE`.

Проверить на телефоне: лог `[wide] DOS2 window 1600x900 -> 2000x900 …`, `WindowInitialize … 2000x900`, `Present at the game's output size
2000x900`; меню и мир без растяжения (круги круглые), по бокам видно больше, HUD у краёв, диалоги / инвентарь / карта не уехали; переключение
док ↔ портатив в меню паузы (1600x720); ролики по центру; fps / память (RT на 25 % больше — пулы `0x932260`: 0x5200000 / 0x21800000 / 0xf00000).
Риски: полноэкранные мувики UI, нарисованные под 1920 по ширине, могут оставить по бокам видимый мир (косметика); если `0x2bb8918` = 0, на 4:3
игра не ставит полосы и даёт Hor− сама; бюджет памяти текстур/RT не проверен.

## Альфа-тест NVN (14.11.2026, ветка dos2-alpha; на телефоне не проверено)
- Было: игра ставит `nvnColorStateSetAlphaTest(GREATER)` (с загрузки, в кампании рисует с ним), хост теста не делал: `nvnCommandBufferSetAlphaRef`
  был пустым, транслятор собирает с `AlphaTestOp.Always`, а шейдеры с support buffer роняли игру (`Nontrivial NVN alpha test`, потом лог).
  Итог — вырезы (листва, волосы, заборы) рисуются непрозрачными квадратами.
- Сделано: `SetAlphaRef` пишет `OP_X_ALPHA_REF` (`nvn_obj.h`, `hle_nvn.c`) → `state.alpha_ref` (хвост `State`, его покрывает pipeline memo).
  В `draw()` при `color.alpha_func != ALWAYS` фрагментный модуль меняется на вариант `alpha_module(shader, func, ref)`: SPIR-V бандла заново
  (`warm::bundle_words` — та же цепочка рерайтов, что в `load_shader`) + `xbg_spirv_alpha_test` (`xbg_spirv_alpha.cpp`): перед каждым
  `OpReturn` точки входа `OpLoad` выхода Location 0 → `.w` → `OpFOrd<cmp>` с константой ref → `OpKill` (`OpDemoteToHelperInvocation`, если
  модуль уже объявляет capability; в DOS2 таких 0). NEVER — `OpKill` вместо `OpReturn`. `EarlyFragmentTests` из варианта убирается
  (иначе выброшенный фрагмент уже записал бы глубину — на тайлерах тоже). Ref — константа, не spec constant: `xbg_stage_key` отвергает
  `pSpecializationInfo` (пайплайн стал бы некешируемым), а список warm-up хранит только ключи модулей. Модуль на (шейдер, func, ref),
  кеш `Shader::alpha`, ключ warm-up `<key>#at<func>_<биты ref>` (`warm::load_module` собирает вариант сам). ALWAYS — прежний путь без
  изменений (одно сравнение). `first_draw_covers` (NOLOAD на тайлерах) при альфа-тесте отказывает. Отказ рерайта (нет float vec4 на
  Location 0 или `.w` не пишется — так же пропускает Ryujinx) → базовый модуль, лог `not applied`.
- Переключатель `DM_GFX_ALPHA_TEST`: 1 по умолчанию в сборке `DM_GAME_DOS2`, 0 в остальных (там при 0 и support buffer — прежний FATAL).
  Логи: `NVN fixed-function alpha test in fragment shaders DM_GFX_ALPHA_TEST=1`, `NVN alpha test GREATER ref=… emulated in the fragment
  shader (first: <key>, …)` — по разу на (func, ref), `NVN alpha-tested draws: N (frame …; variants …, not applied …)` на степенях двойки.
- Проверено на хосте: `tests/spirv_alpha_test.cpp` (синтетика: 6 сравнений, NEVER, demote, dual source, повтор OpTypeBool, снятие
  EarlyFragmentTests, 6 отказов — PASS, MSVC /W4 без предупреждений); все фрагментные шейдеры DOS2 (`work/shaders-vk`, 6192 FS):
  GREATER/0.5 и NOTEQUAL/0.25 — 4228 переписаны (1964 без выхода Location 0 — отказ), NEVER — 6162 (30 без OpReturn);
  **14 618 модулей проходят `spirv-val --target-env vulkan1.1`, 0 ошибок**. Сборка `dm` (arm64, DM_GAME=dos2) — новых предупреждений нет.
- Проверить на телефоне: (1) в логе `DM_GFX_ALPHA_TEST=1`, значение ref и число вариантов / `not applied`; (2) листва / волосы / решётки
  в первой локации с вырезами, а не квадратами; тени от листвы тоже (если тень рисуется FS без выхода Location 0 — она останется сплошной,
  тогда смотреть, как DOS2 пишет альфу в shadow-проходе); (3) fps в кампании против `DM_GFX_ALPHA_TEST=0` (discard снимает early-Z
  у этих draw; если игра держит GREATER на всех непрозрачных — это заметно, особенно Mali/PowerVR); (4) подвисания при первом появлении
  вариантов (перечитывание бандла + модуль + новый пайплайн); со второго запуска их строит warm-up.

### Черновик для KB (shaders-translation.md)
```
### SH-DOS2-02 Фиксированный альфа-тест NVN (ColorStateSetAlphaTest + SetAlphaRef) → вариант фрагментного модуля с OpKill
- **Симптом:** вырезы (листва, волосы, заборы) — непрозрачные квадраты; раньше FATAL `Nontrivial NVN alpha test not translated` на шейдерах
  с support buffer (DOS2 ставит GREATER с загрузки).
- **Устройства/GPU:** любые (в Vulkan альфа-теста нет); DOS2.
- **Причина:** Maxwell сравнивает альфу RT0 с ref после шейдера и выбрасывает фрагмент (цвет, глубина, стенсил); хост игнорировал func,
  SetAlphaRef был no-op, офлайн-транслятор собирает с AlphaTestOp.Always.
- **Фикс:** `OP_X_ALPHA_REF` → `state.alpha_ref`; при func ≠ ALWAYS `alpha_module` строит вариант FS (`xbg_spirv_alpha.cpp`: перед каждым
  OpReturn load Location 0 .w, OpFOrd<cmp> с константой, OpKill; NEVER — OpKill; EarlyFragmentTests снимается). Модуль на (шейдер, func, ref),
  ключ warm-up `<key>#at<func>_<bits>`; spec constant не взят — ключ пайплайна отвергает pSpecializationInfo. ALWAYS — путь без изменений.
  NOLOAD (`first_draw_covers`) при альфа-тесте отказывает. `DM_GFX_ALPHA_TEST=1` в DOS2, 0 в остальных портах (кандидат: XC2/D3/TL2, если
  их игры ставят альфа-тест — у них тогда FATAL/квадраты).
- **Эталон:** dos2-port ветка dos2-alpha: `dm/gfx/vulkan/engine/xbg_spirv_alpha.{h,cpp}`, `xbg_game.cpp` alpha_module / draw / warm::bundle_words,
  `hle_nvn.c` nvnCommandBufferSetAlphaRef, тест `tests/spirv_alpha_test.cpp`.
- **Проверка:** собрано; 14 618 переписанных FS DOS2 проходят spirv-val; на телефоне не проверено.
- **Issues:** —.
```

## Перф и нативизация, ветка `dos2-perf` (14.11.2026, собрано, на телефоне не проверено)
Аудит — [PERF_AUDIT.md](PERF_AUDIT.md), план и разбор main — [NATIVE_PLAN.md](NATIVE_PLAN.md).
- **Загрузки (`DM_FS_ROM_INDEX`, 1; 2 = сверка).** Все 15 стопов o2 (2–10 с) — потоки игры в `getdents64`/`openat` на `fuse_simple_request`:
  `hle_fs.c` разрешал каждый компонент пути через opendir/readdir родителя под общим замком. Теперь дерево RomFS читается один раз при
  монтировании и отвечает из памяти (поиск пути, GetEntryType, листинги, размеры), файл открывается один раз, гостевой OpenFile = dup.
  `DM_ROMFS_APPDATA` (1): RomFS в `Android/data/org.dos2.port/files/romfs` (обход FUSE) важнее Download. `DM_FS_ROM_FADVISE` (0), `DM_FS_ROM_FD_KEEP` (256).
- **Шейдеры (`DM_GFX_SHADER_PACK`, 1; 2 = сверка).** `dm/shaders/shaders.xgspack` (`dm/gfx/tools/xgs_pack.py`, делает `push_phone_dos2.sh`) —
  одно открытие вместо fopen на FUSE на стадию; `spv-override` листается раз (`DM_GFX_SPV_OVERRIDE_PROBE=1` — по-старому).
- **Пайплайны:** `DM_GFX_ASYNC_PIPELINES=1` для DOS2 (полупрозрачные draw не ждут сборки; вход в меню собирал 99 пайплайнов за 1.9 с).
- **Перепись потоков записи NVN (`DM_NVN_THREADS`, 1):** строка на поток раз в 10 с + время ожидания `nvnTextureWriteTexels` на сливе очереди.

### Что мерить (по одному переключателю, ABBA ×2; бэкап `files/save` перед force-stop)
1. Обычный запуск → меню → «Continue»: в boot.log `[fs] rom index (DM_FS_ROM_INDEX=1): … storage FUSE`, `NVN shader pack …: 12329 bundles`,
   строки `[nvn] recording thread` / `[nvn] WriteTexels`; считать `[stall] no frame presented` и их длительность, время до управления героем.
2. То же с `DM_FS_ROM_INDEX=0` в `dm/env` (старый путь) — сравнить стопы; `DM_FS_STATS=1` в обоих.
3. Один прогон `DM_FS_ROM_INDEX=2` и `DM_GFX_SHADER_PACK=2`: 0 строк `rom index mismatch`, 0 `differs from its file`.
4. Первые сборки: окна с `pipelines_new>0` — `async_skips`/`async_built`, `create_ms`; второй запуск — `NVN warm list=` и окна без `created`.
5. (По согласию хозяина, 11 ГБ) RomFS в `Android/data/org.dos2.port/files/romfs`: строка `boot: RomFS from the app directory`, те же стопы.

## 14.11 (ветка dos2-native): WriteTexels без слива очереди; сверка точек хука шага 5
- `DM_NVN_WT_DRAIN` (`dm/gfx/nvn/hle_nvn.c`, блок `wt_*`): 0 (умолчание dos2) — `nvnTextureWriteTexels(Strided)` ждёт только подачу,
  в которой память текстуры называлась последний раз (не называлась — не ждёт); 1 — прежний `x_drain` (умолчание других игр); 2 — правило 0
  + сверка на потоке трансляции. Описание и ограничения — `NATIVE_PLAN.md` §1.6. Собрано (`dm`, arm64, DM_GAME=dos2), на телефоне не запускалось.
- На телефоне: строки `[nvn] DM_NVN_WT_DRAIN=…`, `[nvn] WriteTexels (last 10.0 s): N calls waited X ms` (с `=1` — цена слива, с `=0` — остаток),
  `[nvn] WriteTexels rule …` (never named / already translated / waited for the last use / waited for all; сколько раз ждал бы слив),
  `[nvn] WT: … exact rule off` (если появилась — записать причину: точное правило у DOS2 не действует), прогон `=2`: `check: N clean, 0 named`.
  Глазами при `=0`: нет ли мусора / чужих текстур на 1 кадр при подгрузке (портреты, иконки, шрифты UI).
- Сверка Ghidra (свой headless-проект на копии main): начала и аргументы `ExecuteCommandList 0xa49c30`, `ExecuteRenderCommand 0x93e5f0`,
  `BeginFrame 0x9215f0`, `EndFrame 0x93ccd0`, кадр `0xa66bc0`; формат списка, длины 64 пакетов, третий контекст записи вне кадра,
  кэш состояния рендерера (NVN летит при draw) — `NATIVE_PLAN.md` §1.5; рецепт шага 5 — §3.5.

### Черновик для KB (performance.md)
```
### PERF-DOS2-04 nvnTextureWriteTexels: ждать подачу последнего использования памяти, а не всю очередь трансляции
- **Симптом:** поток-стример текстур стоит на каждом WriteTexels, пока nvn-translate не переведёт весь хвост (строка [nvn] WriteTexels … waited).
- **Устройства/GPU:** любые; DOS2 (ядро dm, применимо к портам с x_drain в WriteTexels).
- **Причина:** на Switch WriteTexels — запись CPU без ожидания GPU; хост сливал очередь ради порядка с уже поданными draw.
- **Решение:** метка «последняя запись команд, назвавшая память» + карта запись→подача; DM_NVN_WT_DRAIN 0 (dos2) / 1 слив / 2 сверка.
- **Где:** dm/gfx/nvn/hle_nvn.c (wt_use, wt_target, wt_before_write, wt_check_submit), ветка dos2-native.
- **Проверка:** сборка; телефон — не проверено. **Проблемы:** точное правило только при подаче записей по одной по порядку (иначе грубое).
```

## Корректность на чужих GPU: R14 / R12 (14.11, ветка `dos2-gpufix`; собрано, на телефоне не проверено)
Из `docs/port-kb/mobile-gpu-field-notes.md` (R12, R14, таблицы §4). Правила — чистые функции по `driverID` в `xbg_vendor_policy.h`
(тест `tests/vendor_policy_test.cpp`), применение — `xbg_game.cpp`. Строка лога при старте:
`NVN driver quirks: driverID=… depth_bias_x2=… clear_rebind=… reversed_depth_broken=…`. На Turnip (18) и стоковом Arm (9) — всё как было.
- **(b) `DM_GFX_DEPTH_BIAS_X2`** — PanVK (20) и RADV (3) берут половину `depthBiasConstantFactor` (ANGLE `doubleDepthBiasConstantFactor`):
  постоянная часть смещения глубины удваивается. По умолчанию вкл. только там; `=0` выкл., `=1` принудительно (на Adreno — для A/B: тени
  «отрываются» сильнее = переключатель работает). Проверить: Mali на JimVulkan/PanVK — нет «акне» и не «висят» тени. Warm-лист, записанный
  на другом драйвере, хранит прежний rs → у такого пайплайна только лишняя сборка прогрева, не неверная картинка.
- **(d) `DM_GFX_CLEAR_REBIND`** — Qualcomm сток (8), AMD (1), IMG (7): `vkCmdClearAttachments` портит привязанные VB/IB (Skia). Все 5
  вызовов движка идут через `cmd_clear_attachments`: после clear кэш привязок сбрасывается (`bind_cache_reset`), следующий draw
  привязывает pipeline/sets/VB/IB/viewport заново. Лог при первом сбросе: `NVN clear rebind: …`. Проверить: стоковый Adreno
  (драйвер «системный» в ⚙) и PowerVR — нет падений/«взрыва» геометрии после очисток; цена — несколько лишних bind на clear.
- **(c) обратный диапазон глубины, только зонд.** NVN `SetDepthRange(n>f)` идёт в Vulkan как `minDepth > maxDepth` (это валидно), а
  стоковый Qualcomm такие вьюпорты рисует неверно (Dolphin). Точная эмуляция без шейдера невозможна (swap меняет записанную глубину), нужен
  флип `z' = w - z` в последней вершинной стадии + swap — не сделан. Пока: одна строка `NVN reversed depth range near=… far=…` при первом
  таком draw (на сток-Qualcomm с пометкой «draws it wrong»). Если строка появится на стоке — делать флип за `DM_GFX_REVDEPTH`.
- **(a) PowerVR ClipDistance + CullDistance — посылка не выполняется.** В этом дереве нет clip-fix (SH-17, `xbg_spirv_clip.cpp` — D16), а
  из 12 329 модулей пакета (`work/shaders-vk`) 6 132 только *объявляют* оба члена `gl_PerVertex`, ни один не объявляет capability
  ClipDistance/CullDistance (= не пишет). Оставлен отчёт: на PowerVR модуль с обеими capability даёт одну строку `NVN PowerVR: …`.
- **(e) Mali `WAIT_BIT` → `VK_NOT_READY` — уже безопасно.** Все чтения `vkGetQueryPoolResults` на CPU (occlusion-фолбэк, timestamps,
  GPASS, report-ts, самотест) идут после фенса своей подачи; occlusion-копия (`vkCmdCopyQueryPoolResults`, WAIT вне Adreno =
  `DM_GFX_QUERY_WAIT`) — на GPU, баг её не касается. Правок нет.
- **R12 — пропущен:** грубых правил dynamic state в дереве нет. Пайплайны используют только `VIEWPORT` и `SCISSOR`; EDS/EDS2/EDS3, VIDS,
  dynamic cull/topology нигде не включаются (MALI-09 «EDS < r42» — только знание в KB). Таблица версий понадобится, если их введут.
- Не сделано: модель в `DM_GFX_SIM_GPU` (квирки — свойства драйвера, а не возможностей; для A/B на Adreno хватает `=1`); Adreno 6xx сток
  «вложения одного размера» (вне задания).

## Качество рендера и DRS: R1, первый шаг (14.11, ветка `dos2-drs`; собрано, на телефоне не проверено)
Модуль `dm/host/hle_dos2_quality.c` (`dm_dos2_quality_install` из main.c сразу после `dm_dos2_wide_install`). Все адреса — main.raw 4979b200.
- **DRS (динамическое разрешение Switch-версии, строк с его именем нет).** Объект 0x50 байт, vtable main+0x299b938, указатель main+0x2bbb198
  (создаёт 0x8a9a40 → ctor 0xb21800). Поля: +0x08/+0x0c max = окно, +0x10/+0x14 min, +0x18/+0x1c шаг вверх (1,25 % окна), +0x20/+0x24 шаг
  вниз (2,5 %), +0x28/+0x2c текущий размер (float), +0x30/+0x34 масштаб = текущий / max (его читает рендер: 0xab2118, 0xab322c, 0x911074),
  +0x38 кадров с изменения (оценка с 4-го), +0x44 мс запаса для шага вверх, +0x48 выкл. (смена режима 0x8a7718, загрузка 0x158cde8).
  `SetSize` 0xb21880(drs, w, h) из 0xa6a770 с размером окна: min = 0,75 × окно (`fmov s2,#0.75` в 0xb21890).
  Контроллер — каждый кадр в 0xc05c80, участок 0xc05da8..0xc06620 (вход: `[[main+0x2bb8a60]+0x6a0] == 2`, +0x48 == 0): вход — GPU-время
  кадра `[[main+0x2bbdc68]+0x18f0]` мс (пишет 0x921780: разность двух `nvnCommandBufferReportCounter` TIMESTAMP / 1e6); цель 31,5 мс
  (main+0x1f1237c): GPU > 31 мс → шаги вниз, пока модель «0,5 + 0,5 × площадь» не влезет, не ниже min; прогноз шага вверх < 30 мс
  (`fmov #30` в 0xc05e48) 1000 мс подряд → один шаг вверх, не выше max. Наш NVN отдаёт настоящие GPU-метки (hle_nvn.c), значит
  контроллер на телефоне живой. Режим «портатив» включает apm-буст (0x92220008 / 0x20004) — на хосте это HLE, не важно.
- **Фиксированное качество.** graphicSettings.lsx (0xa687e0 → 0xa6d130) читает только гамму / HDR; затем 0xa6d400 пишет консольные значения
  в GraphicSettings (0x88 байт, ctor 0xa46d00, указатель main+0x2bc0278; имена — таблица ключей PC 0x8b3d00 + читатели): VSync 1,
  VSyncDivider 2 (30 fps), TripleBuffering 1, TextureDetail 0, TextureFiltering 1, TextureStreaming 1, ModelDetail 2, LightingDetail 2,
  +0x64 = 1 (на PC 2; выбирает шейдер пост-стадии 0xab6f80 — похоже на тип AA), Bloom 1, DOF 0, GodRays 0, LensFlare 0, MotionBlur 0,
  SSAO 0 (+0x6d, читает 0xac5ebc), +0x71 = 0 (много читателей 0xcb8f4c.., 0xd031d0..), +0x72 u16 = 30. Байт ShadowsEnabled / ShadowQuality
  не доказан (+0x6f читают 0xa4cee0 / 0xa69438) — поэтому `DM_DOS2_SHADOWS` в этом шаге нет.
- **Переключатели:** `DM_DOS2_DRS` не задан / `0` — DRS игры (75–100 %); `off` / `100` — без DRS (0xc05db4 `cbnz` → `b 0xc05ef8`, всегда полное
  окно); `<min>:<max>` — min в % окна (50..99, ближайшее значение `fmov`-immediate, шаг 1/32) и потолок max (50..100), который держит
  хост-поток 4 Гц (текущий размер прижимается, шаг вверх обнуляется на потолке; объект проверяется по vtable). `DM_DOS2_DRS_LOG=1` — строка
  раз в 10 с (размер, масштаб, GPU мс); первое появление объекта логируется всегда. `DM_DOS2_DRS_MON=0` — без потока (потолок не держится).
  `dm_dos2_drs_set_ceiling(pct)` — точка для термо-лестницы (NATIVE_PLAN.md § 6). Код сверяется слово в слово: другой билд — ничего не меняем.
- **Сборка:** `cmake --build work/build-android --target dm` — без ошибок и новых предупреждений.
- **Проверить на телефоне:** (1) в логе `[quality] console fixed settings`, `game DRS`, затем `DRS object up: max 2000x900 (или окно wide),
  min …, current …`; (2) `DM_DOS2_DRS_LOG=1` в кампании: как часто и насколько DRS уходит вниз при 30 fps и какое GPU мс; (3) `DM_DOS2_DRS=off`
  — резкость против fps/тепла; (4) `DM_DOS2_DRS=60:85` — потолок держится (`current` ≤ 85 % высоты), картинка без мусора по краям
  (масштаб применяется со следующего кадра); (5) ничего не падает при смене локации (объект +0x48 на загрузке).
- **Осталось:** чистый потолок патчем кода (вместо записи данных потоком; точки — `scvtf s7,w11` 0xc05de8 / `scvtf s2,w12` 0xc05df8 и
  0xc06458..0xc06480, место для двух int — увеличить объект `mov w0,#0x50` в 0x8a9a58); семантика байтов теней
  (+0x6f / +0x70 / +0x71, ShadowQuality) — по читателям в рендере или по замеру; цель 31,5 мс под 60 fps (константа общая — менять только
  через свою копию); термо-лестница (NATIVE_PLAN.md § 6).

### Шаг 2 (14.11, ветка `dos2-drs` после pack 1g; собрано, на телефоне не проверено)
- **Цель DRS под реальный fps.** Игра считает цель 31,5 мс / порог шага вверх 30 мс (под VSyncDivider 2 = 30 fps), а хост выдаёт 60 Гц
  (режим display в hle_nvn.c режет интервал 2) — на 60 fps DRS игры бесполезен (GPU никогда не > 31 мс). Теперь обе загрузки смотрят в
  **нашу копию**: страница данных 16 КБ рядом с main (mmap с подсказкой «сразу за модулем», проверка дальности adrp ±4 ГБ), патчи слово в
  слово: 0xc05e04 `adrp x11,0x1f12000` → `adrp x11,<наша страница>`, 0xc05e08 `ldr s18,[x11,#0x37c]` → `ldr s18,[x11]`, 0xc05e48
  `fmov s5,#30.0` → `ldr s5,[x11,#4]` (x11 после этого мёртв; константа main+0x1f1237c не трогается, у неё один читатель).
  `DM_DOS2_DRS_TARGET_MS`: не задан — бюджет кадра хоста = период present × 0,95 (hle_nvn.c `g_nvn_present_period_ns`, пишется в
  `present_period`: DM_PRESENT_HZ / DM_PRESENT_MAX_HZ / fps из ⚙ / ступень экрана), поток следит 4 Гц (60 Гц → 15,83 мс, 30 Гц → 31,67 мс),
  до первого present — 31,5 мс; `<мс>` — фиксированно (4..100); `0` — цель игры. Порог шага вверх = цель × 30 / 31,5. При `DM_DOS2_DRS=off`
  цель не патчится.
- **Чистый потолок патчем — не влез.** Нужен W × c в зажимах 0xc05de8 / 0xc05df8 и 0xc06458..0xc06480, а в 9 инструкциях там нет места на
  загрузку и два умножения; без code cave не сделать. Потолок по-прежнему держит поток (запись данных объекта, проверка vtable).
- **Термо-лестница `DM_DOS2_THERMAL=1`** (по умолчанию 0), только ступень «потолок DRS»: headroom из perf_android.c (`dm_thermal_headroom()`
  = `AThermal_getThermalHeadroom(10 с)`, weak — на хосте без него лестница пишет «idle»), опрос 1 Гц, EMA по 3; ≥ 0,7 / 0,8 / 0,9 → потолок
  85 / 75 / 65 % (не ниже min DRS); вверх по ступени — после 2 опросов над порогом, вниз — после 20 с ниже порога − 0,1, не чаще раза в 10 с;
  каждая смена — строка `[quality] thermal ladder stage a -> b: headroom …, DRS ceiling … %, current …, GPU … ms`. Итоговый потолок =
  min(max из `DM_DOS2_DRS`, потолок лестницы).
- **Проверить на телефоне:** (1) `[quality] DRS target: our copy at 0x…` и затем `DRS target 15.83 ms GPU (present period 16.67 ms …)`;
  (2) с `DM_DOS2_DRS_LOG=1` в тяжёлой сцене DRS теперь опускается, когда GPU > ~15,3 мс, и fps держится; (3) `DM_DOS2_DRS_TARGET_MS=0` —
  прежнее поведение (сравнение); (4) `DM_DOS2_THERMAL=1` в долгой сессии — строки смены ступеней и что потолок отпускается после остывания.

### Шаг 3 (14.11, ветка `dos2-drs` после fff7a74; собрано, APK собран, на телефоне не проверено)
- **Чистый потолок DRS — code cave.** 0xc05e18..0xc05e24 (`fcmp s17,s7` / `fcsel s19` / `fcmp s4,s2` / `fcsel s20`: зажим шага вверх по max;
  ни одна ветка не попадает внутрь — проверено по всем b/bl/b.cond/cbz/tbz main) → `ldr x16,#8; br x16; .quad cave`. Кейв (своя RX-страница):
  `ldr x16,=page; ldr s22,[x16,#8]; fmul s7,s7,s22; fmul s2,s2,s22`, четыре исходные инструкции, возврат на 0xc05e28 (x16 и v22 в
  контроллере 0xc05c80.. не используются вовсе). Множитель — `page[2]` (= min(max из `DM_DOS2_DRS`, потолок лестницы) / 100). Шаг вверх
  упирается в потолок; если потолок опустили ниже текущего размера, следующий «шаг вверх» (он дешевле) садится ровно на него (0xc06458
  берёт min). **Поток больше ничего не пишет в объект DRS** — только читает (лог, нижняя граница для лестницы) и пишет нашу страницу
  (цель, множитель). Кейв ставится, только если потолок может понадобиться (`DM_DOS2_DRS` max < 100 или `DM_DOS2_THERMAL=1`).
- **Тени — доказано: GraphicSettings +0x50 = ShadowQuality.** Направленная ShadowStage (init 0xb15960, re-init 0xb16a58) берёт q = min(+0x50, 2)
  и таблицы main+0x1f58d92 u16 {1024, 768, 512} (размер карты каскада; атлас ×2 — `lsl` в 0xb16738) и main+0x1f58d98 u32 {2, 3, 3}
  (поля стадии +0x340 / +0x2e0). **Каждый кадр** 0xb16604..0xb16614 сравнивает q стадии (+0x2f0) с +0x50 и при отличии пересоздаёт карты
  (0xb16714) — смена на лету — штатный путь игры (меню PC). Консоль пишет 1 (768); PC-дефолт 0 (1024).
  +0x64 = AntiAliasing (0xab6d4c: `PPFXAA.bshd` / `PPDefault.bshd`; консоль 1 = FXAA). +0x6f (PC 1) читается каждый кадр 0xa69410 (из 0xc072a0) и
  0xa4cee0 / 0xa4d1b0 — по порядку ключей это ShadowsEnabled, но читатели тени не называют → **не выставлен**. +0x70 — читателей нет, +0x71 —
  много (0xcb8f4c.., 0xd031d0..), смысл неизвестен.
- **`DM_DOS2_SHADOWS=0/1/2`** — ShadowQuality при загрузке: 0 = 1024 × 2 (PC), 1 = 768 × 3 (консоль, по умолчанию), 2 = 512 × 3. Патч
  0xa6d444 `str w9,[x8,#0x50]` → `str wzr` / `str w10` (w10 = 2 в той же функции), слово сверяется.
- **AntiAliasing +0x64 — доказано статикой (NB-18b-1, 14.11, ветка `dos2-nb18b`; телефон не трогался).** Пост-шаг «FXAA» — объект 0x50 байт,
  vtable main+0x2998280 (релокации RELATIVE: +0x10 Load 0xab6d30, +0x20 Enable 0xab6e10, +0x38 Execute 0xab6e90, +0x40 / +0x48 prev / next
  0xab4110 / 0xab4120, +0x50 «отрисует ли кто-то дальше» 0xab4130 = свой +0x30 или следующего). Строится в цепочке пост-шагов 0xab1xxx
  (0xab13e4; порядок вставки: …, LUT 0xab54e0, **FXAA**, Vignette vtable main+0x29982f8; связи — vt+0x48 / vt+0x40 по порядку массива;
  `EoCSight` 0x1424494 — из слоя EoC, в каждом кадре P2 стоит между LUT и FXAA). `CombineUI*` (0xb0a77c) — не шаг цепочки, а сведение
  рендерера.
  - **Load 0xab6d30:** +0x38 = `PPFXAA.bshd` (0xab6d48), +0x40 = `PPDefault.bshd` (0xab6d68), +0x48 = оба загружены.
  - **Enable 0xab6e10 (каждый кадр):** шаг включён (+0x30 = +0x48), если float `[[[ctx+0x10]+0x50]+0x2c] != 1.0` (по всей видимости, масштаб
    рендера; в P2 DRS 0,9) **или** +0x64 == 1 (0xab6e34); иначе +0x30 = 0. Затем — Enable следующего.
  - **Execute 0xab6e90:** выключен → Execute следующего. Включён и +0x64 ≠ 1 (0xab6ec8) и следующий «отрисует» (vt+0x50) → **хвостовой вызов
    Execute следующего с теми же аргументами: своего прохода нет** (Vignette читает выход Sight). Иначе — свой полноэкранный проход, шейдер
    `+0x64 ? PPFXAA : PPDefault` (0xab6f80 `cbz`).
  - **Итог для +0x64 = 0:** кадр с виньеткой (тяжёлые кадры кампании) — **прохода FXAA нет вовсе** (−1 проход 2160x972, ≈ −16,8 МБ и ALU
    FXAA); кадр без виньетки (меню, лёгкие кадры) при масштабе ≠ 1 — **копия `PPDefault`** (fs `7b3210191abd`, проход остаётся → её снимает
    псевдоним NB 18b-5); при масштабе 1 (`DM_DOS2_DRS=off` во весь экран) — прохода нет. Поправка к NB-18a п. 2 («проход остаётся» — только
    без виньетки).
  - **Другие писатели +0x64:** 0xc099c8 (применение карты опций, слот vtable main+0x29a6600 → 0xc08630: значение = байт render config
    `[[main+0x2bb8a60]+0x628]` или ключ карты `[main+0x32be528]`; заодно пишет cfg+0x628) и 0x113f848 (синхронизация UI: пишет, когда его
    байт +0x1d9 ≠ cfg+0x628). Оба могут вернуть 1, поэтому поток монитора стережёт +0x64.
- **`DM_DOS2_AA=0|1`** (`hle_dos2_quality.c`) — AntiAliasing при загрузке: 0 = патч 0xa6d434 `str w9,[x8,#0x64]` → `str wzr,[x8,#0x64]`
  (0xb900651f; слово сверяется в `k_fixed`, а также три читателя 0xab6e34 / 0xab6ec8 / 0xab6f80 = `ldr w8,[x8,#0x64]`), плюс поток монитора
  обнуляет +0x64, если прочитал не 0 (строка `【红】… cleared (n)`, до 8 раз, с cfg+0x628) и пишет `[quality] AntiAliasing = …` при смене;
  1 / не задано = FXAA консоли (без патча). cfg+0x628 не трогается (иначе синхронизация UI 0x113f848 начнёт писать). Умолчание — консольное.
  **Телефон (P5):** в `PGRAPH` кампании нет прохода `fs=691768aa` между Sight (`91eb2d97`) и Vignette (`43e8e8ed`), `passes` −1; в меню
  p33 = `fs=7b3210191abd`; `[nb] post: chain LUT>Sight>Vignette>…` / `LUT>Sight>PPDefault>…` (NB-18b-2); строк `cleared` нет.
- **Bloom +0x68** читается каждый кадр включателем пост-шага (0xab4680: step+0x30 = камера+0x2a9 && Bloom && step+0x80; ещё 0xab50b0) —
  переключение на лету только пропускает / возвращает проход.
- **Термо-лестница** (`DM_DOS2_THERMAL=1`): ≥ 0,7 → потолок 85 %; ≥ 0,8 → 75 % + ShadowQuality 2 (если было меньше); ≥ 0,9 → 65 % + Bloom 0;
  спуск по ступеням восстанавливает найденные значения. Запись в GraphicSettings — только после проверки vtable (main+0x29946e8). При
  `DM_DOS2_DRS=off` лестница работает без потолка (тени / bloom).
- **⚙ (Tl2Gear, только DOS2):** «Термо-режим (снижать качество при нагреве)» / "Thermal mode (lower the quality when the phone heats up)",
  по умолчанию выкл., применяется со следующего запуска (`DM_DOS2_THERMAL=1`; выключенный тумблер ничего не ставит — env-файл по-прежнему
  работает). Строки ru / en.
- **Сборка:** `dm` без ошибок и новых предупреждений; `tools/build_apk.ps1` → `work/DOS2-port-debug.apk` (21,3 МБ) — Java собрана.
- **Проверить на телефоне:** (1) `[quality] DRS ceiling: code cave at …` при `DM_DOS2_DRS=60:85` или `DM_DOS2_THERMAL=1`, игра не падает в
  первом кадре с DRS; с `DM_DOS2_DRS_LOG=1` высота не выше 85 %; (2) `DM_DOS2_SHADOWS=0` / `2` — резкость теней и GPU мс против 1;
  (3) лестница: строки `thermal ladder stage …`, на ступени 2 тени перестраиваются без мусора/падения (смена ShadowQuality на лету),
  на 3 — bloom пропадает и возвращается при спуске; (4) тумблер в ⚙ появляется только у DOS2 и включает лестницу (строка `DM_DOS2_THERMAL=1`).
- **Осталось:** доказать +0x6f (ShadowsEnabled?) и точечные тени; ступень «лимит fps» лестницы (на хосте); KB-записи R1/R2.

## Частицы: полуразрешённая offscreen-цепочка → полный размер в основном проходе, `DM_DOS2_PARTICLES_FULL` (14.11, ветка `dos2-particles`; собрано, на телефоне не проверено)
Кандидат 1 из `TILE_SHADER_CANDIDATES.md`. Найдено статикой (main.raw 4979b200, capstone + Ghidra headless), правка в
`dm/host/hle_dos2_quality.c`.
- **Переключатель движка** — байт `[[main+0x2bb8a60]+0xc7]`. Это глобальный объект конфигурации рендера, где лежит и гейт DRS `+0x6a0`;
  в GraphicSettings (`[main+0x2bc0278]`, 0xa6d400) этого переключателя **нет**, и платформенной константы тоже нет. Флаг ставит игра сама.
  Есть ровно два писателя (`strb #0xc7` через этот глобал; всего в main шесть `strb …,#0xc7`, четыре других пишут в стек или другой объект):
  - `0x104cd4c strb w8` в обработчике загрузки уровня (`ClientLoadLevel`, функция 0x104c960): `w8 = (уровень != [[client+0x70]+8])`.
    То есть флаг включён во всех уровнях, кроме одного особого, а какой это уровень — не выяснено (тот же объект сравнивается рядом,
    0x104ccf4, для TextureDetail `+0x34`).
  - `0xd3e268 strb wzr` при загрузке меню (`LoadMenu`).
- **Читатели.** Оба читают флаг, когда он равен 0 — путь как на iPad:
  - Отсечение `0xb0c9b4` (функция 0xb0c570): альфа-объект с флагами `& 0xa00` (частицы) при флаге ≠ 0 уходит в
    `CullStage::m_OffscreenAlphaObjects::*` / `m_OffscreenPostRefractionAlphaObjects::*`, при 0 — в обычные `m_AlphaObjects`.
    Ещё четыре обходчика проверяют то же `0xa00` + флаг: 0xb7e0dc, 0xc6cd04, 0xc7f700, 0xc82e5c.
  - Альфа-стадия: vtable `main+0x299b6c0`, id 13, Init 0xb1d290, Execute 0xb1d6a0. В точке `0xb1ddc4` (`ldrb w8,[x8,#0xc7]; cbz → 0xb1e3c0`)
    при 0 она пропускает **всю** цепочку:
    1. clear HalfResDepth (`+0xc0`, 0.5x) и `DownsampleDepthMax.bshd` (`+0xd0`, пишет глубину);
    2. `LinearizeDepth.bshd` (`+0xe0` / `+0xe8` по `camera+0x92`) в HalfResLinearDepth (`+0xc8`, 0.5x, формат 5);
    3. clear OffscreenParticlesBuffer (`+0xb8`, 0.5x, формат 0x14) и offscreen-списки рендерера `+0x24c0` / `+0x2c80` на ½ с HalfResDepth;
    4. `CrossBilateralUpsample.bshd` (`+0xd8`) обратно в HDR (`+0xa8`).

  При флаге 0 частицы рисуются вместе с остальной альфой (`renderer+0x1480`) в полном разрешении в основном проходе. Так устроен
  iPad: в его бинаре нет ни строк `m_OffscreenAlphaObjects`, ни `Offscreen Particles` / `Downsample Depth`, только `m_AlphaObjects` и
  `m_PostRefractionAlphaObjects`. Init по-прежнему создаёт полуразмерные цели (несколько МБ памяти), но не рисует в них. Имена
  HalfResLinearDepth / OffscreenParticlesBuffer по строкам больше никто не читает: кроме static init / dtor есть только таблица целей
  0x913bf0 / 0x913cb8.
- **`DM_DOS2_PARTICLES_FULL=1`**:
  - сверяет слова 0x104cd3c..0x104cd4c, 0xb1ddbc, 0xb0c9b4, 0xd3e268 (другая сборка — ничего не трогает, красная строка) и меняет
    `0x104cd4c strb w8,[x9,#0xc7]` (0x39031d28) на `strb wzr` (0x39031d3f). Флаг остаётся 0 во всех уровнях, решение принимает сам движок;
  - поток монитора (если не `DM_DOS2_DRS_MON=0`) пишет `[quality] offscreen-particles flag … = 0` при первом чтении и при каждой смене.
    Если флаг вдруг ≠ 0 (пропущенный писатель), монитор обнуляет его не больше 8 раз, каждый раз красной строкой.

  Умолчание 0 — консольная цепочка, пока не проверено на телефоне. При старте всегда одна строка `[quality] particles: …`.
- **Прочая консольная цена того же блока.** Блок GraphicSettings (0xa6d400) частиц не касается; его цены уже разобраны выше
  (ShadowQuality 1 = 768 x 3 каскада, `DM_DOS2_SHADOWS`; Bloom; FXAA). Другой консольной ветки на этом флаге не найдено. Соседние
  байты глобала `+0xc2..+0xcb` читают несвязанные места (не разбирались).
- **Проверить на телефоне** (бэкап `files/save` перед force-stop; один переключатель, ABBA; Форт Радости днём + бой с огнём / ядом / магией):
  1. В логе `[quality] DM_DOS2_PARTICLES_FULL=1: particles at full resolution …` и `offscreen-particles flag … = 0` после загрузки уровня.
     Красной строки `… read 1 … cleared` быть не должно.
  2. `DM_GFX_PASS_GRAPH=1` без флага и с ним, в одном месте: `summary passes=` должно упасть на 4–5. Из графа должны уйти проходы
     размером ½ от кадра: DownsampleDepthMax с записью глубины, LinearizeDepth, проход частиц на ½ с разрывом по чтению глубины
     (`read#N`) и полноэкранный upsample в HDR. В `imgs` полуразмерные картинки больше не пишутся в кадре.
  3. Вид: огонь, дым, магия, туман, капли должны стать резче, без ореолов билатерального апскейла по краям персонажей. Смотреть
     мягкие частицы на пересечении с землёй / стенами: нет ли жёстких срезов или пропавших частиц. Частицы за преломлением
     (вода / стекло) — на месте ли, ведь PostRefraction-списки тоже переходят в обычные.
  4. Цена: `DM_DOS2_DRS_LOG=1` (GPU мс каждые 10 с) или `DM_GFX_PASS_PROF` + `tools/gpass_report.py` и fps в тихой сцене и в
     сцене с большим огнём / дымом. Ожидается −4…5 проходов и ~36 МБ DRAM / кадр, но overdraw частиц теперь ×4 пикселя. В большом дыму
     на весь экран полный размер может оказаться дороже — тогда оставить 0 (или включать только на сильных GPU).
- **Риски:** мягкие частицы в основном проходе читают полную глубину так же, как обычная альфа, — но это не проверено на картинке.
  Особый уровень с флагом 0 у консоли (сравнение в 0x104cd38) не опознан.

## Туман / облака: точки вдоль сетки поверхностей и жёсткие срезы — footprint `textureGather`, `DM_GFX_GATHER_SNAP` (16.11, ветка `dos2-fog`; собрано, на телефоне не проверено)
- **Симптом (хозяин, 16.11, Adreno 750 / Turnip, палуба корабля):** белые облака пара клубятся, но (1) вдоль прямых линий на полу и у
  контура облака лежат одиночные яркие точки / штрихи пунктиром («прозрачность обрезана криво»), в полуразрешённой цепочке частиц их
  много, с `DM_DOS2_PARTICLES_FULL=1` почти нет; (2) облако жёстко срезано там, где встречает пол, перила, лестницу. Снимки
  `work/shots/fog1116*.jpg`, `fog_cmp.jpg`, логи `work/logs/ab0base.log`, `ab1pfull.log`.
- **Что рисует облако.** По логам загрузки это `RS3_Cloud_Steam_STI_FOR_NX` (ps `ca543129dbfa8541`) и тень `RS3_Surface_CloudShadow`
  (`a8b5623fd08b69f4`), не эффект-частицы. Все материалы поверхностей и облаков (`RS3_Surface_*`, `RS3_Cloud_*`, 396 фрагментных
  модулей из 6192) устроены одинаково (разбор SPIR-V `ca543129…` / `c68871db…`):
  1. карта поверхностей уровня (`fp_t_tcb_C`, слот 2; R, G — id типа поверхности в двух слоях, B, A — плотность) читается шестью
     `textureGather` в одной точке: компоненты 0 и 1 — id четырёх текселей, сравниваются с `floor(c3[0].z)`; если ни один из восьми
     не совпал — `OpKill`; компоненты 2 и 3 — плотности;
  2. четыре плотности шейдер смешивает сам: веса `fract(fma(u, c3[0].x, -0.498046905))` (и то же по v). Константа — это
     `-(0.5 - 1/512)` (`0xbeff0001`): **Maxwell округляет координату до 1/256 текселя и берёт footprint gather от
     `floor(u * size - 0.5 + 1/512)`**, шейдер Switch подогнан под это;
  3. мягкое пересечение с геометрией: `alpha *= clamp((t_12(gl_FragCoord.xy / c15[13].zw).x * c15[13].x - viewZ) * 4, 0, 1)`, где
     `fp_t_tcb_12` (слот 5) — линейная глубина из `LinearizeDepth` (R16 unorm, нормирована; в ½-цепочке — HalfResLinearDepth),
     `viewZ` — выход VS Location 5 = `dot(c15[2], world)`. Затухание на 0.25 единицы мира.
- **Причина точек (1) — уверенность высокая, телефон подтвердит.** На GPU, который выбирает footprint gather по другой дроби
  (Adreno / Turnip; так же AMD / Intel у настольных эмуляторов — yuzu добавляет им сдвиг 1/512), в полосе шириной ≈ 1/512 текселя у
  каждой границы текселя шейдер считает вес для текселя k+1 ≈ 1, а gather вернул тексель k. Карта поверхностей — 0.5 м на тексель,
  поэтому полоса — прямые линии мировой сетки шириной ≈ 0.1–0.3 пикселя: пиксель попадает в неё изредка → пунктир. Ошибка равна
  разнице соседних текселей, то есть видна на границе облака; там же сравнение id даёт лишний `OpKill` (тёмная точка). В ½-цепочке
  точка занимает 2x2 пикселя и размазывается `CrossBilateralUpsample` — поэтому там их «много». Счёт в `tests/spirv_gather_test.cpp`:
  без правки footprint не совпадает с шейдерным в полосе 1/512, с правкой — 0 расхождений из 3.2 млн.
- **Что с жёсткими срезами (2) — статикой не доказано.** Проверено и исключено: support buffer (render scale = 1.0, `xbg_support_defaults`),
  origin (`SetWindowOriginMode(1)`, `gl_FragCoord` = строка памяти), формат линейной глубины (0x6 R16 → `VK_FORMAT_R16_UNORM`, как у
  Switch), порядок проходов (`LinearizeDepth` читает D24S8 после G-buffer через разрыв прохода; частицы читают не вложение, а отдельную
  R16), нативный `LinearizeDepth_mobile` (та же формула), альфа-тест (GREATER 0.5 приходит только на `Base_*_DEP`, не на облака),
  `CrossBilateralUpsample` и `DownsampleDepthMax` (точки выборки в середине текселя / ровно в углу 2x2 — от правила округления не
  зависят; сэмплер должен быть NEAREST — это задаёт игра). Часть «срезов» законна: перила перед облаком закрывают его по глубине;
  затухание у игры всего 0.25 м. Что остаётся под подозрением: содержимое карты поверхностей (плотность в B, A) и значения `c15[13]`
  — это видно только захватом (ниже).
- **Фикс:** `dm/gfx/vulkan/engine/xbg_spirv_gather.{h,cpp}` — перед каждым простым 2D `OpImageGather` (без image operands) координата
  заменяется центром footprint Switch: `(floor(fma(coord, size, -0.498046905)) + 1) / size`, `size = OpImageQuerySizeLod(…, 0)`. В
  центре дробь у железа 0.5 — любое правило округления даёт те же четыре текселя, что Maxwell; веса шейдера не трогаются. Вызов
  `gather_snap_rewrite` стоит в обеих цепочках рерайтов (`load_shader` и `warm::bundle_words`) сразу за `gather_ext_rewrite`.
- **Переключатель `DM_GFX_GATHER_SNAP`:** 1 (умолчание в сборке `DM_GAME_DOS2`) — только модули с константой весов `0xbeff0001` /
  `0xbeff0000` (396 модулей, 2196 gather); 2 — все модули с gather (ещё `PPFXAA`, `DownsampleDepthMin/Max`); 0 — выкл. (умолчание в
  остальных портах). Логи: `NVN gather footprint snap DM_GFX_GATHER_SNAP=1 (default)…` при первой загрузке шейдера и
  `NVN gather footprint snap: shader=<key> N gather(s)…` (первые 4 модуля и на степенях двойки).
- **Диагностика:** `dm/gfx-capture-now` принимает третий параметр — начало ключа фрагментного шейдера: `"<skip> <max> <ps>"` захватывает
  только draw с этим шейдером (clear и прочие draw не тратят события); `skip` считает совпавшие draw.
- **Проверено на хосте:** `tests/spirv_gather_test.cpp` (clang++ 21: арифметика + все бандлы `work/shaders-vk`) — 396 модулей
  переписаны, 0 пропущенных gather, все 396 проходят `spirv-val --target-env vulkan1.1`. APK `wt-fog/work/DOS2-port-debug.apk` собран (`tools/build_apk.ps1`, arm64, DM_GAME=dos2, без новых предупреждений), не ставился.
- **Проверить на телефоне** (палуба корабля, облако пара; бэкап `files/save` перед force-stop; цепочка частиц по умолчанию — там точки
  виднее всего):
  1. Сборка `dos2-fog` без env-строк: в логе `DM_GFX_GATHER_SNAP=1 (default)` и строки `gather footprint snap: shader=…`. Видео / снимок
     с того же места, что `fog1116c.mp4`: пунктира из точек вдоль линий на полу и у контура облака быть не должно.
  2. Контроль `DM_GFX_GATHER_SNAP=0` в `dm/env`: точки возвращаются. Если разницы нет — Adreno округляет как Maxwell, причина точек
     другая (тогда п. 3 с `skip` по кадру, где точка видна).
  3. Если жёсткий срез у пола остался: при облаке в кадре создать `dm/gfx-capture-now` с содержимым `0 2 ca543129`, через секунду
     забрать `dm/capture/` (capture.txt + данные, < 64 МБ). Решают: `BUFFER … slot=12` (`c15[13]`: x — множитель глубины, z, w —
     размер цели: должен равняться размеру цели прохода), `TEXTURE` слота 5 (должна быть R16 `LinearizeDepth` того же размера),
     пиксели карты поверхностей (слот 2: B, A должны плавно спадать к краю облака; если там только 0 / 255 — край жёсткий по данным
     игры, не по хосту), `SAMPLER` слотов.
- **Риски:** +7 инструкций на gather (6 gather на облачный пиксель, `textureSize` и арифметика сворачиваются компилятором драйвера в
  один расчёт); первый запуск после обновления пересоберёт пайплайны 396 модулей (кэш драйвера по содержимому модуля). Если карта
  поверхностей когда-нибудь читается не с уровня 0 или её размер не равен `c3[0].xy`, центр footprint считается от настоящего размера
  уровня 0 — это вернее, чем у шейдера, но расходится с его весами так же, как на Switch.

### Черновик для KB (shaders-translation.md)
```
### SH-DOS2-03 textureGather: footprint Maxwell (округление до 1/256 текселя) против других GPU
- **Симптом:** одиночные яркие / тёмные точки пунктиром вдоль прямых линий мировой сетки у края облаков и поверхностей (DOS2: пар,
  дым, кровь, масло…), в полуразрешённой цепочке частиц крупнее; «прозрачность обрезана криво».
- **Устройства/GPU:** Adreno 750 / Turnip (16.11); ожидается на любом не-NVIDIA GPU. DOS2; искать в других играх по константе
  `-0.498046875` / `0xbeff0001` рядом с gather.
- **Причина:** Maxwell берёт footprint gather от `floor(u*size - 0.5 + 1/512)`; шейдеры игры считают билинейные веса под это
  (`fract(fma(u, size, -(0.5 - 1/512)))`). Другое правило округления → в полосе 1/512 текселя веса и тексели не совпадают.
- **Фикс:** рерайт SPIR-V `xbg_spirv_gather.cpp`: координата gather → центр footprint Switch
  `(floor(fma(coord, size, -0.498046905)) + 1) / size`. `DM_GFX_GATHER_SNAP=1` (DOS2: модули с константой весов), 2 — все gather,
  0 — выкл. (умолчание вне DOS2).
- **Эталон:** DOS2 `dm/gfx/vulkan/engine/xbg_spirv_gather.{h,cpp}`, `gather_snap_rewrite` в `xbg_game.cpp`, `tests/spirv_gather_test.cpp`,
  ветка `dos2-fog`.
- **Проверка:** собрано, spirv-val 396/396; на телефоне не проверено.
- **Issues:** —
```

## 14.11 (ветка dos2-native, второй заход): кэш состояния рендерера, счётчик кодов, draw одной записью
- `dm/host/hle_dos2_native.c` (новый, ставится из `main.c` рядом с wide / quality; сверка слов main, трамплины как в `hle_tl2_wide.c`):
  `DM_NATIVE_RC=2` — счётчик на `ExecuteRenderCommand 0x93e5f0`; `DM_NATIVE_DRAW=1` — записи одного draw движка одной записью
  `OP_X_NATIVE_DRAW`; `=2` — сверка кэша текстур рендерера с привязками NVN. Умолчания: оба выключены (хуков нет).
- `dm/gfx/nvn/hle_nvn.c`: `nvn_native_draw_begin/_end/_stats`, сбор записей в `cmd_alloc`, `nd_check`; `nvn_obj.h`: `OP_X_NATIVE_DRAW`,
  `OP_X_NATIVE_CHECK`, `p_native_check`; `xbg_game.cpp`: исполнение обеих записей.
- Поля кэша, оценка пользы и крючки 5b — `NATIVE_PLAN.md` §1.7, §3.6. Собрано (`dm`, arm64), на телефоне не запускалось.
- На телефоне, по порядку: (1) `DM_NATIVE_RC=2` — строка `[native] render commands`: число потоков (ждём 1), `length differs … in 0`, какие коды
  главные; (2) `DM_NATIVE_DRAW=2` — `[native] draws …: X host records and Y bytes a draw` и `[native] draw check` (ждём 0 расхождений; если есть —
  стадия / слот и в какой сцене); (3) `DM_NATIVE_DRAW=1` — картинка та же, `parts_ms` потока трансляции против `=0` (ожидание: без разницы).


## 720p по умолчанию (14.11.2026, хозяин)
Switch-версия (Blitworks) рендерит с DRS 495–720p; вместо поднятия разрешения — тот же 720p по высоте и шире кадр (Hor+): на 2400x1080
окно 1600x720 (док = портатив). Опция ⚙ «Высокое разрешение 900p» (`dos2_res_900`, следующий запуск) — 2000x900, портатив 1600x720.
Java: `DmActivity.dos2Base` / `dos2WideSize`, `Tl2Gear.dos2Res`, `aspectEnv` (форма «16:9» при 720p → `DM_DOS2_WIDE=1280x720`);
хост: `hle_dos2_wide.c` `DM_DOS2_RES=720|900` для авто-размера, при `<W>x<H>` с H ≤ 720 портатив = док. Пак 1j (0.0.11).

## 1080p: точечные огни пропадают — кластерный буфер на 1920x1080 (14.11.2026, ветка `dos2-hires`; собрано, на телефоне не проверено)
**Симптом (хозяин, 14.11):** окно 2400x1080 (⚙ «1080p») — нет тёплых точечных огней, свечения факелов, подсветки фрески в главном
меню («будто выключили свет»); 2000x900 — всё есть. DRS ни при чём (в плохом прогоне патч DRS даже не встал).

**Логи (q2 900p против l1 / q3 1080p).** Все render target-ы идут от окна одинаково: 3D-цели 0,9 × окна (1800x806 / 2160x972 /
2160x979), полу-/четверть-цепочка bloom — тоже; ни одной цели фиксированного размера. Значит, ломается не размер текстуры.

**Причина (статика, main.raw 4979b200).** Точечные огни — `ClusteredLightingWithSSAO.bshd`, сетка кластеров строится на CPU каждый кадр
(стадия: vtable main+0x299b4f0, ctor 0xb19820):
- SetSize vt+0x50 = **0xb19e50** (w, h = `[[arg+0x20]+0x20/+0x24]` из Init vt+0x18 = 0xb19ad0): тайлы 128 px, +0xe4/+0xe8 =
  ceil(w/128) × ceil(h/128), +0xe0 = тайлы × 32 среза глубины, CPU-массив +0xa8 по 8 байт на кластер; +0xdc = 0x10000 (списки индексов).
- GPU-копия «ClusterInfoBuffer» (+0x90) — function-static буфер, создаётся **один раз фиксированного размера**: **0xb19d04
  `mov w1,#0x8700`** = 34 560 байт = 4320 кластеров = 15 × 9 тайлов × 32 = ровно 1920x1080 (потолок Switch, DRS max). Шейдер объявляет
  тот же блок: рефлексия GLSLC `ClusterInfoBuffer` 34 560 байт, `ClusterInfoBuffer_[4320]` uvec2, FS binding 9.
- Update vt+0x58 = **0xb19f20** копирует кластеры в буфер только если `кластеры × 8 <= размер буфера`, иначе пропускает копию целиком
  → буфер нулевой → ни в одном кластере нет огней. Сетка в остальном динамическая (число тайлов уходит шейдеру float-ами,
  0xb1a988..0xb1a9a4; индекс = tilesX·(tilesY·z + y) + x).
- 2000x896 → 16 × 7 = 112 тайлов (влезает); 2400x1080 (3D 2160x972) → 17 × 8 = 136 (окно — 19 × 9 = 171) > 135 → огни пропадают.
  Порог — не «высота > 900», а число тайлов 128 px > 135 (≈ площадь больше 1920x1080 с округлением тайлов вверх).

**Фикс (`hle_dos2_wide.c` `dw_clusters`, путь DM_DOS2_WIDE, сверка трёх слов 0xb19d00..0xb19d08):** непосредственное 0x8700 →
ceil(W/128) × ceil(H/128) × 256 байт для большего из двух окон (док / портатив), с округлением до 256 и не больше 64 КБ (предел
uniform-блока NVN / Vulkan); если 0x8700 уже хватает — не трогается. 2400x1080 → **0xab00** (171 тайл). Окна больше ≈ 256 тайлов
(например 3200x1440) в 64 КБ не влезут — в логе красная строка.
- `DM_DOS2_CLUSTERS=0` — оставить игровой 0x8700 (A/B: на 1080p огни снова пропадут).
- Привязка: обёртка рендера 0x925984.. берёт размер из записи буфера (+0x30 = размер создания), так что шейдер видит весь новый буфер.
  Кластеры с индексом ≥ 4320 (за объявленным массивом [4320]) — только самый дальний срез глубины при 136 тайлах; Maxwell LDC и наш
  перевод (cbuf как `vec4 data[4096]`, динамический индекс без обрезки) читают их из привязанного диапазона.

**Проверить на телефоне** (⚙ «1080p», бэкап `files/save` перед force-stop): (1) в логе `[wide] clustered lights: ClusterInfoBuffer
0x8700 (135 tiles = 1920x1080) -> 0xab00 (171 tiles …)`; (2) главное меню — подсветка фрески, в локации — тёплый свет факелов, как на
900p; (3) дальние огни (в глубине кадра) не пропадают; (4) `DM_DOS2_CLUSTERS=0` — огни снова выключены (подтверждает причину);
(5) fps против 900p (разница должна быть только от площади).
Лог текстур хоста для таких случаев не нужен: размеры целей видны в строках `NVN read break … fmt=… WxH` и `NVN alias miss`.

## План с iPad-сборкой (IPA хозяин даст позже, 14.11.2026)
Основа остаётся Switch (уже меню + кампания). Из iPad-версии (Larian + Elverils 2021, Metal, тот же движок на тайлере, KB FN-01) взять:
1. тач-интерфейс (UI Iggy / .swf и его логика) — вместо пада там, где это возможно;
2. графику, переделанную под тайловые GPU (слитые проходы, half, пост одним ubershader) — как образец для нативного бэкенда / конвертации;
3. шейдеры MSL вместо транслированного Maxwell, если хорошо переводятся в SPIR-V с нашей раскладкой биндингов (сравнить по ключам материалов:
   в паках Switch уже лежат 744 `_Metal.metal` / `_NX.shd`).
Справочно (iOS → Android вообще): Wicked портирует iOS-сборки (Hades, Cult of the Lamb, Darkest Dungeon, AC Mirage) — метод закрыт; открытые
рантаймы Kreijstal/ios-android-runtime, NastyaKus/iosports (Mach-O + ObjC/Foundation/UIKit HLE, GLES); публичного Metal → Vulkan нет.

## CPU draw(): перенос CPU-стека ETR (14.11.2026, ветка `dos2-cpu`; собрано, на телефоне не проверено)
Задача: меню 2200 draw/кадр, `nvn-translate` ~11 мс/кадр (~5 мкс/draw), GPU 11.5 мс; кампания тяжелее (NATIVE_PLAN §1.7, PERF_AUDIT.md). Цена —
внутри `draw()` `xbg_game.cpp` (пайплайн, дескрипторы, проход), не в NVN-записях. Перенесено из ETR `doom-eternal-port/src-cmp16` (ветка `cmp16`)
то, что не завязано на bindless id Tech 7; умолчание «вкл.» только для `DM_GAME_DOS2` (компиляция), у остальных игр путь прежний.

| Переключатель | DOS2 | Что делает | Эталон ETR | Ожидание |
|---|---|---|---|---|
| `DM_GFX_PERF_SAMPLE` (приложение больше не ставит 0 для DOS2) + `DM_GFX_PERF_BUDGET_US` (200) | 1 из 32 кадров, при оценке > 200 мкс/кадр → 1 из 64 | строки `NVN PERF parts_ms_per_frame … sample= est_cost_ms=` (модель цены выборки A0) и новая `NVN PERF draw_sections_ms_per_frame pre/desc/pipe/vtx/bind/tail` (секции `draw()` между `DRAW_MARK`, только на измеряемых кадрах) | cmp16 A0 (`parts` + cost model), perf4 `DRAW_MARK` (там — только в живом профиле) | замер, не выигрыш; цена ≈ `est_cost_ms` (на измеряемом кадре +1–3 мс, остальные кадры бесплатны) |
| `DM_GFX_DRAW_FAST` | **1** (0 — старый путь, 2 — проверка) | мемо пайплайна (`DM_GFX_PIPE_MEMO=1`, есть с D2R) проверяется **до** сборки списка стадий / vertex input / ~12 CreateInfo; при попадании считаются только `restart_eff` и список VB. `=2`: попадание не берётся, медленный путь идёт без сравнения мемо (полный поиск `pipeline_fast` / `pipeline_lookup`), его пайплайн сравнивается с мемо — `NVN PERF drawfast verify checks= bad=` | cmp16 draw() «fast block» | доля «pipe» в `draw_sections` ↓; ETR: pipememo 2736 хитов / 1.3 промаха на кадр, поиск 0.09 мс |
| `DM_GFX_DYN_STATE` | **2** (только бит 0x2) | POD-карта «ключ мемо → пайплайн» в том же быстром блоке: ловит A-B-A-B (мемо по прошлому draw промахивается). Чистится везде, где `pipeline_fast` (вытеснение, сброс дескрипторов / проходов / пайплайнов), ≤ 65 536 записей. Бит 0x1 (dynamic blend / stencil / bias / line width) **не перенесён**: меняет пайплайны и формат warm-листа — игнорируется с логом | nx2b `20bc37b`, `5f207d1` | `drawfast pod_hits` ≈ промахи мемо; `create`-путь только для новых ключей |
| `DM_GFX_DESC_RECYCLE` | **1** (0 — как было, 2 — проверка) | запись `Descriptors` draw возвращается в free-list в конце `draw()` (тёплая память вместо ~2200 холодных записей до fence слота). Не возвращаются: не последняя в слоте (сабмит внутри draw), с texel view / capture / storage-записью, при capture / provenance. `=2`: дайджест записи в конце draw сверяется при release слота — `NVN PERF descrecycle verify checks= bad=` | xlat X5 `906cc4d` (fast12: −0.27 мкс/draw, verify 3.3M bad=0) | ≈ −0.6 мс/кадр в меню [по ETR на draw] |

**Не перенесено (и почему):**
- `MEMO_GEN` (ключ мемо дескрипторов без обхода биндингов) — нужен поколенческий учёт на каждом bind-опкоде (≈30 мест) + verify; польза у ETR — от
  bindless-источников 9/10/11, которых у DOS2 нет. Решать по `NVN PERF desc_ns_per_frame memo_key=` (если > 1 мс/кадр — переносить).
- `DESC_FAST` — VFRONT/VCLONE опираются на варианты `DESC_REUSE` (в этом дереве нет), LOGCACHE — source-11 ETR, FBLAZY — `fb_begin` ETR. Сначала
  `NVN PERF setcache memo_hits / memo_misses` и `desc_memo_why`: если промахов много из-за другого VkBuffer — переносить `DESC_REUSE`+VCLONE.
- `CPU_FIX`: 0x4 (отчёты пачкой) — у DOS2 `cleanup queries` 0.05 мс; 0x2 (ключ GS-cull) — в DOS2 нет GS-cull; 0x1 (инвалидация снапшотов) — ждать
  `NVN PERF snapfront invalidations=` (у D16 тоже не понадобился).
- `DIAG=0` — в горячем пути DOS2 диагностики нет (crumbs, verify-обходы — выключены; `capture_begin` / `provenance_draw` — по одной проверке);
  единственное, что стоит CPU, — сама выборка `PERF_SAMPLE` (её цену печатает `est_cost_ms`).
- `VB_ONECALL` — у ETR вызовов VB вдвое меньше, fps = 0; `REPORT_FAST`/XLAT X1–X4 — у DOS2 одна эпоха запросов; `DISPATCH_RUNS` — dispatch 0;
  `COND_RENDER` — отдельная задача (NATIVE_PLAN шаг 7b); `PERF_SCOPES` — делит «other» декодера, не `draw()`; `RECXL`/`NREC` — это шаги 5–6 плана.

**Что мерить на телефоне (меню и кампания; ABBA ×2, медиана; бэкап `files/save` перед force-stop):**
1. База (все умолчания): строки `NVN DM_GFX_DRAW_FAST=1`, `DM_GFX_DYN_STATE=2`, `DM_GFX_DESC_RECYCLE=1`, `NVN perf sample cost model: clock read N ns`;
   каждые 60 кадров `parts_ms_per_frame` (draw / descriptor / pipeline / snapshot …, `est_cost_ms` < 0.2), `draw_sections_ms_per_frame`,
   `drawfast hits= pod_hits= pod_misses= pod_size=`, `descrecycle recycled=` (≈ draws), `desc_ns_per_frame`, `setcache`.
2. Проверки (по одному прогону, стоя и в движении): `DM_GFX_DRAW_FAST=2` → `drawfast verify bad=0` (и нет красной строки); `DM_GFX_DESC_RECYCLE=2` →
   `descrecycle verify bad=0`. Картинка не должна меняться.
3. A/B по одному: `DM_GFX_DRAW_FAST=0`, `DM_GFX_DYN_STATE=0`, `DM_GFX_DESC_RECYCLE=0` против базы: `submit_cpu_ms`, `parts draw/pipeline`,
   `draw_sections pipe`, fps. Ожидание суммарно −1…−2 мс CPU в меню [модель]; если `draw_sections` покажут, что основное в `desc` — следующий шаг MEMO_GEN / DESC_REUSE.
4. `DM_GFX_PERF_SAMPLE=0` — проверить, что лишних долгих кадров (> 33 мс) раз в ~0.5 с без замера нет; если есть — поднять `DM_GFX_PERF_SAMPLE=64`.

## Тайловая политика проходов: R3 / R4 / R7 (+ перепись R5 / R6) (14.11.2026, ветка `dos2-tile`; собрано, на телефоне не проверено)
Источники: `docs/port-kb/mobile-gpu-field-notes.md` §3 (FN-10…43) и §5 (R3–R7), FN-01 (iPad-порт DOS2 у Elverils: слитые проходы, явные
load/store), KB `gpu-mali.md` (MALI-03, MALI-10/13), `gpu-powervr.md` (PVR-01/06 и оговорка о twin-проходах), D16 `xbg_vendor_policy.h`
(`TILE_BARRIER`, `NOLOAD`, `STORE_DEAD`). Код: `xbg_game.cpp`, метки `[tile]`; умолчания — `xbg_vendor_policy.h`.

### Что было (статический разбор до правки)
- **Проход = набор RT** (`create_pass`): один `VkRenderPass` на набор вложений, у всех вложений `loadOp/stencilLoadOp=LOAD`,
  `storeOp/stencilStoreOp=STORE`, layout `GENERAL`; зависимости `EXTERNAL→0: ALL_COMMANDS→ALL_GRAPHICS`, `0→EXTERNAL: ALL_GRAPHICS→ALL_COMMANDS`,
  без `BY_REGION`. Depth/stencil сохраняется всегда, даже если его никто не читает.
- **Слияние:** draw с тем же проходом и framebuffer продолжает открытый проход (повторный bind тех же RT — тот же проход, `pass_fast`).
  Проход рвут: чтение картинки, записанной после последнего барьера (`prepare_read`; в сцене с 35 проходами `pass_end_lines L1908=23` —
  следующий пост-проход сэмплит предыдущий, это настоящие зависимости), любая не-draw операция (`Commands(false)`: конец прохода и полный
  барьер `ALL_COMMANDS→ALL_COMMANDS` до и после), смена набора RT.
- **Clear:** полный `ClearColor/DepthStencil` при RT — внутри прохода (`vkCmdClearAttachments`); `DM_GFX_LAZY_CLEAR` (вкл. на Adreno) делает
  из clear в начале прохода `loadOp CLEAR`, если за ним идёт draw; если проход кончается без draw — `begin LOAD + vkCmdClearAttachments`
  (clear-only проход). `nvnCommandBufferClearTexture` (код NVN 38) — вне прохода: конец прохода, барьер, `vkCmdClearColorImage`, барьер.
  Частичный или маскированный clear — отдельный проход с draw-маской.
- **Цифры (o2 boot.log, Adreno 750, меню с 3D-сценой):** `passes=35`, `draws≈2100`, `pass_ends op25=25 op10=3 op26=2 op38=4 op61=1`,
  `lazy_clear folded=1.00 flushed=4.00`, `barriers op25=23 op38=22` — то есть ~11 ClearTexture на кадр (22 полных барьера), 4 из них рвут
  открытый проход, плюс 4 clear-only прохода. Это и есть «3–4 standalone clear» из логов.
- **NOLOAD** (`first_draw_covers`) был, но выключен (`noload=0`); STORE_DEAD / STORE_NONE / TILE_BARRIER / TILE_AREA — нет (только в D16).
- **MSAA:** в логах DOS2 нет вложений с samples>1 и нет resolve; `vkCmdResolveImage` — только путь `Downsample` (OP_X_RESOLVE).
- **Для замеров на Mi 14:** Turnip идёт с `TU_DEBUG=sysmem` — load/store op там ничего не стоят (KB MALI-10). Эффект load/store на Mi 14 не
  измерить; измеримы число проходов, барьеров, CPU и корректность (poison).

### Что сделано
| Переключатель | Что делает | Adreno | Mali (сток, PanVK) | PowerVR | Xclipse (сток, RADV) | прочие |
|---|---|---|---|---|---|---|
| `DM_GFX_CLEAR_FOLD` (R3) | ClearTexture одноуровневого однослойного RT и clear-only ленивый проход становятся «отложенным clear картинки»; следующий проход, который цепляет картинку целиком, начинается с `loadOp CLEAR` (twin `LAZY_CLEAR`); пустой проход не пишется. Любое другое использование сначала кладёт clear по-старому (`vkCmdClear*Image` с барьерами). Проход, оборванный не-draw операцией или чтением своего вложения, пишется как clear-only проход с `loadOp CLEAR` (без `vkCmdClearAttachments`). `=2` — то же и строка на каждое решение (первые 64) | 0 | 1 | 1 | 1 | 0 |
| `DM_GFX_STORE_HIST` (R4) | `STORE DONT_CARE` (оба аспекта depth/stencil вместе, FN-14/17) вложению, которое за всю жизнь никто не читал (сэмпл, storage, источник копии / блита / резолва / презента, `LOAD` следующим проходом) и которое живёт не меньше `DM_GFX_STORE_HIST_FRAMES` (8) кадров. Чтение после выброса — нарушение: счётчик, картинка больше не выбрасывается. `=2` — только решить и сосчитать (STORE остаётся), `=3` — `=1` плюс poison (пурпур / depth 0.5 поверх выброшенного перед концом прохода: на sysmem-драйвере ошибка видна глазами) | 0 | 1 | 1 | 0 | 0 |
| `DM_GFX_TILE_BARRIER` (R7, из D16) | барьер чтения у draw, чьи картинки только во FS, и зависимости прохода — только фрагментные стадии; draw с записью storage до FS после конца прохода получает полный барьер (WAR) | 0 | 1 | 1 | 0 | 0 |
| `DM_GFX_NOLOAD` / маркер `gfx-noload` (R4, load) | первый draw доказанно покрывает весь проход → цветовой `loadOp DONT_CARE`; перенесены отказы D16 (rasterizer discard, пропуск вьюпорта, первый draw сэмплит своё вложение) | 0 | 1 | 1 | 0 | 0 |

- `CLEAR_FOLD` требует `DM_GFX_LAZY_CLEAR` (его twin-проходы и самотест load-op clear); без него выключается со строкой в логе.
  Twin — тот же проход с другими load/store op (по спецификации совместим с framebuffer и пайплайнами).
- **R5 (transient + LAZILY_ALLOCATED) — только перепись:** `transient_cand` — начала проходов с картинкой, которую никогда не читали и
  которая начата полной перезаписью (кандидат в transient), и её мегапиксели. Пересоздание образа с `TRANSIENT_ATTACHMENT` не сделано.
- **R6 (MSAA resolve в проходе) — только перепись:** `msaa_passes`, `resolves` в строке PERF. Если на реальной сцене оба 0, R6 для DOS2 не нужен.
- **BY_REGION:** барьеров внутри прохода у нас нет (любое чтение рвёт проход), у внешних зависимостей источник `ALL_COMMANDS` — framebuffer-local
  зависимостей нет, `BY_REGION` ставить некуда. Сужение стадий — это и есть R7.
- Лог при старте: `NVN tile pass policy (policy family …): DM_GFX_CLEAR_FOLD=… DM_GFX_STORE_HIST=… DM_GFX_TILE_BARRIER=…; defaults …`.
  В окне PERF: `NVN PERF tile clear_fold deferred= carried= folded= flushed read/op/submit/nomatch/other= pending= store_hist atts= dead=
  check_dead= poisoned= violations= transient_cand= (Mpx) msaa_passes= resolves= frag_barriers= war_barriers=`. Нарушения —
  `NVN store-hist VIOLATION` (первые 16); решения при `=2` — `NVN clear-fold deferred / folded / carried / flush`.

### Риски
- `STORE_HIST` предсказывает по истории: первое чтение картинки, которую 8 кадров никто не читал (скриншот сейва, открытие меню), один раз
  получит мусор. Поэтому только «никогда не читалась», бан после нарушения и только тайлеры. Пути чтения в обход `prepare_read`, перехода
  в `TRANSFER_SRC` и bindless-таблиц дадут ложное «не читалась» — это ловят `=2` / `=3`.
- `CLEAR_FOLD`: clear-only проход с `loadOp CLEAR` вместо `LOAD + vkCmdClearAttachments` — FN-16 (старые Mali r14/r26 ломают clear-only
  проходы; у игроков r32+). PowerVR (`gpu-powervr.md`): драйвер может не любить framebuffer с load/store op не того прохода, с которым он создан;
  это касается и уже включённых twin'ов `LAZY_CLEAR` / `NOLOAD`. При артефактах на PowerVR первыми выключать `CLEAR_FOLD=0`, `STORE_HIST=0`.
- `TILE_BARRIER`: как в D16 (на реальном Mali не мерен; вспышки модели D16 от него доказательно не отделены, MALI-10).
- `NOLOAD`: в D16 известен редкий ложный «покрывает» (пурпур под poison); отказы перенесены, остаток не исключён.
- Ничего не исполнялось: только сборка `dm` arm64 и синтаксис хост-теста `vendor_policy_test.cpp` (хост-компилятора для запуска нет).

### Что мерить на Mi 14 Ultra (через `dm/env`, ABBA ×2, бэкап `files/save` перед force-stop; меню и минута в Форте Радости)
1. **Adreno как есть (умолчания 0):** строка `NVN tile pass policy` с нулями, строки PERF `tile` нет — регресс-контроль (fps, `passes=`, `gpu_ms`).
2. **Adreno + `DM_GFX_CLEAR_FOLD=2`:** `passes=` (ожидание 35 → ~31: 4 clear-only прохода не пишутся), `barriers op38=` (22 → ~0), строки
   `deferred≈11 folded=… flushed …` (причины flush), картинка без отличий. Если `passes` и `gpu_ms` не хуже — кандидат включить и на Adreno.
3. **Adreno + `DM_GFX_STORE_HIST=2`:** `check_dead=` (сколько вложений на кадр выбросили бы), `violations=0`, `transient_cand` и мегапиксели
   (оценка R5), `msaa_passes`, `resolves` (R6).
4. **Adreno + `DM_GFX_STORE_HIST=3` (poison):** нет пурпура и «дыр» в глубине; `violations=0`, `poisoned` больше 0.
5. **Модель Mali `DM_GFX_SIM_GPU=mali`** (умолчания Mali: CLEAR_FOLD=1, STORE_HIST=1, TILE_BARRIER=1, NOLOAD=1): fps, вспышки; затем по
   одному откату `DM_GFX_TILE_BARRIER=0`, `DM_GFX_NOLOAD=0`, `DM_GFX_STORE_HIST=0`, `DM_GFX_CLEAR_FOLD=0` (в sysmem load/store не влияют —
   разница fps только от барьеров и CPU). Плюс `DM_GFX_SIM_GPU=mali DM_GFX_STORE_HIST=3` — poison в модели.
6. GPU-время по проходам: `DM_GFX_PASS_PROF="300 120 0"` — цена каждого прохода до и после. Пропускную способность памяти на Mi 14 не снять
   (sysmem, счётчиков нет); на реальном Mali — только `gpu_ms` у игрока тестовой сборкой.

### Дальше
- R5 по-настоящему: пересоздание кандидата с `TRANSIENT_ATTACHMENT` + `LAZILY_ALLOCATED` на границе кадра (по образцу `upgrade_mutable`),
  только Mali/PowerVR, обратно — по первому чтению (кадр мусора неизбежен), если перепись покажет заметные мегапиксели.
- R3 глубже: слияние «RT A → RT B → снова A» (нужен look-ahead по кодам 21/22/5 — хук `0x93e5f0`, NATIVE_PLAN §3.6) и G-buffer → lighting
  subpass'ами; сейчас слияние только для одинакового набора RT без чтения между.
- Включение `CLEAR_FOLD` на Adreno — по замеру п.2.

### store-hist: учёт выброшенного store (16.11.2026, ветка `dos2-storehist`; собрано, на телефоне не проверено)
**Повод.** `ab7rp3` (Mi 14, `DM_NB=1 DM_NB_RP=3 DM_GFX_SIM_GPU=mali`): 7 строк `NVN store-hist VIOLATION: serial=11 fmt=0x35 2400x1080`
(основной depth, ~10 МБ) в кадрах 232, 256, 1193, 1205, 1214, 1223, 2849. В `ab0base` / `ab11base` / `ab6rp2` (Adreno, `STORE_HIST=0`) учёта
нет вовсе, прогона «модель Mali без NB» не было.

**Кто выбросил — план NB-06, не эвристика.** Во всех окнах `NVN PERF tile` `dead=` == `poisoned=` (0,97 / 1,00 / 0,37 …), а poison при
`STORE_HIST=1` ставит только `DM_NB_RP=3` на проходе плана (`open_pass.nb`); `atts≈0` — `store_hist_mask` почти не вызывался. Значит каждый
выброс — store DONT_CARE из плана (`depth && final && !xread` в `nb_plan.c`). После первого нарушения картинка забанена, но бан действовал
только на эвристику: план выбрасывал дальше («never discarded again» было неправдой).

**Кто читал — два механизма (по логам; точный читатель — новой строкой `VIOLATION`, см. ниже):**
1. **Потерянные записи IR (настоящий выброс).** Окно 11:08:49–59 (кадры 1193–1223, 4 из 7): `[nb] ir … overflow 263`, `nodes max 512` =
   `NB_MAX_NODES`. В переполненном кадре проходы после 512-го узла в IR не попадают; планировщик видел последний записанный проход depth
   как «последнее использование» и ставил DONT_CARE, а lighting за пределом таблицы читал выброшенное. `xread` при этом не учится (чтение
   внутри кадра) — поэтому повторялось. На настоящем тайлере это кадр мусорной глубины.
2. **Ложные срабатывания учёта хоста.** Окна без переполнения (232 / 256 — меню; 2849 — первый 3D-кадр после загрузки): IR полный, а
   планировщик за весь прогон не выучил `xread` основного depth (выбросы шли каждый кадр и после нарушений) — значит межкадрового
   читателя нет, а нарушение — учёт. «Выброшено» снимали только отложенный ClearTexture (`pclear_put`) и clear-twin эвристики. Не снимали:
   load-op CLEAR / DONT_CARE плана (`nbrp_lazy_begin`), CLEAR плана, когда хост открыл проход LOAD-ом, а clear пришёл внутри прохода
   (`plan C / host L` — 679 за 10 с в меню), целый in-pass clear, clear-only проход, исполненный хостом (`lazy_clear_flush_old` считал LOAD
   чтением), полный ClearTexture без отложения и полная копия. Глубину переписали целиком, потом lighting её сэмплил — «нарушение».

**Сделано.**
- `dm/gfx/nb/nb_plan.c`: часть кадра с потерянными записями (`overflow` / `stray`) не получает ни одного store DONT_CARE (`store_lost`,
  в дампе `# store DONT_CARE kept as STORE (lost records)`, в `[nb] ir` — `kept STORE for lost records N in M parts`).
- `xbg_store_hist.h` (новый, чистый C++): учёт одной картинки `XbgHist` — чтение / полная перезапись / выброс (кто: эвристика или план,
  кадр, IR-проход) / бан. `xbg_game.cpp`: поля `hist_*` → `Image::hist`; перезапись снимает выброс на всех путях выше; учёт включён при
  `STORE_HIST != 0` **или** `DM_NB_RP` 1 / 3 (`hist_on`), так что выбросы плана судятся и на Adreno.
- План слушает бан: `nbrp_begin` / `nbrp_lazy_begin` снимают store DONT_CARE с забаненных картинок и с картинок, чьё выброшенное
  содержимое этот проход грузит (`nb_rp_keep_store` в `nb_rp.h`, счётчик `kept STORE for store-hist banned images` в `[nb] rp`).
- Строка нарушения: `NVN store-hist VIOLATION: serial=… <как прочитали: sampled / LOADed by a planned pass / … > after its store was
  discarded by <the NB plan (DM_NB_RP, IR part seq=… node=…) | the history heuristic> in frame D (now F)` (первые 32); в `NVN PERF tile`
  — `violations=N (plan M)`; в строке политики — `store-hist bookkeeping=0/1`.
- Умолчания не менялись (`STORE_HIST` 1 на Mali / PowerVR, `DM_NB` / `DM_NB_RP` 0 везде). Решения эвристики не изменились: она выбрасывает
  только «ни разу не читанное», а бан ставится лишь по чтению, так что новые снятия выброса её выбор не трогают — меньше только ложных строк.

**Игроки на Mali / PowerVR сейчас.** NB выключен по умолчанию — механизм 1 их не касается. Эвристика основной depth в 3D не выбрасывает
(lighting его читает → `read_ever`); в меню выбрасывает, а при входе в 3D depth сначала целиком чистится. Свидетельств мусора у игроков нет;
остаётся прежний известный риск эвристики (первое чтение картинки, которую ≥ 8 кадров никто не читал).

**Проверки (ПК).** `tests/store_hist_test.cpp` (новый), `nb_plan_test` (+`test_lost_records`: читатель за пределом таблицы узлов / чтение
без прохода → STORE; 13 золотых дампов без изменений), `nb_rp_test` (+`test_keep_store`), `nb_transient_test` (13 дампов, 0 кандидатов),
`source_structure_test.py` (+проверка store-hist: каждый крючок под `hist_on`, бан до twin'а; NB-20a: `hist_on` R, `nbrp::cur_at/lazy_at` S,
`store_kept` K). NDK-сборка: те же предупреждения, что у базы; `tools/build_apk.ps1` в worktree — APK собран.

**На телефоне (главный агент, с разрешения хозяина; бэкап `files/save`).** Те же места, что `ab7rp3` (меню → загрузка → Форт → назад):
1. `DM_NB=1 DM_NB_RP=3 DM_GFX_SIM_GPU=mali`: в `NVN PERF tile` `violations=0`; если строка `VIOLATION` всё же есть — она сама говорит, кто
   выбросил (IR seq / node) и как прочитали; `[nb] ir` `kept STORE for lost records` > 0 в тяжёлых кадрах загрузки, `overflow` прежний;
   пурпура / глубины 0.5 на кадрах перехода нет.
2. `DM_GFX_SIM_GPU=mali` без NB (умолчания Mali, как у игроков): `violations=0`, `store-hist bookkeeping=1`.
3. Контроль Adreno без env: `store-hist bookkeeping=0`, строки PERF `tile` нет (байт-в-байт прежнее).

## Нативные шейдеры из GLSL-исходников игры (14.11.2026, ветка `dos2-shsrc`; собрано, на телефоне не проверено)
Идея: вместо Maxwell → SPIR-V (регистровый код транслятора) компилировать те стадии, для которых в паках лежит GLSL-исходник
`<материал>_<проход>_NX.shd`, и подкладывать их в тот же формат бандла.

**Что нашлось.**
- Исходники есть у 744 материалов, `.bshd` с парой `_NX.shd` — 558 (1116 стадий, ключи все уникальные). Но лежат они **только в
  `Engine.pak` (252 стадии: Engine_Default_*, декали, служебные) и `CommunityMods.pak` (726: Gift Bag — ArmorSets, EnemyRandomizer,
  GrowYourHerbs, SummoningImproved)**. В `Materials.pak` (10 890 стадий кампании) исходников нет вообще — только `.bshd` и `.lsb`;
  ни один ключ из 978 нативных не совпадает с ключом из `Materials.pak`. Из 144 шейдеров, загруженных в логах (`src/work/logs/o*/`,
  `r*.log`: загрузка, меню), нативного варианта нет ни у одного. **Для кампании выигрыш ≈ 0**; путь ценен для Gift Bag-контента и
  как готовая инфраструктура (если появятся исходники / iPad-шейдеры).
- Заголовков `.shdh` (`Shaders/NX/CommonHelpers`, `PBR`, `GlobalConstants_NX`, …) нет ни в одном паке, ни для какой платформы
  (`_OGL/_DX11/_PS4/_Metal` тоже их включают). Восстановлены в `tools/dos2_nx_headers/`:
  - `GlobalConstants_NX(_PS_NX)` — из секции рефлексии GLSLC внутри каждого GLSLCoutput (тип секции 3: имена блоков, NVN-биндинги
    по стадиям, смещения, типы, row-major): `EoCGlobalConstantData` b11 (96 байт), `PerView` b12 (240), `PerFrame` b13 (128).
  - `CommonHelpers`: `clip(x)` = `x < 0` (используется как `if(clip(x)) discard;`), `GetTangentFrame(q)` — кватернион → (T = ось X,
    B = ось Y, N = cross(T,B)·sign(w)), снято с транслированного VS.
  - `PBR`: `GBufferData` / `EncodeGBufferData`: rt0 = (Emissive, 0); rt1 = стереографическая нормаль `n.xy/(1+n.z)·0.2812623+0.5`;
    rt2 = (saturate BaseColor, FadeOpacity); rt3 = (saturate Roughness/Reflectance/MetalMask, (FXEmissive + 2·ShadingModel)/255) —
    снято с трёх транслированных FS (константа 0x3e90019d из c1). `Custom` во всех материалах = 0, куда он пишется — неизвестно.
  - Не восстановлены: `RemapReflectance`, `PreExpose`, `EvaluateDistantIBL`, `DirectionLight` (forward-освещение) и кластерные —
    69 FS не компилируются (и их VS не берутся, см. пары).
- Интерфейс 1:1 с транслированным бандлом того же ключа: UBO с NVN-биндингом N ↔ запись `source=2 slot=N`, sampler N ↔ `source=7 slot=N`
  (тип COMBINED, форма 2D/Cube сверяется), входы VS — те же локации (у 163 VS индексы костей `uvec4` против `vec4`+bitcast у транслятора —
  нативно это правильнее), варинги и выходы FS — те же локации; support buffer / c1 / driver-банки нативному коду не нужны (записи
  таблицы остаются, хост их просто привязывает).

**Цифры** (`python tools/dos2_native_shaders.py`, ~3 мин): 1116 стадий → glslc (vulkan1.1, `-O`, `spirv-opt -O
--remove-unused-interface-variables`) компилирует 1047, все 1047 ложатся на таблицу биндингов транслированного бандла, spirv-val —
0 ошибок; после правила пар остаётся **489 VS + 489 FS** (69 FS — нет forward-хелперов, 69 VS — их пары). Инструкций (тела функций):
VS 568 859 трансл. (187 017 после того же `spirv-opt -O`) → 40 748 натив. = **0.22×**; FS 185 835 (60 966) → 34 143 = **0.56×**.
Пак `native.xgspack` — 978 бандлов, 3.1 МБ.

**Инструмент** `tools/dos2_native_shaders.py` → `work/shaders-native/{native/<key>.xgs, native.xgspack, stages.tsv, report.txt}`:
бандл = заголовок, таблица биндингов, список входов и c1 транслированного бандла байт в байт + SPIR-V из исходника (биндинги
перенумерованы на номера таблицы, set 0). Правило пар: нативный VS пишется, только если во всех программах с ним FS тоже нативный
(выходы нативного VS уже, чем у транслированных FS, которые объявляют всё vec4); нативный FS — только если его входы пишут и нативный,
и транслированный VS (хост откатывается по стадиям). `stages.tsv`: статус и причина каждой стадии, заметки по интерфейсу, счёт инструкций.

**Хост** (`xbg_game.cpp`, namespace `xpack::native`): `DM_GFX_NATIVE_SHADERS` (по умолчанию **0** везде). Источник —
`<shaders>/native.xgspack` (тот же формат, что `shaders.xgspack`; один необязательный файл) или `<shaders>/native/<key>.xgs`.
1 = нативный бандл подменяет транслированный внутри `xpack::read`, только если его «голова» (всё до SPIR-V) байт в байт равна
транслированной; иначе берётся транслированный и пишется `NVN native shader <key>: head differs ...` (устаревший пак). Раз подмена
внутри `xpack::read`, её видят и `load_shader`, и `warm::bundle_words` (warm-up, вариант альфа-теста `#at` — переписывается уже
нативный FS), bindless / retype / early-Z / quad / warp — тот же путь. 2 = используются транслированные, нативные читаются и
сверяются (голова + маски локаций входов/выходов): `NVN native shader ... mismatch` / `matches`. Нет пака или ключа — автоматом
транслированный.

**Проверить на телефоне** (когда будет смысл — контент Gift Bag или служебные материалы Engine в кадре):
1. Положить `work/shaders-native/native.xgspack` в `dm/shaders/` (3.1 МБ). `DM_GFX_NATIVE_SHADERS=2` — в логе нет `mismatch`.
2. `DM_GFX_NATIVE_SHADERS=1` — `NVN native shader ... used`; сцена с Gift Bag-бронёй / декалями: картинка как при 0 (G-buffer:
   нормали, шероховатость, флаги rt3 — главное место риска реконструкции), GPU-время прохода G-buffer против 0.
3. Если картинка расходится — по `stages.tsv` найти ключ, `--keep-glsl` даёт собранный GLSL; сверить с транслированным (`spirv-opt -O` + `spirv-dis` его SPIR-V).

**Metal (`_Metal.metal`)**: в паках Switch те же 744 материала, что и `_NX.shd`, и те же отсутствующие заголовки (`Shaders/Metal/*.shdh`) —
покрытия не добавят. Добавить могут только шейдеры кампании из iPad-сборки, если там лежат исходники MSL, а не `.metallib`
(проверить по IPA, когда хозяин её даст).

**Осталось:** forward-хелперы (138 стадий) по транслированному коду; численная сверка «натив против трансляции» (интерпретатор
SPIR-V или прогон обоих на ПК-Vulkan со случайными UBO) — сейчас реконструкция проверена только чтением кода трёх FS и двух VS.

### Черновик для KB (shaders-translation.md)
```
### SH-DOS2-03 Нативные SPIR-V из GLSL-исходников игры вместо трансляции Maxwell (DM_GFX_NATIVE_SHADERS)
- Симптом/цель: транслированный код регистровый (VS ~4.6x, FS ~1.8x инструкций против исходника после spirv-opt -O).
- Причина/находка: DOS2 кладёт _NX.shd только для Engine/CommunityMods (978 стадий из 12 329), в Materials.pak (кампания) исходников нет;
  .shdh не поставляются — GlobalConstants восстанавливаются из рефлексии GLSLC (секция 3 GLSLCoutput), хелперы — из транслированного кода.
- Фикс: tools/dos2_native_shaders.py — бандл с головой транслированного + SPIR-V из исходника; хост xpack::native (0 по умолчанию,
  1 подмена при равной голове, 2 сверка). Рефлексия GLSLC полезна и без исходников: имена/смещения всех uniform любой NVN-игры.
- Проверка: 978 бандлов spirv-val чистые, dm собран; на телефоне не проверено.
```

## ASTC-кэш без ПК: ленивое кодирование на устройстве (14.11, ветка `dos2-astc`; собрано, на телефоне не проверено)
Условие хозяина: на телефон обновляется только APK, ничего из файлов игры с ПК не кладётся. Значит всё, что нужно GPU без BC
(Mali / PowerVR / `DM_GFX_SIM_GPU=mali`), делает само приложение.

**Что в паках.** `.dds` в LSPK — это не DDS, а упакованные текстуры NVN (`DFvN` + блоки `HBvN`, как внутренний блок `.tex` D3).
Перепись по 14 пакам (35 955 текстур, 2,1 ГБ хранилища): BC1 `0x42` — 14 550 / 790 МБ / 933 Мпикс (верхние уровни), BC3 `0x44` —
11 011 / 1 162 МБ / 707 Мпикс, BC2 `0x43` — 85 / 6 МБ; остальное мелочь (BC4 `0x49` 94 МБ, BC5 `0x4b` 36 МБ, BC6H `0x50` 43 МБ, массивы
по 8 слоёв). BC1–BC3 = 91 % байтов — ровно то, что умеет кэш D3 (TEX-15).

**Выбор: вариант (b), ленивый кэш по содержимому.** Вариант (a) (обойти паки на телефоне) требует читателя LSPK + LZ4 на C++ и
совпадения ключа «файл = память игры», которое для DOS2 никто не проверял. Ленивый кэш берёт ключ из той же памяти, которую потом
сверяет поиск, — совпадение гарантировано, паки не нужны, переиспользуется почти всё из D3:
- формат пака и журнала D3 (`xbg_astc_cache.h`, скопирован без изменений: `D3AC`, запись 48 Б, журнал `D3AC-phone 1 …`,
  `xbg_ac_journal_parse`); ключ D3: CRC-32 + Adler-32 хранилища + байты + формат + размер + уровни;
- поиск при создании образа, заливка из пака, «игра поменяла хранилище → образ заново на BC-декодере», «декодированный образ, чьи
  байты есть в кэше → заново из кэша» — порт `astccache` D3 (`xbg_game.cpp`), индекс копируется в образ (`pack_e`), т.к. растёт по ходу игры;
- кодер — правило D3 (`astc_convert.cpp`): mip 0 ≥ 42 дБ к декодированному BC; BC1 6x6 / 5x5 / 4x4 -fast → 4x4 medium / thorough;
  BC2/BC3 4x4 fast / medium / thorough; не дотянула — остаётся BC (журнал, block 0: больше не предлагается); нулевые блоки → void extent (TEX-04).

**Как работает.** На GPU без BC каждая BC1–BC3 2D-текстура декодируется при заливке (`BcUpload`, GPU/CPU-декодер, как раньше). После
заливки `astccache::offer` хэширует хранилище, и если ключа нет в индексе / очереди / списке «остаётся BC» — копирует хранилище в очередь
фоновых потоков (`xbg_astc_lazy.cpp`, nice 10, без Vulkan): deswizzle движка + CPU-декодер `xbg_decode_block` → astc-encoder 5.7.0
(`dm/third_party/astcenc`, Apache-2.0, взят из D3) → дописывает блоки в `dos2-astc.pack.part`, `fdatasync`, строка журнала
`.journal-lazy` (только после записи данных — оборванная строка не считается). Раз в кадр (`perf_frame`) готовые записи вливаются в
индекс; живые декодированные образы этих текстур помечаются к перезаливке и при следующем использовании пересоздаются из кэша (память
декода уходит со старым образом). Следующее создание текстуры (повторная подгрузка или следующий запуск) — сразу ASTC, без декода.
Игровой поток не ждёт никогда: очередь ограничена, переполнение — текстура просто остаётся на декодере и будет предложена снова.
Папка: `Android/data/org.dos2.port/files/astc` (JNI ставит `DM_TEX_ASTC_CACHE_DIR`; приложение-владелец читает её мимо FUSE, разрешение не
нужно, удаляется с приложением). При старте хвост после последней закоммиченной записи отрезается; чужой/битый журнал — кэш с нуля.

| Переключатель | Умолчание | Что делает |
|---|---|---|
| `DM_TEX_ASTC_CACHE` | auto | auto = вкл., если у устройства нет BC (`textureCompressionBC=0`, его же даёт `DM_GFX_SIM_GPU=mali`/`nobc`) и есть ASTC LDR; на Adreno — выкл. (`not used (the device samples BC …)`); `1` — и при BC (попадания используются, кодер видит только декодированные); `0` — выкл. |
| `DM_TEX_ASTC_PRODUCE` | 1 | `0` — только читать кэш, ничего не добавлять |
| `DM_TEX_ASTC_CACHE_MB` | 3072 | потолок пака; дальше кодер не пишет (`the pack reached its cap`) |
| `DM_TEX_ASTC_THREADS` | 2 | потоки кодера (1–8) |
| `DM_TEX_ASTC_QUEUE_MB` | 96 | сколько копий хранилища ждут кодирования; больше — отбрасывается |
| `DM_TEX_ASTC_CACHE_DIR` | `<ext>/astc` | папка кэша (dm/env перекрывает) |

Сборка: только `DM_GAME=dos2` (`dm/gfx/vulkan/CMakeLists.txt`: astcenc + `xbg_astc_lazy.cpp` + `DM_ASTC_LAZY=1` + zlib); другие игры
собирают заглушки `astccache` (путь BC без изменений).

**Лог.** `NVN astc cache: using …/dos2-astc.pack.part: N textures, X MB of ASTC, M stay BC; encoder on (threads 2, cap 3072 MB …); device BC=0`
(или `not used (…)`); на 64, 128, 256 … поисках — `lookups … hits … misses (not in the cache / other bytes / block size) … changed later …
filled later … (made from the cache after its entry was encoded …) … index N (+K this run); encoder: offered … cached … stay BC … pack … MB`;
от кодера — первые 8 текстур (`-> ASTC 6x6 (mip 0 42.7 dB …)`) и сводка на 16, 32, 64 … (`Mpx in … s of worker time, pack … MB, dropped …`);
первые 8 заливок из кэша — `NVN astc cache: upload serial=… -> ASTC 4x4 … bytes (mip 0 … dB)`.

**Проверка на ПК.** `dm/gfx/vulkan/engine/tests/astc_lazy_test.cpp` (WSL g++, сборка в шапке файла) гоняет продовый кодер на настоящих
текстурах DOS2 (`tools/lspk.py extract Textures.pak … "*Animals_Bear_A_*"`, 8 шт. 412x412, BC1/BC3, `pkg_block_h=5`): 7 в кэше
(BC1 → 6x6 42.7–47.0 дБ, один BC1 → 4x4 59.2 дБ после 3 шагов; BC3 → 4x4 42.4–55.5 дБ), одна карта нормалей BC3 осталась BC (41.2 дБ после
thorough); журнал читается парсером движка, дБ из журнала совпадают с перемеренными. Скорость на ядре ПК (SSE4.1): 0,76 Мпикс/с на поток
с учётом мипов и повторных шагов.

**Оценки.** Полный набор BC1–BC3 ≈ 2,2 Гпикс с мипами. Пак целиком: BC3 → 4x4 = 1 Б/пикс ≈ 1,16 ГБ; BC1 → 6x6 ≈ 0,44 Б/пикс ≈ 0,4–0,7 ГБ
(часть уйдёт в 4x4 = 1 Б/пикс), итого **≈ 1,6–2,7 ГБ**, но ленивый кэш держит только увиденное (одна локация — десятки–сотни МБ). Память
GPU на резидентную текстуру: BC1 из RGBA8 (8x от BC) → ~0,9x BC, BC3 (4x) → 1x. Время: телефонное ядро ≈ 0,5 Мпикс/с [оценка], 2 потока ≈
1 Мпикс/с → сцена на ~100 Мпикс ≈ 2 мин фона; вся игра ≈ 35–40 мин фона за всё время игры.

**Что проверить на Mi 14 Ultra (`DM_GFX_SIM_GPU=mali` в dm/env; бэкап `files/save` перед force-stop):**
1. Запуск 1: `NVN astc cache: using …: 0 textures …; encoder on … device BC=0`; при загрузке меню / кампании — `NVN astc lazy: … -> ASTC …`,
   сводки растут; `offered` ≈ числу BC1–BC3 текстур сцены; fps / фризы при загрузке — как без кэша (кодер на nice 10). Через минуту —
   `made from the cache after its entry was encoded` > 0, картинка не меняется (нет квадратов, нет «мыла» на UI/шрифтах).
2. Запуск 2 (тот же сейв): `using …: N textures`, `hits` ≈ большинство поисков, `upload … -> ASTC`, `NVN BC decode vram=` заметно меньше,
   чем в запуске 1 (меньше декодированных МБ), загрузка локации короче.
3. Без SIM (Adreno) — `not used (the device samples BC …)`, всё как раньше. `DM_TEX_ASTC_CACHE=0` — то же на SIM.
4. Если что-то выглядит не так: `DM_TEX_ASTC_PRODUCE=0` (только чтение) / удалить `Android/data/org.dos2.port/files/astc` / `DM_TEX_ASTC_CACHE=0`.

**Не сделано / дальше.**
- На телефоне не запускалось (ни SIM, ни реальный Mali). Совпадение ключа поиска и заливки гарантировано, но доля попаданий во втором запуске
  зависит от того, пишет ли DOS2 текстуру целиком до первого использования (иначе часть станет «filled later» / «changed later» — тоже корректно).
- BC4/BC5/BC6H (≈ 9 % байтов) и массивы не кэшируются (остаются на GPU-декодере).
- Вариант (a) (заранее, обходом паков, с прогрессом во фронте) не сделан: нужен читатель LSPK v13 (+LZ4) на C++; формат пака/журнала уже общий,
  такой обходчик мог бы писать в тот же `.part` + журнал.
- Нет счётчика в HUD/фронте и ограничения по температуре (D3 снижает потоки через `astc_conv_limit`); сейчас только nice 10 и 2 потока.
- Старые версии частично записанных текстур копятся в паке до потолка (уборки/компакции нет).
- KB (`docs/port-kb`, TEX-15 / MATRIX) не обновлён — правка KB вне этой ветки.

## Граф проходов `DM_GFX_PASS_GRAPH` + дизайн слияний A→B→A и subpass (14.11.2026, ветка `dos2-merge`; собрано, на телефоне не проверено)
Задача: прежде чем сливать проходы на тайлерах (FN-01: iPad-порт DOS2 слил skylight + light probes в один проход, переставил тени по
стадиям, пост в один ubershader; FN-24…28 — правила слияния subpass на Mali / Adreno / PowerVR), снять **точный граф проходов кадра**:
какие наборы RT идут подряд, что рвёт проход, кто кого читает и каким шейдером. Код: `xbg_game.cpp` `namespace pgraph`,
`hle_dos2_native.c` (маркеры), `hle_nvn.c` `nvn_engine_mark`, `nvn_obj.h` `OP_X_ENGINE_MARK`.

### Переключатели
| Переключатель | Умолчание | Что делает |
|---|---|---|
| `DM_GFX_PASS_GRAPH=1` | 0 везде (диагностика) | раз в `DM_GFX_PASS_GRAPH_PERIOD` секунд один кадр целиком (present → present) пишется строками `NVN PGRAPH f=…`; в этом кадре мемо дескрипторов пропускается (каждое чтение видно со своим binding), остальное без изменений. Плюс хук `ExecuteRenderCommand 0x93e5f0` (тот же, что `DM_NATIVE_RC`, можно вместе) кладёт в поток команд перед кодами 1, 3, 4, 5, 21, 22, 35, 40, 42, 43, 44, 46, 47, 52, 55, 56, 59, 61 и перед первым пакетом каждого нового списка (код 64) запись `OP_X_ENGINE_MARK {код, младшие 32 бита адреса списка}` — в текущий командный буфер движка `[[[список+0x38]+0xb70]+0x5248]+0x38`, только если тот пишется. Поток трансляции запись не исполняет, только запоминает. |
| `DM_GFX_PASS_GRAPH_PERIOD=<с>` | 10 | период выборки |

Выключено (0) — ни хука, ни маркеров; во всех путях одна проверка `pgraph::armed`.

### Строки (полный формат — комментарий над `namespace pgraph` в `xbg_game.cpp`)
- `p<i> s<подача> L<список> WxH att=[#5CS,#6LD] d=<draws> fs=<ключ первого FS> in=[#3<p2:sfcu,#9<-:suv] end=<причина> eng=<коды>` — проход.
  load: `L` LOAD, `C` loadOp CLEAR (LAZY_CLEAR / CLEAR_FOLD), `N` NOLOAD, `F` LOAD + `vkCmdClearAttachments`, `K` хостовый clear-проход;
  store: `S` / `D` (STORE_HIST). `in` — картинки, прочитанные draw этого прохода: писатель (`pN` проход кадра, `oN` dispatch / transfer,
  `-` не в этом кадре), способ (`s` сэмпл, `u` storage, `t` источник копии) и **класс координат** FS (ниже). `end`: хостовая операция,
  на которой проход закрылся, и строка (`DRAW_IDX/L…` = следующий draw с другим набором RT), `read#N/why` — проход порвало чтение
  картинки (биты как `read_break_why`), `sub` — подача, `host` — clear-проход хоста. `eng` — коды движка с последнего draw прохода
  (причина со стороны движка: 21/22 смена RT, 52 копия RT, 43 compute, 3/4/5 clear, 55 барьер, 64 новый список). `bufw` — draw прохода
  пишут storage-буферы, `cpu` — внутри была CPU-видимая операция.
- `o<i> s<подача> disp|xfer in=[…] out=[…] host=… eng=…` — dispatch или копия / clear вне прохода.
- `imgs #n=serial:формат:WxH[:d]` — легенда картинок.
- `aa p<i>->p<j> end=… eng=…` — два прохода подряд с тем же набором вложений, между ними ничего (проход порван чтением / не-draw операцией).
- `aba p<i>->p<j> via p4,o2,p5: exact | blocked(B-touches-A A2-reads-B buffer-writes cpu-visible-op submit)` — набор вложений вернулся.
  `exact`: между ними ни одна операция не читает и не пишет картинки A; второй проход A не читает ничего, записанного между; нет записи
  storage-буферов, CPU-видимых операций (event, counter, copy B2B, UPDATE_UB, копия в буфер, ClearBuffer) и подачи.
- `sub p<i>(#X) -> p<j> <класс> fs=<ключ> b=<binding> adj=<0/1> d=<draws>` — кандидат в input attachment: проход j сэмплит вложение X,
  записанное проходом i этого кадра, и рисует в размере X; `adj=1` — между i и j нет других проходов / операций.
- `summary passes= draws= ops= images= submits= aa= aba= (exact= touch= dep= buf= submit=) subpass_cand= fc= fcu= uv= … ends: read= sub= <op>=… | eng since last draw: engN=…`.

### Класс координат (статический разбор SPIR-V FS при загрузке, `pgraph::spirv_coords`)
Для каждого сэмплируемого binding фрагментного шейдера — из чего сделаны координаты всех его `OpImageSample* / Fetch / Gather / Read`:
`fc` только `gl_FragCoord` (+ константы) — тот же пиксель при совпадении размеров; `fcu` `gl_FragCoord` + uniform (масштаб, сдвиг,
clamp к размеру — тот же пиксель, только если uniform-часть тождественна); `uv` только интерполированные входы (полноэкранный
треугольник даёт тот же пиксель, но статикой это не доказать); `mix`; `const`; `no` — зависимое чтение (координата из другой выборки),
offset / gather / projective или использование картинки, которого разбор не понимает; `?` — binding не известен. Разбор проходит через
Function / Private-переменные (объединение всех store без учёта потока: переиспользованный регистр даёт только более осторожный ответ),
через вспомогательные функции транслятора (`OpFunctionCall`: аргументы + свод тела — глобальные load, выборки, return, запись через
параметры) и NaN-guard (`OpSelect` / `OpIsNan`). Сверка на 76 FS из `work/new-xgs-1` (хостовый clang, тот же код):
**fcu 12 binding (texelFetch по `gl_FragCoord` через helper с uniform-слагаемым), uv 92, mix 5, no 12, fc 0**. «Тот же пиксель» у DOS2 —
texelFetch от FragCoord, но с uniform-частью; что это за uniform (clamp к размеру из driver-буфера или сдвиг вьюпорта) — следующий разбор.

### Что должен собрать прогон на телефоне (Mi 14 Ultra; `dm/env`, бэкап `files/save` перед force-stop)
1. `DM_GFX_PASS_GRAPH=1` (Adreno, умолчания): меню с 3D-сценой, Форт Радости днём, бой с огнём / магией (много источников света),
   инвентарь / UI поверх сцены — по 2–3 выборки (`logcat | grep "NVN PGRAPH"`, ~100–300 строк на выборку). При старте в логе:
   `[native] ExecuteRenderCommand main+0x93e5f0 hooked: DM_GFX_PASS_GRAPH marks …` и `NVN PGRAPH DM_GFX_PASS_GRAPH=1`.
2. То же с `DM_GFX_SIM_GPU=mali` (включена тайловая политика CLEAR_FOLD / STORE_HIST / NOLOAD: граф, который увидит Mali).
3. Регресс: fps с переключателем вне выборочного кадра против без него — должен совпасть (хук на каждый пакет движка: вызов + проверка).
Что смотреть: `summary aba= exact=`, список `aba … exact` (готовые к слиянию возвраты), `aa` с причинами (каждый — проход, порванный без
нужды на тайлере), `sub … fcu|fc adj=1` с размером кадра (G-buffer → lighting), `ends:` / `eng since last draw`. Цены проходов —
`DM_GFX_PASS_PROF` + `tools/gpass_report.py` в том же прогоне (номера `p<i>` не совпадают с `i=` GPASS — другой кадр; сопоставлять
по WxH / att / fs).

### Дизайн шага 2: слияние A → B → A без look-ahead (точное, спекулятивное; не сделано — ждёт цифр)
Look-ahead по записям NVN одной подачи и неточен, и дорог: привязки по id пула разрешаются только при трансляции, а состояние NVN
наследуется сегментами (переставить сегменты = снимать `state` на каждой границе). Точный путь — **отложить B на уровне Vulkan**:
трансляция идёт в прежнем порядке, меняется только командный буфер, в который пишется B.
1. Конец прохода A по смене RT (не по чтению своего вложения и не по подаче) на тайлере: проход **не закрывается**, а паркуется
   (`parked = open_pass`, главный CB остаётся внутри render pass A); всё дальнейшее (B: проходы, dispatch, копии, барьеры) пишется во
   **вторичный** `VkCommandBuffer` (без `RENDER_PASS_CONTINUE`, может содержать целые проходы).
2. Каждая операция B проверяется до записи: читает / пишет картинку A (вложение, сэмпл, storage, копия) — **сброс**; запись storage-буфера,
   CPU-видимая операция (event, counter, readback, UPDATE_UB, ClearBuffer), occlusion-запрос, подача — **сброс**. Сброс =
   `vkCmdEndRenderPass(main)`, `vkCmdExecuteCommands(main, B)`, дальше всё в main — ровно исходный порядок.
3. Проход с тем же `Pass` (render pass + framebuffer), что у parked — `open_pass = parked` без begin: тот же проход продолжается, второй LOAD
   и первый STORE набора A исчезают. Каждый draw A2 до записи проверяется: читает картинку, записанную в B, или пишет storage — сначала
   `End(main)`, `Execute(B)`, `Begin(A, LOAD)` и этот draw. Порядок «A1, начало A2, B, остаток A2» корректен: начало A2 не зависит от B
   (проверено по draw), B не трогает A (п.2), буферов B не пишет (п.2).
4. Любой конец A — `End(main)`, `Execute(B)`. Итог: A1 ∪ A2 одним проходом, B после него.
Учёт хоста, который надо развести на два CB: `barrier_seq / write_seq / frag_seq` (барьеры B — в его CB), `pclears` (отложенные clear
картинок B — внутри B), layout-переходы картинок B (с A не пересекаются по п.2), `pass_prof` (во вторичном CB без таймстемпов), `crumbs`.
Только Mali / PowerVR / Xclipse (`DM_GFX_PASS_MERGE`; на Adreno 0 — Turnip у нас sysmem, слияние ничего не даёт, KB MALI-10).
Цена: один вторичный CB на парковку + `vkCmdExecuteCommands`.
**Ожидание** `[оценка]`: каждый `aba exact` убирает load + store всего набора A: для G-buffer 4×RGBA8 + D24S8 при 1600×900 это ≈ 29 МБ × 2
≈ 58 МБ DRAM-трафика на кадр, ~1,7 ГБ/с при 30 fps (FN-30: 5 ГБ/с ≈ 500 мВт, то есть ~170 мВт на каждый такой возврат); для одиночного
RGBA8 1600×900 — ~11 МБ на кадр. Сколько их на самом деле — `summary aba= exact=` с телефона. Если `exact` ≈ 0 и всё упирается в
`A2-reads-B`, это не слияние, а перестановка стадий (как Elverils сделали с тенями, FN-01) — правка движка, не хоста.

### Дизайн шага 3: subpass G-buffer → lighting (`DM_GFX_SUBPASS`, умолчание 0 — не сделан)
Кандидат — строка `sub p<i>(#X) -> p<j> fc|fcu adj=1`: размер j = размер X; вложений i ∪ j ≤ 8 цветовых и ≤ 128 бит/пиксель (FN-24; на
PowerVR 128 / 256 бит, FN-27), тот же depth и число сэмплов. Реализация: двухсабпассовый render pass (subpass 0 = вложения i, subpass 1 =
вложения j + input attachment X; зависимость `BY_REGION`, `COLOR_ATTACHMENT_OUTPUT → FRAGMENT_SHADER`, `INPUT_ATTACHMENT_READ`, FN-26) и
вариант FS прохода j: `OpImageFetch(X, координата от FragCoord)` → `OpImageRead` по `SubpassData` с координатой (0,0); выборка X через
сэмплер — только при `fc`, `uv` не трогать. `fcu` требует доказать, что uniform-слагаемое тождественно (clamp к размеру X или сдвиг 0) —
разобрать helper, который строит координату. X дальше сэмплится — STORE остаётся (transient только для никогда не сэмплируемых, FN-22).
Не на Adreno без замера (FN-28: feedback-loop subpass ломается на всех Qualcomm; FN-26: слияние только в binning). Ожидание: минус чтение
X текстурой в j (RGBA8 1600×900 ≈ 5,8 МБ на X на кадр) и, если X дальше не нужен, минус его store.

### Риски / не проверено
- Ничего не исполнялось: только сборка `dm` arm64 (новых предупреждений нет) и хостовая проверка разбора SPIR-V на 76 FS.
- Маркеры пишутся в командную память игры (+16 байт на маркер, сотни на кадр) — при `DM_GFX_PASS_GRAPH=1` её расход немного растёт.
- `[dev+0x5248]` — текущий контекст записи (NATIVE_PLAN §1.5); если список исполняется, когда командный буфер не пишется
  (`recording == 0`), маркер не пишется.
- Граф — best effort: Image, освобождённый и пересозданный внутри кадра, — один узел; набор, не перепривязанный следующим draw, второй
  раз не виден; чтения копий в буфер (`t`) приписываются следующему проходу. Офлайн-инструменты, разбирающие дампы записей
  (`dm/gfx/tools`), о новой записи `OP_X_ENGINE_MARK` не знают (при выключенном переключателе её нет).

## Защита перед первым релизом: общие фиксы, pipeguard v2, ложные отчёты о падении (14.11.2026, ветка `dos2-harden`; собрано, на телефоне не проверено)
`pc_check.py <wt-harden> --nso work/exefs-update`: audioout-keeper, lse-select, thread-exit-hoststack, vma-guard, pc-core — «present, in sync»;
pace-fractional — не нужен (нет display_timing-пейсинга); ldn-lan — DIVERGED (локальная сеть, офлайн-игре не нужна, не трогал);
pipeguard-v2 — был MISSING (v1 + правило двух загрузок для graphics), теперь перенесён (ниже).

### Сверка с MATRIX «Top gaps» (DM_GAME=dos2 = DM_GAME_D3 + DM_GAME_TL2 + DM_GAME_DOS2)
| ID | Что | В wt-harden | Где / чем проверено |
|---|---|---|---|
| HLE-17 | AudioOut: слово сессии + сторож AAudio | ✅ | port-common audioout-keeper in sync, `host/hle_audio.c` |
| HLE-06 | `pthread_exit` на хостовом стеке | ✅ | thread-exit-hoststack in sync, `dm_call_on_stack` (`entry.S`, `hle_thread.c`) |
| APP-01 | PadSurface (пад вне UI-потока) | ❌ | `PadSurface.java` / `DoomPadView.attachSurface()` есть, но DOS2 рисует `D3PadView` на UI-потоке; нужен Painter + замок и телефон — не переносил (→ сделано в `dos2-padsurf`, раздел «Пад на своём Surface» ниже) |
| APP-02 | Stall watch | ✅ | `stall_android.c`, `DM_STALL_WATCH_MS`=2000, старт в `jni_android.c` (от TL2) |
| HLE-09 | Защита от `vm.max_map_count` | ✅ | vma-guard in sync (`hle_vma.c`, `DM_VMA_WATCH`) |
| MALI-01 | `DM_GFX_VTX_RETYPE` | ✅ | `xbg_vertex_retype.h`, варианты `<vs>#vi<code>` |
| MALI-02 | Фолбэки не-Adreno + `DM_SIM_GPU` | ✅ | `xbg_vendor_policy.h` |
| APP-04 | LSE / ARMv8.0 | ✅ | lse-select in sync, `DM_CPU_NOLSE` |
| SH-06 | Pipeguard v2 | ✅ (этот заход) | см. ниже |
| PVR-04 | `DM_GFX_ANISO_FALLBACK` | ✅ | `xbg_vendor_policy.h`, `xbg_game.cpp` |
| TEX-02 | GPU BC-декодер D16 + самотест | ✅ | `xbg_bc_gpu.h`, `DM_GFX_BC_SELFTEST` |
| PVR-05 | `COMPILE_LOCK` | ≈ как TL2 | модули и пайплайны под `DM_GFX_PIPE_SERIAL` (`gpguard::ModuleLock` / `Lock`); vkCmd dm-record / present под замком — нет |
| SYNC-10 | Unbound VB → нулевой буфер | ✅ | лог «NVN unbound vertex buffer» |
| HLE-25 | Quit с ожиданием записи сейва | ✅ | `hle_hold.c dm_hold_quit` + `dm_fs_save_quiesce` |
| APP-07 | Сборка Android 10 | ✅ (не собирал) | `tools/build_apk.ps1 -Android10` → `work/DOS2-port-debug-android10.apk` |

### Pipeguard v2 (KB SH-06; с D16 `engine/xbg_pipeguard.h` + `tests/pipeguard_test.cpp` дословно, логика `xbg_game.cpp` — по D16 08.11 / ETR `745950b`, `da700ae`)
- Списки `gfx-pipe-skip`, `gfx-lone-lane`, `gfx-gpipe-skip` — по драйверу, который реально загружен (статус `vk_driver.c`, поле active;
  пакет, не перехвативший Vulkan, = system; раньше суффикс брался из запрошенного `DM_VK_DRIVER`, баг XC2/D3), суффикс `-<пакет>`,
  `-occ` отрезается, `DM_PIPEGUARD_DRIVER` переопределяет. Первая строка — `# pipeguard v2 driver= driverVersion= cache=<pipelineCacheUUID>
  translator=<sha16 shaders/_chain.json>`; список с другим заголовком или v1 без заголовка → `<имя>.stale` (лог `[pipeguard] stale:` / `legacy:`).
- Запись `key \t причина \t время \t spv` действует, пока SPIR-V шейдера (как его читает загрузчик: пак `shaders.xgspack`, иначе файл;
  нативный бандл, если `DM_GFX_NATIVE_SHADERS` его подставил; после spv-override, до рантайм-переписываний) хэшируется в spv
  (`pgv2::note_sha` в `load_shader` и `warm::bundle_words`, иначе чтение через `xpack::read`). Новый пак / перевод / включённые нативные
  шейдеры = новый spv = бан не действует (лог `entry not applied`).
- Две смерти **подряд** на том же key + spv (compute: `gfx-pipe-inflight<drv>` / `gfx-pipe-suspect<drv>`; graphics: `gfx-gpipe-inflight<drv>-<tid>` /
  `gfx-gpipe-suspect<drv>`); чистая загрузка снимает подозрение. MIUI-kill, force-stop, `run_dos2.sh DUMP_THREADS=1` больше не банят шейдер.
- DOS2-специфика: варианты `<fs>#at<func>_<bits>` (альфа-тест) и `<vs>#vi<code>` (retype) берут spv базового ключа, тег остаётся в ключе
  списка (бан варианта не банит обычный шейдер). Ключ graphics-сборки на пути draw теперь из модулей, которые реально строятся (как у
  warm-up / async: `warm::ids`), а не из базовых ключей `state.shader[]` — раньше бан из warm-up с `#at` не совпадал с ключом draw и наоборот;
  забаненная комбинация на пути draw — draw пропускается (лог `[pipeguard] draws skipped for gfx-gpipe-skip pipeline ...`), warm-up и async
  её не строят (`build()` проверяет `gskipped`).
- Зашитые ключи D2R/DOOM (Adreno 750: `098a126f…`, `cec755c4…`) в DOS2 не нужны и не пишутся. `DM_PIPEGUARD=1` — поведение v1
  (без заголовка и spv; суффикс всё равно по загруженному драйверу). `gpguard` (не-Adreno) для D3-семейства по-прежнему только пропускает
  упавшие сборки в памяти и сериализует PowerVR.
- Проверено: `cmake --build work/build-android --target dm` чисто; хост-тест `tests/pipeguard_test.cpp` (WSL, ASan/UBSan) с нашим
  `xbg_sha256_hex16` (`xbg_shader_key.c`, на потоковом SHA-256) — all checks passed. На телефоне: в boot.log `[pipeguard] v2: lists of driver …`,
  `[pipeguard] loaded: compute skip=… lone-lane=…`, `[pipeguard] loaded: graphics skip=… suspects=…`; старые списки уйдут в `.stale` один раз.

### Логи падений для игроков
- `crash.txt` / `crash.prev.txt`, `boot.log` / `boot.prev.log`, `logcat.txt` / `logcat.prev.txt` — в `Download/DivinityOriginalSin2Mobile/logs`
  (`DmActivity.FRONT_DATA_DIR`, `CrashLog.java`, `jni_android.c`) — уже было, проверено по коду.
- Ложный отчёт от внешнего `kill -11` (дамп потоков `run_dos2.sh DUMP_THREADS=1`, `run.sh`): `crash.c` смотрит `si_code <= 0` и pid
  отправителя ≠ свой (abort() остаётся падением) → в логе `[dm] EXTERNAL SIGNAL sig=11 sent by pid … (kill, not a fault …)` и
  `CRASH sig=11 (external)`, в маркере `dm-crash.txt` — ` external=<pid>`, `crash.txt` начинается с `reason: external signal, not a crash: …`.

### Осталось
- APP-01 PadSurface для `D3PadView` (Java, проверка ANR на телефоне); PVR-05 полностью (vkCmd под замком компилятора, как D16).
- Телефон: прогон с pipeguard v2 (один запуск: `.stale` старых списков, строки `[pipeguard]`), проверка `DUMP_THREADS=1` → `crash.txt` с
  `external signal`. MATRIX / KB не правил (вне зоны этого задания): DOS2-столбца в MATRIX нет.

## Оптимизация SPIR-V на устройстве: DM_GFX_SPV_OPT (14.11.2026, ветка `dos2-spvopt`; собрано, на телефоне не проверено)
**Зачем.** Офлайн-конвейер (`dm/gfx/tools/dmsh_vk_xc2.py`) не гоняет `spirv-opt -O` (только `--trim-capabilities`). На 300
случайных бандлах DOS2 `-O` сократил инструкции FS 196 705 → 67 199 (0.34x), VS 234 407 → 81 794 (0.35x), 0 отказов. Драйверы
(Turnip NIR, стоковые Qualcomm / Arm) часть этого делают сами, но время создания пайплайнов (первые хитчи ~100–200 мс) и слабые
стоковые компиляторы должны выиграть. Новые файлы с ПК на телефон класть нельзя — поэтому оптимизация делается в самом APK.

**Как устроено.**
- Библиотека: SPIRV-Tools v2022.4 из NDK (`<ndk>/sources/third_party/shaderc/third_party/spirv-tools` + её `external/spirv-headers`),
  `dm/android/spirv_tools.cmake`: списки исходников берутся из её `Android.mk`, таблицы `*.inc` / DebugInfo-заголовки генерируются
  при configure её же `utils/*.py` (нужен Python 3), `-Os -g0`, function/data sections (+ `--gc-sections` NDK), hidden visibility,
  без исключений / RTTI. Линкуется в `dm_vulkan_engine` только при `-DDM_GAME=dos2` (`DM_SPVOPT=1`); в остальных портах
  `xbg_spvopt.h` даёт пустые inline-заглушки.
- Хост: `dm/gfx/vulkan/engine/xbg_spvopt.{h,cpp}` + `xbg_game.cpp bundle_module()`. Все 4 места, где модуль создаётся из SPIR-V
  бандла — `load_shader`, `warm::load_module` (warm-up, в т.ч. `#at`), `vtx_retype_module` (`#vi`), `alpha_module` (`#at`) — идут
  через `bundle_module` с **финальными** словами (после всех переписываний: bindless, gscull, lone, early-Z, invariant, quad, warp,
  tidy, альфа-тест, retype). Анализы (raster facts, storage writes, классы входов VS, uniform extent, `p->words` для retype)
  делаются по неоптимизированным словам, как раньше — оптимизатор их не видит.
- `DM_GFX_SPV_OPT=1`: ключ = FNV-1a 64 по словам (с зерном версии библиотеки/проходов) + число слов. Есть
  `<dm>/spvopt/<hash>-<n>.spv` — модуль создаётся из него (папка читается один раз при старте, без проб файлов на каждый модуль).
  Нет — модуль сразу из исходных слов (без задержки), слова в очередь (≤8192 задания / 64 МБ, дубликаты склеиваются) одному
  фоновому потоку с nice 19: `spvtools::Optimizer::RegisterPerformancePasses()` (= `spirv-opt -O`) с валидацией входа, затем
  `SpirvTools::Validate` результата (Vulkan 1.1; для SPIR-V 1.4/1.5/1.6 — соответствующий env), запись через `.tmp` + rename.
  Любой отказ — оригинал остаётся, хеш пишется в `spvopt/bad.txt` (больше не пробуется). Драйвер отверг оптимизированный модуль —
  тот же откат на оригинал + `bad.txt`. Оптимизированный модуль берётся только при следующем создании модуля (по сути — со
  следующего запуска); под живым пайплайном модули не подменяются.
- `DM_GFX_SPV_OPT=2`: как 1 + строка на каждый модуль: `NVN spvopt <key> <hash>: words A -> B (x) valid=1 ms=… env=…` или
  `… <stage> failed (<сообщение валидатора>)`. При 1 логируются первые 16 отказов. Сводка `NVN spvopt progress|at pipeline cache
  save: … hits … optimized … failed … driver-rejected …` — каждые 256 заданий и при каждом сохранении кэша пайплайнов.
- Папка: `<родитель dm/shaders>/spvopt` — рядом с `vk-pipeline-cache.bin` (тот же путь, что `pipeline_cache_path()`).
- Ключи: пайплайны ключуются строками ключей модулей (`warm::register_module` → `xbg_module_id_hook`), не хендлами и не словами,
  так что warm-up-список, memo и fast-ключи для оригинала и оптимизированного модуля одинаковы; кэш пайплайнов драйвера на
  сменившемся модуле один раз промахивается (пересобирает). Спец-константы не используются (ключ отвергает
  `pSpecializationInfo`), а `-O` их и не замораживает; выходы (и Invariant / RelaxedPrecision на них) проходы `-O` не удаляют
  (ADCE с `remove_outputs=false`); неиспользуемые дескрипторы могут исчезнуть — layout от этого не ломается.
- **По умолчанию 0** до проверки на телефоне. Откат: `DM_GFX_SPV_OPT=0` или удалить папку `spvopt`.

**Размеры** (arm64, Release, DM_GAME=dos2): `libdm.so` неурезанный 24.72 → 31.41 МБ (APK кладёт его без strip), урезанный
2.81 → 5.16 МБ (+2.35 МБ: SPIRV-Tools + потянутые ей iostream/locale из libc++_static); в APK (deflate) libdm.so ≈ +1.8 МБ,
APK `work/DOS2-port-debug.apk` = 22.2 МБ. Без `-g0` у SPIRV-Tools libdm.so раздувался до 103 МБ (APK +22 МБ) — не убирать.

**Проверено на ПК** (хост-сборка того же `xbg_spvopt.cpp` и той же SPIRV-Tools из NDK, clang 21 / Windows, POSIX-шимы): 300 случайных
бандлов `work/shaders-vk` → 253 уникальных модуля (47 одинаковых SPIR-V склеились), **0 отказов**, все `valid=1`; слов 0.43x;
время оптимизатора на модуль p50 8 мс, p90 23 мс, max 77 мс (x86 -O2; на телефоне с -Os на nice 19 — в разы дольше: порядка
десятков минут фоновой работы на ~15 тыс. модулей за первую игру). Второй проход: 300/300 из кэша, ~0.1 мс на модуль (SSD).
Оптимизировались офлайн-слова, без runtime-переписываний; на телефоне валидатор v2022.4 может отказать каким-то переписанным
модулям — они уйдут в `bad.txt` (смотреть `failed` в логе).

**Как проверить на телефоне** (через `dm/env`, бэкап `files/save` перед force-stop):
1. A (база): `DM_GFX_SPV_OPT=0`, меню + минута в Форте Радости, два прогона (второй — с прогретым кэшем пайплайнов): `create_ms` /
   `create_max_ms` / `warm_built` из строк пайплайнов, `module=` из `NVN shader … loaded`, `gpu_ms`, фпс, число хитчей >100 мс.
2. B1 (наполнение): `DM_GFX_SPV_OPT=2`, тот же маршрут, игру не убивать сразу — дать фону поработать (сводки `NVN spvopt progress`);
   проверить `failed=` (ожидается ~0), что фпс не просел от фонового потока, картинка как в A. В `dm/spvopt/` появляются `.spv`.
3. B2 и B3 (использование): `DM_GFX_SPV_OPT=1`, тот же маршрут: `NVN spvopt first cache hit`, сводка `hits=… (words A->B)`,
   `driver-rejected=0`. B2 пересобирает пайплайны (драйверный кэш промахивается на новых модулях) — его `create_ms` против первого
   прогона A (холодный компилятор: оптимизированный SPIR-V компилируется быстрее?); B3 против второго A — `gpu_ms` и хитчи.
   Картинка: вода, листва с альфа-тестом (`#at`), тени, UI — как в A.
4. Turnip против стокового драйвера Adreno (стоковый компилятор слабее — там ждём больше выигрыша по `gpu_ms`); Mali/PowerVR —
   если будут. Если всё ровно — включить 1 по умолчанию в DOS2 (кандидат на общий модуль port-common для XC2/D3/TL2).

**Осталось:** прогон на телефоне; перед включением по умолчанию — подумать о нагрузке фонового потока на слабых телефонах (сейчас
один поток, nice 19, без паузы во время загрузок) и об очистке устаревших `.spv` (смена версии = новые имена, старые файлы остаются).

### Черновик для KB (shaders-translation.md / performance.md)
```
### SH-DOS2-04 spirv-opt -O на устройстве с постоянным кэшем (DM_GFX_SPV_OPT)
- Симптом/цель: транслированный SPIR-V не оптимизирован (офлайн только --trim-capabilities); -O даёт ~0.35x инструкций.
- Устройства/GPU: любые; выигрыш ожидается во времени сборки пайплайнов и на стоковых компиляторах.
- Фикс: SPIRV-Tools v2022.4 из NDK (dm/android/spirv_tools.cmake, -Os -g0, +2.35 МБ stripped / ~+1.8 МБ в APK), xbg_spvopt.cpp:
  кэш <dm>/spvopt/<fnv64>-<n>.spv по финальным словам перед vkCreateShaderModule (xbg_game.cpp bundle_module, 4 места);
  промах — оригинал сразу + фоновый поток nice 19 (-O + Validate vulkan1.1), отказ — bad.txt; ключи пайплайнов по строкам
  модулей — warm-up/memo не ломаются. DM_GFX_SPV_OPT 0 (по умолчанию) / 1 / 2 (лог на модуль).
- Эталон: dos2-port ветка dos2-spvopt.
- Проверка: ПК — 253 уникальных модуля DOS2 оптимизированы и прошли валидацию, 0 отказов, кэш читается; на телефоне не проверено.
```

## Пад на своём Surface: PadSurface для `D3PadView` (KB APP-01; 14.11.2026, ветка `dos2-padsurf`; собрано, на телефоне не проверено)
- **Проблема (APP-01):** `D3PadView` рисовался на окне через HWUI; каждый `invalidate()` (касание, счётчик fps 2 раза в секунду, тики
  `Tl2Touch`) гнал UI-поток в `Choreographer.doFrame → ThreadedRenderer.draw → nSyncAndDrawFrame`, а тот ждёт RenderThread, который ждёт fence
  своего прошлого кадра на GPU. Пока наш Vulkan держит GPU секундами (первая сборка пайплайнов, тяжёлые сцены), UI-поток стоит, ввод не
  разбирается → «Input dispatching timed out» → ANR-kill. На Switch в пути UI ничего не ждёт GPU.
- **Как в DOOM 2016 (§2.45) / ETR anrfix `b1b9a60`:** общий `PadSurface.java` (frontend-common, копия в дереве уже была, не менял):
  `SurfaceView` с `setZOrderMediaOverlay` между SurfaceView игры и окном (игра < Surface пада < `D3PadView` < меню паузы), рисует свой
  поток `dm-pad-draw` на CPU-canvas; `D3PadView` реализует `PadSurface.Painter`, новые `attachSurface()` / `detachSurface()` /
  `drawsOnSurface()`, `setVisibility` дублируется на Surface. Натив: `Java_com_xiaoboluo_d2r_PadSurface_nativeWrap` в `jni_android.c` (как в
  DOOM: второй Java Surface над тем же ANativeWindow, чтобы ждущий буфера поток не держал Java-lock Surface'а SurfaceView).
- **Отличие от `DoomPadView` (почему без общего с касаниями замка):** `DoomPadView` рисует живое состояние на потоке отрисовки и потому держит
  всё под одним `lock`. У `D3PadView` рисуют три класса (`D3PadView`, `Tl2Touch`, `PadEditor`) плюс их таймеры (`Tl2Touch` tick 16 мс /
  iconTick, `menuLong`, `fpsTick`, колбэк `D3SkillIcons`) — разносить по ним замок = много мест для гонок. Сделано так: `invalidate()`
  переопределён — с Surface он лишь ставит запись на следующий vsync (`postOnAnimation`, склеивается); запись идёт **на UI-потоке** тем же
  неизменным кодом рисования (`drawPad`: все раскладки, скины, иконки, тулбар редактора) в `Picture` (CPU display list: ни HWUI, ни GPU, ни
  RenderThread), готовая `Picture` отдаётся потоку отрисовки без замка (`AtomicReference`), он проигрывает её на свой canvas. Состояние пада
  трогает только UI-поток; UI-поток не ждёт поток отрисовки вообще (у DOOM касание ждёт `lock`, пока идёт paint). Касания — как раньше,
  `onTouchEvent` на UI-потоке, `nativePadKey` / `nativePadAxes` уходят сразу (задержки ввода не добавлено); картинка пада обновляется на
  следующем vsync, как было и с HWUI. Битмапы (скины `PadTheme` ARGB_8888, иконки `Tl2Touch` / `D3SkillIcons`) — программные и не меняются на
  месте, проигрывание из другого потока безопасно.
- **Выключатель (как в DOOM):** по умолчанию **вкл. для DOS2** (`DmActivity.DOS2`), D3 / TL2 — без изменений (рисуют на окне).
  `DM_PAD_SURFACE=0` в `dm/env` (применяется после `nativeStart`, `padSurfaceEnv`) или `--ez dmPadHwui true` — старое рисование на окне;
  `DM_PAD_SURFACE=1` — включить и для D3 / TL2. Тест: `--ei dmPadStallMs N` / `DM_PAD_STALL_MS=N` — поток пада спит N мс с буфером (не чаще раза в
  15 с): пад замирает, ввод и UI-поток — нет. Лог: `[java] pad drawn on its own surface (draw thread dm-pad-draw)`, `[java] pad surface WxH`,
  `pad surface: first frame in N ms`, при ожидании буфера `pad draw thread waited N ms for a buffer`; `no nativeWrap in the library` = старая libdm.
- **Проверено:** сборка `dm` (DM_GAME=dos2) + `tools/build_apk.ps1` → `work/DOS2-port-debug.apk`, javac без ошибок. **На телефоне не проверено.**
- **Что проверить на телефоне:** (1) ANR-сценарий: первый запуск с пустым кэшем пайплайнов (удалить `pipeline cache` / новый драйвер) — во
  время провалов fps трогать пад и жать Back, ANR-диалога / kill быть не должно, в boot.log сторож `stall_android.c` (APP-02) покажет
  остановку кадров; то же с `--ei dmPadStallMs 8000` — пад замирает на 8 с, игра и меню реагируют. (2) Все раскладки: Switch-пад (стики,
  d-pad одним скином, ABXY, L/ZL/R/ZR, − ⚙ +), `tl2`, `tl2_side` — вид как раньше (скины, иконки, прозрачность, размер, fps-счётчик), нажатие
  подсвечивается. (3) Редактор (⚙ → «Изменить расположение»): тулбар − + Grouped/Separate Hide/Show Reset Cancel Save, перетаскивание,
  щипок, Save / Cancel / Back, меню паузы возвращается поверх. (4) Сворачивание / разворот, смена ориентации, подключение контроллера
  (остаётся только ⚙). (5) Для сравнения `DM_PAD_SURFACE=0` — прежнее поведение.

## Касание как мышь по-айпадовски: `DM_DOS2_TOUCH` (14.11.2026, ветка `dos2-touch`; собрано, на телефоне не проверено)
Основа — `docs/dos2/IPA_RECON.md` §2. Код: `dm/host/hle_dos2_touch.c` (шапка = полный разбор), `jni_android.c` `nativeDos2Touch`,
`D3PadView` (маршрут касаний), `Tl2Gear.dos2Touch` (галка «Касание как мышь (эксперимент)», со следующего запуска).

**Статика (main 4979b200, смещения main.raw):**
- `0x847690` InjectInput(mgr, rec*, int) — запись 20 байт: `+0 u32 raw = device<<16 | id`, `+4 float value[2]`, `+0xc u8 pressed`,
  `+0xd..0xf` копируются, `+0x10` пишется 0 (iPad кладёт туда 3-й аргумент: 1 у движения мыши). Без замка, массив `mgr+0x278/+0x280`, шаг 0x14.
- `mgr` = глобал `main+0x2bb8960` (опрос берёт его в `0x8a77dc`). Опрос пада `0x8a7530` (x0 = NX-объект ввода): кнопки `0x8a8478`
  (нажатие `{1,1}`, pressed 1) / `0x8a84b8` (отпускание, нули), триггеры `0x8a8640/0x8a8694`, стики через `0xbdf4e0`; устройство пада = `u16 [obj+0xe0]`.
- Коды сырого ввода: таблица имён `main+0x2974b10` (релоцированные указатели), **id = индекс − 2** (сверено с таблицей пада `main+0x1f58f4c`:
  a = 0x8c … и с кодами стиков 0x82..0x85). Мышь: left 0x74, middle 0x75, right 0x76, motion 0x79 (= iPad `0x10079`), motion_xneg/ypos/xpos/yneg
  0x7a..0x7d, wheel_xpos/xneg/ypos/yneg 0x7e..0x81; touch_tap 0x9b, hold 0x9c, pinch_in 0x9d, pinch_out 0x9e, rotate 0x9f, flick 0xa0, press 0xa1.
  Устройство мыши = 1 (iPad; на Switch не проверено — `DM_DOS2_TOUCH_DEV`).
- Привязки Switch: грузится `PlayerProfiles/Default/inputconfig.json` (`0xa68d14`: FlashLeftMouse/Action1 ← mouse:left, ContextMenu/FlashRightMouse
  ← mouse:right, FlashMouseMove ← mouse:motion, FlashScroll ← wheel) и `Eoc/controller_1.json` (CameraZoomIn/Out ← `c:nx_touch_pinch_in/out`).
  **`Eoc/keyboard_1.json` на Switch не грузится** (пути нет в main) — колёсного зума и мышиной камеры нет, поэтому щипок = touch_pinch на устройстве пада.
- **Курсор UI найден (проход 2, Ghidra headless на копии проекта):** указатель-глобал `main+0x2bb88e8` → объект 12 байт
  `{float2 pos, u8 +8 = 1}`, создаётся в init NX-ввода `0x8a8e94…0x8a8eb4` (структура глобала: `+8/+0x10` = 0, `+0x30` = 1). Близнец iPad
  `[0x101ecf428]` (iPad пишет туда позицию в пикселях окна в `sendXYMotion` → `0x1003c42a8`). На Switch **писателя нет вовсе** (33 читателя,
  запись только в init) — курсор стоит в 0,0. Читатели: диспетчер Flash-событий `0xd23800` (слот vtable `main+0x29c3bd8`): событие `0x58` =
  FlashMouseMove → `player vt+0x190(x, y)` (позиция мыши Iggy), `1/2/3` = FlashLeft/Right/MiddleMouse → `0xd27700` (кнопка Iggy, читает курсор
  в `0xd27714`), `0x59…0x66` стрелки/Enter/PgUp…, `0x67/0x68` FlashScrollUp/Down → `0xd27680`; менеджер UI (глобал `main+0x2bbb230`)
  `0xae83e0` (перетаскивание элемента к курсору), `0xaee2c0` (начало перетаскивания), `0xae3600` (активация: позиция мыши в фильм);
  `0x110e1f0` и остальные читатели (пикинг мира `0x1388d10/0x1389ad0` получают курсор аргументом). То есть **PC-путь мыши во Flash в Switch-сборке
  целиком есть**, не хватало только позиции. Теперь `DM_DOS2_TOUCH` по умолчанию пишет позицию в `*[main+0x2bb88e8]` перед каждым движением
  и кнопкой, а движение шлёт с `{1,1}`, как iPad. Обе точки (init и читатель `0xd2392c`) сверяются пословно.
- **Режим контроллера:** фильмам ставится Flash-переменная `isControllerMode` (`0x1209224`, тип bool) = (запись ввода игрока `+0x2c0 == 1`;
  записи в хеше `[main+0x3298140]`, ключ — u16 игрока `+0xf4` объекта UI). `+0x2c0` обнуляет конструктор записи (`0xe616e4`) и ставит только
  SetInputDevice `0x12a04a0` (`0x12a04b8 str w1,[x0,#0x2c0]`; хвостовой вызов из `0x129f400` — обработчик «устройство ввода сменилось»).
  `DM_DOS2_TOUCH_UI=1` (по умолчанию 0, только вместе с `DM_DOS2_TOUCH=1`) меняет эту запись на `str wzr`: устройство игрока остаётся 0
  («не контроллер»), фильмы получают `isControllerMode = false`. Гипотеза: на PC 0 = клавиатура и мышь. Не проверено: выбор PC- или `_c`-фильма
  делается там, где UI создаётся (таблица типов `0x10a9670` регистрирует и `msgBox.swf` — тип 0x10, и `msgBox_c.swf` — тип 0x12); читает ли
  это место то же `+0x2c0`, не установлено. Риск: при `_UI=1` пад может перестать вести героя (движение стиком завязано на тип устройства).
  Ещё не разобран бит `param_3 & 1` у `0xd23800` (при нём FlashMouseMove игнорируется).

- **Падение на тап (телефон, pack 2a, 14.11):** SIGSEGV addr 0x8 в `main+0x84586c` на первой же записи мыши. ProcessInputEvent `0x845350`
  (x0 = InputManager, запись; зовётся из обновления менеджера `0xb06a40` для очереди `+0x278`, `0xb07e94`) ищет устройство записи в хеше
  привязок (`+0x88/+0x90`; устройство 1 = мышь там есть — `Default/inputconfig.json`), проходит его привязки и при совпадении сохраняет сырое
  состояние `{float2, u8}` в `[[mgr+0x18] + device*8] + id*12` (`0x84585c…0x84588c`; пустой элемент пропускается `cbz`, пустой массив — нет).
  `[mgr+0x18]` — массив состояний по устройствам: на PC его заполняют клавиатура и мышь, на NX ни одного такого устройства не создаётся,
  массив null (пад до этого места не доходит — его устройства нет в том хеше). Массив используется только здесь и в `0x8453a8` (модификаторы
  клавиатуры, id 0x6b…0x72) — это единственные два поиска «device*8» (`sbfx #13,#19`) во всём main. **Фикс:** перед первой записью хост
  отдаёт `[mgr+0x18]` обнулённый массив на 64 устройства и мыши блок 256 × 12 байт (остальные элементы null = пропуск). `DM_DOS2_TOUCH_STATE=0`
  — не выделять, а выключить касание одной красной строкой; при несовпадении слов у `0x84585c` хук не ставится вовсе. Риск: если деструктор
  менеджера освобождает `[mgr+0x18]` гостевым free, на выходе из игры будет падение (деструктор на Android обычно не вызывается).

**Реализация:** прыжок 16 байт на пролог опроса `0x8a7530` + трамплин (как `hle_dos2_native.c`), всё сверено пословно (пролог опроса, первые
8 слов InjectInput, `bl` в `0x8a8478`, загрузка mgr в `0x8a77dc`). После оригинального опроса, в том же потоке, очередь касаний (мьютекс,
движения одного пальца склеиваются) разбирается в записи мыши. Жесты: 1 палец — курсор следует; тап = left down+up; сдвиг > 2.5 % высоты окна =
left down → движение → left up (перетаскивание); удержание без сдвига `DM_DOS2_TOUCH_HOLD_MS` (500) = right click; 2 пальца — щипок = touch_pinch
in/out импульсом на 2 кадра на устройстве пада, вертикальный сдвиг двумя = колесо. Координаты: 0..1 вида × размер окна из `hle_dos2_wide.c`
(`dm_dos2_wide_size`, 1600×900, если широкий экран не ставился; окно показано на весь экран 1:1 по форме) или `DM_DOS2_TOUCH_WIN`.
Фронт: при галке касание мимо показанных кнопок и «дома» стиков идёт в игру (плавающая зона левого стика уступает); без галки — как было.

| Переключатель | Значение |
|---|---|
| `DM_DOS2_TOUCH=1` | хук + маршрут касаний (галка гира ставит его); 0/нет — ничего |
| `DM_DOS2_TOUCH_LOG=1` | лог первых 64 разных raw у InjectInput (видно устройство/id пада и наши записи); работает и без `DM_DOS2_TOUCH` |
| `DM_DOS2_TOUCH_DEV=<n>` | устройство мыши (1) |
| `DM_DOS2_TOUCH_MOTION=one/abs/delta` | value записи движения: `{1,1}` как iPad (по умолчанию) / позиция в пикселях окна / приращение |
| `DM_DOS2_TOUCH_CURSOR=<hex>` / `=0` | по умолчанию позиция пишется в курсор UI `*[main+0x2bb88e8]`; `<hex>` — другой float2 в данных main; `0` — не писать |
| `DM_DOS2_TOUCH_STATE=0` | не выделять менеджеру массив состояний `[mgr+0x18]`: касание выключается с красной строкой вместо падения |
| `DM_DOS2_TOUCH_UI=1` | SetInputDevice `main+0x12a04b8` пишет 0: устройство игрока «не контроллер», фильмам `isControllerMode = false` (эксперимент) |
| `DM_DOS2_TOUCH_WIN=<W>x<H>`, `DM_DOS2_TOUCH_HOLD_MS`, `DM_DOS2_TOUCH_PINCH_SWAP=1` | размер окна, удержание, поменять щипок in/out |

**Проверка на телефоне (по порядку, с разрешения хозяина):** (1) `DM_DOS2_TOUCH_LOG=1` без галки: в логе `[touch] InjectInput raw …` от пада —
устройство пада; (2) галка: в логе `pad poll … hooked … position -> UI cursor [main+0x2bb88e8]`, `pad device = N`, при тапе — raw `0x10079/0x10074`;
(3) в меню (главное / пауза — фильмы `_c`): реагируют ли кнопки на наведение и тап. Если реагируют — touch-as-mouse работает в controller-UI;
(4) если нет — `DM_DOS2_TOUCH_UI=1`: появились ли PC-фильмы (в логе открытий `GUI/*.swf` без `_c`), кликаются ли; ходит ли герой стиком;
(5) если курсор смещён — проверить `DM_DOS2_TOUCH_WIN` (размер окна в логе); при `DM_DOS2_WIDE=0` поправки на полосы нет;
(6) щипок — зум камеры (наоборот — `DM_DOS2_TOUCH_PINCH_SWAP=1`), удержание — контекстное меню; (7) без галки пад работает как раньше.

### Касание: режим UI и фильтр устройств (14.11.2026, проход 3, ветка `dos2-touch2`; собрано, на телефоне не проверено)
Повод: на телефоне (pack 2c, галка «Касание как мышь», без `DM_DOS2_TOUCH_UI`) «тач или не работает, или работает неправильно». Разбор
статический (capstone + Ghidra headless на своей копии проекта), main 4979b200, смещения main.raw.

**Что найдено:**
- **Режим контроллера — один глобальный байт `main+0x2bb8918`.** Его ставит init NX-ввода: `0x8a8eb8 strb w19,[x8,#0x30]` (x8 = `main+0x2bb88e8`,
  сразу за указателем курсора; w19 = 1). 670 читателей `ldrb`, второго писателя нет: PC-код, который его переключает (`ToggleInputMode` ←
  `ctrl+shift+q` в `keyboard_1.json`), в сборку не попал. 1 = контроллерный UI, 0 = клавиатура и мышь. Живого переключения, как на PC, в
  Switch-сборке нет — режим выбирается один раз при старте.
- **Выбор PC- или `_c`-фильмов:** набор UI игрока создаёт `0x10c06d0`: читает байт (`0x10c07ac ldrb w24`), кладёт в запись игрока `+0x2b0`
  (узел хеша `+0x2c0` — именно его `0x1209208` сравнивает с 1 для `isControllerMode`) и при 1 создаёт `_c`-типы (`bottomBar_c` 0x3b,
  `contextMenu_c` 0xc, `minimap_c` 0x3c, `combatLog_c` 0x41, `equipmentPanel_c` 0x40 …), иначе (`0x10c0b30`) PC-типы. `CreateUIObject` =
  `0xaebbf0(mgr, слой, тип, флаги, x4, игрок)`, 93 вызова; карта создателей `[main+0x2bbb230]+0xd8`. Идентификаторы типов **те же, что на PC**
  (в регистраторе около `0x10a9670` id блока относится к пути ПЕРЕД ним: `mainMenu.swf` 0x1c, `msgBox.swf` 0x1d, `msgBox_c.swf` 0x4b,
  `gameMenu.swf` 0x13, `gameMenu_c.swf` 0x4d, `hotBar.swf` 0x28, `partyInventory.swf` 0x74, `characterSheet.swf` 0x77). PC-классы UI
  (`UIKeyboardBottomBar`, `UIKeyboardTrade` …) в main есть.
- **`DM_DOS2_TOUCH_UI` был не тем местом:** `+0x2c0` у SetInputDevice `0x12a04a0` — поле объекта UI (создание персонажа), а не режим. Оставлен
  только для A/B.
- **Фильтр событий по устройству** (`0x844014`, раздача событий слушателям; две копии — `0x84407c…` и `0x844234…`): устройство события (u16)
  даёт класс: 0/1 (клавиатура/мышь) = 0, 3…6 (пады) = 1, прочее = −1. Слушатель получает событие, если у него бит 4 во флагах (`+4`), или класс 0
  и байт `[mgr+0x170] != 0` («контроллеру разрешены клавиатура и мышь»), или класс == байт режима, или класс −1, или в маске слушателя
  (`+0x68`) стоит бит класса. Значит, **в контроллерном режиме наши записи мыши доходят только до слушателей, которые сами их просят** — это
  самая вероятная причина «не работает». В PC-режиме так же отсекаются записи пада (как на PC: в режиме клавиатуры пада нет). Там же
  `0x844078` пишет устройство последнего события в `[mgr+0x238 + игрок*2]`.
- **Привязки:** `0xc05ab0` грузит `Default/inputconfig.json` (мышь и клавиши движка: `Flash*`, `Action1`, `ContextMenu`) и
  `Eoc/controller_1.json` для четырёх игроков при любом режиме. `Eoc/keyboard_1.json` (PC-привязки игры: `CameraZoomIn/Out` ← колесо,
  `ToggleInGameMenu`/`UICancel` ← escape, `CameraRotateLeft/Right` ← delete/end, `ToggleInventory` ← b …) лежит в `Game.pak`, но пути в main нет.
- **Пространство курсора:** `FlashMouseMove` → `player vt+0x190` = `0xd26a60(x, y)`: `x −= max(player+0x10, 0)`, `y −= max(+0x14, 0)` (позиция
  фильма в окне), затем `IggyMakeEventMouseMove` (`0x3ec970`) и раздача `0x3ec290`; кнопка `0xd27700` шлёт курсор как есть. То есть курсор —
  в **пикселях окна, в котором игра работает сейчас**: docked (2400×1080 на тестовом телефоне) или handheld (1920×864), а не виртуальные
  1920×1080. Старый код всегда брал docked-размер — в handheld-режиме курсор уезжал в 1.25 раза. Теперь размер читается у объекта окна игры:
  `[[main+0x2bbb260]+0x40][0]`, `+0x18/+0x1c` (u32; пишет `Window::SetSize 0x93d21c`, читатель `0xc0b50c`) при каждом касании.
- **Мир:** `Action1` ← `mouse:left`, `ContextMenu` ← `mouse:right` уже в `Default/inputconfig.json`; пикинг `0x1388d10` зовётся из `0x109b004` и
  `0x1389ad0` (← `0x1258608`), курсор получает аргументом. Какие ветки клика по миру контроллерный режим выключает, не разобрано (670 читателей
  байта, из них 2 рядом с пикингом — `0x1386bfc`, `0x1386e3c`); проверяется на телефоне шагами 2 и 3 ниже.

**Что сделано (`dm/host/hle_dos2_touch.c`, всё за переключателями, каждое место сверяется пословно, при несовпадении красная строка и пропуск):**

| Переключатель | Значение |
|---|---|
| `DM_DBG=1` (вместе с `DM_DOS2_TOUCH=1`) | первые 64 записи: жест → запись (устройство, id), касание 0..1, окно, пиксели, **курсор, прочитанный обратно**, байт режима, `[mgr+0x170]`; раз в 600 опросов при активности — сводка: записей по видам, курсор, устройство пада, байт режима, окно |
| `DM_DOS2_TOUCH_KBM=1` | контроллерный UI остаётся; `[mgr+0x170]` держится в 1 (перезаписывается каждый опрос): записи мыши проходят фильтр ко всем слушателям. Пад работает как раньше |
| `DM_DOS2_TOUCH_PCUI=1` | PC-режим со старта: `0x8a8eb8` → `strb wzr` (байт режима 0) → PC-фильмы, мышь родная. **Пад (экранный и BT) игра отфильтрует** — управление только касанием. Щипок = колесо (зум камеры и прокрутка списков), третий палец = escape (меню / отмена) |
| `DM_DOS2_TOUCH_PCUI_KEYS=0` | при `_PCUI`: не менять строку пути привязок `main+0x1ebfc35` (`…/controller_1.json` → `…/keyboard_1.json`; по умолчанию меняется, иначе нет зума колесом и escape) |

Размер окна теперь читается у игры всегда (строка `[touch] the game's window is WxH …`, если он не равен docked-размеру); `DM_DOS2_TOUCH_WIN`
по-прежнему задаёт его жёстко. Клавиатуре (устройство 0) выдаётся такой же блок сырого состояния, как мыши.

**Проверка на телефоне (по порядку, с разрешения хозяина; везде галка «Касание как мышь» и `DM_DBG=1`):**
1. Как сейчас, без новых флагов. В логе `grep "\[touch\]"`: строки `#N left down … | touch … -> window WxH px X,Y | cursor read back X,Y | mode 1 kbm 0`.
   Смотреть: (а) есть ли строка `the game's window is …` — значит игра в handheld-размере и раньше курсор был смещён; (б) `cursor read back`
   равен `px`; (в) `mode 1`. На экране: тап по кнопкам главного меню и паузы.
2. `DM_DOS2_TOUCH_KBM=1`: в логе `[mgr+0x170] 0 -> 1`, в записях `kbm 1`. На экране: реагируют ли `_c`-меню на тап, ходит ли герой по тапу в
   мир, открывается ли контекстное меню удержанием; стик и кнопки пада должны работать как раньше. Это самый дешёвый вариант, если хватит.
3. `DM_DOS2_TOUCH_PCUI=1`: в логе `stores 0 in the mode byte`, `bindings path … -> …keyboard_1.json`, в записях `mode 0`. На экране: главное меню
   PC-вида (кнопки под мышь), в игре — PC-HUD (панель навыков снизу, миникарта, портреты слева); тап по земле = идти, тап по кнопкам панели,
   удержание = контекстное меню, щипок = зум, третий палец = меню. Экранный пад работать не должен — это ожидаемо.
4. Если в шаге 3 меню не PC-вида или игра падает на старте — лог целиком (PC-ветки на Switch никогда не исполнялись, возможны недостающие
   ресурсы); `DM_DOS2_TOUCH_PCUI_KEYS=0` отделяет падение из-за `keyboard_1.json`.
5. Курсор смещён при верном `cursor read back` — сравнить `window` в логе с размером из строки `[wide]`; полосы (`DM_DOS2_WIDE=0`) не учтены.

**Дальше (не сделано):** живое переключение KBM ↔ пад — сделано проходом 4, см. следующий подраздел; в PC-режиме перевод кнопок пада в клавиши (`ToggleInventory` b, `ToggleMap` m, камера delete/end) через ту же инъекцию;
галка гира для выбранного режима после проверки.

### Касание: живое переключение PC-UI ↔ контроллерный UI, `DM_DOS2_TOUCH_LIVE` (14.11.2026, проход 4, ветка `dos2-touch3`; собрано, на телефоне не проверено)
Цель: поведение PC-сборки — UI следует за последним использованным устройством, — пересобранное для пары «касание / пад». Разбор статический
(capstone, свои скрипты вне дерева): iPad 3.6.72 (VA Mach-O; символов функций в бинаре нет — 69 штук, опора на строки и совпадение кода) и
Switch main 4979b200 (смещения main.raw).

**Как это сделано на iPad / PC:**
- Режим — `u32` по `0x101ecf458` (структура `0x101ecf428`+0x30, та же, что на Switch `main+0x2bb88e8`+0x30; 744 читателя). Код пишет только 0
  (клавиатура и мышь) и 2 (касание).
- Писатели: `0x10088ac64(mgr, force)` → 0 и `0x10088adb8(mgr, force)` → 2 (7 вызовов из тач-слоя и UI). Оба: `[mgr+0x228] = −1`, запись
  режима, `0x10088a368(mgr, игрок 0, устройство 0 / 0xc)` (класс устройства игрока `[mgr+0x228 + игрок*4]`: устройство 0/1 → 0, 3…6 → 1,
  0xc → 2), сброс устройств списков `mgr+0xa8…` (`0x100889b70`) и рассылка слушателям ввода: `0x10088aadc(mgr+0x1d0, слот 0x48, virtual)` —
  слот 9 vtable слушателя = «режим ввода сменился».
- Работа UI — `0x1012f1f58(ui, сейчас)`: `сейчас = 0` ставит себя в очередь заданий (лямбды `0x1012fed0c` / `0x1012fed7c`), `сейчас = 1`
  делает дело: `0x1012dfbf8(ui, 1)` уничтожает UI-объекты режима; дальше **только в состоянии игры 0x1c** (в 0x13 / 7 — только уничтожение,
  новый UI создаёт код самого состояния) заново создаёт общие объекты (типы 0x63 / 0x64) и для каждого игрока менеджера игроков зовёт
  сборщик набора `0x1012dfea4` (лямбда `0x1012fedf8`, флаги 1 — на Switch такой лямбды нет), затем три обновления: `0x1012eae08(ui, 0)`,
  `0x1012f2874`, `0x1012eb390(ui, 0)`.

**Что осталось в Switch main** (кода классов устройств игрока, рассылки слота 0x48 и обработчика нет: пары `mov w1,#0x48; mov w2,#1` в main
нет, писатель байта один). Тот же класс — менеджер UI игроков, глобальный указатель `main+0x3298140`:

| Адрес | Что |
|---|---|
| `0x10c4580(ui)` | создать всё: общие 0x63 / 0x64 (`0x10c2840`) + `0x10c06d0(ui, id, 0x20000000)` для каждого игрока (`[main+0x2bc0510]`, хеш `+0x94/+0x98`; лямбда `0x10c4720`). Один вызов — старт игры `0x104daa0` (`x0 = [main+0x3298140]` в `0x104da98`) |
| `0x10c4730(ui)` | уничтожить всё: общие (`0x10c41f0`), по игрокам `0x10bc250` + `0x10bc750(ui, id, 0)` (71 UI-объект на игрока: `0xaeba10`), очистка хеша игроков `ui+0xf0` (`0x10c25e0`). Один вызов — выгрузка игры `0xd4006c` |
| `0x10bb080` / `0x10bafe0` (переходники `0x10bba70` / `0x10bba50`) | игрок удалён / добавлен: уничтожение (`0x10bc250`, `0x10bc630`, `0x10bc750(…, 1)`) / создание (`0x10c06d0(…, 1)`) набора одного игрока — игра сама пересобирает набор посреди сессии |
| `ui+0xf0` | хеш наборов игроков: `+0` u32 число записей (обнуляет `0x10c25e0`, корзины остаются), `+4` u32 число корзин, `+8` массив корзин; узел: `+0` следующий, `+8` u16 id игрока, `+0x2c0` режим, под который набор собран |

**Что сделано (`dm/host/hle_dos2_touch.c`, `DM_DOS2_TOUCH_LIVE`, по умолчанию 0, нужен `DM_DOS2_TOUCH=1`):**
- `=1`: первое касание после пада включает PC-UI, первый ввод пада (нажатие либо стик / курок дальше половины) после касания — контроллерный
  UI. Не чаще, чем раз в `DM_DOS2_TOUCH_LIVE_MS` (1000 мс); к паду — только когда пальцев на экране нет. Переключение идёт в потоке игры, в
  перехвате опроса пада (`0x8a7530`), в порядке iPad: байт режима `main+0x2bb8918`, затем — **только если в хеше `ui+0xf0` есть наборы** (мы в
  игре; это замена айпадовской проверки «состояние 0x1c», номера состояний Switch не сверялись) — `0x10c4730` и `0x10c4580`. Наборов нет
  (главное меню, загрузка) — меняется только байт: UI, созданный позже, прочитает его сам.
- `=2` — минимальный вариант: в игре не пересобирает никогда, переключение ждёт, пока хеш опустеет (меню / загрузка).
- Пад в PC-режиме (пункт «что остаётся работать»): игра отсекает его записи фильтром `0x844014`, но хост видит их в `InjectInput`
  (`0x847690`, общий перехват с `DM_DOS2_TOUCH_LOG`; свои записи хост шлёт мимо перехвата) во время опроса и переключает режим — следующее
  нажатие уже работает. Это поведение PC: нажал на пад — получил контроллерный UI. Перевод кнопок пада в клавиши `keyboard_1.json` не делался:
  в живом режиме файл привязок остаётся `Eoc/controller_1.json` (иначе пад после возврата останется без привязок; `DM_DOS2_TOUCH_PCUI` при
  `_LIVE` задаёт только стартовый режим и строку пути не трогает), а без `keyboard_1.json` клавиши игры ни к чему не привязаны.
- Жесты: «PC-режим» теперь определяется по байту (щипок = колесо, третий палец = escape). Колесо в `Default/inputconfig.json` — только
  прокрутка списков (`FlashScrollUp/Down`); зума камеры колесом в живом режиме нет — камера возвращается вместе с падом.
- Сверка: 8 мест пословно (`0x10c4580`, `0x10c4730` — прологи; `0x104da98`, `0xd40064` — загрузка `[main+0x3298140]` и `bl` на обе функции;
  `0x10c4748`, `0x10c47c4` — смещение и раскладка хеша; `0x10c06d0`, `0x10c4720` — сборщик и его лямбда) + прежние `0x8a8eb8` / `0x10c07ac`.
  Не совпало — красная строка, `_LIVE` не включается. В код игры `_LIVE` ничего не пишет, кроме перехода в `InjectInput`.
- Лог: строка установки; каждое переключение при `DM_DBG=1` (без него — первые четыре): `[touch] live #N: touch|pad input|deferred -> PC|controller
  UI (mode byte … = 0|1); UI rebuilt … | byte only; destroy N us, create N us, player UI sets A -> B`.

**Риски (всё не проверено на телефоне):**
- `0x10c4730` + `0x10c4580` игра зовёт на выгрузке и на старте, а не посреди кадра ввода. Пересборка в диалоге, ролике, торговле, бою с
  открытым окном может оставить висящие ссылки на уничтоженные объекты → падение или пустой экран (какие окна входят в набор игрока, не разбиралось). Для этого `=2`.
- После пересборки iPad делает три обновления (`0x1012eae08`, `0x1012f2874`, `0x1012eb390`); их Switch-двойники не найдены. Возможны: пустая
  панель навыков до смены персонажа, неверная раскладка сплит-скрина.
- `0x10c4580` создаёт наборы с флагами `0x20000000`, iPad в обработчике — с 1 (как «игрок добавлен»). Если наборы выйдут невидимыми — менять
  на поигроковый путь `0x10bc250` / `0x10bc630` / `0x10bc750(…, 1)` + `0x10c06d0(…, 1)`.
- PC-ветки UI на Switch никогда не исполнялись (тот же риск, что у `_PCUI`): нужные PC-фильмы в `Game.pak` есть, но Iggy-варианты не проверены.
- UI вне этого менеджера (главное меню, уже открытое окно сообщения) остаётся в том виде, в каком создан.
- Первый ввод пада в PC-режиме теряется (игра его отфильтровала до переключения).

**Проверка на телефоне (по порядку, с разрешения хозяина; бэкап `files/save`; `dm/env`: `DM_DOS2_TOUCH=1`, `DM_DBG=1`):**
1. Сначала шаги 1–3 прохода 3 (особенно `DM_DOS2_TOUCH_PCUI=1`): если PC-UI со старта не создаётся или падает — живое переключение смысла не имеет.
2. `DM_DOS2_TOUCH_LIVE=2`. В логе `DM_DOS2_TOUCH_LIVE=2: the UI follows…`. Главное меню: тап → `live #1: touch -> PC … byte only`; кнопка
   пада → `live #2: pad input -> controller … byte only`. Загрузить сейв после тапа в меню: HUD должен быть PC-вида; в игре нажать пад — в логе
   `waits for the menu / a load`, UI не меняется.
3. `DM_DOS2_TOUCH_LIVE=1`, в игре, персонаж стоит, окон нет: тап → `UI rebuilt`, `player UI sets 1 -> 1`, время destroy / create; на экране
   PC-HUD. Нажать кнопку пада (первое нажатие теряется) → контроллерный HUD. Повторить 5 раз; смотреть рост памяти и время.
4. То же с открытым инвентарём, в диалоге, в бою — здесь ожидаются падения; по результату решить, какие состояния запрещать.
5. `player UI sets 1 -> 0` или пустой экран после пересборки — лог целиком; следующий шаг — поигроковый путь (см. риски).

**Дальше (не сделано):** двойники трёх обновлений iPad; запрет переключения по состоянию игры (диалог / ролик); загрузка `keyboard_1.json`
вторым файлом привязок, чтобы в PC-режиме работали зум колесом и escape; галка гира после проверки.

### Касание: левый клик, движение, щипок (14.11.2026, проход 5, ветка `dos2-touch4`; собрано, на телефоне не проверено)
Повод — два прогона на телефоне (pack 2l, логи `work/logs/t1.log`, `t2.live.log`): записи приходят как задумано, курсор `[main+0x2bb88e8]`
читается обратно верно, но **левый клик не делает ничего** (и в контроллерном UI с `_KBM`, и в PC-UI `_PCUI`), долгое нажатие (правый клик)
работает, щипок не зумит. Разбор статический (capstone + свой разбор `.rela.dyn` для vtable; main 4979b200, смещения main.raw).

**Путь события до Iggy (сверено, гипотеза «движение теряется в `0xd23800`» отвергнута):**
- `UIManager::OnInputEvent 0xae83e0` (менеджер `[main+0x2bbb230]`) идёт по UI-объектам сверху вниз и дважды зовёт `0xaea7b0(mgr, объект,
  событие, проход, …)`. Условия там: флаги объекта (`+0x14`) бит 1, бит ввода игрока (5 + игрок; игрок = событие `+4`), а для событий 1…3
  (`FlashLeft/Right/MiddleMouse`) — байт `[mgr+0x10b] != 0` (`0xaea8ac`; конструктор `0xae7430` ставит 1, слушатель `0xc03ab0` переключает его
  по событию 0xd6). Дальше `UIObject vt+0xa8` (`0xae38b0`) → `FlashPlayer vt+0x30` `0xd23420` → `vt+0x38` `0xd23800(событие, 0)`:
  `FlashMouseMove` (0x58, нажато) → `vt+0x190` `0xd26a60` = Iggy mouse move (курсор минус начало фильма `+0x10/+0x14`), 1…3 → `0xd27700` =
  кнопка Iggy с координатой курсора. Второй вызов `0xd23800` (из `0xaea844`, флаг 1 = «событие уже взял объект выше») движение пропускает —
  это и есть бит `w2 & 1`. Отдельного «Iggy-положения мыши вне события» нет: положение ставит только `FlashMouseMove`.
- Ломалось раньше, в менеджере ввода:

| № | Причина | Адреса | Правка |
|---|---|---|---|
| 1 | **Отпускание мыши не давало события.** Массив сырого состояния — движковый Array `mgr+0x18` {буфер, `+0x20` ёмкость, `+0x24` размер}. `ProcessInputEvent 0x845350` сравнивает запись с сохранённым состоянием входов привязки: читает его только при `устройство < [mgr+0x24]` и ненулевом элементе, иначе «сохранено 0» (`0x84566c…0x845684`); `0x845740`: равно → события нет; было нажато, стало отпущено → путь отпускания `0x8463d0`. Писатель `0x84585c` размер не проверяет. Проход 1 дал менеджеру буфер (падение по тапу), но размер остался 0: состояние писалось и не читалось. Нажатие (0 → 1) давало событие, отпускание (0 == 0) — нет: `FlashLeftMouse` не «отжимался», кнопке Flash нужно down + up. Правый клик действует по нажатию (`ContextMenu`) — поэтому работал только он | `0x84566c`, `0x84567c`, `0x845740`, `0x84585c`; те же проверки «граница, потом null» в обновлении менеджера `0xb06f80`, `0xb06fec`, `0xb071d8`, `0xb07c08`, `0xb07610` и в `0x84609c` / `0x846104` | хост ставит `[mgr+0x24] = 64` (размер своего буфера; элементы падов остаются null = как раньше). `DM_DOS2_TOUCH_DEVN=<n>`, по умолчанию 64, `0` = как было |
| 2 | **Движение — событие только на изменении.** iPad: `InjectInput` копирует третий аргумент в `+0x10` записи (1 для движения); `ProcessInputEvent` (`0x845498 ldrb [rec+0x10]` → `0x845f94…0x84620c`) кладёт сырой id в список `mgr+0x218`, а следующее обновление (`0xb07e40` → `0xb075b4…0xb075e0`) до новой очереди подаёт для каждого id запись «значение 0, отпущено» — автоотпускание. Так постоянное `{1,1}` даёт `FlashMouseMove` каждый кадр. Switch-овский `InjectInput` пишет туда 0 (`0x84771c strb wzr,[x8,#0x10]`), потребитель цел. С живым состоянием (п. 1) без автоотпускания событием было бы только самое первое движение | `0x84771c`, `0x8476ec` (буфер очереди `mgr+0x278`, шаг 0x14, счётчик `mgr+0x280` биты 24…47), `0x845498`, `0xb075bc`, `0xb075d4` | после `InjectInput` хост ставит `+0x10 = 1` в только что добавленной записи движения; второе движение в том же опросе сначала отпускается явной записью. `DM_DOS2_TOUCH_AUTOREL=1` (по умолчанию), `2` = явная запись отпускания после каждого движения, `0` = как было |
| 3 | **У щипка не было привязки.** `controller_1.json`: `CameraZoomIn/Out` ← `c:rightstick_yneg/ypos` и `c:nx_touch_pinch_in/out`; сырого имени `nx_touch_pinch_*` в main нет (таблица `main+0x2974b10` кончается на `item11` 0xac, есть только `touch_pinch_in/out` 0x9d/0x9e) — эта половина привязки не разрешается, наши импульсы 0x9d/0x9e шли в никуда | таблица имён `main+0x2974b10`; код стика пада `0xbdf4e0` (`0xbdf654`: значение {\|ось\|·чувствительность/30, \|ось\|}, нажато = за мёртвой зоной; стик в центре записей не шлёт) | щипок в контроллерном UI = правый стик по y на устройстве пада: 0x89 `rightstick_yneg` (приблизить, пальцы врозь) / 0x87 `rightstick_ypos`, держится `DM_DOS2_TOUCH_PINCH_FRAMES` (8) опросов на шаг, значение `DM_DOS2_TOUCH_PINCH_V` (1.0). `DM_DOS2_TOUCH_PINCH=touch` — старые импульсы. В PC-UI щипок по-прежнему колесо |

- Заодно: байт `[uimgr+0x10b]` («кнопки мыши идут во Flash») хост держит в 1 и пишет строку, если застал 0 (`DM_DOS2_TOUCH_UIMOUSE=0` — не трогать).
- **Не доказано статикой:** что игрок события мыши (устройство 1 → хеш `mgr+0x88`) = 0 и что у объектов главного меню стоит бит ввода игрока;
  что `_c`-фильмы контроллерного UI вообще слушают мышь (скорее нет — рабочий режим касания это PC-UI или `_LIVE`). Для этого отладочные
  перехваты ниже: следующий прогон покажет, на каком шаге событие пропадает.
- Сверка: 12 мест пословно (`k_q_flag`, `k_q_buf`, `k_rec_flag`, `k_autorel`, `k_autorel2`, `k_state_rd`, `k_state_rd2`, `k_uim_btn`,
  `k_uim_ctor`, `k_iggy_move`, `k_iggy_btn`, `k_stick`); не совпало — красная строка и откат этой части (автоотпускание → явная запись,
  размер / байт / стик не трогаются). В код игры пишутся только два отладочных перехода при `DM_DBG=1`.
- Лог при `DM_DBG=1`: перехваты `0xd26a60` (Iggy mouse move) и `0xd27700` (кнопка Iggy). Строки `[touch] Iggy button #N: player … code 1/2
  (left down/up) flag F at cursor X,Y -> handled H | Iggy mouse moves so far M, last x,y` (первые 48) и в сводке `[touch] pass 5: motion flagged
  N, released by hand N | raw-state size … | UI mouse byte … | Iggy: mouse moves N (last x,y, movie origin ox,oy), buttons N`. В записи движения
  — пометка `(+0x10 = 1: auto release)`.

**Проверка на телефоне (с разрешения хозяина; сохранение `files/save` скопировать заранее):**
1. Главный прогон, PC-UI: `DM_DBG=1`, `DM_DOS2_TOUCH=1`, `DM_DOS2_TOUCH_PCUI=1`. Ждём: тапы по кнопкам главного меню работают; в логе на каждый
   тап `Iggy button … code 1` и `code 2`, `Iggy mouse moves` растёт с каждым касанием, `raw-state size 64`.
2. Контроллерный UI: `DM_DBG=1`, `DM_DOS2_TOUCH=1`, `DM_DOS2_TOUCH_KBM=1` — щипок двумя пальцами зумит камеру (в логе `pinch down dev 3 id 0x89` /
   `0x87`), пад работает как раньше. Зум не в ту сторону — `DM_DOS2_TOUCH_PINCH_SWAP=1`; слишком резко / слабо — `DM_DOS2_TOUCH_PINCH_FRAMES`, `_PINCH_V`.
3. Если в п. 1 кнопки молчат — по логу: `Iggy button` нет вовсе → событие режется до Flash (игрок / биты объекта: смотреть `0xaea7b0`);
   `code 1` есть, `code 2` нет → отпускание не дошло (`DM_DOS2_TOUCH_AUTOREL=2` и сверить `raw-state size`); оба есть и `handled 0` при
   `Iggy mouse moves` = 0 или с чужой координатой → движение (`DM_DOS2_TOUCH_AUTOREL=2`, затем `DM_DOS2_TOUCH_MOTION=abs`).
4. Откат к поведению прохода 4 для сравнения: `DM_DOS2_TOUCH_DEVN=0 DM_DOS2_TOUCH_AUTOREL=0 DM_DOS2_TOUCH_UIMOUSE=0 DM_DOS2_TOUCH_PINCH=touch`.

### Касание: прогон t3 и проход 6 (14.11.2026, ветка `dos2-touch4`; собрано, на телефоне не проверено)
Прогон t3 (pack 2m, `DM_DBG=1 DM_DOS2_TOUCH=1 DM_DOS2_TOUCH_PCUI=1`, лог `work/logs/t3.log`, два запуска): «любой клик вешает меню».
Игра при этом рисует кадры как обычно (не падение и не зависание потока) — не отвечает только фильм меню.

**Что показал лог:**
- **Сверка размера массива не прошла на телефоне** (красная строка `main+0x84566c / +0x84567c differ`): слово `0x845670` — `sbfx x12, x18, …`,
  а загрузчик переписывает инструкции с `x18` (на Android это регистр платформы), поэтому оно никогда не совпадает с main.raw. Теперь это слово
  не сравнивается. Правило на будущее: в пословные проверки не брать инструкции с `x18`.
- **Массив сырого состояния на NX есть** (вывод прохода 1 «его нет» неверен): игра создаёт его, когда регистрируется устройство пада
  (во втором запуске `[mgr+0x24] = 4`, первое касание было после строки `pad device = 3`). В первом запуске касание пришло раньше, хост
  подставил свой буфер с размером 0, игра потом заменила массив — наши блоки устройств 0 / 1 пропали, `g_state_ok` остался 1, состояние
  больше не писалось. В этом запуске в логе только `code 1`: ни одна кнопка не отжалась — «мёртвое» меню. То же было причиной t2.
- **Координаты верны.** Перехват печатал аргумент `0xd26a60` до вычитания начала фильма, «курсор Iggy 552,544» — это касание того момента
  в пикселях окна. Фильм 1920 по ширине стоит в окне 2400 со сдвигом 240 (`movie origin 240,0`): движение уходит в координатах фильма,
  событие кнопки несёт сырой курсор окна (`0xd27714`) — асимметрия самой игры (на PC с ультрашироким экраном начало фильма тоже не 0), то есть
  Iggy координаты события кнопки, скорее всего, не использует. Проверка — `DM_DOS2_TOUCH_BTNXY=1`.
- **Каждому событию достаётся 9 вызовов** — это не «несколько игроков», а обход UI-объектов (`0xae83e0`, два прохода `0xaea7b0`): четырём
  фильмам событие приходит с флагом 1 («не твоё»), одному (фильм меню) — с флагом 0. Так устроен движок.
- **Второй запуск не объяснён статикой:** клик прошёл полностью (down `handled 1` у фильма меню, up доставлен), после него фильм меню на
  нажатия не отвечает (`handled 0`). Отпускание доходит до каждого фильма дважды (два события «отпущено» на одну запись). Кандидаты: двойной
  up; штатное действие кнопки PC-меню, которое на Switch-сборке ничем не кончается. Нужен снимок экрана в момент «зависания».

**Что сделано:**
- сверка без слова с `x18`;
- массив проверяется на каждом опросе с касаниями: пока его нет или он короче устройства мыши — касания отбрасываются (одна строка в логе),
  блоки устройств 0 / 1 добавляются заново, если пропали (`blocks re-added` в сводке). Свой массив хост делает только если игра не создала
  его за `DM_DOS2_TOUCH_STATE_WAIT` (300) опросов после первого отброшенного касания — аллокатором движка `0x83cc00` (сверка места вызова
  `0xd234c8`), ёмкость 64, размер = мышь + 1;
- `DM_DOS2_TOUCH_BTNXY=1` (по умолчанию 0): событие кнопки с курсором минус начало фильма; `DM_DOS2_TOUCH_UPONCE=1` (по умолчанию 0): второй
  up тому же фильму отбрасывается. Оба ставят перехваты Iggy и без `DM_DBG`;
- лог: `Iggy button` помечает цель (`flag 0 (THE TARGET)`), пишет начало фильма; в сводке — массив (указатель, размер, ёмкость, блоки),
  захват мыши менеджера UI `[uimgr+0x10d]` / `[uimgr+0x218]`.

**Проверка на телефоне:**
1. `DM_DBG=1 DM_DOS2_TOUCH=1 DM_DOS2_TOUCH_PCUI=1`. Ждём в логе на тап `code 1` и `code 2` у цели. Если меню снова «зависло» — снимок экрана и
   что именно нажато.
2. То же + `DM_DOS2_TOUCH_UPONCE=1`.
3. То же + `DM_DOS2_TOUCH_BTNXY=1` (без `_UPONCE`).

## Мобильный резолв теней CascadedPCF (14.11.2026, ветка `dos2-shadows`; собрано, на телефоне не проверено)
Проход «Light Shadows» (полный кадр 1800x806, пишет долю освещённости для `ClusteredLightingWithSSAO`) — фрагментная стадия
`EngineShaders:Shaders/NX/CascadedPCF_<N>x<S>.bshd`. Заменена своим GLSL в интерфейсе транслированного бандла (кандидат 2 в
`TILE_SHADER_CANDIDATES.md`). Подмена штатным `DM_GFX_NATIVE_SHADERS=1` (namespace `xpack::native`).
- **Найден и исправлен баг хоста** (`native::apply`, `xbg_game.cpp`): «голова» сравнивалась вместе со словом 6 заголовка (длина SPIR-V,
  смещение 24), а у нативного бандла длина своя — **любой** нативный бандл (и 978 материальных из `dos2-shsrc`) отвергался с
  `head differs ... translated used` (режим 2 — `mismatch: head differs`). Теперь сравнивается всё, кроме этого слова. Без этой правки
  пак из этой главы ничего не подменит.

**Что делает шейдер Switch** (снято с транслированного SPIR-V, `spirv-opt -O` + декомпиляция; имена — из рефлексии GLSLC в `.bshd`):
- Входы: `TexCoord0` (loc 1, noperspective, луч вида) × `LinearDepth` (t0, texelFetch по `gl_FragCoord` с поправкой масштаба из support
  buffer `_m5[0]/_m5[1]`) + `global_ViewPos` (PerView b12 = c15[12]) → мировая точка.
- PGlobals2 (b1 = c4): `LightProperties` c4[0] (x — мягкость, z — ширина полосы затухания, w — смещение глубины), `ShadowFarPlanes` c4[1],
  `ShadowFrustumWidths` c4[2], `ProjectionMatrix[i]` c4[23+4i..] (row-major). Атлас 2×размер каскада, каскад 0 — квадрант (0,0),
  1 — (0,0.5), 2 — (0.5,0).
- Вес каскада w = затухание к краю квадранта по x и y (`sat(k − k·|2t−1|)`, k = 1/LP.z) × затухание к дальней плоскости
  (`sat(sat(1−z)·F·2/(LP.z·W))`). Обход: первый каскад с w > 0 решает; при w < 1 подмешивается следующий с весом (1 − w) (его собственный
  w только разрешает выборку); у последнего — `1 − w + w·lit`. Радиус `clamp(LP.x·3.5/W_i, 1/A, 10/A)/s_i`, смещение
  `z − s_i·LP.w/F_i`, s_i = W_i/W_0, A — сторона атласа. Выборки: 9 точек Poisson (c1[0..8]) × `textureLod`, сравнение `ref < depth`.
- **Поправка к кандидатам:** «45 выборок на пиксель» — статический счёт (5 путей × 9). Пиксель делает **9** выборок, **18** в полосе
  смешения (≈13 млн/кадр при 1800x806, не 65). iPad делает 16 выборок с аппаратным сравнением (32 на шве) — у него выборок **больше**,
  зато каждая — билинейный 2x2 PCF.

**Замена** `tools/dos2_pcf/CascadedPCF_mobile.frag` (один GLSL, `-DNCASC -DATLAS -DPCF_MODE`), сборка `python tools/dos2_pcf_mobile.py`
(~5 с) → `work/shaders-native-mobile/{exact,mobile,ipad16}/` + `stats.tsv` + `README.md`. Ключи: 3x768 `1372db843749e1d3`,
2x1024 `a758e42af8853343` (DM_DOS2_SHADOWS=0), 3x512 `2a221ee245d55aab` (=2). Бандл = голова транслированного байт в байт
(6 биндингов: support 4, c15 22, c4 28, c1 34, t0 10, t1 16 — объявлены те же номера, без перенумерации) + новый SPIR-V; spirv-val
vulkan1.1 чистый, входы / выходы (локации, тип, noperspective) равны транслированным — проверяет сам скрипт. Каскады развёрнуты руками
(индекс каскада — константа, без динамической индексации vec4 / массивов), циклы выборок — `[[unroll]]`.
- `exact` — 9 точек + ручное сравнение = результат Switch без накладных трансляции (база для A/B картинки).
- `mobile` — **4 `textureGather`** (повёрнутая решётка ±0.25/±0.75 радиуса), ручное сравнение 4 текселей + билинейные веса = аппаратный
  2x2 PCF на выборку, 16 текселей; центр выборки зажат в квадрант с отступом в 1 тексель (отпечаток 2x2 не вылезает в соседний каскад).
- `ipad16` — 16 `textureGather` на диске Poisson из iPad `CascadedPCF_2x1024` (64 текселя): вид iPad, дороже Switch (для сравнения).
- **Почему gather, а не `sampler2DShadow`:** Dref требует сэмплер с `compareEnable` и depth-формат атласа. Сэмплер игры для атласа
  сравнения не включает (Switch сравнивает вручную), формат атласа в логах не виден (его чтения не попадали в `read break`). Для Dref хосту
  нужен вариант сэмплера: в записи дескриптора (`xbg_game.cpp`, ветка `COMBINED_IMAGE_SAMPLER`, `samp_par parameters`) поставить
  `cmp_mode=1`, `cmp_func=2` (LESS, NVN = Vk+1) и линейный фильтр, если SPIR-V модуля объявляет образ с Depth=1. Gather даёт тот же 2x2 PCF
  без этого; экономия Dref — 4 сравнения + 3 lerp на выборку. Не делалось.

**Цифры** (`stats.tsv`, инструкции SPIR-V после `spirv-opt -O`; на пиксель — оценка по сборкам на 2 и 3 каскада):
| 3x768 | трансл. | exact | mobile | ipad16 |
|---|---|---|---|---|
| инструкций (статически) | 1445 | 821 | 846 | 1986 |
| на пиксель, один каскад (шов) | ≈345 (≈620) | ≈200 (≈360) | ≈210 (≈370) | ≈440 (≈820) |
| текстурных операций на пиксель (шов) | 9 точек (18) | 9 (18) | 4 gather (8) | 16 gather (32) |
| сравниваемых текселей (шов) | 9 (18) | 9 (18) | 16 (32) | 64 (128) |
| SPIR-V, байт | 92 236 | 20 764 | 21 256 | 48 600 |
Итог: `mobile` — в 2.25 раза меньше выборок и ≈1.6 раза меньше ALU при более гладкой полутени; выигрыш в мс ожидается небольшим
на Adreno 750 (проход и так полоса 1 вход + 1 выход), заметнее на слабых тайлерах. Время прохода не мерилось.

**Включить на телефоне** (с разрешения хозяина):
1. Сохранить `files/save`. Положить `work/shaders-native-mobile/mobile/native.xgspack` в `dm/shaders/native.xgspack` (≈57 КБ; только 3
   ключа теней — материальные бандлы не включаются; вместе с ними — `mobile/with-materials/native.xgspack`, 3.1 МБ). Хост не ищет
   `native/<key>.xgs`, если пак есть — класть ровно один пак.
2. `DM_GFX_NATIVE_SHADERS=2` — в логе `NVN native shader 1372db843749e1d3 matches ...`, нет `mismatch`.
3. `DM_GFX_NATIVE_SHADERS=1` — `NVN native shader 1372db843749e1d3 used`; затем A/B папками `exact` / `mobile` / `ipad16`.

**Что сравнивать глазами** (одна и та же сцена кампании, солнце, камера близко и далеко):
- `exact` против без подмены — должно совпасть (иначе ошибка в разборе: смотреть обход каскадов / квадранты).
- `mobile`: мягкость края тени (у Switch — ступеньки 9 точек на 768-тексельной карте; у `mobile` — гладкий билинейный край; при большом
  радиусе возможны 4 «призрачные» копии края), **акне / «полосы» на плоских поверхностях** (смещение то же, но билинейный отпечаток
  захватывает соседний тексель: если поверхность покрылась рябью — смещение мало для 2x2), **шов каскадов** (вдали, где каскад 0 переходит
  в 1 и 1 в 2: скачок мягкости, светлая/тёмная полоса, «утечка» соседнего квадранта атласа на краю), тени тонких объектов (решётки, столбы —
  4 выборки могут их терять).
- `ipad16`: эталон мягкости iPad; если `mobile` выглядит сильно хуже — промежуточный вариант (8 gather) делается правкой `TAPS` в GLSL.
- GPU мс (`NVN PERF`, `DM_GFX_PASS_PROF`, если есть) против без подмены; на Mali/PowerVR — особенно.

**Риски:** пиксели не сравнивались (нет запуска); `textureGather` по depth-представлению атласа (D24S8 / D32) требует только
SAMPLED_IMAGE — штатно; если атлас окажется не depth, а цветной — работает так же. Режим `exact` повторяет и странности Switch (у края
каскада без следующего каскада результат уходит к `w·lit`).

## Мобильные полноэкранные пост-шейдеры (14.11.2026, ветка `dos2-postsh`; собрано, на телефоне не проверено)
Кандидаты 5 и 8 из `TILE_SHADER_CANDIDATES.md`: пять фрагментных стадий, которые идут каждый кадр на весь экран, заменены своим GLSL в
интерфейсе транслированного бандла — тем же методом, что тени (голова бандла байт в байт + новый SPIR-V, `DM_GFX_NATIVE_SHADERS=1`).
Цель — только ALU: выборки, полоса и проходы те же. Сборка `python tools/dos2_post_mobile.py` (~8 с): `tools/dos2_post/<имя>_mobile.frag`
(+ `common.glsl`) → glslc `-O` vulkan1.1 → spirv-val vulkan1.1 → проверка интерфейса (входы / выходы равны, дескрипторы ⊂ транслированных)
→ `work/shaders-native-mobile/post/native/<key>.xgs`, `post/native.xgspack` (5 ключей, 23 КБ), `post/with-shadows/native.xgspack`
(5 + 3 ключа теней из `mobile/`, 81 020 байт) = копия в **`packs/2a/native.xgspack.post-mobile`**; `post/stats.tsv`, `post/equivalence.txt`.

**Проверка эквивалентности** — `tools/spv_interp.py`, свой скалярный интерпретатор SPIR-V (float32 через numpy, GLSL.std.450, fetch /
sample / gather по CPU-текстурам, UBO по Offset/ArrayStride модуля). На каждый шейдер 400 случайных вызовов (300 с render scale 1 — что пишет
хост, 100 со случайными масштабами 0.5 / 2 / −2 / −1.5 в `_m5[0..4]`): случайные текстуры (маски с 0 / 1 / долями, глубина с 0 / 1,
HDR с inf / NaN / 2.0 / 1e38), cbuffer, `gl_FragCoord`, варинги. Транслированный модуль (после `spirv-opt -O`) и новый дают **одинаковые
биты на всех 400 / 400** для каждого из пяти; сырой транслированный (без -O) = -O на каждом 8-м вызове (50 / 50). Контроль: заведомо
испорченные варианты ловятся (`k.y / (d − k.x)` вместо `(1/(d − k.x))·k.y` — 52 из 200 расходятся на 1 ulp; перепутанная диагональ маски
FadeBlending — 95 из 200). Допуск — 0 (бит в бит) в интерпретаторе; на GPU возможна разница ≤ 1 ulp, если драйвер по-разному сольёт
mul+add (явные `fma` стоят там же, где у транслированного).

| ключ | шейдер | что делает | инстр. SPIR-V -O трансл. → наш | исполнено на пиксель (макс.) | выборок на пиксель |
|---|---|---|---|---|---|
| `62d683cc49953ad6` | FadeBlending | шахматный фейд кадра по маске (ниже) | 847 → 505 | 174 (182) → 83 (89) | 4 / 7 → 3 / 6 |
| `e94ea226e689801c` | EoCFog | туман: глубина → мир xz → карта тумана t_E, при `c8[0].w > 0` × вторая карта t_C | 186 → 93 | 113 (142) → 71 (84) | 3–4 = |
| `010472595b342188` | EoCOutlineCombine | обводка поверх кадра, `c1[0].x = 0.15` | 157 → 147 | 93 → 53 | 3 = |
| `f23c09d198b55dc5` | LinearizeDepth | `d∈{0,1} ? d : (1/(d − c3[0].x))·c3[0].y` | 46 → 25 | 40 → 22 | 1 = |
| `cbe8649300e9c8cd` | PPDownSampleLog2 | `rgb > 2 ? log2 : ·0.5`, NaN/inf → (0,0,0,1) | 123 → 49 | 93 (103) → 38 (43) | 1 = |
«Исполнено» — инструкции, выполненные интерпретатором на пиксель при render scale 1 (без merge / OpLine), средн. по 300 вызовам; статический
счёт у Fade / OutlineCombine завышен общим путём (render scale ≠ 1), который на телефоне не исполняется. Для справки (unopt, из
manifest): Switch 973 / iPad 292 (Fade), 211 / 127, 168 / 59, 55 / 34, 137 / 78. Наш путь у LinearizeDepth / DownSampleLog2 / Fog
уже ≈ iPad; у OutlineCombine iPad меньше потому, что у него нет фикса render scale — у нас он за одной однородной проверкой.

**FadeBlending** (разобрано по транслированному SPIR-V): `i` = пиксель, `a0 = min(i, wh−1)`, `a1 = min(i+1, wh−1)` (wh = `c15[13].zw`),
чётность `(i.x+i.y)&1`. Пара B квадрата 2x2 = анти-диагональ `(a0.x,a1.y),(a1.x,a0.y)` у чётных, главная `a0,a1` у нечётных; A —
другая. `s` = сумма альфы маски t_A (binding 10) по B; если `0 < s/2 < 1` — цвет = `0.5·fma(s, fma(ΣB, 0.5, ΣA·−0.5), ΣA)` по цвету t_8
(binding 16), иначе цвет t_8(a0); альфа = t_8(i).w. Наш быстрый путь (`_m5[0..2] == 1`, однородная ветка `[[dont_flatten]]`): без
14 фиксов render scale на каждую выборку, центр t_8(a0) читается один раз и даёт и цвет, и альфу (отдельная выборка t_8(i) только при
`i ≠ a0`, т.е. за краем `wh`) — 3 выборки вне полосы фейда (Switch 4), 6 в полосе (7). Общий путь — дословно транслированный.
**EoCFog**: те же обратные величины (`1/k`, потом умножение) и порядок fma — потому бит в бит. Сэмплинг неявный (`texture`) как у
трансляции, NaN-защита координат (`isnan → 0`) сохранена во всех пяти.

**Включить на телефоне** (с разрешения хозяина; хост — ветка `dos2-shadows` или новее, там исправлено сравнение головы):
1. Сохранить `files/save`. Положить `packs/2a/native.xgspack.post-mobile` как `dm/shaders/native.xgspack` (81 КБ: 5 пост-ключей + 3 ключа
   теней `mobile`). Только пост, без теней — `work/shaders-native-mobile/post/native.xgspack`. Пак один: хост не ищет `native/<key>.xgs`,
   если пак есть.
2. `DM_GFX_NATIVE_SHADERS=2` — в логе `NVN native shader <key> matches ...` для всех 8 ключей, нет `mismatch`.
3. `DM_GFX_NATIVE_SHADERS=1` — `... used`. Картинка должна совпасть с выключенной подменой (результат тот же); сравнить GPU мс прохода
   (`DM_GFX_PASS_PROF`, если есть), особенно FadeBlending на Mali / PowerVR.

**Риски:** на устройстве не запускалось; интерпретатор берёт lod 0 у неявной выборки (полноэкранные проходы, у трансляции так же) и
возвращает 0 за краем texelFetch (обе стороны читают одинаковые координаты). Выигрыш в мс ожидается малым: проходы ограничены полосой.

## Условный рендер NVN: `DM_GFX_COND_RENDER` (14.11.2026, ветка `dos2-condr`; собрано, на телефоне не проверено)

Находка `GPU_BREAKDOWN.md` (п. 1 топа): хост игнорировал `SetRenderEnable(false)` / `SetRenderEnableConditional` («the host draws
unconditionally», p2b 17:06:28) и рисовал всё; на Switch GPU пропускает draw за проваленным occlusion-тестом. Перенос ETR PERF-E05
(`doom-eternal-port/src-cmp16` `namespace condr`, `e24b901` / `c812f46`) с поправками под DOS2.

**Режимы** (`dm/env`, читается раз; умолчание **0**):

| `DM_GFX_COND_RENDER` | что делает |
|---|---|
| 0 | как раньше, поток записей байт-в-байт тот же (HLE не пишет `OP_X_RENDER_COND`), строки `condrender` нет |
| 4 | **только счёт**: всё рисуется, предикат считается обоими способами (`would_skip2` / `would_skip3`). Включает подсчёт samples (см. ниже) — GPU дороже, для `gpu_ms` не годится |
| 2 | хост пропускает draw с проваленным предикатом; отчёт, ещё не вернувшийся с GPU, = «рисовать» (`stale`) |
| 3 | как 2, но по ещё не вернувшемуся отчёту судит значение в гостевой памяти — пара прошлого кадра (объект, ставший видимым, появится на кадр позже) |
| 1 | `VK_EXT_conditional_rendering` вокруг условных draw, предикат (как у 2) в host-visible буфере; без расширения — только счёт |

`SetRenderEnable(false)` в режимах 1–3 пропускает draw хостом. Предикат: два 16-байтных отчёта `NVNcounterData` по `addr` и `addr+16`,
сравнение слов счётчика; `RENDER_IF_NOT_EQUAL` (DOS2 шлёт режим 1) рисует, если счётчики различны (между отчётами прошли сэмплы).
Логика — чистый C `dm/gfx/vulkan/engine/xbg_condr.h`, хост-тест `tests/condr_test.c` (`clang -std=c11 -Wall -Wextra condr_test.c`).

**Отличия от Eternal (важно):**
- **Игра ни разу не сбрасывает счётчик** (`NVN PERF … ops=0.0/1.0/1050` — 0 ResetCounter; `occlusion=0.0 samples=0`). Хост считал
  сэмплы только после ResetCounter, поэтому все отчёты samples = 0 → пары «равны» → при `RENDER_IF_NOT_EQUAL` пропало бы **всё**.
  На Maxwell счётчик идёт с создания канала, поэтому при любом режиме ≠ 0 движок включает подсчёт с самого старта
  (`condr::init_counting` → `state.count_samples`; нужны precise-запросы, иначе режим → 4). Цена: occlusion-прогоны (один на отчёт:
  ~1050 в 3D-меню, ~226 в кампании, закрываются внутри прохода `DM_GFX_REPORT_INPASS`) — на a7xx ≈ 1,4 мкс каждый (ETR GPUCOST) →
  до ≈ 1,5 мс в меню. Выигрыш режимов 2/3 должен это перекрыть; проверяет ABBA.
- Отчёты попадают в гостевую память в `write_report` (таблицы отчётов, как `reptab` ETR, нет — запись всегда в гостевую память, т.е. туда,
  откуда читает предикат), но только при возврате слота. При 1 сабмите на кадр пара текущего кадра почти всегда ещё на GPU → режим 2
  почти ничего не пропустит, рабочий кандидат — **3**.
- Ожидающие отчёты ищутся по адресу (`pend_at`, O(1)), а не обходом `pending_reports` на каждое условие (~1000 × ~1000 за кадр).
- Пара, в которую в этом запуске ещё не писал ни один отчёт, = «рисовать» (`unknown`): иначе нули первого кадра или чужие остатки
  слота читались бы как «не видно».
- Условие, открытое в конце хэндла верхнего уровня, сбрасывается (`left_open`).

**DM_NB_OPS (NB-09/10):** при режиме ≠ 0 `SetRenderEnable*` внутри скобки draw идут в тень (последний вызов побеждает) и едут во флагах
`OP_X_NB_DRAW`; `xbg_nb.inc nbdraw::apply / unpack` применяет их до draw (`condr::from_nb`) так же, как `OP_X_RENDER_COND`. Любой сброс тени
(чужая запись, другой командный буфер, переполнение, конец скобки без draw) выписывает невзятое условие отдельной записью `OP_X_RENDER_COND`.
В сверке `DM_NB_OPS_CHECK` пишется и старая запись (её исполняет бэкенд). При режиме 0 тень и флаги прежние.
**NB-16:** `nb_hazard` в отводе на ReportCounter (`NB_HZ_COUNTER`) и SetRenderEnableConditional (`NB_HZ_COND`; SetRenderEnable(true/false) память не читает — не помечается) — только при `DM_NB`; `nb_track` не
включён (UBO / storage / CPU-копии ещё не отводятся), планировщик по-прежнему честно `untracked`. Вне прохода это узел `fence` в IR — номера
узлов в дампах `DM_NB_DUMP` после этого коммита сдвигаются относительно старых золотых кадров.

**Строки лога:** `NVN DM_GFX_COND_RENDER=<m> (…)`, `NVN cond-render: samples counted from the start …`, первые 24 оценки
`NVN cond-render: nvn_mode= addr= counters=a,b … known= stale= -> render|skip`, и раз в окно PERF:
`NVN PERF condrender mode= conditions= hidden= enables= disables= draws_inside= skipped= would_skip2= would_skip3= gpu= stale= stale_judged=
unknown= bad_addr= nb= left_open= …` (на кадр).

**Шаги на телефоне** (Mi 14 Ultra; бэкап `files/save` перед force-stop; одна правка `dm/env` за прогон):
1. `DM_GFX_COND_RENDER=4`, 3D-меню (≥ 2000 draw) и минута кампании (Форт Радости). Смотреть `condrender`: `conditions` / `draws_inside`
   (сколько draw под условием), `would_skip3` (сколько пропустил бы режим 3), `would_skip2`, `stale`, `unknown`. Первые 24 строки оценок:
   похожи ли `counters` на пару «до / после» (растут, разница = сэмплы). Если `unknown ≈ conditions` — пара не по `addr+16`, режимы 2/3
   бесполезны (ничего не пропустят), нужен разбор кода 63 / 61.
2. Если `would_skip3` ≥ ~300 в меню: ABBA `DM_GFX_COND_RENDER=0` ↔ `=3` (×2, по ~60 с), сравнить `gpu_ms` и `draws` в окнах меню ≥ 2000
   draw и в кампании; то же `=2` (ожидается почти без пропусков). `=1` — только если 3 выигрывает и хочется проверить GPU-путь.
3. Глазами: при повороте камеры в меню и кампании нет пропадающих / мигающих объектов (допустимо: объект, вышедший из-за угла,
   появляется на кадр позже), нет чёрных дыр на месте персонажей.

**Риски:** на устройстве не запускалось. Раскладка пары (`addr`, `addr+16`) взята из ETR / Maxwell — для DOS2 не разобрана (код 63
`0x93eddc`); `unknown` в шаге 1 это покажет. Принудительный подсчёт samples меняет то, что игра читает в отчётах (реальные числа вместо 0;
так на Switch) и отключает `async_skippable` у draw с подсчётом. С `DM_NB_RP` / `DM_NB=2` пропущенный draw всё ещё считается в сверке
(`nbchk::draw`), но не открывает проход. DrawTexture и draw помощников (blit / clear) условие не проверяют.

## Мобильное освещение: DeferredSky + ClusteredLightingWithSSAO (14.11.2026, ветка `dos2-lightsh`; собрано, на телефоне не проверено)
Кандидат 3 из `GPU_BREAKDOWN.md` (освещение — самый дорогой ALU кадра). Тот же метод, что тени и пост: свой GLSL в интерфейсе
транслированного бандла (голова байт в байт + новый SPIR-V, `DM_GFX_NATIVE_SHADERS=1`), результат бит в бит. Ключи — из графа p2b
(`NVN PGRAPH f=835 p10 fs=640a4c497bcc` 2 MRT → `p11 fs=d236b8d5b7ff` → `p12 fs=d8aeec640d46` DeferredFog) и `shaders-jobs/jobs.tsv`.
Сборка `python tools/dos2_light_mobile.py` (~8 с): `tools/dos2_post/{DeferredSky,ClusteredLightingWithSSAO}_mobile.frag` (+ `common.glsl`)
→ glslc `-O` vulkan1.1 → spirv-val → проверка интерфейса → `work/shaders-native-mobile/light/native/<key>.xgs`, `light/native.xgspack`
(2 ключа, 47 КБ), `light/all/native.xgspack` = **`packs/2a/native.xgspack.light-mobile`** (10 ключей: 3 тени `mobile` + 5 пост + 2 свет,
128 156 байт, SHA-256 `eb47d3a3…148975d`; `xgs_pack.py --check` — 0 расхождений); `light/stats.tsv`, `scenarios.tsv`, `equivalence.txt`.

**Что делают шейдеры** (разобрано по транслированному SPIR-V после `spirv-opt -O`; имена наши):
- G-buffer: t_E = (шероховатость, reflectance, металл, флаги·255: бит 0 FXEmissive, биты 1–2 модель затенения), t_A.xy — стереографическая
  нормаль (`fma(n, c1[0].x, −1.7777)`), линейная глубина, t_C — альбедо, t_8 — эмиссия, t_10 — касательная (октаэдр, x/y/z 12+12 бит) и
  анизотропия (w) только для модели ≠ 0, t_32(0,0) — экспозиция (0 → 0.01).
- **DeferredSky `640a4c497bcc748b`** — не небо, а IBL освещённых пикселей: out0 = префильтрованный куб(R, lod = rough·c6[2].y)·
  fma(fma(металл, альбедо, F0), lut.x, lut.y)·c6[2].z·100·e, out1 = (1−металл)·альбедо·lut.z·irradiance-куб(N)·c6[2].z·100·e,
  lut = t_30(N·V, rough); небо (d = 1) и модель 1 → 0. Оба MRT нужны: ClusteredLighting читает результат как t_14.
- **ClusteredLightingWithSSAO `d236b8d5b7ffad3c`** — солнце (c16 строки 0..2: .x направление, .z цвет; R подгибается к диску солнца при
  R·L < c1[0].z) + кластерные огни: кластер = (срез·c15[14].w + y/128)·c15[14].z + x/128, срез = clamp(fma(log2(d·c15[13].x), c15[14].x,
  c15[14].y), 0, 31); c12[2k] — начало в байтовом списке индексов c11, c12[2k+1] = точечных | прожекторов << 16; огонь в c13 — 48 байт
  (pos, 1/радиус; цвет, cos внешн.; направление, cos внутр.). Модель 0 — нормированный Блинн-Фонг (pw = 2/rough⁴), затухание
  sat(1 − (d²/r²)²)² / max(d², eps); модель ≠ 0 — анизотропный GGX по касательной из t_10. Тень (t_18, проход «Light Shadows») — только
  на солнце; t_14 прибавляется в конце: out = (t_14 + fma(max(0, spec + diff + E), e, U), 1). **SSAO в шейдере нет** (имя историческое;
  t_14 — IBL). Модель 1 / небо — `fma(max(0, E), e, U)`, альфа 0.

**Что убрано** (значение не меняется):
- поправка render scale у всех выборок (9 у света, 6 у Sky) — быстрый путь под одной однородной проверкой `_m5[0..9] == 1`; общий путь — дословно трансляция;
- лишние выборки: у неба / модели 1 не читаются t_A, t_C, t_14, t_18 (у Sky — t_A, кубы, LUT), у модели 0 — t_10 (−1 выборка на пиксель стандартных материалов);
- на огонь: скалярная адресация c11/c12/c13 (сдвиг / маска / загрузка на каждый float → одна загрузка vec4 на 16 байт), эмуляция rcp
  трансляции `x == 0 ? FLT_MAX : (x != 1 ? 1/x : x)` = `x == 0 ? FLT_MAX : 1/x` (1/1 точно; у `0.3183/(π·x)` проверка `== 1` нужна и
  оставлена), работа перенесена под ветки, которые её используют (вне радиуса / тыльная сторона), инварианты цикла вынесены (2/rough⁴,
  альфы GGX; `max(rough, c1[0].y) == rough`), мёртвое `1/(1 + |n|²)` в выборе эмиссии;
- прожектор вне конуса (att = 0): трансляция всё равно прибавляет цвет·0·спекуляр, т.е. ±0 к суммам, которые кончаются `max(0, x)` —
  пропуск даёт те же биты для любых конечных данных огней (отличие только там, где у трансляции сумма уже NaN: inf/NaN в цвете огня).

**Проверка** (`tools/spv_interp.py`; добавлены кубические текстуры и lod/bias: ненулевой lod масштабирует выборку на 1 + lod/8, так что
ошибка lod — расхождение; пост-шейдеры после этого по-прежнему 400 / 400): на шейдер 400 случайных вызовов (300 с render scale 1, 100 со
случайными масштабами), G-buffer со всеми моделями и флагами, d = 1, кадр 1800x806 (меняются тайлы кластеров), для света — 48 случайных
огней (в т.ч. 1/радиус = 0), сетка кластеров на 64 КБ (до 8 точечных + 4 прожекторов), байтовый список на 64 КБ, мировые точки вокруг
огней. **400 / 400 бит в бит** у обоих; сырой транслированный = -O на 50 / 50; плюс 8 сценариев × 60 освещённых пикселей с фиксированным
числом огней (без цвета солнца, эмиссии, t_14 и иногда без диффузного множителя — чтобы 1 ulp в огне доходил до выхода): все равны.
Контроль ловли (200 вызовов + сценарии): у Sky перепутанные x/y LUT — 152 / 200, lod спекуляра 0 — 152, −z куба — 94; у света
перепутанные тайлы x/y — 69 / 200 и 58 / 240 сценариев, перепутанный октаэдр — 10 / 200 и 13 / 240, переставленное умножение спекуляра
точечного огня `(col·F)·D` вместо `col·(F·D)` (1 ulp) — 0 / 200 случайных, но 18 / 240 сценариев. **Ловушка:** glslc `-O` сам сливает
`a*b + c` в `Fma` — где у трансляции отдельные FMul + FAdd, нужен `precise` (здесь таких мест не оказалось).

| ключ | шейдер | инстр. SPIR-V: сырой → -O трансл. → наш | исполнено на пиксель, средн. (макс.) | выборок на пиксель | SPIR-V, байт |
|---|---|---|---|---|---|
| `640a4c497bcc748b` | DeferredSky | 1107 → 455 → 427 | 243 (298) → 173 (215) | 6.58 → 6.30 | 20 840 → 10 988 |
| `d236b8d5b7ffad3c` | ClusteredLightingWithSSAO | 7655 → 2240 → 1463 | 906 (2560) → 508 (1262) | 8.09 → 6.81 | 128 164 → 34 516 |

«Исполнено» — случайные вызовы с render scale 1 (смесь неба / unlit / моделей, 0–8 точечных + 0–4 прожектора). Сценарии
(`scenarios.tsv`, освещённые пиксели):

| сценарий | модель 0: трансл. → наш | анизотропная модель |
|---|---|---|
| без огней (солнце + IBL) | 463 → 345 | 542 → 425 |
| 1 точечный | 576 → 392 | 659 → 473 |
| 1 прожектор | 592 → 399 | 675 → 480 |
| 8 точечных + 4 прожектора | 1874 → 945 | 2014 → 1047 |

Цена на огонь: точечный ≈ 113 → 47, прожектор ≈ 129 → 54 (анизотропный ≈ 117 → 48 / 133 → 55). **Куда идёт ALU света:** постоянная
часть ≈ 345 (декод нормали и G-buffer, вид / мир, кластер, солнце с подгибом R к диску: 4 inversesqrt + pow через exp2/log2); трансляция
добавляла к ней ≈ 120 (поправки масштаба выборок, адресация c12). Остальное — цикл огней: у трансляции ≈ 60 % его — адресная арифметика
(`>>2`, `&3`, загрузка по float, байт индекса), rcp-эмуляция и вычисления, ненужные вне радиуса / с тыльной стороны; у нас на огонь
≈ 25 инструкций геометрии (L, d², inversesqrt, N·L, проверка радиуса) + ≈ 22 освещения (H, inversesqrt, exp2/log2, 6 fma). Без потери
точности дальше почти некуда; дальше — только приближения (pow → полином, общий `inversesqrt`) или меньше огней на кластер (тайлы
128 px крупные: каждый пиксель тайла перебирает все огни кластера). iPad-вариант больше (2634 инстр.) — замена сделана по Switch.

**Включить на телефоне** (с разрешения хозяина; хост — ветка `dos2-shadows` или новее):
1. Сохранить `files/save`. Положить `packs/2a/native.xgspack.light-mobile` как `dm/shaders/native.xgspack` (128 КБ: 3 тени `mobile` +
   5 пост + 2 свет). Только свет — `work/shaders-native-mobile/light/native.xgspack`. Пак один.
2. `DM_GFX_NATIVE_SHADERS=2` — `NVN native shader <key> matches ...` для всех 10 ключей, нет `mismatch`.
3. `DM_GFX_NATIVE_SHADERS=1` — `... used`; картинка как без подмены (огни, металл, анизотропные материалы — волосы, ткань);
   GPU-время проходов p10 / p11 (`DM_GFX_PASS_PROF` / `NVN GPASS`) до / после.

**Риски:** на устройстве не запускалось; интерпретатор не моделирует мипы (lod — только множитель) и считает Fma через double.
Чувствительность случайного теста к 1-ulp ошибкам в мелких слагаемых низкая (см. контроль) — поэтому код повторяет порядок операций
трансляции дословно, а сценарии без солнца / эмиссии ловят 1 ulp в огнях. Выигрыш в мс не мерился: оценка `GPU_BREAKDOWN.md` —
−0,6…−1,2 мс на 750 (проход полноэкранный и ALU-тяжёлый, выборок на 1–3 меньше).

### Черновик для KB (shaders-translation.md / performance.md)
```
### SH-DOS2-05 Нативное отложенное освещение бит в бит (DeferredSky + ClusteredLightingWithSSAO, DM_GFX_NATIVE_SHADERS)
- Симптом/цель: самый дорогой ALU кадра; трансляция Maxwell: поправка render scale на каждую выборку, скалярная адресация UBO в цикле
  огней, rcp-эмуляция, всё вычисляется до веток.
- Фикс: tools/dos2_light_mobile.py + tools/dos2_post/*_mobile.frag; один однородный быстрый путь при _m5 == 1, vec4-загрузки огней,
  работа под ветками, инварианты из цикла, выборки только там, где нужны. Исполнено на пиксель: свет 906 -> 508 (12 огней 1874 -> 945),
  Sky 243 -> 173; выборок 8.1 -> 6.8.
- Проверка: spv_interp.py 400/400 бит в бит + сценарии; glslc -O сливает a*b+c в Fma (писать precise, где трансляция не сливала).
  На телефоне не проверено. Пак packs/2a/native.xgspack.light-mobile (тени + пост + свет).
```

## Лимит кадров 90 / 120 / без ограничения: `DM_DOS2_FPS` (16.11.2026, ветка `dos2-fps`; собрано, на телефоне не проверено)
Зачем: игрокам с экранами 90 / 120 Гц и для замеров прироста в fps на сильных телефонах. На Mi 14 Ultra (Adreno 750, 120 Гц) игра упирается
в 60 при GPU ~10 мс, частоты плавают — A/B на 60 разницы в fps не показывает.

### Что держало 60
- **Хост-пейсер** (`hle_nvn.c present_period → present_auto_hz`): шестерёнка ставила `DM_PRESENT_MAX_HZ=30|60` (общий ряд
  `GearPages.fpsLimit`, у DOS2 `Tl2Gear.FPS_LIMITS = {30, 60}`), период = `nvn_pace_cap_hz(экран, 60)` → 60 на 120 Гц (45 на 90 Гц). Без
  переменной — `nvn_pace_hz(экран, 60)`, тоже 60.
- **VSyncDivider 2 игры** (консольные `GraphicSettings +0x24 = 2`, читатель 0xa68758 копирует его в конфиг окна → интервал present 2 = 30 fps
  на Switch) **уже срезан**: при окне `hle_nvn.c` работает в режиме `display` (`g_display_mode`, «каждый тик — кадр», интервал игры не
  смотрится). Поэтому порт и идёт в 60, а не в 30.
- **Голос Android** (`vulkan_present.cpp frame_rate_vote = 120`, `perf_android.c vote`) — только просьба к дисплею; темп не меняет.
- **Свой ограничитель игры** (статически, main.raw 4979b200): объект приложения, ограничитель +0x90 (`0xa7dc60 SetFPS`: период мкс =
  1e6 / fps, 0 = нет; `0xa7dd00 Wait`: спит до периода). Цель кадров ставит `0xcb8f10` по состоянию клиента (`[obj+0x10]`, имена —
  таблица main+0x29bddf8): маска 0x6fb1ff5e → **60** для Init* / Load* / Unload* / SwapLevel / Exit / Disconnect / Join / StartLoading /
  StopLoading / StartServer / **Movie** / Installation / ModReceiving / **Lobby**, **0 (без лимита)** для Unknown / Idle / **Menu** / Paused /
  PrepareRunning / **Running** / Save / GameMasterPause / BuildStory. `FrameCapEnabled` (+0x71, консоль 0) / `FrameCapFPS` (+0x72 u16, консоль
  30) — PC-опция, на консоли выключена. Итого: в игре и в меню игра сама не ограничивает, загрузки и видео — 60.

### Время игры (проверка «не ускорится ли на 120»)
- Главный цикл (0xb076f4 → 0xb07714): `dt = (now − prev)` ограничителя (мкс / 1e6), **не больше 1/15 с** (0x3d8888b5), дальше
  `GameTime {double Time += dt; float DeltaTime = dt; int Ticks++}` и накопитель `[app+0xc8]`. Шаг переменный, по реальному времени.
- Поток состояний с загрузкой (0xcb8850): свой пол 33 мс (`sleep(33000 − elapsed)`), `dt ≤ 0,2 с` — от частоты present не зависит.
- Опыт: на Switch игра идёт в 30, порт уже месяцами в 60 (×2) — жалоб на скорость анимаций / ходьбы / роликов нет.
- Что по кадрам: `GameTime.Ticks` (счётчик кадров) — его читатели не разобраны; Iggy (SWF-интерфейс) тикает по своему времени (строка
  «Cannot maintain framerate specified in SWF file»); PhysX — переменный шаг из `dt`. Это и проверить глазами на 120 (ниже).
- Вывод: выше 60 оставлено для игроков (экспериментально), не только тестом; если найдётся что-то по кадрам — опустить ряд до 60 / тест-only.

### Как устроено
- `nvn_pace.h`: `nvn_fps_parse` (0 / 30 / 60 / 90 / 120, пробелы вокруг допустимы, иначе −1 = как раньше), `nvn_pace_target_hz(экран, цель)`:
  0 → без пейсинга; 30 / 60 → `nvn_pace_cap_hz` (ровно прежнее правило, умолчание 60 не меняется); 90 / 120 → целая доля частоты экрана
  в пределах 2 % (ровно: 120 на 120 / 240, 90 на 90 / 180, 119.88 → 119.88), иначе не выше частоты экрана (120 на 60 / 90 Гц = частота
  экрана), иначе сама цель неровно (90 на 120-герцовом без режима 90: кадры по 1 и 2 обновления, в среднем 90). `nvn_fps_vote_hz`: 90 голосует
  90, остальное — маркер / 120.
- `nvn_vk.c dm_dos2_fps()` читает `DM_DOS2_FPS` один раз (env-файл к первому present уже прочитан); `hle_nvn.c present_auto_hz` при цели
  ≥ 0 берёт `nvn_pace_target_hz`, иначе старые правила. Если частота экрана ещё не пришла, DOS2 всё равно включает авто-режим и display-интервал
  (без этого был бы фиксированный 60 Гц / интервал игры 2).
- Present остаётся **FIFO** (KB UNP-09: MAILBOX на этих путях заменяли на FIFO; отдельного довода за MAILBOX на Adreno / Turnip нет).
  «Без ограничения» = без хост-пейсинга, FIFO держит не выше частоты экрана; > 120 на 120-герцовом не будет.
- ADPF (`perf_android.c`) и так берёт цель из периода present (8,33 мс на 120, 11,11 на 90); без ограничения — 1 / частота экрана.
  Термо-голос (SEVERE) считается от периода как раньше: для 90 / 120 не опускается (кратно цели ≥ 60), без ограничения — 60 Гц.
- Голос окна (`vulkan_present.cpp`): без маркера `gfx-frame-rate` — `dm_dos2_fps_vote` (90 → 90, иначе 120 как было).
- Шестерёнка (`Tl2Gear.dos2Fps`, вместо `GearPages.fpsLimit` только для DOS2; общий `frontend-common` не тронут, ряд — в своём `Tl2Gear.java`):
  30 / 60 / 90 / 120 / «Без ограничения», pref `fps_limit` (0 = без ограничения; сохранённые 30 / 60 остаются), след. запуск. 30 / 60 → как
  раньше `DM_PRESENT_MAX_HZ`; 90 / 120 / без ограничения → `DM_DOS2_FPS` (+ `DM_PRESENT_MAX_HZ` снят). 90 / 120 показываются, только если у
  дисплея есть режим кратной частоты (`GearDisplay.modeFor`); сохранённое значение без режима опускается до ближайшего доступного.
  Строки `dos2_fps_uncapped`, `dos2_fps_note` (EN / RU, `d3_settings.xml`).
- После `nativeStart` (`Tl2Gear.dos2FpsApply`): действующая цель (`DM_DOS2_FPS` из env-файла главнее шестерёнки), одна строка в boot.log с
  целью и режимами дисплея; для 90 / 120 окно просит режим кратной частоты (той же разрешающей способности), для «без ограничения» — самый быстрый
  режим текущего разрешения; 30 / 60 ничего не просят (как было).

### Шаги на телефоне (Mi 14 Ultra; бэкап `files/save` перед force-stop, после логов — force-stop)
1. `dm/env`: `DM_DOS2_FPS=120` (или `90`, `0`; без строки — шестерёнка). Для A/B оставить DRS выключенным (умолчание шестерёнки) — иначе
   `DM_DOS2_DRS` без `_TARGET_MS` возьмёт цель из периода (7,9 мс на 120) и начнёт снижать разрешение.
2. В boot.log: `[java] fps target 120 (DM_DOS2_FPS): display modes <id@Гц/WxH …>, current …, requested <id> @ 120.0 Hz`;
   `[fps] DOS2 target 120: present pacing 120.00 Hz on a 120.00 Hz screen (even: …)`; `[perf] ADPF target 8.33 ms`;
   `[perf] window frame-rate vote 120 Hz` (для 90 — `90 Hz`). Для 90 на телефоне без режима 90 — `uneven: …`.
3. `NVN PERF … fps=` в игре (Running) — выше 60, примерно min(120, 1000 / GPU мс) (GPU 10 мс → ~100); на экранах загрузки и в видео — 60
   (ограничитель игры). Строки `NVN PERF` идут раз в 60 кадров — на 120 вдвое чаще.
4. Скорость игры: одна и та же пробежка / анимация / диалог на 60 и на 120 (секундомер, звук ролика в синхроне с губами), интерфейс (панели,
   подсказки) — без ускорения. Это и есть проверка `GameTime.Ticks`.
5. `DM_DOS2_FPS=0`: `[fps] DOS2 target uncapped: present pacing 0.00 Hz … (uncapped: …)`, fps ≤ частоты экрана.

### Риски / не проверено
- Ничего не прогнано на телефоне. Читатели счётчика кадров `GameTime.Ticks` не разобраны — если что-то в UI / эффектах ускорится, оставить
  > 60 только тестовым (ряд шестерёнки 30 / 60).
- HyperOS может не дать режим 90 (или навязать частоту): тогда 90 неровный, пейсинг идёт по частоте, которую сообщает `Display`.
- Больше нагрева и расхода на 90 / 120; на Mi 14 Ultra частоты GPU и так плавают — сравнивать fps прогонов одинаковой длины после прогрева.
- Переключение режима дисплея после старта — одно на запуск; на части телефонов возможна короткая чёрная вспышка.
- Тесты: `dm/gfx/tests/t_pace.c` (+ cap, цели, разбор, голос, пейсер на 120 / 90 / 0; 84 проверки), `source_structure_test.py` — PASS.

## Present на своём потоке: `DM_GFX_PRESENT_THREAD` (KB PERF-D07; 16.11.2026, ветка `dos2-present`; собрано, на телефоне не проверено)
Зачем: на Mi 14 Ultra (палуба корабля, `DM_DOS2_FPS=0`, 16.11) кадр 9,4 мс = `submit_cpu_ms` 5,5 + `present_cpu_ms` 3,6 на потоке трансляции;
в present ≈ 1,8–2,3 мс настоящей работы (fence / acquire / blit / QueuePresent), остальное — ожидание GPU (8,4 мс); нагретый телефон
упирается в CPU (`NATIVE_BACKEND_PLAN.md`, NB-20b-ctx, п. 2 «что рекомендую вместо»). Приём — DOOM 2016 `DM_GFX_PRESENT_THREAD`
(`vulkan_present.cpp pt::loop`), оформлен общим модулем `port-common/modules/present-thread` (XC2 / D3 / TL2 возьмут его же).

### Переключатели
- **`DM_GFX_PRESENT_THREAD`** (умолчание **0**; у D16 — 1): `1` — present на потоке `dm-present`; `2` — тот же протокол (замок очереди,
  передача) на потоке трансляции без отдельного потока — цена самого протокола; `0` — как раньше (замок — одно сравнение). Читается при
  инициализации движка.
- `DM_GFX_PRESENT_UNPIN` (1, как D16): поток present уходит с ядер транслятора; `0` — оставить маску, унаследованную от транслятора.
- `DM_GFX_PRESENT_CPUS=<hex>` (как D16): маска потока present вручную.

### Как устроено
- `dm/android/present_thread.h` (новый, чистый C++): `dmpt::Handoff` — кольцо из 4 заданий, тикеты `enq` (передано) / `sub` (blit
  подан в очередь) / `done` (завершено), ожидания только на condition variable; `dmpt::place_mask` — ядра потока present.
- `vulkan_present.cpp`: прежний `present()` разрезан на `present()` (замок рендера + `pin_update` + `present_body`) и `present_body` (всё
  остальное без изменений: fence полёта, acquire, letterbox, RCAS, blit, submit, QueuePresent, пересоздание swapchain / Surface, ожидание
  окна в фоне, `dm_aperf_present`). Все `vkQueueSubmit` / `vkQueuePresentKHR` / `vkQueueWaitIdle` файла идут через три `#define`-обёртки
  с замком очереди (в режиме 0 замок не берётся); обёртка submit на потоке present отмечает «blit подан», обёртка present — время показа.
  Поток `dm-present` (nice −8) создаётся при первой передаче, исполняет `present_body` под замком рендера по порядку тикетов.
- Движок (`engine/xbg_present_thread.inc`, `namespace ptq`, + 8 однострочных правок `xbg_game.cpp`): перед **каждой** подачей слота
  `ptq::before_submit()` — ждать, пока все переданные present подали свой blit (blit кадра N всегда между слотом N и слотом N+1 — тот же
  порядок очереди, что при present на месте); подачи слота и самотеста BC — под замком очереди; `xbg_legacy_finish` / `xbg_game_shutdown` —
  сначала дождаться потока present, потом `vkQueueWaitIdle` под замком; в `xbg_present_texture_now` вместо `cfg.present` — `ptq::handoff`
  (кроп копируется в задание). Ошибка present на потоке останавливает игру на потоке трансляции при следующей подаче / передаче
  (`Game presentation failed: N (present thread)`), как раньше — на месте.
- `nvn_vk.c`: для каждого номера present хранится тикет потока present. `mtl_present_wait` (AcquireTexture ждёт прошлый present этой
  текстуры) ждёт, пока его blit в очереди — контракт `nvn_obj.h` (как у Metal-потока present); обычно он давно выполнен. `mtl_presented_time`
  — момент, когда поток present поставил кадр в очередь показа.
- Пейсер не тронут: темп задаёт `hle_nvn.c` (`nvn_pace`, `DM_DOS2_FPS` 30 / 60 / 90 / 120 / 0, слот показа и `avail` текстуры), FIFO
  swapchain держит частоту экрана. Порядок «подача → present → номер кадра» на потоке трансляции (NB-21 `x_run_own`, тикет present = номер
  задания кадра) не меняется: present просто возвращается после передачи.
- **ADPF**: `dm_aperf_present` теперь зовёт поток present, поэтому он — первый поток сессии и всегда в ней (`perf_android.c` не тронут);
  поток трансляции входит по нагрузке (≥ 15 % ядра, сканы каждые 300 present), как главный поток игры и воркеры. С модулем `adpf-boost`
  правило то же.
- **Ядра**: поток рождается на запиненном трансляторе и наследует пин (урок D16). Поэтому при старте и каждые 128 present: `DM_GFX_PRESENT_CPUS` >
  план модуля `cpu-place` (если он в дереве) > все ядра, кроме `pin_mask` / текущей маски транслятора, и без самого медленного класса, если
  остаётся другой. Mi 14 Ultra (транслятор на cpu7): ожидаю `cpus 80 -> 7c` (A720, без A520). Если транслятор не запинен — все ядра.
- Строки: `NVN DM_GFX_PRESENT_THREAD=1: …`, `NVN present thread mode=1 (…)`, `[pc] present-thread: applied (…)`, при первом present —
  `NVN present thread started tid=… nice=… inherited cpus=…` и `NVN present thread tid=… cpus X -> Y (all but the translation thread's cores;
  translation thread T on Z) result=0`; рядом с каждой `NVN PERF v=1` — `NVN PERF present_thread mode=1 handed=… jobs=… busy_ms=… full_ms=…
  wait_ms=… waits=… drain_ms=… drains=…` (на кадр: передано, исполнено, время потока present на present, ожидание свободного места в кольце,
  ожидание поданных blit перед подачей слота, ожидание потока present при finish).

### Шаги на телефоне (Mi 14 Ultra; главный агент, с разрешения хозяина; keyguard проверить, бэкап `files/save` перед force-stop, после логов — force-stop)
База `dm/env` как в d0base: `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_FPS=0 DM_NB=1`, палуба корабля, по 90 с, чередовать (нагрев): **P0** база;
**P1** `+ DM_GFX_PRESENT_THREAD=1`; **P2** `+ DM_GFX_PRESENT_THREAD=2`; потом P1 и P0 ещё раз (ABBA).
1. Старт P1: строки выше, `cpus … -> 7c` (или другое — записать), `[perf] ADPF session threads:` — через несколько секунд (скан каждые 300 present) в списке и поток present, и
   tid транслятора (`nvn-translate` в `[cpu]` / `[nvn]` строках).
2. Решающие числа (`NVN PERF`, медианы окна): **`present_cpu_ms`** — жду 3,6 → ≈ 1,3–1,8 (остаются подача слота и ожидание поданного blit);
   **`submit_cpu_ms`** — без изменений (≈ 5,5; не должен вырасти больше чем на 0,2 — иначе мешает соседство ядер / замок очереди); **fps** —
   рост там, где упор в CPU (нагретый телефон; холодный стоит на GPU 8,4 мс — там 0…+5 %); `gpu_ms` — без изменений. Строка `present_thread`:
   `busy_ms` ≈ 1,8–3 (поток present медленнее на A720, это нормально, пока < кадра), `wait_ms` ≤ 0,3, `full_ms` 0, `drains` 0 в игре.
   P2 против P0: цена протокола — `present_cpu_ms` и fps в шуме.
3. Темп не меняется: `DM_DOS2_FPS=60` и `=120` с P1 против P0 — fps 60,0 / ≈ min(120, GPU), строки пропусков кадров (`@display_ms`,
   сводка раз в 300 кадров) и `[perf] ADPF target 16.67 / 8.33 ms` те же.
4. Жизненный цикл (с P1): «домой» → вернуться (3 раза; один раз подождать 30 с в фоне), блокировка экрана → разблокировка, разворот телефона на
   180° (обратная альбомная — пересоздание swapchain), загрузка сохранения. Нельзя: зависание, `Game presentation failed`, `【红】` про
   очередь / тикеты; можно: новые `Swapchain:` строки, `drains` > 0 в окне паузы. Картинка та же.
5. По желанию: сборка `-Validation` + `DM_VK_VALIDATION=1` с P1 — без новых `SYNC-HAZARD` против P0.
Умолчание 1 — только после 2–4 на Mi 14 и одного прогона на слабом телефоне (класс Snapdragon 7 / MTK).

### Проверки (ПК)
- `port-common/modules/present-thread/test/t_present_thread.py <дерево>` (WSL g++ 11, TSan и ASan + UBSan; код вырезается из дерева как
  есть): порядок очереди = порядку present на месте (6 сидов), ни одного одновременного вызова очереди, drain перед каждым `WaitIdle`
  движка, копия кропа в задании, ≤ 4 переданных, `pin_update` на потоке трансляции, AcquireTexture после `dm_present_wait_ticket` видит
  blit в очереди, времена показа монотонны; режимы 2 и 0; ошибка present (стоп через ≤ 1–2 подачи с её VkResult); фон 3 × 200 мс (< 40 мс CPU,
  движок встаёт за кадр, потом идёт дальше); простой поток спит; ядра (уход с cpu0 запиненного транслятора, `pin_mask`, `DM_GFX_PRESENT_CPUS`,
  `UNPIN=0`); две мутации ловятся (нет ожидания перед подачей — нарушения порядка в 6 / 6 сидах; нет замка — перекрытия). 39 прогонов, PASS.
- `dm/gfx/vulkan/engine/tests/source_structure_test.py` — PASS. Сборка `libdm.so` + `work/DOS2-port-debug.apk` в `wt-present`: новых
  предупреждений нет (`xbg_game.cpp` 13, `nvn_vk.c` 2 — как в базе).

### Риски / не проверено
- **На телефоне не исполнялось.** Цифра выигрыша — оценка NB-20b-ctx; у D16 вышло меньше ожиданий, пока поток present сидел на ядре
  транслятора (здесь это закрыто, но соседство с главным потоком игры / воркерами / `dm-nbmt` на A720 возможно).
- Замок очереди: пока поток present держит `vkQueuePresentKHR` / `vkQueueWaitIdle` (пересоздание swapchain), подача слота ждёт. Видно как
  рост `submit` / `present_cpu_ms` в P1.
- ADPF: первые ~300 present (≈ 3 с) в сессии только поток present; если транслятор < 15 % ядра (30 fps, лёгкая сцена), он в сессию не войдёт —
  там это не узкое место.
- AcquireTexture теперь ждёт, пока blit прошлого present текстуры в очереди (раньше это давал порядок потока трансляции). Если поток present
  отстал (acquire ждёт композитор), игра ждёт здесь, а не позже. `mtl_presented_time` — время потока present; пока он не дошёл, «нет данных» (0).
- Время жизни образа: окно-текстура не должна уничтожаться в том же кадре до своего present — окно риска у потока больше (blit пишется позже);
  в игре такого не видно.
- Ошибка present всплывает на кадр позже. Поток не останавливается (живёт с процессом), объект передачи не освобождается.
- Другие подачи в очередь вне `xbg_game.cpp`: `xbg_selftest.cpp` (только при инициализации, до первого present), `xbg_vulkan.cpp` (в DOS2 не
  вызывается). Новый код с `vkQueue*` в движке — только через `ptq::` (тест модуля считает прямые вызовы в `xbg_game.cpp`).
