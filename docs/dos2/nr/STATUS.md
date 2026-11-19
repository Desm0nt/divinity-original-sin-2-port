# NR implementation status (main session, night 17.11)
| Step | Branch | Base | State | Notes |
|---|---|---|---|---|
| NR-02 KEY | dos2-nr02 b1111ae c8b43fc | dos2 94d287d | done, PC only | fixes NR-01 pack key (program key = control+code); 2.4 % pipelines > 8 dyn UBO (Mali/PowerVR) |
| NR-03 OCC | dos2-nr03 7c160a0 eb78ad0 | dos2 | done, PC only | gpu path Adreno/Xclipse, prev path Mali/PowerVR; gain unknown until =2 run |
| NR-04 SH | dos2-nr04 b8c3b98 ae0fdb4 aa7a07d | dos2-nr02 | done, PC + libdm build | swap ~-0.1 ms on Mi 14 (not ALU-bound), model -0.5..2.5 ms S7; setup stage 8 also builds native-nr.xgspack (13 bundles); exact shadows not bit-exact (PCF default 0) |
| NR-05 TEX | dos2-nr05 db1a243 5784eea | dos2 | done, PC + APK build | gain depends on =2 run (owner changes vs late writes); Mali gain small (91 % BC bytes) |
| NR-06 BUF | dos2-nr06 7296521 8864762 | dos2 | done, PC + libdm build | ring mirror 19.5 MB w/o zero-copy, static VB/IB copies (192 MB budget, may be high for 6-8 GB phones) |
| NR-07 PIPE | dos2-nr07 ecb0ae0 4fe6de0 94dbc0a | dos2-nr02 | done, PC + APK build | big find: warm-up cost = pipeguard marker files on FUSE (~13 ms/pipeline), fixed by port-common pipe-marker-mmap (default on, commit ecb0ae0); per-draw table waits for NR-09 |
| NR-10 RP | dos2-nr10 e1552e4 36f66a7 | dos2 | done, PC + APK build | barrier narrowing only (65 full barriers/frame -> ~0); FOUND live Mali/PowerVR regression in 0.1.1: NB PREBAR undoes TILE_BARRIER (43 full pre-pass barriers), NR-10=1 fixes; NOLOAD never fires (computed VS position) -> NR-10b |
| NR-10b COVER | dos2-nr10b b29c912 b63d805 | dos2-nr10 | done, PC + APK build | post VS position proven (117 shaders), 22 post passes/frame -> load DONT_CARE on tilers (~70-90 MB), area passes at DRS<100 %; Mi 14 ~0 | coverage proof for computed positions: the real tile lever (~70-90 MB loads/frame) |
| NR-11 PRESENT | dos2-nr11 e81e18f adc021b | dos2 | done, PC + APK build | f1pt1 "regression" was a 90 Hz phase artefact; -1 full-screen blit (Mi 14 est +2..5 % fps, Mali -0.6..1.2 ms); first run needs DM_VK_VALIDATION=1 |
| NR-08 DESC | dos2-nr08 973b8f2 1581325 d908b91 92cb1a3 40461f6 | nr02+nr05+nr06 merged | done, PC + libdm build | desc 2.9-3.5 -> ~0.5 ms Mi 14 (est), MTK 9-12 -> 1.5-2.5, PowerVR 18-45 -> 3-6; split path for >8 dyn UBO; finding: DM_GFX_HIT_FAST skips nothing in default build |
| NR-09 DRAW | dos2-nr09 95acec0 5c57b5f 7e0c061 bbab3bb | nr08 + nr07 merged | done, PC + APK build | translator -1.4..1.7 ms Mi 14, MTK/Mali -5..8 ms (est); OP_X_NB_DRAW + NBD_F_NR tail |
| NR-12 UI | dos2-nr12 21f132e 7957b66 | dos2-nr09 | done, PC + APK build | menu 2100 draws are 3D (not UI); UI = 145-185 Iggy draws/frame in campaign -> lane; est -0.2..1.0 ms CPU |
| NR-08b PowerVR desc | dos2-nr08b | dos2-nr08 | done, PC + libdm build, DM_NR_UBODYN (PowerVR: dyn UBO only for stages that read them; est 27-55 -> 8 ms, with NR-08 ~1-1.5 ms) | PowerVR no-dyn-UBO: memo misses, +11..22 ms |
| NR-13 | - | all | waiting | |
To fix in NATIVE_RENDERER_PLAN.md §0: pack key is xbg_shader_program_key (NR-02 finding).

