# DOS2: depth pre-pass ("FadePattern(depth)") — разбор и `DM_DOS2_PREPASS`

Ветка `dos2-prepass` (worktree `wt-prepass`). Данные: `work/logs/gp1.log` (Mi 14 Ultra, Adreno 750, Turnip 27, `TU_DEBUG=sysmem`,
3D 2160x972, окно 2400x1080, `DM_GFX_PASS_PROF=300,180,0`), `python tools/dos2_gpu_breakdown.py work/logs/gp1.log --gpass`.

## 1. Что в проходе

| проход (3D-меню, кадры 661–1561) | draw | indices×inst | GPU мс |
|---|---|---|---|
| i=4 depth-only 2160x972 fmt 0x81 (**pre-pass**) | 526 | 1 854 249 | 2.77 / 3.87 (два уровня, чередуются) |
| i=5 G-buffer 5 MRT, тот же depth | 529 | 1 858 380 | 1.43 (стабильно) |
| i=8 shadow atlas 1536² fmt 0x7c | 1116 | 5 650 290 | 2.73 (один кадр 3.90) |

- Имя «FadePattern(depth)» — это только **первый** draw прохода (`mixed=1`, последний — `TR_Layer_PBR_ST_DEP_NX`). Проход — обычный
  depth-only pre-pass по материалам: каждая модель рисуется своей программой `*_DEP_NX.bshd` (jobs.tsv), G-buffer — `*_DEF_NX.bshd`.
  Геометрия pre-pass и G-buffer совпадает (526 vs 529 draw, 1.854 M vs 1.858 M индексов). Geometry shader нет (programs.tsv: gs = «-»).
- `FadePattern` PS (`9c8e09d06f456fdf`): шахматка `((int)(x*c15[13].z) + (int)(y*c15[13].w)) & 1 → OpKill`, без текстур — дизеринг
  исчезающих объектов (то, что заслоняет камеру). Этот draw рисует объект вместо его DEP-программы; G-buffer того объекта потом с EQUAL
  проходит только в «нечётных» клетках — **прозрачность фейда держится на EQUAL против глубины pre-pass**.
- Перепись всех 2001 DEP-программ (транслированные `work/shaders-vk/*.xgs`, OpKill/Demote/Terminate по SPIR-V; 436 разных модулей):

  | DEP FS | DEF FS | материалов | примеры |
  |---|---|---|---|
  | kill | **без kill** | 1056 | AlphaTest-растения, деревья (`ATM_Wind_Tree_*`), `TR_Layer_*`, AlphaBlend |
  | kill | kill | 624 | декали, облака/FX |
  | **пустой main** (нет kill / выходов / текстур) | без kill | 315 | `*_Opaque`, PBR-опак; один модуль `97c653b5` на 276 материалов |
  | без kill | kill | 6 | FX-частицы |

  Т.е. у 1056 материалов вырез (alpha test, слои террейна, фейд) делается **только в pre-pass**, а G-buffer полагается на EQUAL.
  Значит, вариант «просто выкинуть pre-pass + LESS_EQUAL в G-buffer» (как iPad, где Larian переписали материалы под HSR) ломает
  растительность, деревья и фейд — у нас DEF-шейдеры без alpha test.
- Почему pre-pass вдвое дороже G-buffer при той же геометрии: в G-buffer EQUAL + FS без kill → early-Z отбрасывает всё невидимое; в
  pre-pass у большей части материалов FS с kill + запись глубины → late-Z, FS (1–2 выборки текстуры) на каждый фрагмент с overdraw, LRZ
  без записи (FN-37 `mobile-gpu-field-notes.md`: discard гасит LRZ-write). Опак-DEP (пустой main) дешёвые: early-Z, только вершины.
- Глубина и функции: в pre-pass дефолт движка — test+write (`ds_st` из `hle_nvn.c`), в G-buffer — EQUAL (вывод из шейдеров выше:
  иначе вырезы бы не работали). Пер-draw значения на телефоне ещё не сняты — их даёт строка `NVN PREPASS` (`DM_DOS2_PREPASS=2`).
- Кто читает глубину pre-pass до G-buffer: никто — i=4 и i=5 идут подряд на одном depth (GPASS i=4 → i=5). Occlusion-запросы (90–99
  условных draw/кадр в t1.log): draw с `count_samples` в pre-pass не трогаются (причина `counting`); если запросы стоят после опак-draw в
  pre-pass, при пропуске опака они видят меньше окклюзии → только меньше отсечения, не ошибка картинки.

## 2. Что сделано: `DM_DOS2_PREPASS` (по умолчанию 0)

