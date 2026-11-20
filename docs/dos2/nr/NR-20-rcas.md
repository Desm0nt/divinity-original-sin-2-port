# NR-20-rcas — pre-rotation together with RCAS (`DM_NR_ROTATE` + `DM_GFX_SHARPEN`)

Ветка `dos2-nr20rcas` (worktree `wt-nr20rcas`, база `dos2-nr-all` 617f4af). Продолжение `NR-20.md`. На телефоне **не запускалось**.

## 1. Проблема (телефон, Mi 14 Ultra, Turnip, 17.11)
`nr4_rot1/2/3.dm.log`: `RCAS sharpening at present: stops=0.5 (DM_GFX_SHARPEN)` (шестерёнка, «резкость: средняя») →
`nrr_decide` = `NRR_R_SHARPEN` → swapchain identity + blit, композитор поворачивает каждый кадр:
`[nr] rotate (DM_NR_ROTATE=1, last 10 s): 854 presents, turned draw 0, blit 854; swapchain identity (surface rotate 90, RCAS sharpening on)`.
`nr4_rot1.sf.txt` (тот же прогон): оба слоя `SurfaceView[org.dos2.port/…DmActivity](BLAST)` — `CLIENT`, `ROT_90`. То есть NR-20 на DOS2
не работал ни разу: пользователь с включённой резкостью (по умолчанию у хозяина) всегда шёл старым путём.

## 2. Что сделано
**Один проход.** Раньше с RCAS: compute RCAS в своё изображение (`rcas::image`, полный размер текстуры окна) → blit в swapchain →
(identity) композитор поворачивает на GPU. Теперь при повёрнутом swapchain **сам поворачивающий draw NR-20 делает RCAS**:
новый фрагментный шейдер `dm/android/shaders/nr_rotate_rcas.frag` (тот же фильтр, что `rcas.comp`, байт в байт по тексту — тест
структуры сравнивает тела) вычисляет RCAS в той текселе текстуры окна, которую показывает данный пиксель повёрнутого образа, и пишет
результат прямо в образ swapchain. Ни отдельного изображения RCAS, ни blit, ни поворота у композитора.

**Отображение (store mapping) — целочисленное, без интерполяции** (`nr_rotate.h`: `nrr_rcas_map`, CPU-эталон шейдера
`nrr_rcas_src_texel`): пиксель образа (x, y) во вьюпорте vp → пиксель окна в прямоугольнике blit-а d: `q = M·(x−vp.x, y−vp.y) + c`,
M/c — точный поворот на 90 / 180 / 270 (для 90: `q = (b, vp.w−1−a)`, 270: `q = (vp.h−1−b, a)`), затем тексель
`s = (sx, sy) + floor((q + 0.5)·k)`, `k = (sbw/d.w, sbh/d.h)` — ровно формула nearest-blit-а Vulkan. Значит каждый пиксель фильтруется в
той же текселе, которую старый путь скопировал бы из `rcas::image`; соседи фильтра клампятся по размеру текстуры (как в `rcas.comp`, не по
crop-прямоугольнику). Push-константы: 0..23 — `nr_rotate.vert` (не меняется), с 32 — блок фрагмента (`ivec4 m, o, s; vec2 k; float
sharpness`; `static_assert` в презентере, тест смещений 32/48/64/80/88 — те же, что `OpMemberDecorate … Offset` в `spirv-dis` шейдера).

**Почему фрагментный draw, а не compute RCAS со `STORAGE` в swapchain** (как предполагалось в задаче):
- `STORAGE` на образах swapchain — это gralloc-usage для буферов дисплея: на Mali выключает AFBC (ARM: storage-образы без AFBC), на
  Adreno может выключить UBWC → контроллер дисплея читает несжатое (шина, тепло) — на всех кадрах, не только в RCAS;
- swapchain `B8G8R8A8` (Android 13+ его тоже отдаёт) с `layout(rgba8)` в шейдере — несовпадение формата вида (по спецификации нельзя), иначе
  нужен `shaderStorageImageWriteWithoutFormat` и второй вариант шейдера;
