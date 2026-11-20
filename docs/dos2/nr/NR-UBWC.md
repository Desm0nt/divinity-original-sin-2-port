# NR-UBWC / NR-LRZ: что драйвер Turnip делает со сжатием кадра и с LRZ у DOS2 (19.11.2026, ночь, раунд 2 N3, без телефона)

Ветка `dos2-nrender` (worktree `wt-nrender`, после слияния `dos2-night1119`). Драйвер DOS2 — стоковый `turnip27` = Mesa `1c3a936`; читался
исходник `<tools>/src/mesa-full-dm` (база `d68c70f` = тот же коммит; патчи DM для Eternal `tu_image.cc` / `tu_lrz.cc` / `fd6_layout.c` не трогают).

## 0. Коротко

| Вопрос | Ответ |
|---|---|
| Теряют ли цели DOS2 UBWC на Mi 14 (A750) | **Нет, кроме маски теней R8 2160x972 (≈ 4.2 МБ / кадр)** — у a7xx в `freedreno_devices.py` не выставлен `has_8bpp_ubwc` (его ставит только `a6xx_base`, gen2–4 сбрасывают, `a7xx_base` не ставит), и хвоста яркости ≤ 8 px (линейный, 0 МБ). Все остальные (G-буфер R11G11B10F / RG16F / RGBA8, D24S8, R16, R11G11B10F света / bloom, RGBA16F частиц, RGBA8 поста / UI, D16 атласа) — UBWC. Рычага для Mi 14 здесь **нет**. |
| Где теряют | **Adreno 6xx (A618…A660, Snapdragon 7xx / 8xx Gen 1-класс до 8 Gen 1) на Turnip:** `supports_uav_ubwc` нет → любой образ со STORAGE без UBWC. DOS2 ставит STORAGE образам с флагом NVN IMAGE (0x4), compute в кадре 0 (`dispatches=0.0`). Если у целей этот флаг есть — до 269 из 312 МБ трафика целей идут несжатыми (модель). Есть ли он — покажет `DM_NR_UBWC=2` («with STORAGE usage»). |
| Что сделано | `DM_NR_UBWC` (0 / 2 счёт / 1): **спрашиваем сам драйвер** (`VK_EXT_image_compression_control`: `vkGetImageSubresourceLayout2EXT` + `VkImageCompressionPropertiesEXT` — Turnip отвечает `fdl_ubwc_enabled` раскладки, Mali — AFBC) по каждому образу-вложению, причина — повторный вопрос по тому же create info без STORAGE / MUTABLE / INPUT_ATTACHMENT. `=1`: 2D-цвет со STORAGE, который драйвер сжал бы без STORAGE и не сжимает с ним, создаётся без STORAGE; первый storage-bind пересоздаёт его со STORAGE и копирует (путь `upgrade_mutable`). На Mi 14 `=1` ничего не меняет (драйвер отвечает «сжато» и со STORAGE). Модель правила Turnip — `nr_ubwc.h` + хост-тест. |
| LRZ | **Рычага нет, доказано замером:** `m5` (`TU_DEBUG=sysmem,nolrz`) против `m7` (база), 14.11: GPU 12.56 против 12.59 мс (медианы 123 / 120 окон). Причины по исходнику и списку конвейеров — §3. `DM_NR_LRZ` не делался: единственная правка, которая вернула бы LRZ (перестановка трафаретных draw пре-пасса в конец прохода), не бит в бит (порядок операций трафарета). |

## 1. Условия, при которых Turnip `1c3a936` выключает UBWC (исходник)
`tu_image.cc` `ubwc_possible` → `tu_image_init`, `fd6_layout.c fdl6_layout_image`, props из `freedreno_devices.py`:
1. a702 — всегда без UBWC. 2. `SPARSE_RESIDENCY`. 3. Сжатые форматы (BC / ASTC / ETC), `E5B9G9R9`, `S8_UINT`. 4. SNORM без
`ubwc_unorm_snorm_int_compatible` (до a740). 5. 8-битные однослойные форматы без `has_8bpp_ubwc` (A6xx gen2–4 **и все a7xx**). 6. 3D с мипами.
7. STORAGE без `supports_uav_ubwc` (все A6xx). 8. A690 и глубина. 9. `HOST_TRANSFER`. 10. D24S8 с выборкой трафарета без
`use_z24uint_s8uint` (только с drirc ANGLE). 11. D24S8 / X8D24 MSAA без `has_z24uint_s8uint`. 12. R64. 13. Линейная плитка: `TILING_LINEAR`,
«фальшивые» форматы, 1D, FDM, HIC со свопом, модификатор DRM LINEAR (импорт AHB / dma-buf). 14. `MUTABLE_FORMAT`: без совместимого списка
форматов — выкл. (на a7xx_gen3 со списком — вкл., если каждый формат списка проходит п. 1–12). 15. `VK_IMAGE_COMPRESSION_DISABLED_EXT`,
`TU_DEBUG=noubwc`. 16. В раскладке: байт на пиксель не степень двойки (RGB8 / RGB16 / RGB32), 3D-глубина > 1 с мипами, ширина меньше порога
линейного отката (16 текселей × ширина блока): такие уровни линейные. Режим sysmem / GMEM UBWC не меняет.