## Audit (dos2-nraudit a8f9378, docs/dos2/nr/AUDIT-REMAINING.md): steps NR-14..26
| NR-14 SAMPLES_TS | dos2-nr14 33642c0 | dos2 | done, PC + libdm build, DOS2 default 0 when no consumer | 526 unread BOTTOM_OF_PIPE stamps/frame, est -0.8 ms GPU deck |
| NR-15 NB defaults | dos2-nr15 6eee5fb d6b6f9f 406466c | dos2 | done, PC (no APK); +NR-15b DM_NB_MX off (NR-16 analysis) | RELOC/ORDER/TRANSIENT/HIT_FAST do nothing on DOS2, cost ~1.4 ms CPU; reverses part of "врубай всё" -> owner decides at merge |
| NR-16 CPU attribution | dos2-nr16 cac4056 f713a9e 8bed26c | dos2 | done, PC + libdm; finding: NVN calls are on the recorder thread, not presenter; old DM_NVN_PROF=1 never ran on Android | also decides DM_NB_MX and present thread defaults |
| NR-17 cpu-place + adpf-boost | in dos2-nr19 | dos2 | modules merged; DOS2 job thread names unknown (from R-NR19b log) | |
| NR-18 ASTC cached read | dos2-nr18 7bda418 1d5c913 d83f17c | dos2 | done, PC (syntax only), default ON (DM_NR_ASTCREAD=1) | TL2 fix, Mali/PowerVR zone hitches |
| NR-19 desc-memo-keys | dos2-nr19 7dc29e0 1a0d412 cfcb336 8eff979 | dos2 | done, PC | memo fix was already in dos2 (counter+test added); cpu-place + adpf-boost merged (default on, separate commits); PowerVR policy est +11-22 ms translator CPU -> needs NR-08 on PowerVR |
| NR-20 pre-rotation | dos2-nr20 882005d 9640fe3 | nr11 | done, PC + APK build | alternative to NR-11 (not stacked); decide by dumpsys SurfaceFlinger Comp Type (CLIENT -> on); Mali/Exynos likely CLIENT -1..3 ms GPU |
| NR-21 depth-only sort | dos2-nr21 14475c7 c84ee1f 7566077 | nr09+nr16 | done, PC + APK build | DM_NR_SORT, bit-exact proof (fixed-point depth, LESS/LEQUAL); pipeline binds pre-pass 239-526 -> 60-182; risk early-Z loss -> default 0 |
| NR-22 texture stream worker | dos2-nr22 11aab34 7b5fc80 fbd6cc0 | nr05 + nr18 | done, PC + APK build | est -10..25 ms per load event Mi 14 (not 40->5); Mali: ASTC cache reads off the translator -25..160 ms per zone entry |
| NR-23 NVN call bypass | dos2-nr23 4b4c5c0 a39e6ac 589a9b9 9814932 | nr09+nr16 | done, PC + APK | premise mostly wrong: only 13-24 % of calls cleanly cuttable, est 0.02-0.06 ms Mi 14 -> likely no-go; counting mode decides; lean entry for all calls = NR-28 |
| NR-28 lean NVN entry | dos2-nr28 0485557 bb0a45a 72054aa | nr23 | done, PC + APK | 94 -> 40 instructions per NVN call, ~0.15 ms/frame Mi 14, 0.2-0.46 ms MTK; DM_NR_LEANCALL |
| NR-24 HLE fast gates | dos2-nr24 2d743a3 cb4f06e edba8e7 | dos2+nr16 | done, PC + libdm | TICK (cntvct fast entry), ARB (256 per-address buckets, 3.5x on ping-pong), YIELD (futex wait); all default 0 |
| NR-25 spirv-opt in setup | dos2-nr25 e19aaee 5dd0f8f | dos2 | done: NO GAIN (malioc: -0.7..2.1 % weighted), not implemented; finding: NR-04 shaders -48..56 % cycles on Mali; UBO scalar reads = Mali bottleneck -> NR-27; SPIRV-Tools lib in APK removable (NR-26) |
| NR-27 UBO vec4 merge | dos2-nr27 574448b a7290e0 | dos2 | done, PC + libdm; LOAD-TIME pass (not setup); Mali default 1 in branch; malioc G57 -32 % frag / -32 % vertex on 2211 modules; 14129 interp pairs 0 diff | setup SPIR-V pass, Mali -40..60 % on 1152 shaders |
| NR-26 cleanup | - | nr09, nr13 | queued | |
OWNER DECISION: DRS default on weak GPUs (33 ms budget, 75 % floor) = biggest weak-GPU lever, conflicts with docked-quality rule.

## Final audit (dos2-nraudit2 99edbd8, AUDIT-FINAL.md) -> new steps
| P0 tiler policy + N5 census | dos2-nrp0 115ccd1 c245df5 ae1e78b | nr-all | done, PC | DM_NR_RP=1 on Mali/PowerVR by family; N5: NR-27 changes 0 of 117 position programs. Release hotfix dos2-nrp0-release 46fddd0 (from dos2): DM_NB_RP only on Adreno -> tilers back to 0.1.0 heuristics; needs SIM mali run + test prerelease |
| NR-29 DM_NR_MEM | dos2-nr29 3d7526a bfe3ed9 156576e | nr-all | done, PC + APK, default 1 (caps only lower; trims + MemAvailable poll) | 6 GB: 552 -> 156 MB caps (-396 MB) when NR caches on |
| NR-30 pre-pass hybrid on tilers | dos2-nr30 11b6338 f8ef474 | nr-all | done, PC + APK | old hybrid wrong every frame when unchecked -> rewritten with previous-frame memory + gates; also fixed NR-09 lane stale depth-state bug with PREPASS; default 1 on Mali/PowerVR in branch (needs prerelease) |
| NR-31 portable pipeline seed | dos2-nr31 0a60d7b a03ded1 f0f23ec adbc19c | nr-all | done, PC + APK | converter + runtime ready, NO seed yet (needs pulled Mi 14 list); iPad .pso cannot replace it; legal audit: hashes + Vulkan numbers only; DM_NR_SEED default 1 (no-op without seed) |
| NR-13/26 DM_NR_LEAN | dos2-nr13 ed627a6 5817b99 ec66765 | nr-all | done, PC + APK | premise wrong (lane reads the records); skips NB-13a capture + NB-12 notes only, est -0.1..0.2 ms game thread; removable-code list in NR-13.md §9 (SPIRV-Tools +1.8 MB APK) |
| T2 IDVS DM_NR_IDVS | dos2-idvs | nr-all 85d2cfa | done, PC + APK, default 0 everywhere | position slice already minimal (malioc: position variant = position-only module 79/79); textures in it only FX materials (477 VS, FJ 0.06 % verts), dynamic UBO = skinning; ONE translator artefact: gl_PointSize=1 in all 6132 VS -> strip (points / tess / GS keep it). malioc FJ weighted: G52 IDVS back on (binning 17.9 -> 4.5 M cyc/frame), G57/G78/G715 position -21 %; proof PASS; needs Mi 14 =2 / =1+SIM mali, then real Mali A/B (NR-IDVS.md §10) |

