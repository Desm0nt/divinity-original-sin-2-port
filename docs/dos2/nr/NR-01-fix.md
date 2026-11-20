# NR-01 fix — ключи пака шейдеров не считались ни для одной программы (`process_vm_readv` и zero-copy пулы)

Ветка `dos2-nr01fix` (база `dos2-nr-all` 63e2d80). Телефон не трогался.

## Симптом (первый прогон NR-01: `src/work/logs/nr1_r1.dm.log`, Mi 14 Ultra, `DM_NR=2 DM_NR_DUMP=1`, палуба, 60 с)
- Каждая проверка шейдера красная: `【红】[nr] check: shader 0x14400001000000xx: program - name '...' programs vs 0x106449b320 () fs 0x106449b4b0 () ..., 2 from the tap, 2 in the record`
  — программы найдены и сопоставлены с записью движка, но ключ пуст (`()`), а `nr_check_shader` считает пустой ключ ошибкой.
- Перепись: `shaders 512 (507 named, 1022 programs, 0 keyed) ... unreadable 1066 ... programs with unreadable code 1066`;
  `checks (DM_NR=2): 3500 objects checked, 534 mismatches: program 534`. `unreadable` (= все неудачные `nr_peek`) ровно равно
  `programs with unreadable code`: все остальные чтения через ядро (записи движка, имена, пакеты) проходят, отказ — только на коде шейдеров.

## Причина
- `sd[i].data` программы — это хост-указатель (`nvnMemoryPoolGetBufferAddress` = `pool->mem`), транслятор (`xbg_game.cpp load_shader`)
  читает код по нему обычными загрузками, и это работает.
- Но пулы на Adreno — zero-copy (`DM_GFX_ZEROCOPY`, умолчание 1 по политике: в логе `zero-copy default=1`, `NVN zero-copy guest memory=1`,
  пулы `0x10ce64f000+0x21800000` и др. `mapped`): `zc::map_chunk` делает AHardwareBuffer и кладёт его dma-buf `mmap(MAP_SHARED | MAP_FIXED)`
  поверх гостевого диапазона. dma-buf heap отображает страницы через `remap_pfn_range` → VMA с `VM_IO | VM_PFNMAP`.
- `process_vm_readv` (так NR-01 читал гостевую память, `nr_peek`) закрепляет страницы через GUP, а GUP отказывает `VM_IO | VM_PFNMAP`
  VMA с `EFAULT` (`mm/gup.c check_vma_flags`). Поэтому проба первого / последнего байта кода в `nr_pack_key` падала всегда → нет ключа.
- Проверено на ПК (`tests/nr_peek_test.c`, WSL2 5.15): страницы `[vvar]` (VmFlags `pf io`) — `process_vm_readv refused 4 (EFAULT)` из 4,
  а чтение через pipe читает предоставленную страницу и даёт 0 (без сигнала) на остальных. (vgem в WSL2 отображает dumb-буферы без
  `VM_PFNMAP`, так что там `process_vm_readv` проходит — сам dma-buf-случай телефона на ПК не воспроизводится, только механизм.)
- Не GPU VA и не PROT_NONE/ленивая VAMM: адрес — обычный хост-указатель в живом пуле, обычные загрузки его читают.

## Что сделано (только при `DM_NR != 0`; `DM_NR=0` — байт в байт как было: ничего из этого не исполняется)
- **`dm/gfx/nr/nr_peek.h`** (новый, header-only): `nr_pipe_open` / `nr_pipe_read` — чтение своей памяти через `write()` в собственный pipe
  (`copy_from_user`: обычные загрузки с exception-table fixup) кусками ≤ 4 КиБ (PIPE_BUF) с немедленным вычитыванием. Отображённая
  страница читается при любых флагах VMA (и `VM_PFNMAP`), неотображённая / PROT_NONE — возврат `EFAULT`, **никогда не сигнал** (обработчики
  SIGSEGV хоста/игры не трогаются). Pipe после любого исхода пуст; сломанный pipe закрывается и создаётся заново.
- **`dm/host/hle_dos2_nr.c`**: `nr_peek` сначала `process_vm_readv`, при отказе — тот же диапазон через pipe (один pipe, свой листовой
  mutex, создаётся при первом отказе). Первый отказ — одна строка лога (адрес, errno, результат pipe, флаги NVN-пула). В переписи
  добавлено `reads process_vm_readv refused N, of them read through the pipe M`. `nr_pack_key` больше не делает «пробу + чтение
  загрузками»: весь код читается через `nr_peek` и хешируется (`xbg_shader_program_key_rd`) — ни одной загрузки гостевой памяти,
  нечитаемый код = нет ключа (счёт `programs with unreadable code`), не падение. Аллокаций нет (кусок 4 КиБ на стеке).
