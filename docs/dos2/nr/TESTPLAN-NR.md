# TESTPLAN-NR — один APK, все шаги NR по переключателям (ветка `dos2-nr-all`)

План телефонных прогонов для хозяина / главной сессии. Все шаги NR-01…NR-31 (NR-26 — внутри NR-13) собраны в одной ветке
`dos2-nr-all`, каждый включается своим `DM_*` в `dm/env`. Порядок ниже: сначала то, что ничего не меняет в картинке (счёт / сверка), потом
включение и A/B. Подробности по каждому шагу — в его `docs/dos2/nr/NR-XX.md` (раздел «Прогон на телефоне»); здесь — только порядок,
env, сцена, длительность и строки лога, которые решают.

## 0. APK и что в нём включено по умолчанию

- APK: `wt-nrall/work/DOS2-port-debug.apk` (54.9 МБ, SHA-256 `a876e40ba4084ab7e284a76240802730b35a0066560b2d5f3d83ba8644c9f7bc`, с NR-23 / 24 / 28 / P0 / 13 / 29 / 30 / 31;
  копия `wt-nrall/dist/DOS2-Mobile-v0.1.1-arm64.apk` — та же сборка, **не релиз**). Id `org.dos2.port`, та же подпись: ставится поверх.
  54.9 МБ > 50 МБ — по удалённому adb ставить только с разрешения хозяина.
- Пак NR-04 (нужен только для прогонов с `DM_NR_SH`): `wt-nrall/work/native-nr/native-nr.xgspack` (185 КБ, 13 бандлов, собран на ПК из
  `work/shaders-vk/shaders.xgspack`; SHA-256 `0ae6617a…71be`) → в `Download/DivinityOriginalSin2Mobile/dm/shaders/`. Если пак телефона
  другой — NR-04 сам откажет по сверке головы (`refused`), вреда нет.
- Слой валидации в APK **не** упакован (нет `work/vvl`): прогоны с `DM_VK_VALIDATION=1` ниже — только со сборкой `build_apk.ps1 -Validation`.

Отличия этого APK от `dos2` (0.1.1) **без единого env**:

| Что | Шаг | Где видно в логе |
|---|---|---|
| Маркеры сборки пайплайнов — mmap во внутренней памяти, не файлы на FUSE (`DM_MARKER_DIR` = `files/`) | NR-07 (port-common pipe-marker-mmap) | boot.log `[pc] pipe-marker-mmap: applied …` |
| Метки samples-passed не пишутся, пока их никто не читает (`DM_GFX_SAMPLES_TS` авто 0 на DOS2) | NR-14 | `DM_GFX_SAMPLES_TS=0 (auto: no consumer …)`, `NVN PERF samplets mode=0 … dropped≈526` |
| ASTC-кэш читается через буфер потока + санитизация (`DM_NR_ASTCREAD=1`) — работает только там, где ASTC-кэш (Mali / SIM mali) | NR-18 | `NVN astc cache: read N uploads … (… DM_NR_ASTCREAD=1)` |
| Размещение потоков по ёмкости ядер (cpu-place, `DM_CPU_CORES` авто) + ADPF-подсказки boost / reset, GameState | NR-19 (NR-17) | `[pc] cpu-place: applied`, `[cpu] topology/plan/placed`, `[pc] adpf-boost: applied`, `[perf] game state: …` |
| UBO vec4 merge **только на Mali** (`DM_NR_UBOVEC` = 1 для семейства Mali; Adreno 0) | NR-27 | `NVN DM_NR_UBOVEC=… (family default; policy family … default …)` |
| Из умолчаний DOS2 убраны `DM_NB_RELOC=1`, `DM_NB_ORDER=3`, `DM_NB_TRANSIENT=1`, `DM_GFX_HIT_FAST=1`, `DM_NB_MX=1` (код остался, env включает) | NR-15 / NR-15b | нет строк `[nb] key` / `[nb] order` / `[nb] transient` / `[gfx] hit fast`; `[nb] own … with every key 0` |
| `DM_NR_RP=1` (барьеры по точкам видимости прохода) **только на Mali / PowerVR** (= семейства `DM_GFX_TILE_BARRIER`); Adreno / Xclipse / Maleoon 0 — чинит регресс 0.1.1 (PREBAR NB-RP отменял TILE_BARRIER) | NR-10 P0 | `[nr] rp DM_NR_RP=1 (family default, policy family mali): …` (на Mi 14 — только под `DM_GFX_SIM_GPU=mali`) |
| `DM_DOS2_PREPASS=1` (гибрид пре-пасса с воротами истории) **только на Mali / PowerVR**; Adreno 0 | NR-30 | `NVN DM_DOS2_PREPASS=1 (family default; policy family mali default 1)`, `NVN PREPASS mode=1 …` |
| `DM_NR_MEM=1`: потолки кэшей NR по классу ОЗУ (4 / 6 / 8 ГБ; **в классе 12+ ГБ потолков нет**), поток `dm-nrmem` (опрос `MemAvailable` раз в 1 с, строка раз в 10 с), очистки по `onTrimMemory` (DmActivity → `nativeTrimMemory`), `nativeMemInfo` до `nativeStart` | NR-29 | `[nr] DM_NR_MEM=1 (NR-29: on): RAM … GiB (ActivityManager) = 12+ GB class, budget 0 MB`, `[nr] mem (…)`; Home → `[nr] mem: onTrimMemory level 20`, `trim tier 2 …: freed … MB` |
| `DM_NR_SEED=1` — **в этом APK ничего не делает**: файла сида нет (`build_apk.ps1` кладёт `dm/android/dos2/dos2-pipe-seed.bin` в ассеты, только если он есть; DmActivity копирует ассет в `files/`) | NR-31 | `[nr] DM_NR_SEED=1 …: no seed … nothing to do` |