- у тайлеров render pass с `loadOp DONT_CARE` пишет плитку прямо в буфер дисплея; compute пишет в память мимо тайловой;
- поворачивающий draw уже есть со всем хозяйством (render pass с переходами layout, полосы letterbox через `CLEAR`, пурпур `=3`, пробы
  `=2`, отказ → identity, опрос поворота). Нужен только `COLOR_ATTACHMENT`, который NR-20 уже проверяет (`NRR_R_USAGE`).

Поэтому проверка «`STORAGE` разрешён при повёрнутом extent» не нужна: новых usage нет. Откат: если не создался конвейер RCAS-draw-а
(`pipe[2]`), решение — `NRR_R_SHARPEN` (имя `RCAS on, its turned draw failed`), красная строка, swapchain identity, старый путь RCAS +
blit — картинка у игрока та же, что раньше.

**Покадрово.** `sharpen = rcas::enabled() && формат источника RGBA8/BGRA8 UNORM` — та же проверка, что в `rcas::record`, т.е.
повёрнутый путь резчит ровно те кадры, которые резчил бы старый. Кадр с линейной фильтрацией (только TL2-pillarbox / OHOS) — повёрнутый
draw без RCAS (счётчик `unsharpened`); у DOS2 таких нет.

**Переворот телефона 90 ↔ 270.** Отображение RCAS берётся каждый кадр из текущего `nrr::turn`, который ставит `decide` при каждом новом
swapchain; путь «`SUBOPTIMAL` / опрос раз в 60 presents → `swap_dirty` → `create_swap` → `decide`» не менялся (конвейеры не
пересоздаются, `setup` возвращается сразу). Тест: образы RCAS для 90 и 270 — поворот друг друга на 180° пиксель в пиксель.
180° — тот же код (`ROTATE_180`, `M = −I`), бесплатно.

**`DM_NR_ROTATE` — смысл значений сохранён:**

| значение | без RCAS | с RCAS (`DM_GFX_SHARPEN`) |
|---|---|---|
| 0 / не задан | как было (байт в байт: ни одной новой ветки, конвейеры RCAS-draw-а не создаются) | как было: RCAS compute + blit |
| 1 | повёрнутый draw | **повёрнутый draw с RCAS (один проход)**; не создался конвейер → identity + RCAS + blit, причина в строке |
| 2 | identity + blit, проба повёрнутого draw-а против текстуры окна | identity + **RCAS + blit (старое)**; проба: RCAS-draw в свой образ повёрнутого размера, 12 пикселей сравниваются с текселями **`rcas::image` этого же кадра** (неповёрнутый вывод RCAS), допуск ±1 (фрагмент vs compute могут округлить на 1) |
| 3 | как 1, сначала пурпур | как 1 с RCAS, сначала пурпур |

**NR-11 (`DM_NR_PRESENT`)** — без изменений и как в `NR-11.md` / `NR-20.md`: при включённом RCAS `dm_present_nrp_query/take` отвечают
`NRP_P_SHARPEN` раньше, чем проверка поворота (`NRP_P_ROTATED`); без RCAS — `pre-rotated`; `nrp_park_next` для повёрнутого swapchain не
паркует. NR-11 с RCAS не работал и не работает; NR-20 теперь работает и с RCAS.

## 3. Строки лога (`dm-vulkan`)
- на swapchain: `[nr] rotate (DM_NR_ROTATE=1): surface rotate 90 (supported 0x1ff) -> pre-rotated: swapchain rotate 90, image 1080x2400,
  window extent 2400x1080; RCAS sharpens in the turned draw (one pass)` (=2: `… (=2: would pre-rotate; …); RCAS: the sharpening turned
  draw is probed against RCAS's own image`);
- раз в 10 с: `[nr] rotate (DM_NR_ROTATE=N, last 10 s): P presents, turned draw D, blit B; RCAS on: sharpened in the turned draw S,
  unsharpened U; swapchain …; probes ok K bad 0 skipped 0 (RCAS R); red 0` (`RCAS off` без резкости; `on, no turned pipeline` при откате);
- откат: `【红】[nr] rotate: the sharpening turned draw's pipeline failed (VkResult …)`; плохая проба: `【红】… probe (DM_NR_ROTATE=2,
  rotate 90, RCAS): image pixel … the texel of RCAS's own image …`.