- **`dm/gfx/vulkan/engine/xbg_shader_key.c`**: `xbg_shader_program_key_rd(control, code, rd, out)` — тот же SHA-256 (control своей длины +
  `ncode` байт кода при `0 < ncode < 16 МиБ`), что `xbg_shader_program_key` (ключ пака / `load_shader`), только код читается через `rd`.
  Старая функция не изменена.

## NR-02 / NR-04 / NR-07
Все три берут ключ из одного места — `nr_pack_key` в `dm_nr_tap_program` (`b->key[]` → запись шейдера реестра → NR-02 `t_nk.key`,
NR-04 `nr_sh_names(..., key[1])`, NR-07 `nr_pipe_hint`, в т.ч. программы вне вызова движка), так что починка одна на всех.
`dm_nr_prog_key` (NR-02 `DM_NR_KEY=2`, ключ на первом draw) читает код обычными загрузками — ровно как транслятор в тот же момент
(это и есть сравниваемая сторона), проба там не нужна и не менялась.

## Тесты
- `tests/nr_peek_test.c` (новый, WSL gcc; `gcc -std=gnu11 -O2 -Wall -Wextra nr_peek_test.c ../xbg_shader_key.c`): ключ `_rd` = ключ пака
  для всех классов длины control и размеров кода вокруг кусков 4 КиБ, NULL / 0 / ≥ 16 МиБ, отказ любого куска = -1 и пустой ключ;
  `nr_pipe_read` на rw / read-only / многостраничных диапазонах, 0 без сигнала на PROT_NONE / неотображённой / смешанной; dma-buf vgem,
  положенный `MAP_FIXED` поверх анонимной памяти как в `zc::map_chunk` (ключ через pipe = ключ загрузками); `[vvar]` как `VM_PFNMAP`.
  Итог: `PASS: 310 checks, 0 failed`.
- `source_structure_test.py`: NR-01 — `nr_pack_key` ключует только через `xbg_shader_program_key_rd` + `nr_peek`, `nr_peek` идёт
  `process_vm_readv` → pipe, в `nr_peek.h` нет обработчиков сигналов.
- Полный прогон хост-тестов как на `dos2-nr-all` (+ `nr_peek_test`): `pass=66 fail=0` (было 65), `dos2_nr_check` 0 problems.
- NDK 29 `-fsyntax-only` `hle_dos2_nr.c` и `xbg_shader_key.c` (флаги `work/build-android`) — чисто.
- `tools/build_apk.ps1` в этой ветке: `work/DOS2-port-debug.apk`, 54 880 374 байт,
  SHA-256 `33760bd38cf503e76191d4ecabb9ff63a74f5fa103e35fc1ea9f89eec385fb95`; в `libdm.so` есть `xbg_shader_program_key_rd` (T) —
  слабая ссылка из `hle_dos2_nr.c` разрешена, `nr_pack_key` не отвалится в «нет ключа» из-за отсутствия символа.

## Прогон на телефоне, который это подтвердит
Тот же, что `nr1_r1`: бэкап `files/save`, APK этой ветки, env `DM_NR=2 DM_NR_DUMP=1` (по желанию `DM_NR_KEY=2`, `DM_NR_PIPE=1`), палуба,
60 с после загрузки, force-stop. Ждём:
- один раз `[nr] guest read: process_vm_readv refused 0x...+0x... (returned -1, errno 14); one byte through a pipe (copy_from_user): read; NVN pool flags of the range <флаги> ...`;
- в `[nr] objects ...`: `shaders N (… M programs, M keyed)` — keyed = programs (было 0), `unreadable 0`, `programs with unreadable code 0`,
  `reads process_vm_readv refused K, of them read through the pipe K` (K ≈ число кусков кода по 4 КиБ, обе цифры равны);
- `[nr] checks (DM_NR=2): … 0 mismatches`, ни одной красной `check: shader`; в `nr_shaders.tsv` колонки `vs_key` / `fs_key` заполнены
  16-значными ключами, совпадающими с именами бандлов `<key>.xgs`;
- с `DM_NR_KEY=2`: 0 различий ключа пака (SetShaders vs первый draw); с `DM_NR_PIPE=1`: `pipe hints … 0 programs without a key`.

## Риски
- Pipe читает и то, что `process_vm_readv` раньше отказывался читать (любая VMA с `VM_PFNMAP`): у процесса нет MMIO-отображений, только
  dma-buf / kgsl-память, чтение которой безопасно (некэшируемая — медленнее). Стоимость — только при отказе: `write` + `read` на 4 КиБ,
  для ~1000 программ за загрузку — доли миллисекунды на программу.
- Страница, которую коммитит обработчик SIGSEGV (ленивая VAMM, как в Eternal), через pipe даст `EFAULT` (нет ключа), а не коммит; в DOS2
  код шейдеров в таких страницах не лежит.