Всё остальное (`DM_NR`, `_KEY`, `_TEX`, `_TEXSTREAM`, `_BUF`, `_PIPE`, `_DESC`, `_UBODYN`, `_DRAW`, `_UI`, `_SORT`, `_OCC`, `_SH`, `_RP`,
`_COVER`, `_PRESENT`, `_ROTATE`, `_CPU`, `_NOCALL`, `_LEANCALL`, `_LEAN`, `_GATE_TICK`, `_GATE_ARB`, `_GATE_YIELD`, `DM_NVN_PROF=2`; `_RP` и `DM_DOS2_PREPASS` — вне Mali / PowerVR) — 0 по умолчанию; со всеми 0 поведение = `dos2` + строки таблицы. Постоянная цена
выключенных шагов — проверки флагов (одна загрузка / сравнение): `NBMT_PRE` NR-21 в 35 обёртках vkCmd*, по строке в `draw()` / `execute()`
у NR-03 / NR-12, указатель ворот NR-16, загрузка флага на входе NVN у NR-23 / NR-28, `prepass::mode` на draw у NR-30, крючки NR-31 в warm-up.
**Внимание для SIM-прогонов:** `DM_GFX_SIM_GPU=mali` / `powervr` теперь сам включает `DM_NR_RP=1`, `DM_DOS2_PREPASS=1` и (mali) `DM_NR_UBOVEC=1`.
Если прогон проверяет другой шаг в изоляции (C10, D2, D4b, D5, E1, …), допишите `DM_NR_RP=0 DM_DOS2_PREPASS=0` (и `DM_NR_UBOVEC=0`) — иначе
строки шагов смешаются; сами эти умолчания проверяет сеанс G. Коммиты NR-15 / NR-15b — три последних в ветке на момент сборки (`git log --oneline --grep "^NR-15" dos2-nr-all`): снять их —
`git revert` этих трёх (или `git reset --hard` на коммит перед ними, пока сверху ничего нет); всё остальное останется.

## 1. Правила каждого прогона (безопасность)

1. Только с разрешения хозяина; телефон водит один агент. Хозяин не играет в этот момент.
2. **Бэкап `files/save`** (`src/work/save-backup/<дата-время>`, как раньше) — перед установкой APK и перед каждым force-stop.
3. Перед запуском: `adb shell dumpsys window | grep -iE "keyguard|mCurrentFocus"` — keyguard не показан, фокус у игры (иначе кадры 0).
4. env — файл `Download/DivinityOriginalSin2Mobile/dm/env`; в каждом прогоне всегда `DM_DBG=1`, `DM_DOS2_TOUCHUI=1`, `DM_DOS2_FPS=0`
   (без лимита кадров) + строки прогона. После серии вернуть env хозяина.
5. После сбора логов (`dm.log`, `boot.log`, дампы) — сразу `adb shell am force-stop org.dos2.port`. Между сериями ABBA ≥ 2 мин без нагрузки.
6. Любая `【红】`-строка шага в прогоне сверки — стоп этого шага (следующие прогоны того же шага не делать), логи главной сессии.
7. Сцены: «палуба» = стартовый корабль; «тяжёлая» = Форт Радость с боем или город. Одна палуба не представительна — в каждом сеансе
   хотя бы одна тяжёлая сцена.

**Правила текущего этапа (хозяин, 17.11) — важнее длительностей и сцен ниже:**
- **Длительность:** любой замер — **60 с от загрузки уровня**, не 90 и не «2–3 мин». FPS понятен сразу, артефакты видны сразу.
- **Только палуба / стартовая локация.** Сохранений дальше нет: всё, что ниже помечено «тяжёлая / Форт Радость / бой / город / лес», на этом
  этапе **пропускается** и уходит в будущий отдельный этап «форт». Сейчас сравниваем показатели на палубе и проверяем принципиальную
  работоспособность, отсутствие ошибок (`bad 0`, без `【红】`) и артефактов.
- **Как читать цифры:** на Mi 14 Ultra часть приростов не видна в полной мере — одни раскрываются только на слабых CPU / GPU, другие только
  на настоящих Mali / PowerVR (SIM их не эмулирует полностью). Нулевой прирост на Mi 14 не значит «шаг бесполезен». Статы ещё могут
  измениться в форте.

## 2. Сеанс A — новые умолчания, база, атрибуция CPU (≈ 30 мин)

**A0 — база этого APK** (палуба 90 с → тяжёлая 90 с → «домой» → назад → ещё 60 с). Маркер `gfx-present-timing` в каталоге данных.
```
DM_DBG=1
DM_DOS2_TOUCHUI=1
DM_DOS2_FPS=0
```
В кадре игры: `adb shell dumpsys SurfaceFlinger > sf0.txt` — `Comp Type` и `Transform` слоя `SurfaceView[org.dos2.port/…]` (**решает NR-20**:
`CLIENT` → сеанс D9 полный, `DEVICE` → NR-20 на Mi 14 не нужен).
Доказывают умолчания: строки таблицы §0; `NVN PERF present_gpu_us=… extent=…` (P0 NR-11); `[cpu] placed (...)` — **записать гостевые имена
потоков** (NR-17 `DM_CPU_JOB_NAMES`), `[cpu] where 30s:`, после возврата `[cpu] resume:` и `[perf] ADPF workload reset (back in front)`;
`[nb] own (DM_NB_OWN=1): … with every key 0 …`, `[nb] own (backend …): … differ 0`; нет `[nb] key` / `order` / `transient` / `hit fast`.

**A1 — старые умолчания dos2 (ABBA с A0: A0 A1 A1 A0, по 60 с на палубе)** — решает, оставлять ли NR-15 / NR-14 / NR-19:
```
DM_DBG=1
DM_DOS2_TOUCHUI=1
DM_DOS2_FPS=0
DM_NB_RELOC=1
DM_NB_ORDER=3
DM_NB_TRANSIENT=1
DM_GFX_HIT_FAST=1
DM_NB_MX=1
DM_GFX_SAMPLES_TS=1
DM_CPU_TOPO=index
DM_ADPF_BOOST=0
DM_GAME_STATE=0
DM_ADPF_TARGET_MS=period
```
Сравнить A0 / A1: `[dm] [nvn] 主线程 至帧 …：每帧 CPU X ms` (A0 ниже на ≈ 0.7–0.9 мс — NR-15), `NVN PERF … submit_cpu_ms` минус
`poolreset … wait_ms` (A0 ниже на ≈ 0.6–0.9 мс), `NVN PERF v=1 gpu_ms` (A0 ниже на ≈ 0.8 мс — NR-14), fps; `passes=` / `draws=` / `[nb] post …
elided` / `[nb] rp … plan applied` одинаковы; скриншоты одной точки равны.
Если A0 не лучше — делить: A1a только 4 строки NR-15; A1b только `DM_NB_MX=1`; A1c только `DM_GFX_SAMPLES_TS=1`; A1d только 4 строки NR-19.