## Телефон 18.11 вечер (Mi 14 Ultra, Turnip sysmem, Fort Joy, 50 с, `DM_DOS2_FPS=0`, без теплового капа)
**Партия C (включения, ходьба; база C_base 79.4 fps, GPU 11.81 мс):** C_surf (`SH_SURF=1`) 79.1, C_mat 79.3, C_state (`STATE=4`) 79.5,
C_ubo (`UBO=1 UBD=1`) 79.5, C_trx (`TRX_STICKY=1 ZC=1`) 78.9, C_tap 80.9, C_tex (`TEX=1 TEXSTREAM=1`) 79.5, C_fold (`DRAW_FOLD=3`) 78.8,
C_surf3 (яд) 79.3 — пурпура нет (на Adreno волна целиком почти не умирает), C_aao (`NB_POST_AA_OUTLINE=1`) 81.1, C_all (всё, кроме fold) 78.2.
Все чистые (0 расхождений, 0 красных строк), FPS на Mi 14 не двигают — кадр упирается в GPU ~12 мс; ценность — слабые CPU / тайлеры.
Отвергнуто: `TU_DEBUG=gmem` −20 fps; `DM_SUSTAINED=1` — HyperOS Mi 14 не поддерживает; R11G11B10F — уже так (PERF-X17).
**Кэш теней (NR-SHCACHE):** shc4 бит в бит, ABBA стоя +2.15 fps, умолчание 1 на Adreno (`55c455a`), см. NR-SHCACHE.md §12.
**Партия D (тайлерные пути под `DM_GFX_SIM_GPU`, ходьба, APK `8427667e`):** везде 0 падений, 0 красных строк, 0 пурпурных пикселей на скриншотах.
- D_sim (`SIM=mali`, умолчания семейства): `DM_NR_RP=1` (family default) работает: барьер перед IR-проходом none 27 / fragment 1 / full 16 из 44;
  одна `store-hist VIOLATION` (serial 11, 2400x1080 fmt 0x35, кадр 237, «never discarded again» — самоизлечилась), как и в D_uix1 / D_rp3.
- D_onchip2 / D_onchip3: **цепочек 0** — все ~770 частей «sizes differ» (небо и свет разного размера) → NR-ONCHIP на текущей сборке ничего не делает; разобрать.
  **Разобрано 19.11 (ветка `dos2-onchip2`, NR-ONCHIP.md §9):** проходы одного размера (лог хоста: DeferredSky и ClusteredLighting оба 2160x972),
  врала таблица размеров тапа — она по адресу памяти и хранит ПОСЛЕДНИЙ размер, а память Sp потом в том же кадре — цель блума 1080x486.
  Теперь размер берётся у каждого прохода на момент привязки; на дампах ra1/rb1/rc1 старое правило 0 из 34, новое 34 из 34. Нужны 2 прогона (§9). Коммит 118c858, APK `bde6c3fd`.
- D_uix2 (счёт): отвязать DS 18.9 прохода/кадр (373 МБ load+store на тайлере), сузить область 21.8 (817 МБ), разрывы для слияния 0, кэш UI 0 %.
- D_uix1 (`UIX_DS=1 AREA=1` + SIM): отвязано 18.8, сужено 9.0 прохода (83 МБ), UI на скриншоте тот же.
- D_rp3 (`RP=3` яд свежих + SIM): 14.1 свежих вложений/кадр начаты с DONT_CARE и отравлены — пурпура нет → `DM_NR_RP_FRESH` кандидат в умолчание тайлеров (−~60 МБ загрузок/кадр).
- D_mat (`MAT=1` + `family-mali`): XLQ-близнецы 143, `check_bad=0`, картинка = база.
- D_shc2 (`SHCACHE=2` + SIM mali): ходьба, пропуск 41 %, `differ 0`.
**Тайлерный план, шаг 1 (ветка `dos2-tdef` от `dos2-nr-all`, 19.11, без телефона):** на Mali / PowerVR (`xbg_tile_family`, и под `DM_GFX_SIM_GPU=mali`) по умолчанию `DM_NR_RP_FRESH=1` (по `D_rp3`) и `DM_NR_UIX_DS=1` + `DM_NR_UIX_AREA=1` (по `D_uix1`); Adreno и остальные — без изменений; `dm/env` `=0` выключает; ступень 1 безопасного режима гасит UIX. Строки старта называют источник (`family default` / `env`). `store-hist VIOLATION serial=11` — UI depth-stencil, один проход одного кадра на запуске, не чинил (NR-10.md §10.1), строка теперь называет читателя. Проверка: один прогон `DM_GFX_SIM_GPU=mali` без другого env (NR-10.md §10, NR-UIX.md §9).

**NR-GMEM (T8, 19.11, ветка `dos2-gmem`, NR-GMEM.md):** свой GMEM / sysmem по проходу на Turnip без пересборки драйвера: `DM_GFX_PASS_GMEM=1` → `TU_DEBUG=gmem` + барьер VERTEX → VERTEX перед концом каждого прохода, который правило оставляет в sysmem (1…5 draw, ≤ 3 вложений, ≥ 600000 пикс.; вето storage / чтение своей глубины / подпроходы); `=2` счёт, умолчание 0. Хост-тест `nr_gmem_test` 14004 PASS, `source_structure_test` PASS, APK SHA-256 `ba60eadb…770e`; на телефоне не запускалось (ожидание ≈ 0, ABBA решает).

