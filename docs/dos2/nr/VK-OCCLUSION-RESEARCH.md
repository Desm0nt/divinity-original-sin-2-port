# Occlusion culling на мобильных GPU — обзор для DOS2 (Vulkan backend)

Контекст: изометрический рендерер DOS2 (depth pre-pass + G-buffer + clustered lighting), Adreno 750 /
Turnip. Oracle-тест говорит, что 410 из ~683 draw'ов depth/G-buffer на кадр реально скрыты, но обе
наши реализации occlusion culling (A — CPU-reuse прошлого вердикта с доказательством неизменности
экранного прямоугольника; B — grouped queries + `VK_EXT_conditional_rendering`, ~40 запросов/8
условий) оказались медленнее, чем просто рисовать всё. Простые per-object query + conditional
rendering тоже проиграли (48 против 54 fps). CPU record thread — боттлнек (~13 мс/кадр), GPU ~12.7 мс.
Ниже — что реально умеют мобильные игры/движки и почему.

Это документ для передачи другому агенту (Opus) на реализацию — поэтому выводы сформулированы
конкретно, а не «можно попробовать то или это».

---

## 1. Ранжированный шорт-лист — что реально пробовать в DOS2

### #1. Precomputed PVS / cell-based visibility для статической геометрии города (приоритет №1)
Для каждой "ячейки" карты (или даже для фиксированных ракурсов камеры — у изо-камеры их по факту
немного, вращение на 90°/фиксированный угол наклона) офлайн считается набор видимых статических
объектов, как в Quake-PVS или в кастомном PVS для Republique VR (там для 53 фиксированных позиций
камеры в сцене из ~4000 рендереров строился per-camera PVS через cubemap capture с цветовой
кодировкой id объекта). Runtime-стоимость — только lookup по текущей ячейке/камере и enable/disable,
без единого draw call на тест видимости, без GPU round-trip вообще.
- **CPU/GPU cost**: ~0 во время кадра (один lookup по словарю), вся стоимость — offline bake.
- **Точность**: консервативная (PVS может завышать видимость), но без popping — переходы между
  ячейками делаются с запасом, как у Quake. Разрывов изображения не даёт.
- **Какие GPU выигрывают**: все — Adreno/Mali/PowerVR/Xclipse, т.к. это чистая CPU-логика перед
  записью команд, никаких query/conditional rendering/compute.
- **Минусы**: требует перебейка PVS при правке уровня; не покрывает динамические объекты (NPC,
  подвижные предметы, двери) — для них нужен separate fallback (манекен-occluders, пункт #2).
- **Источники**: Republique VR custom PVS — https://developers.meta.com/horizon/blog/occlusion-culling-for-mobile-vr-developing-a-custom-solution/ ;
  общее PVS/cell-portal culling — https://archive.gamedev.net/archive/reference/programming/features/culling/page2.html ,
  https://www.cs.princeton.edu/courses/archive/spr01/cs598b/papers/teller91.pdf ;
  UE Precomputed Visibility Volumes (офлайн бейк, рекомендован именно для мобильных и low-end) —
  https://dev.epicgames.com/documentation/en-us/unreal-engine/precomputed-visibility-volumes-in-unreal-engine .

### #2. Ручные/упрощённые occluder-объёмы + CPU-софт-растеризация (software occlusion без GPU round-trip)
Для динамических объектов и там, где PVS не подходит — классический Intel Masked Software Occlusion
Culling (растеризация низкополигональных occluder-мешей в маску глубины на CPU, SIMD), адаптированный
на NEON. Тест видимости — чисто CPU, перед command buffer recording, без единого обращения к GPU.
- **CPU/GPU cost**: CPU — несколько сотен occluder-треугольников на кадр при хорошей SIMD-реализации
  (оригинал Intel рассчитан на SSE/AVX; прямого ARM NEON порта от Intel нет, нужен самостоятельный
  порт через `sse2neon` + ручная переработка `shuffle`/`movemask`, у которых нет прямого аналога в
  NEON). GPU cost — 0, никаких query.
- **Точность**: зависит от разрешения маски и качества occluder-мешей; возможны редкие false positives
  на границах (решается запасом/consertivism), popping не характерен, если occluder-меши консервативны.
- **Какие GPU выигрывают**: все одинаково — это чисто CPU-технология, не зависящая от драйвера/GPU.
- **Источники**: Intel Masked Software Occlusion Culling — https://intel.com/content/www/us/en/developer/articles/technical/masked-software-occlusion-culling.html ,
  алгоритм — https://fileadmin.cs.lth.se/graphics/research/papers/2016/culling/ ;
  попытка ARM-порта похожей SSE-culling библиотеки на NEON (Unity Burst) с конкретными проблемами
  (`shuffle`/`movemask` без NEON-аналога) — https://over17.github.io/2025/10/01/burst-occlusion-culling.html ;
  ручные occluder-боксы (Stingray/Unity OcclusionArea) как источник геометрии для occluder-мешей —
  https://help.autodesk.com/cloudhelp/ENU/Stingray-Help/stingray_help/building_levels/create_game_objects/create_occluders.html ,
  https://docs.unity3d.com/Manual/class-OcclusionArea.html ; Godot делает то же самое (растеризует
  simplified occluder mesh в low-res CPU буфер) — https://docs.godotengine.org/en/4.7/tutorials/3d/occlusion_culling.html .