**A2 — атрибуция CPU (NR-16 M; ≥ 2 мин палуба + 2 мин тяжёлая):**
```
DM_DBG=1
DM_DOS2_TOUCHUI=1
DM_DOS2_FPS=0
DM_NR_CPU=1
DM_NVN_PROF=2
DM_SLEEPSTAT=1
```
Ждём `[cpuprof] DM_NR_CPU=1: sampler …`, `[cpuprof] gate probe …`, `DM_NVN_PROF=2: … calibration …`, раз в 10 с `threads / cores / hle /
hle svc / nvn / nvn top / gfx / passes`, раз в 30 с `roles`; ни одной `【红】[cpuprof]`. Разбор: `python tools/dos2_cpuprof.py <dm.log>`.
По желанию в той же сессии simpleperf 20 с (NR-16 §8; push бинаря 3.7 МБ, pull `perf.data` 10–30 МБ). Это вход для NR-23 / NR-24.

## 3. Сеанс B — R1 и реестр движка (≈ 35 мин)

**B1 — R1, NR-01 впервые (короткий; если упадёт — сеанс B стоп):** меню ≥ 20 с → сейв → 2–3 мин ходьбы со сменой локации, инвентарь /
карта, ≥ 40 с после загрузки.
```
DM_DBG=1
DM_DOS2_TOUCHUI=1
DM_DOS2_FPS=0
DM_NR=2
DM_NR_DUMP=1
```
Ждём: `[nr] DM_NR=2 (registry + census + checks): … 18 of 18 hooks in, opcode 55 in the shared ExecuteRenderCommand chain, shader pack keys on
(control + code, as the shader pack)`; 26 сэмплеров, 64 состояния; `checks … 0 mismatches`; `overflow / bad handles / unreadable / nesting` = 0;
`replaced` у текстур 0. Забрать 10 файлов `nr_*.tsv` из каталога дампов.

**B2 — реестр + ключи + текстуры + подсказки пайплайнов + шейдеры по имени** (тот же маршрут + бой; пак NR-04 положен):
```
DM_DBG=1
DM_DOS2_TOUCHUI=1
DM_DOS2_FPS=0
DM_NR=2
DM_NR_DUMP=1
DM_NR_KEY=2
DM_NB_FOREIGN=1
DM_NR_TEX=2
DM_NR_PIPE=2
DM_NR_SH=2
```
- NR-02: `[nr] DM_NR_KEY=2 (count + checks): the renderer's texture flush main+0x924730 hooked …`; `draw keys`: `engine draws` ≈ 900/кадр
  в кампании, `engine draws with no NVN draw` 0, eligible ≥ 90 %; `key checks` — 0 mismatches, `lost` 0. Потом
  `python tools/dos2_nr_names.py --dump <dir>/nr_names.tsv` без красных строк.
- NR-05 (R-NR05a): `NVN nr-tex verify: … storage changed since the create event A; … missed D; … mismatches G` — нужно G = 0, A ≈ 0, D = 0.
- NR-07 (шаг 2): `[nr] DM_NR_PIPE=2 (NR-07: check …)`, `[nr] pipe hints: N program keys sent`, 0 красных `pipe check`. Забрать
  `dm/vk-pipeline-list.bin` → `python tools/dos2_nr_pso.py --list <файл>`.
- NR-04 (шаг 1): `[nr] sh … native-nr.xgspack: 13 bundles`; 7 строк `… check: … (proven bit for bit)`; тени `translated kept (DM_NR_SH_PCF=0…)`;
  сводка `entries passing every check 7 of 10 … refused 0 … red lines 0`; ни одной `not the verified pass`.
- NB-15: `[nb] foreign` — доли (вход NR-12).

**B3 — окклюзия + проходы шейдеров при загрузке** (палуба: стоя 30 с, поворот камеры + ходьба 60 с; кампания: Форт Радость, ходьба + бой ≥ 2 мин):
```
DM_DBG=1
DM_DOS2_TOUCHUI=1
DM_DOS2_FPS=0
DM_NR_OCC=2
DM_NR_UBOVEC=2
DM_NR_UBODYN=2
```
- NR-03: `[nr] occ DM_NR_OCC=2 (check…) path=gpu …`; раз в 10 с «the Switch culls X draws/f = Y % of draws, Z % of vertices», «prev strict …
  WRONG», «pairs … unusable» ≈ 0, «conditions … no pair» ≈ 0. **Порог: Y ≥ 5 % или Z ≥ 10 % → прогоны D11; иначе NR-03 закрыт «мало выигрыша».**
- NR-27: `NVN DM_NR_UBOVEC=2 (env; …)`, `NVN PERF ubovec mode=2 modules=N … check_bad=0`, без `【红】`.
- NR-08b (R-NR08b-a): `[nr] DM_NR_UBODYN=2 (NR-08b: dry run …)`, `[nr] ubodyn: … modules N (not understood 0) … refused 0 … red 0`.
- Картинка и fps как в A0.

## 4. Сеанс C — транслятор: буферы, дескрипторы, draw, UI, сортировка (≈ 60 мин)

Каждый шаг — только если предыдущий дал `bad 0`. Сцены каждого прогона: палуба 60 с + Форт Радость / бой + инвентарь, карта; для UI-прогонов
ещё лист персонажа, торговля, диалог, тач-кнопки, пауза (≥ 30 с каждая). Во всех: `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_FPS=0` + строки ниже.