## Тайловый план PERF-X19, шаг 3 (счёт, одобрено хозяином 18.11)
| Step | Branch | Base | State | Notes |
|---|---|---|---|---|
| NR-TCOUNT T3/T4/T5 count | dos2-tcount (this commit) | dos2-nr-all 85d2cfa | done, PC + APK, на телефоне не запускалось | `DM_NR_TCOUNT=1` (умолч. 0): 4 строки `[nr] tcount T3/T4/T5/top` раз в 10 с на кадрах записывателя `pgraph` (1 из 4): VS-only draw и разрывы у записи storage, гистограмма открытий цели + МБ повторов (4 Б/px), переставимые пары (exact / если буферы позволяют), копии / очистки внутри прохода, соседние проходы; пороги и мс на G57 — NR-TCOUNT.md §4; прогон TC1 / TC2 (`+DM_GFX_SIM_GPU=mali`) §6; APK `wt-tcount/work/DOS2-port-debug.apk` 56 346 813 B SHA-256 54dc79b7cd79ea8455ca0d2b92142f6a91765ac003bac33be5f6b077f99823bf (not run on the phone); host suite 87 + 1 flaky (nr_ubo_test thread contention, 3/3 PASS on rerun) |
**Dref для теней (PERF-X19 T11, ветка `dos2-dref`, 19.11):** `DM_NR_SH_PCF=3` — 4 отвода `mobile` через `sampler2DShadow`, хост ставит сравнение
LESS + линейный фильтр на binding атласа (`dref_bindings` / `dref_sampler`), без фильтра глубины на устройстве — сам на `=2`. `malioc` G57:
длинный путь A 6.06 (mobile) → 3.56, регистры 32 → 19; оценка G57 −1.3…−1.9 мс против mobile, Mi 14 ≈ −0.1 мс (проход 0.29 мс). Умолчание 0.
Игра рисует 3 ключа теней: PCF_3x768 / 3x512 покрыты, `CascadedPCSS_3x1024` (`DM_DOS2_SHADOWS=0`) — нет. На телефоне не запускалось.