Логика — чистый C `dm/gfx/vulkan/engine/xbg_prepass.h`, хост-тест `tests/prepass_test.c`
(`clang -std=c11 -Wall -Wextra prepass_test.c`: классификация, учёт, модель глубины сцены — бит в бит та же глубина, тот же победитель
на пиксель для опака / выреза листвы / фейд-шахматки / базы террейна + слоя на одном меше / одного меша дважды). Вход — `xbg_game.cpp`
`namespace prepass`, вызов в начале `draw()` сразу после `condr::skip_draw()`; `Shader::empty_fs` считается при загрузке модуля.

- **=1 гибрид.** В depth-only проходе на сцене (нет цветных целей, depth ≥ 640x360 и не квадрат — тени 1536² и портреты 80x100 не
  трогаются) draw **не рисуется**, если: FS пустой (одна функция, только OpLabel/OpReturn) или его нет; depth test+write; stencil test
  выключен; alpha test выключен; нет `count_samples`; indexed (не GPU-indirect); нет rasterizer discard; G-buffer на этом depth в кадре
  ещё не начался. Ключ геометрии = (index buffer, first, count, base vertex, instances, base instance), счётчик кратности.
  Потом в проходе с цветными целями на **том же** depth (по serial) первые N draw с этим ключом (N = сколько пропущено) и функцией
  EQUAL/LEQUAL получают **LEQUAL + depth write** на один draw (RAII `prepass::Guard` возвращает `state.ds`; memo пайплайнов ключуется
  сырым `state.ds`, так что вариант кэшируется отдельно). Всё с kill (листва, деревья, слои, фейд) остаётся в pre-pass, их G-buffer — EQUAL.
  Таблица 4096 ключей на кадр; полная таблица = draw остаётся (никогда не пропуск без ключа).
- **=2 счёт.** Решения как у 1, ничего не меняется (картинка = 0), та же строка. Сначала гонять его — он даёт недостающие факты.
- Строка (каждые `DM_DOS2_PREPASS_LOG` кадров, 300):
  `NVN PREPASS mode= frame= pre_draws= skip= pre_verts= skip_verts= keep_verts= keep kill= fs= state= stencil= alpha= counting= notindexed= late= full= | gbuffer draws= dw= df never..always= override= refused= unmatched= ambiguous= keys=`.
  - `skip / skip_verts` — доля опака: это и есть верхняя граница выигрыша (вершины + запись глубины опака).
  - `df equal=` vs `lequal=` и `dw=` — какую функцию реально ставит G-buffer (проверка допущения).
  - `refused` — ключ найден, но у G-buffer-draw функция не EQUAL/LEQUAL (LESS / GREATER): такой опак в режиме 1 потеряет глубину.
  - `unmatched` — пропущенные draw, которым G-buffer так и не нашёлся (геометрия только для глубины: невидимые окклюдеры и т.п.):
    в режиме 1 их глубины нет. Если ≠ 0 — смотреть картинку/туман/SSAO в этих местах.
  - `ambiguous` — ключ и пропущен, и оставлен (тот же меш опак и с фейдом в одном кадре): порядок G-buffer решает, кто получит LEQUAL.

### Риски режима 1 (решает A/B на телефоне)
- **Overdraw G-buffer.** Опак в G-buffer теперь LEQUAL+write: early-Z отсекает только то, что уже нарисовано ближе; если игра рисует не
  спереди назад, 5-MRT FS опака выполнится больше раз. Это может съесть выигрыш pre-pass (iPad обходится HSR, у Adreno sysmem его нет).
- Фейд того же меша, что и опак рядом (`ambiguous`), объекты только-в-pre-pass (`unmatched`), декали, нарисованные в G-buffer **раньше**
  опака под ними (видят неполную глубину) — проверять глазами: меню, лагерь у Форта Радость, лес/листва, исчезающие стены при наведении камеры.
- Не работает через путь `DM_NB_*` hit (он рисует мимо `draw()` только при включённом NB; по умолчанию выкл.).

### Ожидание
Модель по GPASS: G-buffer той же геометрии стоит 1.43 мс → вершинная часть ≤ ~0.75 мс на 1.85 М индексов. Опак-доля в меню неизвестна
(узнаем `skip_verts`); если ~40 % индексов — минус ~0.3–0.6 мс pre-pass, плюс неизвестный overdraw G-buffer. Дорогая часть pre-pass
(2.8–3.9 мс) — это **kill-материалы**, и этот режим её не трогает. Честно: ожидаемый выигрыш 0…0.6 мс, может быть отрицательным.