| Прогон | env (добавить) | Шаг | Решающие строки |
|---|---|---|---|
| C1 | `DM_NR=1` `DM_NR_BUF=2` | NR-06 (Adreno) | `[nr] buf: constant ring at … handed`, `served in place`; `M of K uniform binds in the ring`, `checks: ring N compared / 0 bad` |
| C2 | C1 + `DM_GFX_ZEROCOPY=0` | NR-06 (класс Mali) | `mirrored into one 19.5 MB buffer`, `0 bad`, `race 0`, `hold` ≈ 0, `static: … 0 bad`, `written before first draw 0 of N` |
| C3 | `DM_NR=1` `DM_NR_BUF=1` `DM_NR_DESC=2` `DM_NR_UI=2` `DM_NB_FOREIGN=1` | NR-08a + NR-12a | `[nr] DM_NR_DESC=2 … device: 16 dynamic …`; `[nr] desc:` would serve ≥ 90 %, **bad 0** по 8 видам; `[nr] ui:` Iggy draws (сколько в инвентаре / торговле), would serve ≥ 80 %, **`[nr] ui check: … bad 0`** |
| C4 | C3 + `DM_GFX_ZEROCOPY=0` | NR-08b-зеркало + NR-12b | то же, `race 0`; `vkCmd* a served draw` / `vertex` |
| C5 | `DM_NR=1` `DM_NR_BUF=1` `DM_NR_DESC=2` `DM_GFX_ZEROCOPY=0` `DM_GFX_SIM_GPU=ubdyn8`, затем то же с `DM_NR_DESC=1` | NR-08c (лимит 8) | `bad … tuple 0`; при =1 `shader set … has N uniform blocks, the device allows 8`, `sets: reused ≫ written`, картинка равна |
| C6 | `DM_NR=1` `DM_NR_BUF=1` `DM_NR_DESC=1` `DM_NR_DRAW=2` (+ вариант `DM_GFX_ZEROCOPY=0`) | NR-09a / b | `[nr] draw ident …: OP_X_NR_DRAW records ≈ 980 a frame (… without an identity: 0)`; would serve ≥ 75 %; **`[nr] draw check: … bad 0`** |
| C7 | `DM_NR=1` `DM_NR_BUF=1` `DM_NR_DESC=1` `DM_NR_DRAW=1` `DM_NR_UI=1` `DM_NR_SORT=2` (+ вариант `DM_GFX_ZEROCOPY=0`) | NR-09c, NR-12c, NR-21a | `served` ≈ would serve C6; `vkCmd* a served draw` ≤ 3.8 (≤ 2.8 без zero-copy); `[nr] ui:` served ≈ C3; **`[nr] sort check: … not a permutation 0, unstable 0; … bad 0; … NOT 0`**, `not registered` ≈ 0; скриншоты HUD / инвентаря / размытия равны A0 |
| C8 | C7 с `DM_NR_SORT=3` `DM_GFX_PASS_PROF=300,180,0` (палуба стоя, листва, тени) | NR-21b (яд) | картинка **та же**; `NVN GVAL` пре-пасса / атласа — те же порядки, что в =0; GPASS пре-пасса = цена плохого порядка |

**C9 — цена транслятора (ABBA × 2, 60 с, палуба + тяжёлая; в env `DM_NB_MT=0` для `draw_sections`):**
- A: `DM_NR=1 DM_NR_BUF=1`; B: `DM_NR=1 DM_NR_BUF=1 DM_NR_DESC=1 DM_NR_DRAW=1 DM_NR_UI=1`. Смотреть `draw_sections_ms_per_frame` (desc ≤ 0.6 мс,
  pipe / vtx / bind / tail ↓), `parts_ms_per_frame draw`, `submit_cpu_ms`, `ns a served draw after create_pass` ≈ 1.1–1.4 тыс., `[nb] frame …
  recording … ms` (цена хука у потока игры), температура. На Mi 14 fps не обязан расти (упор в GPU) — решает CPU.
- Затем B против B + `DM_NR_SORT=1` (+ `DM_GFX_PASS_PROF=300,180,0`): GPASS пре-пасса (i=4) и атласа (i=8), `gpu_ms`, `[nr] sort … us a frame`.
- По желанию: те же пары с `DM_GFX_ZEROCOPY=0` (модель Mali).

**C10 — политика PowerVR в SIM (по 90 с, палуба с персонажами + бой):**
1. NR-19a: `DM_GFX_SIM_GPU=powervr` против `DM_GFX_SIM_GPU=powervr DM_GFX_UBO_DYNAMIC=1`: `NVN gpu policy: … ubo_dynamic=0`; персонажи без
   «шипов»; `NVN PERF setcache … why=…/ubosnap N` — N ≈ draw с UBO, `memo_hits` ≈ 0.
2. NR-08b-b (ABBA × 2): `DM_GFX_SIM_GPU=powervr` против `+ DM_NR_UBODYN=1`: `[nr] DM_NR_UBODYN=1 (NR-08b: on …)`, `memo_hits` ≈ draw,
   `ubosnap` ≈ 0, `desc_ns_per_frame` / `submit_cpu_ms` ниже; скриншоты равны.
3. NR-08b-c / NR-08d: `DM_GFX_SIM_GPU=powervr DM_GFX_ZEROCOPY=0 DM_NR=1 DM_NR_BUF=1 DM_NR_UBODYN=1 DM_NR_DESC=2` (`bad 0`), затем `DM_NR_DESC=1`:
   `sets: reused ≫ written`; и `… DM_NR_DESC=1 DM_GFX_ZEROCOPY=0 DM_GFX_UBO_DYNAMIC=0` без `DM_NR_UBODYN`: `sets: written` ≈ draw, рециклер.

## 5. Сеанс D — проходы, барьеры, present, шейдеры (GPU; ≈ 60 мин)

Во всех: `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_FPS=0`. Сцены: палуба + тяжёлая по 60 с, меню, инвентарь, карта.