**NR-ZERO (N2, ночь 19.11, ветка `dos2-ntrans`, NR-ZERO.md):** перепись того, что осталось от транслятора (стоя в ФР, Mi 14): поток игры 6.4–7.1 мс
(наша прослойка ≈ 4.9: запись draw 2.45 мкс × 1 658 + S3 0.77), `nvn-translate` 6.0–6.4 мс CPU (стена 9.3–11.3, ждёт GPU 3.7–4.8), рекордер 2.5–3.2,
вызовов NVN через thunk ≈ 1 025 / кадр, HLE 0.03–0.04 мс. Крупнейший отдельный кусок транслятора стоя — хэш кэша теней 0.9–1.6 мс. Ступени Z1–Z7
(Z5 = пакет рекордера прямо с потока игры: транслятор −4…5 мс [м]). Сделано Z1 `DM_NR_SHC_MEMO` (0 умолч. / 2 сверка / 1): точные памяти поисков
хэша (узлы регистрации текстур, версии страниц по эпохе слота трекера `nvn_dirty_version_tok`), значения хэша те же; разбивка хэша по частям
в строке `[nr] shcache memo`. Тесты: `nr_dirty_tok_test` (WSL, настоящие mprotect / сбои, гонка 2 с, мутация ловится), `nr_shc_test` 12 707 PASS,
`source_structure` блок NR-ZERO. Ожидание −0.4…0.8 мс транслятора стоя [м], слабые −1…2.4; FPS Mi 14 0. На телефоне не запускалось.
**NR-TRAFFIC (N5, 19.11, ветка `dos2-nmem`, NR-TRAFFIC.md):** таблица байт CPU за кадр (стоя ≈ 0.4 МБ, ходьба ≈ 0.7, тяжёлые окна ≈ 3–4; UBO 0 копий,
readback 0); «10–26 МБ/кадр в бою» = перезаливки pitch-linear текстуры тумана 1424x1602 (9 МБ, ~45 МБ работы CPU, 2.5–12.8 мс). `DM_NR_LINTEX`
(умолч. 0; 2 сверка, 1 — только написанные строки через трекер, без хэшей, защита до чтения, и для block-linear частичных — найдена гонка
«защита после чтения»). Хост `nr_lintex_test` 17 798 PASS; на телефоне не запускалось (3 прогона, §7).
**NR-FRAME (N3, 19.11 ночь, ветка `dos2-nrender`, NR-FRAME.md):** кадр таблицей (`tools/dos2_frame_table.py`: дамп IR + тепло + список конвейеров; 3D-часть ≈ 258 МБ / кадр
тайлерной модели). На Mi 14 (sysmem) перестройка проходов почти ничего не даёт (очистка 6–12 мкс, загрузки / сохранения бесплатны); найдено и сделано:
`DM_NR_COVER_FMASK=1` (внутри `DM_NR_COVER`, который по-прежнему 0) — маска записи по каналам формата: 18 отказов «маска» на телефоне = пост-шейдеры
R11G11B10F 0x7 / R8 / R16 0x1, +9 проходов / 22 МБ загрузок на тайлере (G57 до −1.1…2.2 мс); `DM_GFX_PASS_GMEM_M=1` (умолч. 0) — M NR-ONCHIP через запрет
NR-GMEM (Mi 14 модель −0.1…0.35 мс сверх NR-GMEM, снимает конфликт ONCHIP=1 + GMEM=1). Отвергнуто с цифрами: CLEAR → DONT_CARE, DS вне UI, склейки bloom /
яркости (не бит в бит), форматы G-буфера (всё ≤ 4 Б/пкс). Хост: nr_frame_test 166, nr_gmem_test 14015, source_structure PASS. На телефоне не запускалось.
**NR-ZERO итерация 2 (19.11, `dos2-ntrans`):** перепроверка по O_ab_* до кода: Z6 −0.07…0.15, Z2 −0.06…0.08 (ПК-бенч поиска 47 нс), Z4 −0.1…0.16 мс —
не окупаются, отвергнуты (NR-ZERO.md §8). NR-08 в установившихся окнах 1.05–1.19 мкс / draw (не 0.6). Сделана разбивка NR-08 / NR-09 по частям
(`[nr] desc by part`, `[nr] draw by part`, 1 из 128 draw, только замер) — по ней выбирается первая часть Z3.
**N6 «Хаки скорости» (19.11 ночь, ветка `dos2-nhacks`, NR-HACKS.md):** страница ⚙ «Хаки скорости…» (Выкл. / Лёгкий / Средний / Максимум / Свой) и
`DM_HACK_*` (умолчание 0 = картинка как сегодня): разрешение 3D через объект DRS игры (фикс или потолок), **FSR 1 EASU** в проходе растягивания
(модуль PPFXAA: 1:1 = `PPFXAA_mobile` бит в бит 400/400, EASU = эталон numpy 400/400; RCAS = «Резкость»), без свечения, без FXAA, карты теней
512, лёгкий / аппаратный фильтр теней, без дальнего каскада теней (перед кэшем теней), mip-сдвиг и потолок анизотропии. Замер-основа: 75 % =
GPU 11.98 → 8.24 мс (`rc1` / `rc3`). Модель Mi 14: Лёгкий +18…25 %, Средний +30…40 %, Максимум +45…70 % (ходьба). EASU на Mali / PowerVR из
уровней не включается (`malioc` G57 10.25 цикла/пиксель против FXAA 3.0). Хост: `nr_hacks_test` 118 PASS, `source_structure_test` PASS;
APK `wt-nhacks/work/DOS2-port-debug.apk` SHA-256 `fddd47f5…6688`; на телефоне не запускалось (прогоны H0–H5, NR-HACKS.md §6).
**NR-DRIFT («теряем 10–15 fps после старта локации навсегда», ветка `dos2-ndrift`, 19.11, NR-DRIFT.md, KB PERF-X24):** это DRS игры, не порт:
первые кадры после загрузки 26–55 мс GPU → DRS сразу на пол 75–85 %, подъём ~20 с (45 прогонов: первые 10 с 106 fps, полка 81; при фиксированных
75 % — 120 fps ровно; три загрузки одного сейва — одна полка, 3 мин без наклона). Утечек / роста кэшей, цены кадра, памяти не найдено.
Сделано: `DM_DOS2_DRS_SETTLE=1` (умолч. 0, `=2` счёт) — цель 100 мс на загрузке + 6 с; `[nr] growth` раз в 10 с (размеры, «с загрузки», уборка,
DRS, тепло, RSS, CPU) + проба часов GPU / памяти без root (`DM_NR_GROWTH` / `DM_NR_GPUPROBE`, у DOS2 умолч. 1); `tools/dos2_drift.py`.
Хост `nr_drift_test` 58 PASS, `source_structure` PASS; APK собран; на телефоне не запускалось (прогоны D1 / D2, §5).
**N1 NR-NSHADER (ночь 19.11, ветка `dos2-nshader`, NR-NSHADER.md):** офлайн-компилятор **Adreno 750** — турнип того же коммита, собранный
под Linux хоста, + поддельный `/dev/kgsl-3d0` (`tools/tu_stats`: статистика ir3 по любому SPIR-V, `tu_survey.py` ранжирует heat-логи
сразу по Adreno и `malioc`). Вывод по цифрам: на 750 переведённый код после цепочки загрузки ≈ нативный (NR-04 / 33A −18…+17 % слотов,
проходы в полосе); NR-27 / NR-32 на Adreno 0, NR-IDVS −0.6 %, fp16 +8 %; материалы переписывать нечего; точные перестройки цикла
Clustered ≤ 5 % — не внедрял. ALU-упор только у Clustered в сценах со светами (TUT 2.65 мс) и F1A (0.81 мс). Главный оставшийся рычаг —
слить DeferredSky → ClusteredLighting (прототип: ALU не растёт, полоса −46…59 МБ/кадр, оценка Mi 14 −0.45…0.6 мс) — нужен план хоста
(§6), не сделан. Кода движка ветка не меняет, APK = база.
**NR-MDI (19.11, `dos2-ntrans`, NR-MDI.md):** склейка depth-only draw в multi-draw — сначала замер. Turnip: `EXT_multi_draw` есть, но на draw всё равно
VS params + `CP_DRAW_INDX_OFFSET` → экономия GPU только состояние между draw, а это перепривязка набора с новыми смещениями (`SP_UPDATE_CNTL` 0xff).
Пачки упираются в буферы вершин (619 смен на 1 121 depth-draw после сортировки) → ≈ 500 перепривязок × 0.3–1.0 мкс = −0.15…−0.5 мс GPU ходьба [м].
Сделано `DM_NR_MDI=2` (счёт пачек, шаг блоков в кольце, смены между draw) и `=3` (проба: без перепривязок смещений, картинка неверная — потолок
выигрыша). `=1` не построен: GO по пробе (≥ 0.4 мс). `nr_mdi_test` 8 558 PASS. Дороже склейки — выборка перемеженных вершин (копия «только позиция»).
**NR-UBWC / LRZ (N3 раунд 2, 19.11, `dos2-nrender`, NR-UBWC.md):** по исходнику turnip27 (Mesa 1c3a936) все цели DOS2 на A750 уже UBWC, кроме маски теней R8
(у a7xx нет `has_8bpp_ubwc`, 4.2 МБ / кадр) — на Mi 14 рычага нет. На Adreno 6xx (нет `supports_uav_ubwc`) STORAGE от флага NVN IMAGE снимает UBWC — до 269 из
312 МБ целей. `DM_NR_UBWC` (умолч. 0): 2 = спросить сам драйвер (`VK_EXT_image_compression_control`) по каждому вложению + причину; 1 = создать без STORAGE там, где
драйвер говорит «мешает только он» (вернуть при storage-bind с копией). LRZ: `nolrz` = база (m5 12.56 / m7 12.59 мс), G-буфер EQUAL, трафарет пре-пасса — отвергнуто.
Хост: nr_ubwc_test 33 PASS, source_structure PASS. На телефоне не запускалось.
**N6 раунд 2 (19.11 ночь, `dos2-nhacks` 1daaddd / 4faebc0 / ba2951b, NR-HACKS.md §8):** EASU в fp16 (`FsrEasuH`, mediump: `malioc` G57 10.25 → 6.62
цикла, ошибка fp16 ≤ 0.79 шага 8 бит) — теперь во всех уровнях на всех GPU; новые хаки не про разрешение: **`DM_HACK_LOD`** (дистанции LOD и
отсечения в 12 обходчиках игры, пещеры в коде) и **`DM_HACK_LIGHTS`** (первые 4 / 2 освещающих пиксель кластерных ламп, вариант света = `x` на префиксе списка бит в бит);
SSAO = 0 (выключен консолью, входа AO нет), частицы ≤ 0.33 мс (не хак), UI через кадр — отвергнуто (≈ 0.17 мс, слой не переживает кадр на тайлерах),
дальний каскад через кадр — бэклог. «Средний» теперь 85 % + LOD 80 + 4 лампы + тени / mip / аниз.: оценка ≈ +35 % (+25…55 %, LOD и свет без прогона).
Лампы: «первые N, что освещают пиксель». APK SHA-256 `091e1f56…900a`; прогоны H6–H9 (§8.5).
**Пузыри между проходами на Adreno (N3 раунд 3, NR-FRAME §9):** < 0.2 мс — граница прохода в Turnip уже WFI + CCU (полные внешние зависимости), A/B `nr10_d5`
(полных барьеров 63 → 16) = GPU 9.09 → 9.12 мс; промежутки меток 82–274 мкс / кадр (верх). Рычага нет, время ушло на S2F (Sky → Light → Fog) для тайлеров.
**S2F заготовка (N3 раунд 3, NR-FRAME §10):** варианты `light.s2f` (0x48) и `fog.s2f` (0x02) доказаны бит в бит 300 / 300 (`dos2_onchip_shaders.py`), Fog без `OC` — те же байты;
перепись дампов: 34 / 34 проходов света образуют Sky → Light → Fog (`dos2_frame_table.py --s2f`). Исполнение (планер с узлом G-буфера для c0, M на 3 подпрохода, пак) не сделано.
**NR-TRAFFIC раунд 2 (19.11, `dos2-nmem`):** (1) порядок трекера «штамп → защита до полной RO → чтение» — **по умолчанию** на всех путях
(vcache, проверка / tracked, заливка частичных текстур, NR-06), `DM_DIRTY_PROTECT_ORDER=0` = старое; `t_dirty_order` на настоящем трекере:
старые порядки теряют 20 / 20, новый 0 / 20; модуль `port-common/dirty-order` для D16 / ETR / D3 / TL2 / XC2. (2) `DM_NR_ARENA` (умолч. 0):
stream-арена −64…128 МБ (до 6 чанков на слот навсегда после загрузки → по нужде за 600 подач). (3) синхронизации CPU ↔ GPU за кадр —
устранимых нет (NR-TRAFFIC §11).
**NR-DRIFT раунд 2 (19.11, `dos2-ndrift`):** `DM_DOS2_DRS_SPIKE` (умолч. 0 / 2 счёт / 1) — фильтр одного плохого кадра в контроллере DRS
(пещера `main+0xc05dcc`, Unicorn-тест против C-модели), `3d98963`; аудит переноса (PORTS-AUDIT.md): SH-20 Adreno 8xx (opt-in), `DM_NVN_SYNC_XWAIT`
(умолч. 2 = счёт), источники частот D3 в `[nr] growth` (`87ab61c`, `2c1f042`); остальное есть или не нужно — с причинами.

