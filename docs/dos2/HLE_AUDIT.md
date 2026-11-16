# DOS2: аудит nn::-импортов main (14.11.2026, ветка `dos2-hleaudit`)

Прошлая сверка (DOS2_PORT.md «HLE: сверка импортов») сравнивала импорты main только с импортами TL2. Здесь — каждый nn::-импорт main
(381 C++-имя + 63 C-функции nnos / nnsocket / nnssl / nnResult / nndetail) против того, что реально собрано в `libdm.so` для `-DDM_GAME=dos2`,
и для всего, что хост не берёт, — куда этот код ведёт внутри нативного sdk.

## Как устроен путь вызова (то, на чём держится аудит)
- Загрузчик (`loader.c`): для каждого символа релокации сначала таблица `swforce` (HLE_FORCE / SKF / SKD / SSF / LANF …), потом определения в
  загруженных модулях (sdk экспортирует всё nn::), и только потом `swhle`. В сборке dos2 `swhle` = одна запись (`getentropy`), то есть
  **хост берёт ровно то, что в `swforce`** (513 записей после правки); всё остальное исполняет нативный sdk 9.3.
- sdk 9.3 вызывает свои же экспортные функции через PLT (7986 заглушек), поэтому HLE_FORCE ловит и внутренние вызовы sdk.
- Клиенты сервисов sdk выходят к ядру только через `nn::sf::hipc::*` (`hle_sf.c`, всё forced). Сервисов (sm, процессов) у хоста нет:
  `ConnectToHipcService` отказывает (0xE15, строка `【红】sf: the SDK connects to service X`), кроме имён из `DM_SF_SERVE`.
  svc без своего `case` в `hle_svc.c` / `hle_thread.c` отвечают «успехом-заглушкой» и пишут `SVC 0x.. (占位返回成功)`.
- В логах запусков до кампании (`src/work/logs/o2/boot.log`, `r5.log`, `o1`): **ни одной** строки `sf:`, ни одного `SVC 0x..`-заглушки,
  `stubs=0` (нет нерешённых импортов). Т.е. до загрузки кампании ни один нативный путь в сервис не ходил.

## Метод
Скрипт (scratchpad сессии, не в репо): таблица `swforce` из `work/build-android/libdm.so` (секция `swforce`, RELR — указатели на месте);
для каждого не взятого импорта — обход графа `bl`/`b` по `sdk.raw` (границы функций = экспорты + цели `bl`; PLT → имя → если forced, лист;
diag/abort — стоп), отметки: `svc #n`, вызовы `ConnectToHipcService` с именем сервиса из `adrp/add x1` перед вызовом.
Слепое пятно — `blr` (виртуальные вызовы); его закрывает то, что любой выход в сервис всё равно идёт через forced `sf::hipc` и виден в логе.

## Итог по пространствам имён
F = берёт хост (forced), N = нативный sdk без выхода в сервис, S = нативный sdk с выходом в сервис.

| ns | F | N | S | Что в нативной части и почему это безопасно |
|---|---|---|---|---|
| fs | 25 | 0 | 0 | всё forced: Mount/Ensure/Extend/GetSaveDataSize, Commit (транзакция clone+swap), Create/Delete/Rename, Open/Read/Write/Flush/GetSize, каталоги (Open/Read/Count/Close, DeleteRecursively), MountRom, MountTemporaryStorage |
| account | 20 | 0 | 0 | всё forced (Initialize, ListAllUsers, OpenPreselectedUser, Nickname, NSA-функции, AsyncContext, LoadProfileImage). `StoreSaveDataThumbnailImage` main не импортирует |
| friends | 15 | 1 | 0 | `Friend::GetAccountId` — чтение поля объекта |
| oe | 15 | 0 | 0 | всё forced: фокус, режим работы, уведомления (OperationMode / PerformanceMode / Resume), Exit-request section, язык, CPU boost |
| hid | 16 | 0 | 0 | forced: Npad FullKey / JoyDual / Handheld, StyleSet, SupportedNpadId / StyleSet, JoyHoldType, вибрация (handles / init / send / GetActual), мышь, клавиатура. Six-axis, JoyLeft/Right, DebugPad main не импортирует |
| audio | 14 | 21 | 0 | forced: AudioOut целиком, Open/Close/GetWorkBufferSize рендерера, ручной рендер (`hle_audren.c`). Нативно: Config/Voice/MemoryPool/FinalMix/DeviceSink, Start/Stop/RequestUpdate — через vtable в локальный CPU-рендерер SDK, который открыл хост; звук на телефоне есть (r2–r5) |
| ldn | 24 | 2 | 0 | `MakeIpv4Address`, `MakeBroadcastAddress` — арифметика |
| nifm | 10 | 0 | 0 | всё forced, сеть «недоступна» |
| socket | 32 | 7 | 0 (было 1) | нативно `Inet*` (aton/pton/ntoa/htonl/htons/ntohl/ntohs) — чистые; **`GetHostEntByName(const char*)` уходил в sfdnsres → добавлен** |
| nnsocket* / nnssl* (C) | 17 + 19 | 2 | 0 | `nnsocketInetHtons/Ntohs` — чистые; остальное forced (offline) |
| ssl | 5 | 0 | 0 | forced |
| err | 6 | 5 | 0 | forced ShowError (оба), ctor ErrorResultVariant (SDK-конструктор), IsNetworkServiceError; нативно операторы/GetState — поля объекта |
| web | 3 | 7 | 0 | forced ShowWebPage (отказ), ctor Arg / ReturnValue (SDK-конструктор); нативно сеттеры Arg (TLV в буфере игры) и геттеры ReturnValue |
| swkbd | 1 | 5 | 0 | forced ShowKeyboard (`hle_swkbd.c`); нативно MakePreset, SetGuideTextUtf8, SetInitialTextUtf8, GetRequired*Size — запись в структуру; раскладка проверена для 9.3 (ниже) |
| time / settings | 5 / 3 | 0 | 0 | forced |
| kpr / ngc / nsd / vi / codec | 1 / 4 / 1 / 4 / 1 | 0 / 1 / 0 / 0 / 0 | 0 | `ngc::CountNumbers` — чистая |
| os (C++ и nnos*) | 2 | 79 + 20 | 0 | потоки, мьютексы, события, условные переменные, очереди, TLS, память — svc 0x01–0x35, 0x45 (все обслуживает хост) |
| mem | 0 | 10 | 0 | StandardAllocator — svc памяти (0x02/0x03/0x06/0x2C/0x2D) |
| util / crypto / image / cstd | 0 / 1 / 0 / 0 | 16 / 9 / 7 / 2 | 0 | UTF-конвертеры, SNPrintf, DecompressZlib, SHA1/MD5/AES, JpegDecoder, IsInf/IsNan — чистые (svc только блокировки) |
| diag / nnResult* / nndetail* | 0 | 1 + 4 + 1 | 0 | AbortImpl → svc Break → отчёт хоста |