| Прогон | env (добавить) | Шаг | Решающие строки |
|---|---|---|---|
| D1 | `DM_NR_RP=2` `DM_NR_COVER=2` | NR-10 (1), NR-10b (1) | `[nr] rp DM_NR_RP=2 (check…; tile barrier 0)`: `barriers saved ≈ 60 a frame`, `never expected 0`; `[nr] cover DM_NR_COVER=2`: `old proof yes 0.00`, `new proof: whole target ≈ 15–22 (… MB of load)`, `never expected … 0`; записать `refused:` |
| D2 | D1 + `DM_GFX_SIM_GPU=mali` | NR-10 (2) | `tile barrier 1`, `passes split by a pre-fragment read` → ≈ 0 |
| D3 | `DM_NR_COVER=2` `DM_DOS2_DRS=75:75` | NR-10b (2) | `a part ≈ 15–20 (… MB)` |
| D4a | `DM_NR_RP=3` `DM_GFX_SIM_GPU=mali` (30–60 с на сцену) | NR-10 яд | **нет пурпура**; `fresh … begun with load DONT_CARE X, poisoned X` |
| D4b | `DM_NR_COVER=3` `DM_GFX_SIM_GPU=mali`, затем + `DM_DOS2_DRS=75:75` | NR-10b яд | **нет пурпура** (и у правого / нижнего края 3D при DRS); `applied: whole X`, `area ≈ 15–20`, `restarts by a draw ≈ 0` |
| D5 | ABBA × 2: `DM_NR_RP=1 DM_NR_COVER=1` против без них; затем то же с `DM_GFX_SIM_GPU=mali` | NR-10 (3), NR-10b (4) | `barriers op25` 65 → < 5; `[nb] rp … applied: load DONT_CARE` 0 → X; `[nb] check: split` 0; `gpu_ms` / fps; картинка та же. На Turnip sysmem COVER ≈ 0 |
| D6 | `DM_NR_PRESENT=2` (60 с) | NR-11 P2 | `[nr] present (DM_NR_PRESENT=2): swapchain gen …, colour attachment allowed`; `would start ≈ F … red 0`; причины `not started` |
| D7 | `DM_NR_PRESENT=3` (20 с) | NR-11 P3 | пурпура нет |
| D8 | ABBA по 90 с: `DM_NR_PRESENT=1` против A0; в одном P1: «домой → назад», поворот на 180°, загрузка сейва; потом `DM_NR_PRESENT=1 DM_GFX_PRESENT_THREAD=0` 60 с | NR-11 P1 / P1i | `started ≈ presented (direct) ≈ frames`, `parked ≈ taken ≈ presented`, `lost 0`, без `【红】`; fps +2…5 %, `present_thread busy_ms`, `gpu_ms`; без зависаний |
| D9 | только если A0 `CLIENT`: `DM_NR_ROTATE=2` 60 с; `DM_NR_ROTATE=3` 20 с + `adb exec-out screencap -p > r3.png`; ABBA `DM_NR_ROTATE=1` против A0 по 90 с + поворот 180° / «домой» / сейв. Если `DEVICE`: только `=2` и `DM_NR_ROTATE=1 DM_NR_PRESENT=1` 30 с | NR-20 | `[nr] rotate (DM_NR_ROTATE=2): surface rotate 90 …`, `probes ok … bad 0`, `red 0`; R3 картинка не перевёрнута, без пурпура; R1 `dumpsys` → `DEVICE`, `Transform 0`, `turned draw ≈ presents`, `blit 0`, fps / `gpu_ms`; R1+P1 — `not started: presenter (pre-rotated)` |
| D10 | ABBA: A0 + `DM_GFX_PASS_PROF=300,180,0` против `DM_NR=1 DM_NR_SH=1 DM_GFX_PASS_PROF=300,180,0` (пак положен); потом тени `DM_NR_SH=1 DM_NR_SH_PCF=1`, затем `=2` (солнце, камера близко / далеко) | NR-04 (2, 3) | `entries replaced 7 of 10`; `python tools/dos2_gpu_breakdown.py <log> --gpass`: FadeBlending ≈ 0.27 → 0.15 мс; скриншоты = A0; тени PCF=1 совпадают с A0, PCF=2 — решение хозяина глазами |
| D11 | только если B3 дал порог: ABBA по 60 с `DM_NR_OCC=1` против 0 (та же точка кампании); `DM_NR_OCC=3` (видимые объекты с тестом исчезают); `DM_NR_OCC=1 DM_NR_OCC_PATH=prev DM_GFX_SIM_GPU=mali` | NR-03 (2–4) | `VK_EXT_conditional_rendering: … enabled`, `executed: gpu predicate N/f` N > 0, `false culls on screen 0`; `gpu_ms` / fps / `submit_cpu`; prev — красные `FALSE CULL on screen` за минуту |
| D12 | `DM_NR_UBOVEC=1` (скриншоты = A0, `check_bad=0`, `cpu_us`); `DM_NR_UBOVEC=3` 20 с (свет на воде / скиннинг сломаны — проход живой); `DM_GFX_SIM_GPU=mali` без `DM_NR_UBOVEC` | NR-27 | `NVN PERF ubovec mode=1 … check_bad=0`; при SIM — `family default … default 1` |
| D13 | (по желанию) `DM_GFX_SAMPLES_TS=4`, затем `=3` в кампании ≥ 2 мин | NR-14 (2) | `readers=0`, `fallbacks=0`, нет `samples reports: access` |

## 6. Сеанс E — загрузки и стриминг текстур / пайплайнов (≈ 45 мин)

Маршрут каждого прогона один и тот же: меню → загрузка того же сейва → 2–3 мин бега со сменой локации (палуба → Форт Радость), инвентарь /
карта, ≥ 40 с после загрузки. Во всех: `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_FPS=0`.

1. **E1 NR-18** (нужен наполненный ASTC-кэш; если пуст — сначала один прогон `DM_GFX_SIM_GPU=mali` по маршруту): `a18r0` =
   `DM_GFX_SIM_GPU=mali DM_NR_ASTCREAD=0`, `a18r1` = `DM_GFX_SIM_GPU=mali`. Строки: `NVN arena chunk cached=0 … flags=0x7`; `NVN astc cache:
   read N uploads, X MB in Y ms (Z ms/MB; DM_NR_ASTCREAD=…)` — Z ≈ 5–8 в r0 и ≈ 0.5–1 в r1; `zero blocks fixed K, read errors 0` одинаковы.
2. **E2 NR-05b + NR-22a** (ABBA с A0 по маршруту): `DM_NR=1 DM_NR_TEX=1`. NR-05: `first uploads from the worker's blob N (… MB, … ms on this
   thread)`, `no blob: …`; `[长帧]` `上传贴图 … MB / N ms` меньше базы. NR-22a: `NVN nr-texstream off (DM_NR_TEXSTREAM=0): images made here H
   (X ms …)` — X / H = цена создания образа на трансляторе.
3. **E3 NR-22** : `DM_NR=1 DM_NR_TEXSTREAM=2` (сверка: `NVN nr-texstream verify: claims N, … mismatches 0 … mismatches 0`, красных 0), затем
   `DM_NR=1 DM_NR_TEXSTREAM=1` (`adopted C (… ms here)`, `images made here` ≪ E2, `refused:` без `cap` / `vkCreateImage` / `memory`; в `[长帧]`
   входа остаток меньше на X из E2; глазами — нет чёрных / мусорных текстур). По желанию + `DM_GFX_SIM_GPU=mali` (ASTC на рабочем потоке).