## 3. Что не сделано и почему
- **(a) нативный мобильный FS для pre-pass с «быстрым путём без фейда».** 2001 DEP-программа / 436 разных модулей, kill у 1680, и он
  не только от фейда: alpha test по текстуре (листва, `fp_c3.x` — режим/порог материала у деревьев), маска слоя террейна. Чтобы
  early-Z заработал, kill должен исчезнуть из модуля, т.е. хост должен по значениям cbuffer на каждый draw выбирать вариант без kill —
  это возможно только там, где kill зависит лишь от uniform (`FadePattern` — нет: шахматка всегда режет; фейд-ветки внутри материалов
  надо искать по модулям). Метод (`dos2_post_mobile.py` + `spv_interp.py`) годится на 1–5 модулей, не на 436. Следующий шаг, если
  `NVN PREPASS keep kill=` окажется основной массой: снять на телефоне топ DEP-модулей по draw (дамп кадра: пустой файл `gfx-dump-now` в папке над
  `dm/shaders` → `dump/draws.txt`, строки `DRAW … ps= … depth=` прохода без цветных целей) и для 3–5 самых частых проверить, зависит ли kill только от cbuffer.
- **Перестановка draw внутри pre-pass** (опак-пустые первыми, kill-материалы после): для depth-only прохода с LESS/LEQUAL без stencil
  результат = min по фрагментам, т.е. порядок не влияет на глубину бит в бит, а kill-фрагменты за опаком отсекаются early-тестом.
  Требует отложенной записи draw (хост пишет команды сразу, снимки буферов на draw) — больше часа, кандидат на NB-план (`DM_NB`),
  где draw уже лежат списком.

## 4. Turnip / LRZ (вариант c)
- KB: FN-37 (LRZ гаснет на blend / stencil / A2C / discard / colour mask / смене направления depth / ALWAYS / NOT_EQUAL с записью до
  следующего clear); ADR-05 (`gpu-adreno-stock-vs-turnip.md`); с `TU_DEBUG=sysmem` load/store не стоят ничего, но LRZ в sysmem у a7xx есть.
- В pre-pass LRZ-write выключается на каждом kill-draw; при чередовании опак/kill (порядок движка: FadePattern первым, слои террейна
  последними) LRZ почти не наполняется. G-buffer — отдельный проход (i=5), EQUAL: LRZ там бесполезен, early-Z работает и так.
- Дешёвая проверка на телефоне — две строки env (Turnip читает при загрузке драйвера, нужен перезапуск):
  - `TU_DEBUG=sysmem,nolrz` против базы `TU_DEBUG=sysmem`: если i=4 не меняется — LRZ в pre-pass и так не работает.
  - `TU_DEBUG=sysmem,perf` — Turnip пишет в logcat причины отключения LRZ (`perf_debug`), искать `LRZ` в логе (один короткий прогон).
  - (по желанию) `TU_DEBUG=gmem`: в GMEM LRZ строится биннингом; KB ADR-01 — на 750 sysmem и gmem были равны.

## 5. Тени (только заметка)
- 1116 draw, 5.65 М индексов (3× pre-pass) за 2.73 мс ≈ 0.48 мс на М индексов — тот же темп, что вершинная часть G-buffer (≤0.77):
  проход **вершинно/draw-bound**, а не заливка (атлас 1536² ≈ кадру по пикселям, FS — DEP с kill только у листвы).
- `DM_DOS2_SHADOWS=2` (3×512) режет только заливку (0.44× пикселей) — ждать мало. `DM_DOS2_SHADOWS=0` (2×1024) — **на треть меньше
  draw** (2 каскада вместо 3) при большем атласе: при вершинной границе может быть дешевле, проверить A/B вместе с резкостью.
- Отсечение draw по каскадам игра делает сама (≈372 draw на каскад); от хоста — только `DM_GFX_COND_RENDER` (occlusion), к теням не относится.

## 6. A/B на телефоне (по одному запуску, 3D-меню и кампания)
```
# база (как gp1)
DM_GFX_PASS_PROF=300,180,0
# A: факты, картинка не меняется
DM_GFX_PASS_PROF=300,180,0  DM_DOS2_PREPASS=2
# B: гибрид
DM_GFX_PASS_PROF=300,180,0  DM_DOS2_PREPASS=1
# C: LRZ (отдельно, перезапуск)
TU_DEBUG=sysmem,nolrz  DM_GFX_PASS_PROF=300,180,0
TU_DEBUG=sysmem,perf   DM_GFX_PASS_PROF=300,180,0
# D: тени
DM_DOS2_SHADOWS=0  DM_GFX_PASS_PROF=300,180,0
```
Смотреть: `python tools/dos2_gpu_breakdown.py <log> --gpass` (prepass / gbuffer / shadow группы, сумма), строки `NVN PREPASS`
(`skip_verts`, `df`, `unmatched`, `refused`), скриншоты листвы / фейда стен / террейна. B принимать, только если prepass+gbuffer
вместе меньше базы и `unmatched=0`, `refused=0`.