**Не взятых импортов с выходом в сервис: 1 (`socket::GetHostEntByName(const char*)`) → закрыт.**

## Отдельно проверено
- **swkbd (имя персонажа)**: DOS2 вызывает ShowKeyboard из одной точки (main 0xbdf7a0…0xbdfa98): `MakePreset(cfg, 0)`, `isUseUtf8 = 1`
  (arg+0x3bd), max длина = min(поле, 500) в arg+0x3ac, рабочий буфер `GetRequiredWorkBufferSize(false)` (0x1000) в arg+0x4d0/+0x4d8,
  `SetGuideTextUtf8` (sdk 9.3 пишет u16[257] в cfg+0x1a8), `SetInitialTextUtf8` (длина в +0x3c4, смещение 0x14 в +0x3c0, текст в
  рабочем буфере), строковый буфер 0x7D4, результат `0x29F` = отмена. Это ровно раскладка, которую читает `hle_swkbd.c`
  (KeyboardConfig = 0x4d0 байт, как в 3.3) — правка не нужна. Риск: при отмене игра зовёт колбэк `[obj+0x60]` (пустой → `abort`), так и
  на Switch.
- **Сохранения**: все fs-импорты forced; коммиты в логе идут (`存档提交 #N … 原子换入`), Profile/config/_ftime_ пишутся.
- **Steam Cross-save** (диалог в меню): игра открывает `ShowWebPageArg(https://steamcommunity.com/oauth/login…)`, `ShowWebPage` отказывает,
  игра пишет `ERR> Operation has failed!` и продолжает — чисто.
- **Выход из игры**: `exit` / `_Exit` / возврат из nnMain → `__nnmusl_ProcessExit` → `nnosExitProcess` → svc 0x07, у которого нет
  `case` (заглушка-успех) → возврат в `udf` (sdk 0xce7c8) → SIGILL и ложный отчёт о падении. В main: 0xc03c64 (`_Exit` после разборки
  подсистем), 0x7ce8f4 (`exit(-1)` при отказе OpenPreselectedUser). **Добавлен** перехват `nnosExitProcess` → `dm_host_shutdown`.
- ro (NRO), aoc, six-axis, NFP, ec: main не импортирует.
- Падение в o2 (`FATAL: Nontrivial NVN alpha test not translated`) — не HLE, ветка `dos2-alpha`.

## Что добавлено (`dm/host/hle_dos2.c`)
| Имя | Ответ |
|---|---|
| `_ZN2nn6socket16GetHostEntByNameEPKc` | NULL (offline), строка в лог; единственный вызов main 0x789338 проверяет NULL |
| `nnosExitProcess` (C, через PLT sdk) | `dm_host_shutdown("DOS2 exit")` (аудио/графика стоп, `_exit(0)`), не возвращается |

Сборка: `cmake --build work/build-android --target dm` — ок; обе записи в `swforce` новой `libdm.so`.

## Риски, которые статикой не закрыть
1. `blr`-пути: любой выход в сервис всё равно виден в логе как `【红】sf: … service X` — грепать после каждого прогона.
2. svc без `case`: строка `SVC 0x.. (占位返回成功)`; кроме 0x07 (теперь не достигается через ExitProcess) других не встречалось.
3. Семантика forced-ответов под DOS2, не проверенная на телефоне: коммит больших сохранений (clone+swap всего дерева на каждый Commit —
   время «克隆换入» на сейве кампании 10–50 МБ), ShowKeyboard блокирует поток игры на время диалога Android, ExitRequest от oe хост не шлёт,
   GetSaveDataSize = 256 МБ при NACP 24 МБ (безвредно), Steam-логин всегда «Operation has failed!».

## Что должен прогнать телефон (бэкап `files/save` перед force-stop)
1. Новая игра → создание персонажа → ввод имени (Android-диалог; кириллица и латиница, отмена, пустая строка) → старт.
2. Быстрое сохранение и ручное сохранение (новый слот, перезапись) в Форте Радости; смотреть время коммита в логе.
3. Загрузка этого сохранения из меню паузы и из главного меню (Continue / Load), удаление слота.
4. Options: графика/звук/управление, применить, выйти — `config.lsx` коммитится.
5. Выход в главное меню и (если в меню есть) выход из игры — в логе `[dos2] the game exits (nnosExitProcess)`, без crash.txt.
6. После прогона: `grep -E "sf: |SVC 0x|UNIMPL|\[dos2\]" boot.log`.