4. **E4 NR-07 медленный компилятор** (имитация слабого CPU; **пересоберёт кэш драйвера** — следующий обычный запуск как первый):
   `DM_GFX_PIPE_CACHE_LOAD=0 DM_NR_PIPE_THREADS=1` + (A) `DM_NR=1 DM_NR_PIPE=2`, (B) `DM_NR=1 DM_NR_PIPE=1`, (C) `DM_NR=1 DM_NR_PIPE=3`, ABBA A/B,
   потом C; сразу после меню грузить сейв. Решают `created` / `create_ms` / `create_max_ms` у входа, длинные кадры (B, C < A), `[nr] pipe`:
   `built … from hints`, `waited (ready …)`, `claimed`. Плюс шаг 1 NR-07 уже в A0: `warm_ms` после `warm_built=12xx` ≪ 16.6 с.
5. **E5 NR-16 L:** `DM_NR_CPU=1 DM_FS_STATS=1`: `[fs] N reads X MB in 5s: mean …, max …, K over 20 ms` в окнах загрузки, `threads` (кто грузит).

## 7. Сеанс F — NR-23 (обход thunk-вызовов NVN), NR-24 (быстрые ворота HLE), NR-28 (лёгкий вход NVN) (≈ 55 мин)

Оба шага — CPU-шные, на Mi 14 (упор в GPU) fps расти не обязан: решают мс CPU потоков из `[cpuprof]`. Во всех прогонах базовые строки
`DM_DBG=1`, `DM_DOS2_TOUCHUI=1`, `DM_DOS2_FPS=0`; сцены — палуба 60 с, затем тяжёлая (бой / город) 60 с, **логи по сценам отдельно**.
Разбор всех прогонов: `python tools/dos2_cpuprof.py <dm.log>` (ответы `NR-23` / `NR-24 switches`), A/B — `--ab a1 b1 b2 a2`.

**F1 — R-NR23a, счёт + сверка (первым; он же отвечает на вопросы NR-16):**
```
DM_DBG=1
DM_DOS2_TOUCHUI=1
DM_DOS2_FPS=0
DM_NR=1
DM_NR_KEY=1
DM_NR_NOCALL=2
DM_NVN_PROF=2
DM_NR_CPU=1
```
Ждём: `[nr] DM_NR_NOCALL=2 (NR-23): count + compare …; the three functions hash to the analysed code`, `[nr] nocall: the engine's …
pointers are the thunks of the bodies the direct entries call`; раз в 10 с четыре строки `[nr] nocall …` (карта вызовов по кодам пакетов,
`what =1 would not call through the thunks: … S NVN calls a frame (P% …)`, `cost model …: =1 would save about X ms a frame`,
`nocall check (DM_NR_NOCALL=2, totals): … bad 0 …; … draws 36 / 37 compared, bad 0`). **Обязательно `bad 0` в обеих сценах.** `=1`
имеет смысл только при `would save about ≥ 0.15 ms` (строка `GO for =1` у `dos2_cpuprof.py`); иначе F2 не делать, шаг закрыть.

**F2 — R-NR23b, включено (только после F1 с `bad 0` и GO), ABBA по 60 с в одной сцене:** A = `DM_NR=1 DM_NR_KEY=1 DM_NR_CPU=1`,
B = A + `DM_NR_NOCALL=1` (по желанию отдельно `DM_NR_NOCALL_PARTS=1` / `2`). В B: `[nr] nocall (DM_NR_NOCALL=1, parts 3, …): native texture
flushes … native draws ≈ 983 a frame …`, картинка та же, красных нет. Сравнивать `[cpuprof] threads` — мс / кадр потока `rec`, `NVN PERF`
`draws` / `ops` / fps те же. Совместимость с лентой транслятора: один прогон B + `DM_NR_BUF=1 DM_NR_DESC=1 DM_NR_DRAW=1` (C7 без SORT) — без
красных строк, картинка та же.

**F3 — NR-24, безопасность: по одному ключу `=2`, 2–3 мин игры, без ABBA** (база каждого: `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_FPS=0
DM_NR_CPU=1 DM_SLEEPSTAT=1`):
1. F3a `+ DM_NR_GATE_TICK=2`: `[nr24] DM_NR_GATE_TICK=2: …`, раз в 10 с `… reads compared, largest gap X us` с **X < 1000**, ни одной `【红】[nr24]`.
2. F3b `+ DM_NR_GATE_ARB=2`: `[nr24] DM_NR_GATE_ARB=2 …`, `[cpuprof] nr24 … red 0`; игра не зависает (загрузка сейва, смена зоны, бой).
3. F3c `+ DM_NR_GATE_YIELD=2`: `would wait N` — сколько уступок дождались бы.
Любая красная строка / зависание — стоп этого ключа, логи главной сессии.

**F4 — NR-24, ABBA по каждому ключу отдельно (по 60 с; только для ключей, прошедших F3):** A = база F3; B = `+ DM_NR_GATE_TICK=1`, затем
отдельные серии `+ DM_NR_GATE_ARB=1` и `+ DM_NR_GATE_YIELD=1`. Решают (`--ab`): `[NR-24] main thread CPU ms`, `full clock gates`, `G takes`,
`G waited us`, `yield waits`, fps / process CPU; проба `[cpuprof] nr24 probe (…): full gate X ns, short entry Y ns`.
- TICK: `clock reads by the short entry` + `main's GetSystemTick on the host` ≈ прежние `clock` из `hle`, `full clock gates` ≈ 0, CPU ниже.
- ARB: `G takes` падают на число арбитражных SVC, `G waited` → ~0, `bucket takes … found held` мал, fps ≥ A.
- YIELD: CPU главного потока ниже, `woken by progress` ≫ `timed out`, fps ≥ A.

**F5 — R-NR28a, NR-28 лёгкий вход NVN: сверка + калибровка (первым из NR-28; можно сразу после F1):**
```
DM_DBG=1
DM_DOS2_TOUCHUI=1
DM_DOS2_FPS=0
DM_NR_LEANCALL=2
DM_NR_CPU=1
```
Палуба 60 с, затем тяжёлая 60 с. Ждём `[nvn] DM_NR_LEANCALL=2 (NR-28): … stay on the big-stack wrapper …`; после 600 кадров одна строка
`[nvn] leancall calibration (DM_NR_LEANCALL=2, …): cpu0 … saved … cycles; … cpu7 …` (числа на класс ядра); раз в 10 с `[nvn] leancall
(DM_NR_LEANCALL=2, …): … bad 0 (total 0); counters: … lost …; saved by =1 X ms a frame on cpuR`. Разбор: строки `NR-28` у
`dos2_cpuprof.py`. **Решение: `bad 0` и ни одной `【红】[nvn] leancall check` → F6**; потерянные инкременты «plain counter» ≪ 0.1 %.