**NR-LIGHTFUSE (N1 раунд 2, 19.11, ветка `dos2-nshader`, NR-LIGHTFUSE.md):** `DM_NR_LIGHTFUSE` 0 / 2 проверка / 1 / 3 яд — DeferredSky → ClusteredLighting
одним draw: проход Sky начинается и чистит как раньше, его draw идёт с rasterizer discard, draw Light — слитый шейдер `f.ClusteredLightingWithSSAO`
(Sky внутри, ресурсы Sky из тайника на запасных слотах). Доказательство на интерпретаторе: 400 / 400 бит в бит против Sky → R11G11B10F (RNE) →
Light → ONE / ONE, контроль 303 / 303; Adreno 1666 слотов против 300 + 1434, G57 19.75 против 20.6. Бэкенд NR-ONCHIP при нём выключен (метки
берёт), NR-GMEM держит оба прохода в sysmem. Оценка Mi 14 −0.4…0.6 мс GPU. Умолчание 0, на телефоне не запускалось; нужен setup заново (пак).
**NR-POSONLY (19.11, `dos2-ntrans`, NR-POSONLY.md):** поток «только позиция» для depth-only draw статических мешей. Замер (play-1118): 69.8 % вершин
depth-проходов подходят (FS без kill → пустой двойник NR-33B, VS доказан `xbg_spirv_posonly`; 31 из 57 depth-VS, по паку 1 844 из 6 132, интерпретатор
опроверг 0), байты выборки 1.76× меньше. `DM_NR_POSONLY` 0 / 2 счёт / 1 (копия в одной арене, конвейер с одной привязкой в полосе и старом пути, годность по
жетону трекера, сверка 1 из 64). Ожидание −0.25…−0.5 мс GPU в ходьбе Mi 14 [м], слабые −0.5…−1.5. Тест годности нашёл гонку `protect` vs BUSY —
**та же есть у статических копий NR-06 (Mali / PowerVR)**. На телефоне не запускалось.
**S2F исполнение (N3 раунд 4, NR-FRAME §10):** `DM_NR_ONCHIP` (умолч. 0) + `DM_NR_ONCHIP_FOG` (1 внутри, LIGHTFUSE побеждает): M = {Sp, Df, c0}, 3 подпрохода (Sky / Light + входы c0, Sp / Fog + вход Df),
c0 из заголовка Sky (тап), очистка c0 в подпроходе 2; пак: `5.*` необязательные, setup принимает с ними и без. nb_onchip_test 108 / 0 (дампы 34 / 34 с хвостом), nrpack_optional_test,
nr_gmem_test 14016, source_structure PASS; APK собран; на телефоне не запускалось (F2 / F3 / F1).