### #3. Снижение CPU-стоимости самой отправки draw call (batching / GPU-driven indirect), независимо от culling
Это отдельная ось атаки на реальный боттлнек (CPU record thread 13 мс). Com2uS (Summoners War:
Chronicles) на Vulkan/Android сделал систему batching "Clay": compute shader пишет per-object данные
в SSBO, рендер идёт через instanced indirect draw, в идеальном случае — до 7 draw call на всю сцену
вместо сотен; заявленный прирост производительности 30%. Даже без occlusion culling это снижает
стоимость оставшихся 683 draw'ов.
- **CPU/GPU cost**: CPU — резко ниже (один-два indirect draw вместо сотен vkCmdDrawIndexed), GPU —
  немного выше (compute pass для подготовки SSBO), но это именно то, что снимает сам боттлнек.
- **Точность**: не относится к occlusion — это оптимизация отправки, не меняет what is drawn.
- **Какие GPU выигрывают**: Adreno и большинство Mali — multi-draw-indirect/`vkCmdDrawIndirectCount`
  на Mali уверенно поддерживается только начиная с Mali-G710+ (Valhall CSF); на более старых Mali
  `drawCount` должен быть 1, т.е. нужен path с одиночными indirect draw. На Adreno indirect count
  поддерживается, но по одному форумному тесту Qualcomm (Adreno 610, Android 10) throughput indirect
  count был на уровне одного indirect draw — то есть выигрыш от count-варианта не гарантирован, но
  обычный instanced/indirect draw всё равно резко дешевле CPU-side, чем множество прямых draw call.
- **Источники**: Com2uS Clay — https://developer.android.com/stories/games/com2us-vulkan ;
  Mali G710 indirect draw/`vkCmdDrawIndirectCount` — https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-g710-developer-overview ;
  ограничение `drawCount<=1` на пред-G710 Mali (форум, не официальный докс, но согласуется с
  вышеуказанным постом) — https://community.arm.com/support-forums/f/graphics-gaming-and-vr-forum/49770/do-we-need-to-repack-our-vertex-buffers-for-mali-g76-to-avoid-vk_device_lost ;
  слабый throughput indirect count на Adreno 610 — https://developer.qualcomm.com/comment/17943 ;
  Mali CSF vs Job Manager снижение CPU-стоимости per-draw — https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-g710-developer-overview .