**F6 — R-NR28b, ABBA по 60 с в одной сцене (только после F5 с `bad 0`):** A = `DM_NR_CPU=1 DM_NVN_PROF=2`, B = A + `DM_NR_LEANCALL=1`.
В B: `[nvn] DM_NR_LEANCALL=1 (NR-28): N NVN slots go through the lean entry …`, `[nvn] thunks: plain counter (DM_NR_LEANCALL=1, …)`, раз в
10 с `[nvn] leancall (DM_NR_LEANCALL=1, …): … passed on to the old wrapper ≈ 0 a frame …; threads in the table T, without 0`; картинка та
же, красных нет, падений нет. Сравнивать: `[cpuprof] DM_NVN_PROF=2: … untimed call X ns` (A против B), `[cpuprof] threads` — мс / кадр
потока `rec`, `[cpuprof] nvn … ns a call`; fps / draws те же. Совместимость: один прогон B + `DM_NR=1 DM_NR_KEY=1 DM_NR_NOCALL=1`
(NR-23 + NR-28 вместе, если F2 дал GO) — без красных строк.

## 7b. Сеанс G — новые умолчания тайлеров (P0, NR-30), NR-13, NR-29, NR-31 (≈ 70 мин)

Во всех: `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_FPS=0`; сцены — палуба 60 с, меню, инвентарь, карта, Форт Радость (лес / листва) + бой.

**G1 — P0, `DM_NR_RP=1` по семейству (SIM mali):**
1. G1a `DM_GFX_SIM_GPU=mali DM_NR_RP=2 DM_DOS2_PREPASS=0`: `[nr] rp DM_NR_RP=2 (…; tile barrier 1)`, `never expected 0`, `passes split by a
   pre-fragment read` → ≈ 0 после первых секунд (иначе стоп, логи главной сессии: `need_full` надо заполнять заранее), нет `【红】[nr]`.
2. G1b ABBA по 60 с: A = `DM_GFX_SIM_GPU=mali DM_NR_RP=0 DM_DOS2_PREPASS=0`, B = `DM_GFX_SIM_GPU=mali DM_DOS2_PREPASS=0` (RP из семейства).
   В B: `[nr] rp DM_NR_RP=1 (family default, policy family mali): …`; `NVN PERF … barriers op25` 43+ → < 5; `[nb] check: split` 0; картинка как
   в A — **нет пурпура / мусора** (палуба, меню, инвентарь, карта, бой); `gpu_ms` / fps.
3. Без SIM (Adreno) семейного `DM_NR_RP` нет — это уже видно в A0.

**G2 — NR-13 `DM_NR_LEAN`** (части состояния не требуют NR):
1. G2a R-NR13a, A/B в одном прогоне: `DM_NR_LEAN=1 DM_NR_LEAN_PARTS=1 DM_NR_LEAN_AB=300`. `[nr] DM_NR_LEAN=1 … state capture NB-13a paused in
   the lean phases …`; раз в 10 с `[nr] lean (front …): … X ms a frame lean vs Y ms full, N phase changes (0 put off)` — решает Y − X (ждём
   0.1–0.2 мс; ≤ 0.03 — шум); `[nb] pipe` 100 % попаданий в обеих фазах; картинка не меняется на сменах фаз.
2. G2b R-NR13b: `DM_NR_LEAN=1 DM_NR_LEAN_PARTS=1`, ABBA × 2 против `DM_NR_LEAN=0`: `[nb] DM_NB_OPS state bits …: not taken`, нет `[nb] state`,
   `[nb] frame … recording`, `submit_cpu_ms`; скриншоты = база.
3. G2c R-NR13c: `DM_NR=1 DM_NR_BUF=1 DM_NR_DESC=1 DM_NR_DRAW=1 DM_NR_LEAN=2`: `[nr] lean (backend …)`: `nbubo::fast lookups 1 would skip N`
   (≈ 3 тыс. / кадр), `notes READ … served in place` = 0, `nb_desc_pre ran` ≈ 0.
4. G2d R-NR13d: то же с `DM_NR_LEAN=1`: `ring notes: not made`, `lookups skipped`; `parts_ms_per_frame`, `submit_cpu_ms` ниже, чем в G2c.

**G3 — NR-29 `DM_NR_MEM`** (умолчание 1):
1. G3a умолчания (можно вместе с A0): `[nr] DM_NR_MEM=1 (NR-29: on): RAM 14.xx GiB (ActivityManager) = 12+ GB class, budget 0 MB`, `[nr] mem (…)`
   раз в 10 с, нет строк `[nr] mem: <кэш> … -> … MB`. Вход палуба → Форт Радость, параллельно `adb shell dumpsys meminfo org.dos2.port` раз в
   2–3 с (TOTAL PSS / RSS). Затем **Home на 30 с и назад**: `[nr] mem: onTrimMemory level 20`, `trim tier 2 (onTrimMemory level 20): freed … MB`;
   после возврата игра рисует, без зависаний (с `DM_NR=1 DM_NR_BUF=1` ещё `[nr] buf: memory trim (DM_NR_MEM): N static copies dropped`).
2. G3b класс 6 / 8 ГБ на Mi 14: `DM_NR_MEM_RAM_MB=5632` (потом `7577`) `DM_NR=1 DM_NR_TEX=1 DM_NR_TEXSTREAM=1 DM_NR_BUF=1`: строки
   `[nr] mem: NR-05 texture blobs 128 MB -> 26 MB …` по каждому кэшу; `NR caches hold … (peak …)` ≤ 176 / 356 МБ; пик `dumpsys meminfo` при
   входе в зону против того же без `DM_NR_MEM_RAM_MB` (ждём −300…400 МБ); картинка та же; Home → назад, как в G3a.