## 4. Файлы
- `dm/android/shaders/nr_rotate_rcas.frag` + `.frag.spv.inc` (glslc NDK 29 `--target-env=vulkan1.0 -O -mfmt=num`, `spirv-val` OK);
- `dm/gfx/nr/nr_rotate.h`: `nrr_rcas_pc`, `nrr_rcas_map`, `nrr_rcas_src_texel`, `NRR_RCAS_PC_OFFSET`, `nrr_probe_equal_tol`; смысл
  `NRR_R_SHARPEN` (только отказ конвейера);
- `dm/android/vulkan_present.cpp` (`namespace nrr`): `setup(f, want_rcas)` → `pipe[2]` (rp 0/1), `pipe[3]` (rp 2, =2), `layout_rcas`
  (один push-диапазон VS|FS 0..92); `decide` передаёт отказ RCAS; `draw_pass` / `draw` / `probe_record` / `probe_collect` / `frame`;
  `present_body`: `sharpen` по проверке `rcas::record`, проба =2 получает `rcas::image`;
- тесты: `nr_rotate_test.c` (+ RCAS), `source_structure_test.py` (блок NR-20 + RCAS).

## 5. Проверки (ПК)
- `nr_rotate_test.c` (clang Windows `-Werror`; gcc WSL `-Werror`; ASan + UBSan) PASS: отображение RCAS = эталон поворачивающего draw-а
  (`nrr_src_texel`) = старый blit для 4 поворотов × 9 форм + форма с неравномерным масштабом без текселей на центрах пикселей (там +0.5 и
  k по осям проверяются точно); 90 / 270 на 2400×1080 руками (углы, произвольные точки); **картинка**: RCAS старого пути (весь образ, потом
  nearest-blit) = RCAS-draw, прочитанный как композитор показывает повёрнутый образ, для 4 × 9 форм (crop: соседи вне прямоугольника);
  RCAS не тождествен (проверка, что тест что-то ловит); 90 ↔ 270 с RCAS = поворот на 180°; смещения push-блока; точки проб RCAS; допуск ±1.
  Мутации (смещение c для 90 на 1; коэффициенты 270 как у 90; k по другой оси; без +0.5; c180 на 1; без sx) — тест падает каждый раз.
- `source_structure_test.py` PASS (тела фильтра `nr_rotate_rcas.frag` и `rcas.comp` совпадают; строки отображения шейдера = эталон;
  `static_assert`; `decide` / `sharpen` / старый путь при identity).
- весь набор тестов NR / NB (`run_tests.sh` интеграционного дерева, перенаправленный на этот worktree): 65 / 65 PASS (3 из них —
  `nr_nocall_oracle_obj`, `nr_leancall_test`, `dos2_nr_check` — после нативной сборки, им нужен `libdm.so`).
- NDK 29 `clang++ -fsyntax-only -Wall -Wextra` `vulkan_present.cpp` (`DM_GAME_DOS2`, `DM_GAME_TL2`) — 0 ошибок, 0 предупреждений.
- полный `tools/build_apk.ps1` (cmake NDK 29 / android-31 / Release / `DM_GAME=dos2` в `work/build-android`, libadrenotools и драйверы
  скопированы, ничего не скачивалось; gradle offline) — rc=0, `wt-nr20rcas/work/DOS2-port-debug.apk`, 54 884 470 байт,
  sha256 `50915ac6159c90ed370cec4719e841188382486e9e2a45f515a11864d0815f38`.

## 6. CLIENT-композиция: причина (мнение по `nr4_rot1.sf.txt`)
Факты дампа (поворот не включался, RCAS):
- таблица `HWC layers`: игра `8ad18b5 SurfaceView…(BLAST)` Z 11 и второй `92ba376 SurfaceView…(BLAST)` Z 12 — оба `CLIENT`, `ROT_90`,
  оба на весь экран (`0 0 1080 2400`, crop `2400×1080`); три других слоя (GestureStub, RoundCornerTop/Bottom) — `DEVICE`, Transform 0;