**NR-LIGHTFUSE раунд 3 (19.11, `dos2-nshader`, NR-LIGHTFUSE.md §9):** `DM_NR_LIGHTFUSE_FOG` 0 / 2 / 1 — DeferredFog внутри слитого draw: туман вторым
выходом в Sp (свободен в слитой цепочке), draw Fog пропускается, читатели цели Fog (по дампам 34 / 34 — только FadeBlending) до её следующей записи
сэмплируют Sp. Провал проверки Fog → Fog рисуется как записан (точно). Интерпретатор 400 / 400 бит в бит, контроль 335. Пак-варианты `f.` / `g.`
теперь необязательные (setup проходит с ними и без). Оценка Mi 14 ещё −0.15…0.25 мс. EoCFog → F1A, Bloom-мипы, проход Sky без начала — не сделаны.

**N7 NR-PRESKIN (19.11, `dos2-npreskin`, NR-PRESKIN.md):** предскиннинг для слабых Mali / PowerVR. Скиннинговый VS игры режется минимальным
разрезом графа значений (`xbg_spirv_preskin`): слайс (те же инструкции, пишут сырые слова позиции / нормали после костей) раз на экземпляр меша
и подачу — вершинный draw без растеризации в прологе подачи (своего барьера нет: основной буфер слота начинается с ALL → ALL); потребитель
читает запись uint-атрибутами (кости, веса, мировая матрица уходят из VS). Разрез есть у всех 2 001 скиннинговых VS пака (3 слова depth / 12
G-buffer), интерпретатор: 0 расхождений из 12 406 + 7 204 (чужая запись) прогонов на 3 цепочках загрузки, spirv-val 0. malioc G57: depth-VS
18 + 19.5 → 4 + 5.5 цикла, G-buffer 18 + 27 → 6 + 13, слайс 18–21 → Форт ≈ −1…−2 мс на G57 MC2 [м] (с пре-пассом NR-30, снятым на тайлерах по умолчанию; без него −1.3…−2.6), TUT ≈ −2.6 верх; Mi 14 0 (выключено).
`DM_NR_PRESKIN` 0 везде / 2 счёт / 1 / 3 яд, `_MAXM` / `_MAXV` / `_MB`. На телефоне не запускалось.
**NR-ZERO Z3 (19.11 вечер, ветка `dos2-z3`, NR-ZERO.md §9–§11):** `DM_NR_Z3` 0 умолч. / 2 сверка / 1 — «тот же draw, что уже разрешён»: одна таблица от слов, которые читают три поиска короткого пути (5 шейдеров, фиксированные слова NR-08, хэндлы текстур / сэмплеров, key id, вариант, id групп состояния, определение прохода), к их ответам (план, номер записи NR-08, запись таблицы NR-09); штампы — поколение номеров NR-08, очистки таблицы NR-09, эпоха объектов. Всё, что зависит от момента (смещения блоков через `nrbuf::ubo`, образы через `nr_dimg_skip` / `prepare_read`, набор, проверка прохода, виды вершин, viewport, кэш привязок, `vkCmd*`), считается тем же кодом: `lane()` разрезана на разрешение и общий хвост `lane_tail`, `nrz3` зовёт тот же хвост. Замер-основа (главный агент, `Z_1a` / `Z_1b`): NR-08 1 217–1 374 нс (образы 495–621, поиск 342–369), полоса 1 270–1 286 нс, `nvn-translate` 7.0 мс. Новые строки: `[nr] z3`, `[nr] z3 by part`, `[nr] z3 check`, и `[nr] draw span` (при любом режиме — число, которое сравнивает 0 и 1). Хост: `nr_z3_test` 4 754 420 PASS, мутации 11 из 11 ловятся; `source_structure` (блок Z3 + разрез полосы) PASS; `nr_desc_test`, `nr_draw_test`, `nr_ui_test`, `safe_mode_test` PASS. Ожидание −0.4…0.9 мс транслятора на Mi 14 [м], слабые −1…2.5; FPS Mi 14 0. **Z5 (`DM_NR_Z5`) не построен** — проект и 7 блокирующих фактов в §10, 26–38 агент-часов и 5–7 прогонов (сначала Z3b «слово образов», Z3c «виды меша», Z3d). Безопасный режим: ступень 2 гасит оба ключа. APK SHA-256 `fe5c5c718c42fb137265477e3a4e6f5bb5dd0b6fa2ba67e717def53707510fc4`; на телефоне не запускалось (прогоны §11: Z1a, Z3v-s, Z3v-w, затем ABBA).
**NR-ZERO Z3 при PRESKIN, Z3b / Z3c / Z3d, сторона пакетов Z5 (ночь 19→20.11, ветка `dos2-z5`, NR-ZERO.md §13–§16):** (1) Z3 больше не пуст на тайлерах: `nrps::decide` зовётся из `nrz3::draw` на своём месте, скиннинговый draw при `DM_NR_PRESKIN=1` получает от таблицы дескрипторы и проход и идёт в саму полосу (вариант с битами PRESKIN ищет она); ONCHIP, погашенный LIGHTFUSE, воротами не был (`nroc::mode 0`). (2) `DM_NR_Z3B` — слово образов: запись NR-08, чей обход нашёл все образы тихими, не обходится, пока стоят эпоха образов (двигается **типом** семи полей `Image` и `~Image`), номер подачи, серийный номер слота. `DM_NR_Z3C` — таблица «диапазон вершин / индексов → вид» под эпохой живости кусков zero-copy, версией записи куска, слотом, `hit_pend_gen` (или: эпоха снимков, `nrbuf::st_ep`, `hit_pend_gen` для статических копий NR-06). `DM_NR_Z3D` — блок кольца на месте одним словом (один живой кусок над кольцом, нет липких страниц); на зеркале (Mali / PowerVR) выключает себя. (3) Z5: `nbmtq::Queue::splice` (кусок на позиции записи), сборщик серии `nr_z5.h` (голова серии — транслятору, хвост можно писать заранее), `DM_NR_Z5=2` — сборщик против пакетов хвоста полосы пакет в пакет + длины серий; **половина потока игры не построена, `DM_NR_Z5=1` отказывает** (осталось 22–32 агент-часа и 4–6 прогонов, §14.3). Все умолчания 0, ступень 2 безопасного режима гасит. Хост: `nr_z3e_test` 1 493 315 (мутации 23 из 23), `nr_z5_test` 2 565 232 (мутации 14 из 14, журнал вызовов серий байт в байт = сегодняшнему), `source_structure` 70 строк, прежние тесты PASS. Ожидание [м] Z3 + b + c + d: −1.0…1.8 мс транслятора из 7.0 на Mi 14, слабые CPU ×2.5–3; FPS Mi 14 0. APK `026ce4fe…5aed`. На телефоне не запускалось: 10 прогонов по 50 с, §15.
**NR-ZERO: замер Z3 на телефоне, дешёвый ключ Z3K, пакетная половина второго писателя Z5 (20.11 день, ветка `dos2-z6`, NR-ZERO.md §17–§21):** замер главного агента (Mi 14 Ultra, `X_p0` / `X_p1`): все сверки `bad 0`, таблица Z3 отвечает на 99.1 %, но `[nr] draw span` 2 715 → 3 000 нс и выигрыша нет — вопрос к таблице («ворота, ключ и поиск») стоит 734 нс против 385 + 158 у поисков, которые он заменяет (холодные строки кэша: слот плана, план, запись на 448 байт, копия ключа и хэш); Z3c убыточна (виды 473 нс против 326–362) и оставлена 0. (1) `DM_NR_Z3K` 0 умолч. / 2 сверка / 1 — передняя таблица Z3: место поиска — key id из записи (отпечаток потока игры) + слова, что у транслятора в руках, + до трёх хэндлов текстур по подсказке; точность — полный ключ записи сравнивается **на месте** с живыми привязками по строкам плана, которые запись хранит сама (нет поиска плана, копии, хэша); три штампа чистят таблицу; после попадания идут все прежние живые проверки. При `DM_NR_Z3=2` ответ дешёвого ключа — предсказание, которое сверяется с обычным путём. Порядок полей `nrdesc::Plan` / `Ent`: слова обслуженного draw в первой строке. Новые строки `[nr] z3 key`, `[nr] z3 key by part` (цель: «front table» ≤ 150 нс). (2) Z5: запас кусков очереди для второго писателя (`take_spare` / `splice_spare` / `give_back_spare`), протокол серии `nr_z5w.h` (метки записей, кольцо серий, взять только при совпадении кэша привязок и вердикте транслятора, иначе вернуть кусок и записать как сегодня; сверка куска писателя с пакетами транслятора) — проверено на ПК двумя потоками, **в движок не включено, `DM_NR_Z5=1` отказывает**: у потока игры нет значений draw. Осталось 28–39 агент-часов и 5–7 прогонов (§18.3: свёртка состояния на потоке игры, пролог `draw()`, публикация таблиц, вердикт на серию, учёт пакетом). Все умолчания 0, ступень 2 безопасного режима гасит `DM_NR_Z3K`. Хост: `nr_z3k_test` 11.5 млн проверок (мутации 16 из 16), `nr_z5w_test` (журнал вызовов байт в байт = сегодняшнему, мутации 8 из 8, ThreadSanitizer без находок), `source_structure` 72 строки, прежние тесты PASS. Ожидание [м]: `draw span` 2 500–2 800 нс при базе 2 715 (транслятор −0…0.25 мс на Mi 14; слабые CPU ×2.5–3), FPS Mi 14 0; рычаг > 1 мс — только Z5 (модель: транслятор −3…4.5 мс, поток игры +1.5…2.5). APK `4d1453cf…00cc`. На телефоне не запускалось: 5 прогонов по 50 с, §20.