## 2. Аудит образов порта
| Путь | Флаги / usage | UBWC на A750 |
|---|---|---|
| `get_image_slow` (все текстуры и цели) | OPTIMAL; `TRANSFER_SRC/DST`, `SAMPLED`, `COLOR` / `DEPTH_STENCIL_ATTACHMENT` по формату, **STORAGE при флаге NVN IMAGE 0x4**, `INPUT_ATTACHMENT` только при `DM_NR_ONCHIP` 1 / 3; без MUTABLE (`no_mutable_images`), `CUBE_COMPATIBLE` для кубов | да (цели); BC / ASTC текстуры — нет (сжаты сами) |
| `upgrade_mutable` | MUTABLE + список форматов (`VK_KHR_image_format_list`) | да, если формат списка совместим (в логах DOS2 0 апгрейдов) |
| texstream `ci_of` (NR-22) | как `get_image_slow` | как он |
| NB-19 transient | пересоздание из `im.create` | как исходный |
| кэш теней NR-SHCACHE | D16 OPTIMAL | да |
| present: `vulkan_present.cpp` проба / RCAS-образ | `COLOR|TRANSFER_SRC` / `STORAGE|TRANSFER_SRC` | да (a7xx: STORAGE не мешает) |
| swapchain (NR-11 / NR-20 рисуют прямо в него) | gralloc решает (флаг UBWC буфера окна, `u_gralloc` → модификатор QCOM_COMPRESSED / LINEAR) | **неизвестно**, `DM_NR_UBWC` спрашивает (`[nr] ubwc swapchain: N of M images compressed`) |
`HOST_TRANSFER`, `VkImageCompressionControlEXT`, `TILING_LINEAR`, внешняя память образов, custom border colour в DOS2 не используются (grep).

Цели кадра (NR-FRAME §1.1) по модели `nr_ubwc.h` (хост-тест печатает таблицу, `NRU_SHOW=1`):
| Поколение | Без STORAGE: несжато | Если у всех цветовых целей STORAGE |
|---|---|---|
| A750 (Mi 14), A740, A730 | маска теней R8 (4.2 МБ), хвост яркости ≤ 8 px | то же |
| A650 / A660 / A642 класс | маска теней R8 | **269 из 312 МБ** (всё, кроме глубины) |
| A630 | — | 269 МБ |
Перевод в мс для слабых Adreno 6xx (LPDDR4X 15–25 ГБ/с, UBWC ×0.5–0.7 на цвете): если STORAGE есть, `=1` снимает ≈ 80–130 МБ / кадр
→ **≈ 3–8 мс** при 2160x972, ×0.56 при DRS 75 % (верхняя граница, модель). Если флага нет — 0.