- `HWC3`: `composition: DEVICE/CLIENT` у обоих — SurfaceFlinger просил DEVICE, **HWC (SDM) отказал**; `forceClientComposition=false`;
- SDM: `max. mixer stages: 11` (слоёв 5 — не лимит стадий); `GPU_TARGET` (client target `RGBA_8888_UBWC`) + 3 `SDE`; отдельной строкой
  `0 | SDE | IWE-1 | 2432 x 1088 | RGBA_8888 | 0 0 2400 1080` — путь поворота DPU рассматривался для **одного** слоя (наш, в альбомном
  размере); не принят;
- gralloc: буферы игры `usage 0x900, compressed: false` (линейные), пэда `usage 0x933` (CPU read/write often), тоже линейные; при этом
  RoundCorner — тоже линейный `RGBA_8888`, но не повёрнутый, и он `DEVICE`.

**Второй SurfaceView** — это `PadSurface` (`dm/android/app/src/main/java/com/xiaoboluo/d2r/PadSurface.java`): экранный пэд DOS2, рисуется
CPU-канвой на своём потоке `dm-pad-draw`, `setZOrderMediaOverlay(true)`, `PixelFormat.TRANSLUCENT`, размер окна (2400×1080); включается
в `DmActivity` (`d3Pad.attachSurface()`, по умолчанию для DOS2; выключатель `DM_PAD_SURFACE=0` в dm/env или `--ez dmPadHwui true`).
Обновляется редко (в дампе frame 211 против 7016 у игры).

**Вывод:** причина CLIENT — **поворот** (ROT_90 линейных буферов), а не «два полноэкранных слоя»: неповёрнутые линейные слои этот
дисплей ставит DEVICE, стадий смешивания хватает, отказ — от HWC на обоих повёрнутых слоях. По строке `IWE-1` [м]: DPU SM8650 поворачивает
не больше одного слоя за кадр (и/или только сжатые форматы), а повёрнутых полноэкранных слоёв два → оба уходят на GPU.
**Ожидание после этой ветки (`DM_NR_ROTATE=1`, резкость вкл.):** слой игры — `DEVICE`, Transform `0` (ему поворачивать нечего).
Пэд остаётся `ROT_90`: либо теперь он единственный повёрнутый и DPU его берёт (`DEVICE`), либо остаётся `CLIENT` — тогда SurfaceFlinger
рисует на GPU **только пэд** в client target, и пока пэд не меняется, переиспользует его (`reusedClientComposition=true`) — ежекадровая
GPU-композиция уходит почти вся.

**Чтобы пэд стал DEVICE наверняка** (не сделано, Java + jni, вне шага): (a) pre-rotation пэда — в `nativeWrap`
`ANativeWindow_setBuffersGeometry(1080, 2400)` + `ANativeWindow_setBuffersTransform(<обратный поворот дисплея>)`, а в
`PadSurface.drawNow` повернуть канву (`canvas.rotate/translate` по `Display.getRotation()`, переустанавливать при перевороте); (b) сжать
`PadSurface` до прямоугольников кнопок (маленькие слои — дешевле и для GPU-композиции); (c) рисовать пэд в кадре игры (Vulkan, один слой) —
больше всего работы. Различить причины на телефоне — одним прогоном с `--ez dmPadHwui true` (пэд на окне HWUI, второго SurfaceView нет).

## 7. Ожидаемый выигрыш (не замер; [м] — модель)
Кадр 2400×1080 RGBA8 = 10,4 МБ. Старый путь с RCAS: RCAS (чтение ~10 + запись 10) + blit (10 + 10) + композитор CLIENT на GPU (чтение
игры 10 + пэда 10 + запись client target) ≈ 70 МБ GPU-трафика и два лишних прохода на кадр + переключение GPU на RenderEngine
SurfaceFlinger. Новый: один проход (≈ 20 МБ), игра — DEVICE.
- **Mi 14 / Adreno 750** (кадр палубы упирается в GPU: `gpu_ms` мед. 10,8, 83 fps): −0,5…1,2 мс GPU на кадр [м] (≈ +4…10 % fps) — RCAS-
  изображение и blit (−20 МБ, ~0,3 мс) + GPU-композиция SF; меньше тепла.