### #4. Опираться на аппаратное hidden-surface removal (FPK/Early-ZS/LRZ/HSR), не дублировать его софтом
Главный вывод из разбора архитектур: на Adreno (LRZ), Mali (Forward Pixel Kill + Early-ZS) и PowerVR
(полноценный tile-based HSR до шейдинга) скрытые **пиксели** уже дёшево отбрасываются самим GPU без
участия приложения. Software/CPU occlusion queries экономят не пиксельную закраску (она и так почти
бесплатна), а **vertex processing + сам draw call submission + geometry setup**. Для DOS2 это значит:
сортировка статических occluder'ов (стены, крыши) front-to-back и аккуратный depth pre-pass почти
бесплатно убирают overdraw силами харда; бюджет на явный occlusion culling стоит тратить только там,
где экономится именно CPU draw-call cost (через #1–#3), а не пытаться выжать дополнительную пиксельную
экономию через query.
- **CPU/GPU cost**: 0 дополнительных затрат — это "бесплатная" часть архитектуры, которую нужно не
  ломать (не включать late-Z, blending, discard без необходимости — все эти вещи отключают LRZ/FPK/
  Early-ZS/HSR).
- **Точность**: точная (это honest per-pixel/per-tile depth test), без popping.
- **Какие GPU выигрывают**: Adreno (LRZ) — https://blogs.igalia.com/dpiliaiev/adreno-lrz/ ,
  https://docs.mesa3d.org/drivers/freedreno/hw/lrz.html ; Mali (FPK/Early-ZS) —
  https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/hidden-surface-graphics-removal-methods-in-mali-gpus ;
  PowerVR (TBDR HSR, "the biggest efficiency saving in the architecture") —
  https://blog.imaginationtech.com/the-dr-in-tbdr-deferred-rendering-in-rogue/ .
- **Важно для DOS2**: blending/discard/запись depth в фрагментном шейдере отключают LRZ (Adreno) и
  late-Z path на Mali/PowerVR — если в G-buffer pass есть alpha-test листва/трава, она ломает HW
  occlusion на всех трёх архитектурах одинаково; стоит отделить alpha-test геометрию в отдельный
  под-пасс, чтобы не убивать LRZ/FPK для основной непрозрачной массы.

### #5. Two-phase GPU-driven HZB occlusion (Aaltonen/Haar) — опционально, только high-end путь
Классическая схема: первый проход рисует то, что было видно в прошлом кадре; строится HZB; второй
проход через compute culling против HZB докидывает disoccluded объекты. Экономит именно vertex/draw
cost, не требует query readback на CPU (все решения принимаются на GPU через indirect draw). Но на
мобильных это требует compute + `vkCmdDrawIndexedIndirectCount` + разрыв render pass на tiler'е, что
дорого (depth attachment round-trip на подхват после compute-прохода — по данным доклада Vulkanised
2026, > 0.5 мс только на resume renderpass). Разумно рассматривать только для Adreno 7xx (где LRZ/tiling
management достаточно развиты) и для новых Mali (G710+/Immortalis), с явным fallback на #1–#2 для
старого Mali/PowerVR.
- **CPU/GPU cost**: CPU почти 0 (вся работа на GPU), но GPU cost растёт — compute pass + лишний
  renderpass break (>0.5 мс замечено на мобильном tiler'е для подобного паттерна).
- **Точность**: точная (HZB почти полностью консервативен, редкие false negative на диагоналях).
- **Какие GPU выигрывают**: в первую очередь десктоп-класс; на мобильных — только GPU с нормальной
  поддержкой indirect count и дешёвым resume renderpass (вероятно новые Adreno 7xx; Mali pre-G710
  не подходит из-за `drawCount<=1`).
- **Источники**: оригинальная техника — Haar/Aaltonen, SIGGRAPH 2015 Advances course —
  https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf ;
  практическое описание двух фаз — https://hannomalie.github.io/posts/2018-02-07-culling-part-2.html ;
  стоимость resume renderpass на мобильном tiler'е (Vulkanised 2026, Lucas Silva/DevSh) —
  https://vulkan.org/user/pages/09.events/vulkanised-2026/1415-Lucas-Silva-DevshGraphicsProgramming.pptx.pdf
  (страница недоступна напрямую через автоматический fetch — 403; цитата взята из search snippet,
  рекомендуется скачать и открыть вручную перед реализацией).

### #6. Temporal reuse прошлого вердикта occlusion query с доказательством (наша техника A) — оставить только как добавку, не как основную технику
Общая идея temporal coherence (CHC/CHC++, Mattausch) и depth-warping reprojection (Lee/Kim/Eisemann,
ACM TOG 2018) показывает, что reuse предыдущего кадра работает, но требует явного fallback на
disoccluded-регионы и аккуратного proof — ровно то, что мы и делали. Мы уже измерили, что для DOS2 на
Adreno 750 чистый CPU-overhead ведения истории + доказательства "экран не изменился" перевешивает
экономию на ~683 draw. Технику можно оставить как **дополнительный фильтр поверх #1/#2** (например,
раз в N кадров подтверждать PVS-решение через дешёвый temporal-тест), но не как самостоятельный
механизм — независимые источники не описывают её как основной инструмент на мобильных, только как
слой поверх HZB/query.
- **Источники**: CHC/CHC++ (Mattausch) — https://www.cg.tuwien.ac.at/research/publications/2010/scherzer2010d/scherzer2010d-paper.pdf ;
  depth-warping reprojection occlusion (Lee, Kim, Eisemann) —
  https://publications.graphics.tudelft.nl/rails/active_storage/blobs/redirect/eyJfcmFpbHMiOnsibWVzc2FnZSI6IkJBaHBBdVVJIiwiZXhwIjpudWxsLCJwdXIiOiJibG9iX2lkIn19--bcd9fb883c9556c558df49afa9509c785e209c58/SYMNSWE12a.pdf ;
  предупреждение про disocclusion-артефакты — https://community.khronos.org/t/fragment-shader-occlusion-culling/70545 .

### #7. Явно отказаться от per-object/grouped HW occlusion query + `VK_EXT_conditional_rendering` как основного пути
И наши измерения (48 vs 54 fps для простых query, A/B медленнее "рисовать всё"), и внешние источники
сходятся: hardware occlusion query на мобильных рассчитаны на batched "direct mode" с задержкой
результата ~3 кадра и лимитом ≤512 активных запросов; Qualcomm прямо пишет, что при неверном паттерне
использования query driver остаётся в "binned"-режиме с overhead 20–40% `CP Busy` вместо 4–6% в direct
mode. `VK_EXT_conditional_rendering` на Turnip имеет known firmware-зависимый баг (конкретные dEQP-кейсы
ломаются на одних версиях firmware и работают на других) с fallback на system-memory path — то есть
поведение нестабильно между устройствами. На Mali поддержка query/conditional rendering документирована
хуже, а multi-draw-indirect ограничен `drawCount<=1` на большинстве используемых сейчас чипов.
- **Источники**: Qualcomm occlusion query best practices (direct mode, batching, CP Busy 20-40%→4-6%,
  ≤512 active, ~3 frame latency) — https://developer.qualcomm.com/sites/default/files/docs/adreno-gpu/snapdragon-game-toolkit/gdg/gpu/best_practices_other.html
  и FAQ — https://developer.qualcomm.com/sites/default/files/docs/adreno-gpu/snapdragon-game-toolkit/gdg/gpu/faq.html ;
  Turnip `VK_EXT_conditional_rendering` firmware-bug + sysmem fallback —
  https://android.googlesource.com/platform/external/mesa3d/+/ee2c58d%5E%21/ ;
  Mali multi-draw-indirect `drawCount<=1` до G710 —
  https://community.arm.com/support-forums/f/graphics-gaming-and-vr-forum/49770/do-we-need-to-repack-our-vertex-buffers-for-mali-g76-to-avoid-vk_device_lost .

---

## 2. Детальные заметки по источникам и техникам

### 2.1. Как культят геометрию крупные мобильные игры и движки

**Genshin Impact (разбор через RenderDoc, мобильная сборка, эмулятор).** Источник:
https://habr.com/en/articles/874866 (русскоязычная статья, подробный разбор захвата). Рендер —
кастомный Scriptable Render Pipeline на Unity, **deferred**: G-buffer (3×RGBA8 + D24S8, стенсил кодирует
траву/крону деревьев/персонажей/прочее), затем deferred lighting по стенсил-маскам, затем forward-слой
(небо, облака, туман, вода, частицы, lens flare). Автор явно отмечает: **depth pre-pass в статье не
упомянут, occlusion culling не обсуждается, а recognizable пример — trample-текстура травы продолжает
рендериться даже в интерьерах/данжах, где она не нужна**. То есть команда miHoYo в первую очередь
налегает на instancing, LOD (трава 48–114 треугольников/группа, до 32 групп на draw call; деревья
1300–3500 треугольников, дальние — инстансированные спрайты) и батчинг render target, а не на
occlusion culling как таковой. Официальная презентация miHoYo на Unity Dojo 2021 подтверждает две
раздельные кастомные пайплайна (PC/консоли и Android/iOS) и прямо говорит, что CPU-side/low-level
оптимизации не покрывались в докладе —
https://es.slideshare.net/UnityTechnologiesJapan/210617-unity-dojo20211mihoyozhenzhongyi .
**Вывод**: даже у топовой мобильной игры основная экономия — LOD/instancing/batching, а не сложный
occlusion culling; это косвенно подтверждает наблюдение владельца, что "дешёвого geometry culling"
в индустрии часто банально нет — вместо этого масштабно вложились в снижение числа/веса draw call.

**Unity URP/HDRP, GPU Resident Drawer + GPU occlusion culling (Unity 6).** Документация:
https://docs.unity3d.com/6000.4/Documentation/Manual/urp/gpu-culling.html . Работает только вместе с
GPU Resident Drawer, компьют-культинг против depth-текстур текущего и предыдущего кадра, представление
объекта — bounding sphere (значит, вытянутые/тонкие объекты культятся хуже). Официально **не
поддерживает OpenGL ES** и требует Render Graph (не Compatibility Mode). Сама документация честно
предупреждает: *"whether GPU occlusion culling speeds up rendering depends on your scene"* и что при
слабом эффекте culling рендер может стать **медленнее** из-за сопутствующей работы GPU — то есть ровно
та же ловушка, в которую попал наш DOS2-эксперимент. Маркетинговая цифра Unity — до 50% экономии CPU
frame time, но явно только для high-end mobile, не low/mid-end. Старый baked Umbra occlusion (CPU-side,
бейкается в билде) требует disk/RAM под occlusion-данные, что на мобильных с unified memory ощутимо.

**Unreal Engine mobile.** Документация:
https://dev.epicgames.com/documentation/en-us/unreal-engine/visibility-and-occlusion-culling-in-unreal-engine
и https://dev.epicgames.com/documentation/en-us/unreal-engine/precomputed-visibility-volumes-in-unreal-engine .
Три механизма: (1) Hardware occlusion queries — результат приходит с опозданием на кадр, может
"попнуть" объект при быстром движении камеры; работает только на части мобильных устройств (iOS ES3.1+,
Android только Vulkan); (2) HZB occlusion — консервативнее (культит меньше), тестирует bounding box
актора против mip-уровней depth; (3) **Precomputed Visibility Volumes** — офлайн-бейк во время lighting
build, Epic прямо пишет: *"ideal for lower-end hardware and mobile devices"*, платит runtime-память за
экономию CPU render thread. Для DOS2 (статичный город, изометрия) это прямой аналог пункта #1 нашего
шорт-листа.

**Larian / Baldur's Gate 3 (движок DOS2/BG3, "Divinity Engine").** GDC-доклад Swen Vincke 2019 —
дизайнерский, не инженерный (https://www.gdcvault.com/play/1025734/contactUs). Инженерной информации
про occlusion culling в их движке в открытом доступе почти нет; известно по докладу их графического
программиста Wannes Vanderstappen (GPC) только то, что рендер поддерживал одновременно DX11 и Vulkan,
Vulkan — "пристройка" под Stadia, у команды было мало опыта с Vulkan, и в следующем движке решили
отказаться от обоих API в пользу нового backend'а. Прямых данных об occlusion culling в их рендерере не
найдено — это признанный gap, не стоит выдавать догадки за факт.

**Случай с фиксированной камерой (Republique VR, Darkwind) — близкий аналог изометрии.**
https://developers.meta.com/horizon/blog/occlusion-culling-for-mobile-vr-developing-a-custom-solution/ .
Команда отказалась от встроенного Unity occlusion culling (слишком дорого на каждый кадр на Gear VR/Go)
и сделала offline PVS per fixed-camera: рендер cubemap с объектами, раскрашенными в уникальные solid
цвета, затем readback и matching пикселей → множество видимых id. Прозрачные объекты обрабатываются
итеративно (чтобы не считать их false occluder'ами). Runtime-стоимость — только enable/disable рендереров
по membership в PVS, один раз при смене камеры, не каждый кадр. Это прямой прецедент того, что мы
предлагаем в пункте #1, просто для игры с дискретными фиксированными камерами вместо изометрической
with ограниченной свободой камеры.

### 2.2. Software/CPU occlusion rasterization

Intel Masked Software Occlusion Culling (Hasselgren/Andersson/Akenine-Möller, HPG 2016) —
https://fileadmin.cs.lth.se/graphics/research/papers/2016/culling/ , код —
https://intel.com/content/www/us/en/developer/articles/technical/masked-software-occlusion-culling.html .
Библиотека рассчитана на SSE/AVX и **не переносится на ARM автоматически**. Независимый порт похожей
SSE-реализации на Unity Burst/NEON (https://over17.github.io/2025/10/01/burst-occlusion-culling.html)
прямо говорит, что пришлось переписывать вручную под NEON через `sse2neon`, а у `shuffle`/`movemask` нет
прямого аналога в NEON — то есть порт возможен, но не "бесплатный", и требует отдельной инженерной
работы, а не просто смены intrinsics-заголовка.

### 2.3. GPU-driven / two-phase occlusion culling

Классика — Haar & Aaltonen, SIGGRAPH 2015 Advances in Real-Time Rendering course
(https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf):
GPU строит depth hierarchy, culling output — список кластеров + аргументы indirect draw. Схема
двухфазная: первый проход рисует объекты, видимые в прошлом кадре; HZB строится по этому проходу;
второй проход тестирует disoccluded объекты против HZB и докидывает их через indirect draw. Для
мобильных это требует (а) compute shader culling, (б) `vkCmdDrawIndexedIndirectCount` (на Mali надёжно
доступен только с G710+, см. ниже), (в) разрыв render pass для подхвата compute-результата — по данным
слайдов Vulkanised 2026 (Lucas Silva, DevSh Graphics Programming) **round-trip через depth attachment
при "resume renderpass" стоит >0.5 мс на мобильном тайлере** — то есть сама механика двухфазного
GPU-driven culling дорога именно на архитектуре tile-based рендеринга, если не продумана очень
аккуратно (нельзя просто взять десктопную схему as-is).

### 2.4. Аппаратное удаление скрытых пикселей — ключевая причина, почему наши query не помогли

- **PowerVR TBDR HSR** — https://blog.imaginationtech.com/the-dr-in-tbdr-deferred-rendering-in-rogue/ :
  вся геометрия тайла накапливается перед растеризацией, ISP тестирует глубину per-pixel **до** запуска
  фрагментного шейдера, и в тайл отправляется только финально видимый набор фрагментов. Автор Imagination
  прямо называет это "the biggest efficiency saving in the architecture" и источником энергоэффективности
  PowerVR. Единственное, что ломает механизм, — прозрачность (требует flush видимых пикселей в шейдер
  для блендинга); alpha-test/discard/blending в общем случае отключают это по тем же причинам, что и на
  других архитектурах (см. ниже, Mozilla GPU wiki: https://wiki.mozilla.org/Platform/GFX/MobileGPUs).

- **Mali Forward Pixel Kill (FPK) + Early-ZS** —
  https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/hidden-surface-graphics-removal-methods-in-mali-gpus :
  FPK — аппаратный механизм, который убивает уже проскочившие в шейдер фрагменты, если они позже
  оказываются перекрыты другим примитивом (между early-Z и шейдерным ядром); отключается при late-Z.
  Early-ZS (depth/stencil тест до шейдера) недоступен, если шейдер делает discard/alpha-to-coverage/
  пишет depth. Blending отключает и FPK, и Early-ZS одновременно. Reordering примитивов в раздатчике
  улучшает culling во время растеризации (но не работает со stencil test).

- **Adreno LRZ (Low Resolution Z)** — https://docs.mesa3d.org/drivers/freedreno/hw/lrz.html ,
  https://blogs.igalia.com/dpiliaiev/adreno-lrz/ : low-res depth строится во время binning pass и
  используется для тайл-уровневого отбрасывания контрибуций до полноразмерного Z-теста; по сути это
  "depth prepass, который сам появляется из порядка независимого рисования". Отключается/инвалидируется:
  смена направления теста глубины без записи depth (временно) или с записью (навсегда до конца кадра),
  `EQUAL`/`NEVER`/`ALWAYS`/`NOT_EQUAL` compare-ops, запись depth во фрагментном шейдере, blending/logic
  ops/colorWriteMask, side-effects (SSBO/image writes → late-Z), discard, secondary command buffers (до
  Adreno 650/Snapdragon 865 — начиная с него есть GPU-side direction tracking). LRZ-состояние можно
  переносить между renderpass'ами (Adreno 650+) только если depth attachment был `STORE_OP_STORE`,
  следующий pass делает `LOAD_OP_LOAD` того же view/mip/layer, и между ними никто не трогал образ.

**Практический вывод для DOS2**: все три архитектуры уже дают HW occlusion скрытых **пикселей** почти
бесплатно (если не ломать его blending/discard/late-Z в depth/G-buffer pass'ах). Explicit occlusion
query экономит не это, а стоимость **подготовки и отправки draw call** (vertex processing, API overhead,
geometry setup) — то есть именно то, что CPU record thread и тратит 13 мс. Значит правильная метрика
эффективности culling-техники для DOS2 — не "сколько пикселей не закрашено", а "сколько draw call не
попало на CPU record thread вообще", и выигрывают только техники, решающие это **до** записи command
buffer (PVS lookup, software occlusion, batching), а не query, которые сами стоят CPU-время на
begin/end + readback + conditional rendering state.

### 2.5. Поддержка indirect draw / drawCount / conditional rendering на мобильных драйверах

- `vkCmdDrawIndexedIndirectCount`/`vkCmdDrawIndirectCount` — часть core Vulkan 1.2 (продвинуто из
  `VK_KHR_draw_indirect_count`). На Mali официально заявлена поддержка `drawCount > 1` только начиная с
  Mali-G710 (блог Arm) — https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-g710-developer-overview ;
  форумные сообщения независимо подтверждают, что на более старых Mali `drawCount` должен быть `<= 1`.
  На Adreno поддержка заявлена, но единственный найденный замер (Adreno 610, Android 10, форум Qualcomm,
  https://developer.qualcomm.com/comment/17943) показал, что indirect-count путь не быстрее одиночного
  indirect draw — то есть сам по себе count-вариант не гарантирует выигрыш, важнее само снижение
  количества API-вызовов с CPU.

- `VK_EXT_conditional_rendering` на Turnip имеет документированный firmware-зависимый баг (конкретные
  dEQP-тесты `conditional_rendering.draw_clear.draw.case_N` ломаются на одних версиях firmware и чинятся
  на других; есть fallback с hardware binning на sysmem-путь, когда conditional rendering конфликтует с
  transform feedback) — https://android.googlesource.com/platform/external/mesa3d/+/ee2c58d%5E%21/ .
  Для Mali прямых данных по conditional rendering в открытых источниках не нашлось вовсе — это пробел,
  отмечаем как "неизвестно", а не как "работает".

- Occlusion query на Adreno/Turnip: реализация — порт логики GL `glBeginQuery(GL_SAMPLES_PASSED)` со
  спецобработкой tiled render pass'ов (https://android.googlesource.com/platform/external/mesa3d/+/58fd26c4332/src/freedreno/vulkan/tu_query.c).
  Официальная рекомендация Qualcomm: *запускать все query кадра одним батчем сразу после flush*, стараться
  оставаться в "direct mode" (не "binned"), не больше 512 активных запросов одновременно, задержка
  результата ~3 кадра. При нарушении этого паттерна driver остаётся в binned-режиме, и overhead запросов
  (`% CP Busy` в Snapdragon Profiler) достигает **20–40%** против **4–6%** в direct mode —
  https://developer.qualcomm.com/sites/default/files/docs/adreno-gpu/snapdragon-game-toolkit/gdg/gpu/faq.html .
  Это прямое объяснение, почему наша grouped-query схема (B, ~40 запросов/8 условий) и even простые
  per-object query были медленнее: 683 потенциальных draw'а и ~40+ запросов на кадр — за пределами
  комфортного паттерна "один батч, direct mode", особенно если запросы и их readback переплетаются с
  основным рендером, а не изолированы в отдельный батч.

### 2.6. Портал/PVS/cell-based culling — общий фон

Cell-and-portal: мир делится на ячейки, соединённые порталами; если портал не виден — всё за ним
пропускается (https://archive.gamedev.net/archive/reference/programming/features/culling/page2.html).
Classic PVS (Quake-style) считает для каждой ячейки оффлайн множество потенциально видимых других ячеек;
множество консервативно переоценено, но зато runtime — только lookup + frustum culling. Teller 1991
(https://www.cs.princeton.edu/courses/archive/spr01/cs598b/papers/teller91.pdf) — основополагающая
работа про visibility preprocessing для walkthrough-приложений, откуда это пошло. Для изометрической
статичной локации (городок типа Форт Джой) это прямое попадание: камера почти не вращается произвольно,
набор "комнат"/"кварталов" конечен и стабилен между патчами уровня — PVS можно бейкать один раз на
геометрию уровня и инвалидировать только при правке карты.

### 2.7. Temporal reprojection / coherence как добавка, не основной метод

- CHC/CHC++ (Mattausch, курс Scherzer et al. 2010 —
  https://www.cg.tuwien.ac.at/research/publications/2010/scherzer2010d/scherzer2010d-paper.pdf):
  используют hardware occlusion query, но переиспользуют вердикт предыдущего кадра через иерархию
  bounding volume, чтобы не делать query для каждого объекта каждый кадр — идея похожа на нашу технику
  A, но построена вокруг hardware query hierarchy, а не вокруг "доказательства неизменности экранного
  прямоугольника" как у нас.
- Depth-warping reprojection occlusion (Lee, Kim, Eisemann, ACM TOG 2018) — предсказывают depth buffer
  следующего вида через warp предыдущего кадра, используют для culling без явного выбора occluder'ов;
  поддерживают динамические сцены, но требуют обработки disocclusion-зон отдельно.
- Общее предупреждение с форума Khronos (https://community.khronos.org/t/fragment-shader-occlusion-culling/70545):
  если объекты двигаются, temporal reuse может **скрыть видимый объект** — обязательна консервативная
  проверка движения/дисокклюзии, что ровно и добавляет CPU-стоимость доказательству в нашей технике A.

---

## 3. Что НЕ работает на мобильных и почему (с доказательствами)

1. **Per-object hardware occlusion query + `VK_EXT_conditional_rendering` как основной culling path.**
   Наше измерение: 48 fps vs 54 fps (без culling) на Adreno 750/Turnip — то есть чистый проигрыш.
   Причины, подтверждённые внешними источниками: (а) Qualcomm прямо документирует overhead query вне
   "direct batch" паттерна как 20–40% `CP Busy` —
   https://developer.qualcomm.com/sites/default/files/docs/adreno-gpu/snapdragon-game-toolkit/gdg/gpu/faq.html ;
   (б) результат запроса на Adreno задержан на ~3 кадра, что плохо совместимо с per-object branching в
   том же кадре; (в) `VK_EXT_conditional_rendering` на Turnip имеет firmware-зависимый баг с fallback на
   медленный sysmem path — https://android.googlesource.com/platform/external/mesa3d/+/ee2c58d%5E%21/ .

2. **Grouped queries (~40/кадр) + 8 conditional-rendering условий (техника B).** Та же причина, только
   масштабированная: количество query и conditional branches в кадре выходит за пределы "один батч,
   direct mode", каждая query/condition добавляет CPU-side API overhead (begin/end, barrier, readback
   синхронизацию), который на 683 потенциальных draw перевешивает экономию от пропуска part из них —
   это согласуется и с нашим измерением, и с общей рекомендацией Qualcomm батчить запросы в один проход,
   а не распределять их по кадру вперемешку с рисованием.

3. **CPU-reuse вердикта прошлого кадра с доказательством неизменности screen rect (техника A) как
   самостоятельная техника.** Внешние аналоги (CHC/CHC++, depth-warping reprojection) используют
   temporal reuse **как надстройку** над hardware query hierarchy или depth-warp, а не как замену всему
   culling pipeline, и явно платят дополнительной CPU-стоимостью за disocclusion-safety (предупреждение
   Khronos-форума). У нас эта CPU-стоимость доказательства, судя по всему, не окупилась на ~683 draws —
   ожидаемо, раз даже в "правильных" реализациях reuse — это оптимизация query-based pipeline, а не
   замена ему, и сам по себе не убирает query/CPU overhead пункта 1–2.

4. **"Наивный" прямой порт Intel Masked Software Occlusion Culling на мобильный CPU.** Библиотека
   написана под SSE/AVX и не компилируется под ARM без переписывания; независимый опыт переноса похожей
   SSE-культинг-библиотеки на NEON прямо говорит, что `shuffle`/`movemask` не имеют NEON-аналога и
   требуют ручной работы — https://over17.github.io/2025/10/01/burst-occlusion-culling.html . Это не
   значит, что software occlusion не работает на мобильных (технология рабочая, см. п.#2 шорт-листа), но
   "взять код Intel и просто перекомпилировать" не работает.

5. **Two-phase GPU-driven occlusion culling "как на десктопе" без адаптации к tile-based архитектуре.**
   Сама механика (разрыв render pass для подхвата compute-culling результата, readback HZB между
   проходами) стоит заметных денег на мобильном тайлере — доклад Vulkanised 2026 называет >0.5 мс только
   на resume renderpass через depth attachment —
   https://vulkan.org/user/pages/09.events/vulkanised-2026/1415-Lucas-Silva-DevshGraphicsProgramming.pptx.pdf .
   Плюс `vkCmdDrawIndexedIndirectCount` с `drawCount > 1` не гарантированно доступен на распространённых
   Mali (pre-G710) — https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-g710-developer-overview .
   Десктопная GPU-driven схема Haar/Aaltonen (SIGGRAPH 2015) создавалась не для tile-based GPU и
   архитектурно игнорирует именно эти ограничения.

6. **Дублирование аппаратного HSR/FPK/Early-ZS/LRZ дополнительным software/API-level occlusion слоем
   без учёта того, что он уже делает то же самое почти бесплатно.** Все три мобильные архитектуры
   (PowerVR TBDR HSR, Mali FPK+Early-ZS, Adreno LRZ) уже убирают стоимость закраски скрытых пикселей на
   уровне железа при аккуратном рендере (без blending/discard/late-Z в depth/G-buffer проходах). Явный
   query/conditional rendering пытается сэкономить ту же пиксельную стоимость ещё раз, но платит за это
   CPU API overhead — то есть тратит ресурс там, где он дороже, чтобы сэкономить то, что уже почти
   бесплатно. Экономить стоит именно CPU draw-submission cost (vertex setup, API calls) — а это делают
   PVS/software-occlusion-before-recording/batching (п.#1–#3 шорт-листа), не GPU query.

---

## Источники (сводный список)

- Habr, разбор Genshin Impact через RenderDoc (мобильная сборка) — https://habr.com/en/articles/874866
- miHoYo, Unity Dojo 2021, две кастомные рендер-пайплайны PC/mobile — https://es.slideshare.net/UnityTechnologiesJapan/210617-unity-dojo20211mihoyozhenzhongyi
- Unity URP GPU occlusion culling / GPU Resident Drawer (6.4) — https://docs.unity3d.com/6000.4/Documentation/Manual/urp/gpu-culling.html
- Unity форум, ограничения GPU occlusion culling на мобильных — https://discussions.unity.com/t/unity-6-gpu-resident-drawer-gpu-occlusion-cullling-compatibility-with-android/1539448
- Unreal Engine, Visibility and Occlusion Culling — https://dev.epicgames.com/documentation/en-us/unreal-engine/visibility-and-occlusion-culling-in-unreal-engine
- Unreal Engine, Precomputed Visibility Volumes — https://dev.epicgames.com/documentation/en-us/unreal-engine/precomputed-visibility-volumes-in-unreal-engine
- Meta/Darkwind, Occlusion Culling for Mobile VR — custom PVS (Republique) — https://developers.meta.com/horizon/blog/occlusion-culling-for-mobile-vr-developing-a-custom-solution/
- Arm, Hidden Surface Removal methods in Mali GPUs (FPK, Early-ZS) — https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/hidden-surface-graphics-removal-methods-in-mali-gpus
- Arm, Mali-G710 developer overview (CSF, multi-draw-indirect) — https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-g710-developer-overview
- Arm форум, ограничение drawCount<=1 на Mali pre-G710 — https://community.arm.com/support-forums/f/graphics-gaming-and-vr-forum/49770/do-we-need-to-repack-our-vertex-buffers-for-mali-g76-to-avoid-vk_device_lost
- Imagination Technologies, The DR in TBDR: deferred rendering in Rogue (PowerVR HSR) — https://blog.imaginationtech.com/the-dr-in-tbdr-deferred-rendering-in-rogue/
- Mozilla GFX wiki, что отключает per-pixel HSR на мобильных GPU — https://wiki.mozilla.org/Platform/GFX/MobileGPUs
- Igalia, Low-resolution-Z on Adreno GPUs (LRZ правила) — https://blogs.igalia.com/dpiliaiev/adreno-lrz/
- Mesa3D docs, Freedreno LRZ — https://docs.mesa3d.org/drivers/freedreno/hw/lrz.html
- Qualcomm Adreno GDG, FAQ (occlusion query direct mode, CP Busy 20-40%→4-6%, лимиты) — https://developer.qualcomm.com/sites/default/files/docs/adreno-gpu/snapdragon-game-toolkit/gdg/gpu/faq.html
- Qualcomm форум, слабый throughput indirect-count на Adreno 610 — https://developer.qualcomm.com/comment/17943
- Mesa/Android, tu: Implement VK_EXT_conditional_rendering (firmware bug, sysmem fallback) — https://android.googlesource.com/platform/external/mesa3d/+/ee2c58d%5E%21/
- Mesa/Android, tu_query.c (occlusion query implementation Turnip) — https://android.googlesource.com/platform/external/mesa3d/+/58fd26c4332/src/freedreno/vulkan/tu_query.c
- Intel, Masked Software Occlusion Culling (библиотека и статья HPG 2016) — https://intel.com/content/www/us/en/developer/articles/technical/masked-software-occlusion-culling.html , https://fileadmin.cs.lth.se/graphics/research/papers/2016/culling/
- Независимый ARM/NEON порт похожей SSE occlusion-culling библиотеки (Unity Burst) — https://over17.github.io/2025/10/01/burst-occlusion-culling.html
- Haar & Aaltonen, GPU-Driven Rendering Pipelines, SIGGRAPH 2015 Advances course — https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf
- Практический разбор two-phase occlusion culling — https://hannomalie.github.io/posts/2018-02-07-culling-part-2.html
- Vulkanised 2026, Lucas Silva (DevSh Graphics Programming) — мобильный GPU-driven rendering, стоимость resume renderpass — https://vulkan.org/user/pages/09.events/vulkanised-2026/1415-Lucas-Silva-DevshGraphicsProgramming.pptx.pdf (403 при автоматическом fetch — открыть вручную)
- Cell-and-portal / PVS, общий обзор — https://archive.gamedev.net/archive/reference/programming/features/culling/page2.html
- Teller, Visibility preprocessing for interactive walkthroughs (1991) — https://www.cs.princeton.edu/courses/archive/spr01/cs598b/papers/teller91.pdf
- Scherzer et al. / Mattausch, Coherent Hierarchical Culling course — https://www.cg.tuwien.ac.at/research/publications/2010/scherzer2010d/scherzer2010d-paper.pdf
- Lee, Kim, Eisemann, depth-warping reprojection occlusion culling (ACM TOG 2018) — https://publications.graphics.tudelft.nl/rails/active_storage/blobs/redirect/eyJfcmFpbHMiOnsibWVzc2FnZSI6IkJBaHBBdVVJIiwiZXhwIjpudWxsLCJwdXIiOiJibG9iX2lkIn19--bcd9fb883c9556c558df49afa9509c785e209c58/SYMNSWE12a.pdf
- Khronos форум, риски temporal reuse при движении объектов — https://community.khronos.org/t/fragment-shader-occlusion-culling/70545
- Stingray/Autodesk, ручные occluder-боксы — https://help.autodesk.com/cloudhelp/ENU/Stingray-Help/stingray_help/building_levels/create_game_objects/create_occluders.html
- Unity, OcclusionArea (baked occlusion, manual areas) — https://docs.unity3d.com/Manual/class-OcclusionArea.html
- Godot, Occlusion culling (simplified occluder mesh, CPU software rasterization) — https://docs.godotengine.org/en/4.7/tutorials/3d/occlusion_culling.html
- Com2uS/Android Developers, Vulkan GPU-driven batching "Clay" (Summoners War: Chronicles) — https://developer.android.com/stories/games/com2us-vulkan
- GDC Vault, Swen Vincke, The Making of Divinity: Original Sin 2 (design talk, не инженерный) — https://www.gdcvault.com/play/1025734/contactUs
- VideoGamer / 80.lv, Larian про Vulkan/DX11 в BG3 (движок = развитие Divinity Engine) — https://www.videogamer.com/?p=1127173 , https://80.lv/articles/baldur-s-gate-3-dev-explained-why-it-supports-two-apis

**Пробелы, отмеченные честно, а не замаскированные:** инженерной информации про occlusion culling
конкретно в движке Larian (DOS2/BG3) в открытых источниках не нашлось — только факт поддержки
DX11+Vulkan и признание слабого Vulkan-опыта команды на момент релиза BG3. Данных по
`VK_EXT_conditional_rendering` на Mali не нашлось вовсе. Слайды Vulkanised 2026 не открылись напрямую
(403) — цифра про resume renderpass взята из поисковой выжимки, перед реализацией стоит открыть PDF
руками и перепроверить контекст цифры.