3. G3c стресс: `DM_NR_MEM=3 DM_NR=1 DM_NR_TEX=1 DM_NR_TEXSTREAM=1 DM_NR_BUF=1`, 3–5 мин в бою / городе: `trim tier 1/2/3 (DM_NR_MEM=3 stress)`
   каждые 30 с, без `【红】`, без падений, без пурпура / пропавших мешей (NR-06 `bad 0`; хитчи нарочно).
4. G3d (по желанию) `DM_NR_MEM=0` против A0 — старые умолчания байт в байт (строки `[nr] mem` нет, `onTrimMemory` игнорируется со строкой).

**G4 — NR-30 `DM_DOS2_PREPASS`** (во всех `DM_GFX_PASS_PROF=300,180,0`; сцены + исчезающие стены при наведении камеры):
1. G4a R-NR30a (Mi 14): `DM_DOS2_PREPASS=2`: `NVN DM_DOS2_PREPASS=2 (env; policy family adreno default 0): compare …`; строки `NVN PREPASS mode=2 …`:
   `cand / pre_draws`, `skip_verts / pre_verts`, `refused …`, `hazards …`, `unsafe_keys`, `bad=` ≈ 0, `total bad_frames` мал; `【红】NVN PREPASS`
   — единицы при движении камеры. Картинка как без флага.
2. G4b R-NR30b: `DM_GFX_SIM_GPU=mali DM_NR_RP=0` (PREPASS из семейства): `NVN DM_DOS2_PREPASS=1 (family default; policy family mali default 1)`,
   `mode=1`, без `NVN PREPASS fallback`; скриншоты (листва, фейд, террейн) как в G4a; GPASS пре-пасса / G-buffer против `DM_DOS2_PREPASS=0`.
3. G4c R-NR30c: как G4b + `DM_NR=1 DM_NR_BUF=1 DM_NR_DESC=1 DM_NR_DRAW=2`, затем `DM_NR_DRAW=1`: `[nr] draw check … bad 0`, `pre-pass override …
   asked N, would serve / served M` (M ≈ N после прогрева).
4. G4d R-NR30d яд: `DM_GFX_SIM_GPU=mali DM_DOS2_PREPASS=3` — опак-объекты исчезают (решения доходят до draw), листва / фейд остаются.

**G5 — NR-31 сид пайплайнов** (последним: нужен длинный список с телефона):
1. G5a **сборка сида на ПК** после длинного прогона (палуба → город → бой; подходят логи A2 / B2): забрать
   `Download/DivinityOriginalSin2Mobile/dm/vk-pipeline-list.bin` и `dm.log`;
   `python tools/dos2_pipe_seed.py build --list vk-pipeline-list.bin --log <dm.log> --bundles ../work/shaders-vk --out dm/android/dos2/dos2-pipe-seed.bin`
   → `left out: nothing` (или только `duplicate`), `layouts: … limit is ..16`; `audit` → `every byte accounted for: True`.
2. G5b тождество на Mi 14 (сид push'ем в `Download/DivinityOriginalSin2Mobile/dm/shaders/dos2-pipe-seed.bin`, этот APK): `DM_NR_SEED=2`:
   `[nr] DM_NR_SEED=2 … this device: tile 0 … features 0x1f`, `[nr] seed (done …): expanded N, refused none`, к концу сцены `the seed had P` ≈
   все сборки в кадре, **`a different record for the same shaders 0`**, 0 строк `【红】[nr] seed check`.
3. G5c перенос на политику Mali: `DM_GFX_SIM_GPU=mali,ubdyn8 DM_NR_SEED=2` (список не трогать): `this device: tile 1 … dynamic UBO 1 (limit 8),
   NOOUT 1 …`, `refused none`, `a different record for the same shaders 0`.
4. G5d первый запуск (медленный компилятор, как E4): `DM_GFX_SIM_GPU=mali,ubdyn8 DM_GFX_PIPE_CACHE_LOAD=0 DM_NR_PIPE_THREADS=1`; список и маркер
   `.seed` **отодвинуть** (переименовать в `.bak`; **вернуть после!**), ABBA: A `DM_NR_SEED=0`, B `DM_NR_SEED=1`. Решают у входа
   `NVN PERF … created / create_ms / create_max_ms` (B ≪ A), `[nr] seed (done …): built B, taken by draws K`, `[nr] seed <id> is in this device's
   list now`; второй запуск B — `already expanded … nothing to do`. Картинка та же. Кэш драйвера пересоберётся (как в E4).
5. Сид в APK — только после чистых G5b / G5c: положить файл в `dm/android/dos2/` и пересобрать (`build_apk.ps1` упакует ассет).

## 8. Что покрыто чем (сводка)

| Шаг | Прогоны | Шаг | Прогоны |
|---|---|---|---|
| NR-01 | B1 | NR-11 | A0 (P0), D6–D8 |
| NR-02 | B2 | NR-12 | C3, C4, C7, C9 |
| NR-03 | B3, D11 | NR-14 | A0, A1, D13 |
| NR-04 | B2, D10 | NR-15 / 15b | A0, A1 |
| NR-05 | B2, E2 | NR-16 | A2, E5 |
| NR-06 | C1, C2 | NR-17 / 19 | A0, A1, C10 |
| NR-07 | A0, B2, E4 | NR-18 | E1 |
| NR-08 | C3–C5, C9 | NR-20 | A0 (dumpsys), D9 |
| NR-08b | B3, C10 | NR-21 | C7, C8, C9 |
| NR-09 | C6, C7, C9 | NR-22 | E2, E3 |
| NR-10 | D1, D2, D4a, D5 | NR-25 | не нужен (офлайн malioc); косвенно D10 на Mali |
| NR-10b | D1, D3, D4b, D5 | NR-27 | A0, B3, D12 |
| NR-23 | F1, F2 | NR-24 | F3, F4 |
| NR-28 | F5, F6 | NR-10 P0 | G1 |
| NR-13 / 26 | G2 | NR-29 | A0, G3 |
| NR-30 | G4 | NR-31 | G5 |

Итого ≈ 80 запусков в 7 сеансах (A ≈ 30 мин, B ≈ 35, C ≈ 60, D ≈ 60, E ≈ 45, F ≈ 55, G ≈ 70 мин телефона). Настоящие Mali / PowerVR / MTK / Xclipse — только
тестовые сборки у тестеров с умолчанием по детекту (игрокам флаги не выдаются); этот план — Mi 14 Ultra (Adreno 750) + SIM-режимы.