## 3. LRZ (tu_lrz.cc) и DOS2
A750: `has_lrz_feedback` → LRZ работает и в sysmem (`tu_lrz_sysmem_begin` = tiling begin). Что его выключает (исходник) и где это у DOS2
(список конвейеров `play-1118/vk-pipeline-list.bin`, 2 038 записей с глубиной):
| Условие Turnip | Действие | У DOS2 |
|---|---|---|
| сравнение EQUAL / NEVER | LRZ выкл. на draw (как у blob) | **G-буфер: 574 конвейера EQUAL** после пре-пасса — основная масса G-буфера всегда без LRZ |
| трафарет, который может убить фрагмент | запись LRZ выкл. до конца прохода | пре-пасс: 21 конвейер LEQ+запись с таким трафаретом, 73 EQUAL; один такой draw гасит запись LRZ на остаток пре-пасса |
| запись глубины + смешивание | запись LRZ выкл. до конца прохода | forward / вода: 291 конвейер (LESS / LEQ + запись + смешивание) |
| ALWAYS / NOT_EQUAL + запись | LRZ недействителен | DepthDownsample (FragDepth) и др.: 2 |
| FS пишет глубину / discard без early-z | пропуск / сброс | DepthDownsample, альфа-тест материалов |
| глубина не загружена и не очищена, смена направления | сброс | нет (план NB: CLEAR / LOAD) |
Итог: пре-пасс (LEQ + запись, 262 конвейера) — единственный, где LRZ мог бы работать, а он упирается в цену draw (10–11 мкс / draw, от
разрешения не зависит — AUDIT-REMAINING); G-буфер — EQUAL. Замер подтверждает: `nolrz` = база (12.56 / 12.59 мс). Перестановка
трафаретных draw в конец пре-пасса вернула бы запись LRZ, но порядок трафаретных операций не коммутирует — не бит в бит; отвергнуто.

## 4. `DM_NR_UBWC` (сделано)
- `vulkan_present.cpp`: при `DM_NR_UBWC != 0` в env — `VK_EXT_image_compression_control` (+ `_swapchain`) только для вопросов (ни один образ не
  создаётся с `VkImageCompressionControlEXT`), `dm_vk_compr_ctl`, `dm_vk_ifp2` (`vkGetPhysicalDeviceImageFormatProperties2` через
  `vkGetInstanceProcAddr`), строка `[nr] ubwc DM_NR_UBWC=…: … -> enabled / not available`, после создания swapchain `[nr] ubwc swapchain: N of M images compressed`.
- `xbg_nr_ubwc.inc` (namespace `nrubwc`): `att()` в `create_pass` (каждое вложение раз в кадр: ответ драйвера один раз на образ, причина,
  первые 24 несжатых — полной строкой), `lean()` (=1, оба пути создания, потокобезопасно), `upgrade()` (storage-bind), строка раз в 10 с:
  `[nr] ubwc DM_NR_UBWC=N: … attachment images X, compressed Y, not Z (storage usage …, mutable format …, input-attachment usage …, format / size / tiling rule …, no answer …); attachment bytes … MB, on uncompressed images … MB; with STORAGE usage … (… MB: uncompressed on Adreno 6xx), created without it here …; created without STORAGE (=1) … images so far, given STORAGE back at a storage bind …`.
- `upgrade_mutable(im, want, add_usage)`: при `add_usage` — то же пересоздание + копия всех уровней, без MUTABLE и без смены списка форматов.
- Умолчание **0 на всех семействах** (`xbg_nr_ubwc_default`): без env ничего не включается и не вызывается.
- Риски: `=1` — если игра когда-то свяжет такой образ как storage, один раз пересоздание с копией (как mutable upgrade; в DOS2 compute нет);
  драйвер без расширения — строка «not available», счёт пуст, `=1` ничего не делает.

## 5. Тесты (ПК)
`tests/nr_ubwc_test.c` (WSL gcc): **33 проверки, all passed** — размеры / snorm / сжатые форматы против `vulkan_core.h`, углы правила
(STORAGE a7xx / a6xx, 8 бит, snorm a740, mutable со списком и без, узкие, RGB8, E5B9G9R9, 3D + мипы, D24S8 + ANGLE) и цели DOS2 на 5
поколениях. `source_structure_test.py` блок `nrubwc_check`: PASS (чистый заголовок, умолчание 0, расширение только из env, ни одного
`VkImageCompressionControlEXT`, 5 крючков за `mode`, `lean` только 2D-цвет и только при ответе драйвера «STORAGE мешает», апгрейд usage-only).
APK собран (§6).

## 6. Прогоны телефона
| # | env | Зачем | Длительность |
|---|---|---|---|
| UB2 | `DM_NR_UBWC=2` | Mi 14: подтверждение «сжато всё, кроме R8 и хвоста яркости»; swapchain сжат ли; **сколько целей несут STORAGE** (решает ценность `=1` для Adreno 6xx) | 60 с ходьбы |
| UB2s | `DM_NR_UBWC=2 DM_GFX_SIM_GPU=mali` | те же ответы под решениями Mali (сам драйвер — Turnip; для настоящего AFBC нужен тестер на Mali) | 60 с |
Решает строка `[nr] ubwc` и строки `attachment not compressed`. Если «with STORAGE usage» ≈ 0 — `=1` бесполезен и закрывается.