- **Snapdragon 7 / Adreno 7xx:** −0,8…2 мс из ~33 [м].
- **Mali / Xclipse / PowerVR (тайлеры, CLIENT вероятнее):** −1…3 мс GPU [м]; фрагментный проход с `DONT_CARE` пишет плитку прямо в буфер
  дисплея (у compute с `STORAGE` — нет, и AFBC бы пропал).
- **Слабый CPU:** ≈ 0 (запись дескриптора та же, compute-dispatch и барьеры RCAS ушли).
- Без резкости — ничего не меняется (NR-20 как было).

## 8. Риски
1. **Не запускалось на телефоне;** NR-20 на DOS2 вообще ни разу не включался (из-за RCAS), так что первый прогон `=1` проверяет и
   направление поворота самого NR-20 — **со скриншотом**.
2. Округление: фильтр во фрагментном шейдере против `rcas.comp` — возможна разница 1 LSB (пробы `=2` терпят ±1; на глаз не видно).
3. Пэд может остаться `CLIENT` (см. §6) — тогда выигрыш от снятой композиции частичный (обычно client target переиспользуется).
4. Кадры с линейной фильтрацией при повороте — без резкости (DOS2 их не имеет; TL2 pillarbox).
5. Валидация (`DM_VK_VALIDATION=1`) не прогонялась; новый push-диапазон VS|FS 0..92 и `layout(offset=32)` в FS — по правилам Vulkan
   (`maxPushConstantsSize` ≥ 128 гарантирован).

## 9. Прогон телефона (главный агент, по разрешению хозяина; бэкап `files/save`, keyguard, погасить игру после логов)
APK этой ветки; окружение как `nr4_rot*`: умолчания DOS2 + `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_FPS=0`, резкость в шестерёнке «средняя»
(`RCAS sharpening at present: stops=0.5`), маркер `gfx-present-timing`. Палуба, 60 с от загрузки.
1. **R2** `DM_NR_ROTATE=2`: строка swapchain `… -> pre-rotated: swapchain identity, image 2400x1080 … (=2: would pre-rotate; …); RCAS: the
   sharpening turned draw is probed against RCAS's own image`; раз в 10 с `blit P; RCAS on: …; probes ok ≈12·P/120 bad 0 skipped 0 (RCAS
   ≈P/120); red 0`.
2. **R3** `DM_NR_ROTATE=3`, 20 с + `adb exec-out screencap -p > r3.png`: картинка не перевёрнута / не зеркальна, резкая как у S0, без
   пурпура.
3. **R1** `DM_NR_ROTATE=1`: `Swapchain: window=2400x1080 extent=2400x1080 currentTransform=2 supported=511 selected=2`;
   `… -> pre-rotated: swapchain rotate 90, image 1080x2400, window extent 2400x1080; RCAS sharpens in the turned draw (one pass)`;
   раз в 10 с `P presents, turned draw P, blit 0; RCAS on: sharpened in the turned draw P, unsharpened 0; … red 0`.
   `adb shell dumpsys SurfaceFlinger > sf_r1.txt`: в `HWC layers` строка `8ad18b5 SurfaceView…(BLAST)` (имя меняется) — **`DEVICE`,
   Transform `0`**; в `HWC3` — `composition: DEVICE/DEVICE … transform: 0/0/0`; в таблице SDM слой игры — `SDE`. Слой пэда (Z 12): записать
   `DEVICE`/`CLIENT` и `reusedClientComposition`. ABBA с S0 (`DM_NR_ROTATE` не задан) по 90 с: `fps`, `gpu_ms`, `present_gpu_us`.
4. В R1 перевернуть телефон на 180°: `surface rotate 270 … swapchain rotate 270`, строка `phone turned 1`, скриншот не вверх ногами.
5. **R1-pad** = R1 + `--ez dmPadHwui true` (или `DM_PAD_SURFACE=0` в dm/env): dumpsys — один SurfaceView, `DEVICE` / 0. Если в R1 пэд был
   `CLIENT`, а здесь всё DEVICE — остаток CLIENT от пэда (§6, пути a–c).
6. **R1v** `DM_NR_ROTATE=1 DM_VK_VALIDATION=1` (APK `-Validation`) 30 с: нет `dm-vvl` про push constants / render pass / layout.
