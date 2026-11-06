# NNK-DS-Recomp

Clean-room native recompilation runtime for the Nintendo DS game *Ni no Kuni:
Shikkoku no Madoushi* (B2KJ).  Research / interoperability project — no game data
of any kind is included in this repository.

## Legal notice

This repository contains only our own code and documentation.  It does NOT include:

- the game ROM or any extracted game data
- DS BIOS / firmware dumps
- game code, assets, screenshots, traces, or melonDS-derived artifacts

To run this project you must supply your own legally obtained game ROM and BIOS
files locally.  The published code is the runtime and tooling only.

## Status

A Rust recompiler (`nnktool`) decodes the ARM9 and every overlay and emits one C
function per guest program counter; the generated dispatch image plus a C runtime
boots the game natively, with the DS GPU modelled by our own renderer (`gxrender`).

| milestone | state |
|---|---|
| boot, hardware init, first frame | working |
| fan-translation notice screen, Mobiclip splash | working |
| Ghibli intro movie (`data/movie/op_u.mods` + `op_d`, 1317 frames) | working, decode bit-exact |
| title screen (`New Game`) | working |
| in-game cutscene (`data/movie/s01.mods`, 581 frames) | working, read from the ROM |
| in-game prompt *"Skip the cutscene?"* — box, text, **Yes** and **No** | working |
| answering the prompt → town map, dialogue that advances with input | working |
| full 3D scene: geometry, textures, strips, clipping, GX matrix readback | working |
| frame pacing at the console rate (59.826 Hz) | working |
| sound | not implemented |

Runs are deterministic: same build + ROM + step budget + input script produce
identical counters and byte-identical frames.  Known defect: a strict savestate
round trip is not yet bit-exact (cold vs reloaded state hashes differ while the
frames match); it is tracked separately and does not affect normal F5/F9 use.  Savestates (`F5`/`F9`) are part of
the runtime and load across builds (regions are identified by name and version, a
mismatched layout restores best-effort with a per-region report).

## Prerequisites

- Rust + Cargo (builds `nnktool`)
- a C compiler (`cc`, e.g. `clang` or `gcc`)
- Python 3 (stdlib-only tools)
- SDL2 development libraries (for `NNK_GX=window`)
- `make` (builds the `gxrender` library and its tests)

## Build

```sh
cd nnktool && cargo build --release && cd ..      # build the recompiler FIRST
NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20 \
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c \
    bash tools/build_split.sh out/p5aj/nnk_runner
```

The build chain shells out to `target/release/nnktool` for the dispatch image, the
ROM-derived pc→form map, the savestate field table and the image splices — a stale
binary in the tree you build from fails the splices with
`expected '…', recompiler emitted '…'`, so build `nnktool` there first.

`tools/prelude_audit.py` runs inside the build and fails it when the compiled
runtime is missing a feature the tree carries (see `PRELUDE_AUDIT.md`).

## Run

```sh
./run.sh "/path/Ni No Kuni.nds"                      # SDL2 window, F5 save / F9 load
NNK_GX=headless ./nnk_runner "/path/Ni No Kuni.nds"  # headless (reproducible)
```

The card image is the only input.  At start-up the runtime derives in memory
everything the earlier phases needed as side files:

| derived | from the ROM |
|---|---|
| guest image 445,304 B @ `0x02000000` | ARM9 slice `0x4000+0x49328`, zero-padded |
| decompressed ARM9 | the DS compression of that slice |
| overlay arena window @ `0x020C0000` | overlay 17 (OVT + FAT), decompressed |
| movie frames | `data/movie/*.mods`, decoded in-process (Mobiclip HLE) |
| files the guest opens | the ROM's Nitro filesystem (FNT/FAT) |

## Controls (window)

Arrows = D-pad · `X` = A · `Z` = B · `S`/`A` = X/Y · `Enter` = Start ·
`Shift` = Select · `Q`/`W` = L/R.

## Scripted runs and savestates

```sh
NNK_INPUT_FILE=script.nnk NNK_STEP_LIMIT=4300000000 ./nnk_runner "Ni No Kuni.nds"
nnk_runner: F5 saves ./nnk_save1.nnks, F9 loads it
NNK_SAVESTATE=state.nnks ./nnk_runner "Ni No Kuni.nds"     # boot from a state
NNK_SAVESTATE_STRICT=1 ...                                 # refuse a layout mismatch
```

An input script is `<display frame> <KEYINPUT mask>` per line (`0x3FF` = nothing
held, active-low), terminated by `end`.  A press must be held for **≥ 6 frames** —
the guest polls `KEYINPUT` once per frame, so a one-frame tap is invisible.
`NNK_INPUT_RECORD=out.nnk` records a live session for verbatim replay.

## Performance

Headless, `/usr/bin/time`, two runs each:

| steps | time | rate |
|---|---|---|
| 200 M | 9.1 s | ~22 M steps/s |
| 600 M | 24.5 s | ~24 M steps/s |

The per-step DMA/VBlank/timer/IPC tick is event-driven: the runtime computes the
exact next cycle at which anything can change and runs the tick only then, so 200 M
steps execute ~450 k ticks instead of 200 M.  Debug probes are compiled in but
skipped unless `NNK_PROBES=1` (~9% of the run).

`NNK_PERF=1` splits every presented frame into render (3D rasteriser + 2D compose +
present) and guest, `NNK_PERF=2` adds the renderer's own sections.  Measured on a
3D town scene, operator savestate, headless:

| | frame | render | guest | fps |
|---|---|---|---|---|
| before the idle-spin skip (Phase-5BX) | 39.6 ms | 10.1 ms | 29.5 ms | 25 |
| after 5BX (idle spin skipped to the next event) | 22.4 ms | 10.1 ms | 12.2 ms | 45 |
| after 5BY/5BZ (renderer passes) | 18.7 ms | 7.0 ms | 11.7 ms | 53 |

The windowed run presents at 59.826 Hz (`NNK_FPS_LIMIT=<hz>` changes it,
`NNK_FPS_LIMIT=0` disables pacing): the emulator is faster than the console, so
without pacing the game runs at 5-6x speed.  A windowed run is not capped by the
200 M `STEP_LIMIT` harness guard; headless runs keep it so scripts cannot hang.

## Verification

```sh
cd nnktool && cargo test                      # 65 decoder/build regression classes
SKIP_MELONDS=1 bash tools/verify_phase2a.sh   # boot-chain trace gate: 1000/1000 PCs vs melonDS
```

## Debug overrides (never requirements)

| variable | what it does |
|---|---|
| `NNK_STEP_LIMIT=<n>` | stop after n guest instructions |
| `NNK_GX=window` / `headless` | SDL2 window vs headless renderer (default headless) |
| `NNK_GX_SHOT=<base>`, `NNK_GX_SHOT_EVERY=<n>` | PPM captures of both screens |
| `NNK_GX_STATE_DUMP=1` | dump DISPCNT / BGxCNT / OAM / VRAMCNT at exit |
| `NNK_INPUT_FILE`, `NNK_INPUT_RECORD`, `NNK_INPUT_TRACE` | scripted input, record, trace |
| `NNK_SAVESTATE`, `NNK_SAVESTATE_SAVE[_AT]`, `NNK_SAVESTATE_STRICT`, `NNK_STATE_HASH_AT` | savestates and determinism hashes |
| `NNK_TRACE_FROM`, `NNK_SMC_TRACE`, `NNK_EXC_TRACE`, `NNK_DEBUG_AT_PC`, `NNK_VRAM_TRACE`, `NNK_AOBJ_TRACE`, `NNK_DMA_TRACE`, `NNK_MOVIE_TRACE`, `NNK_PROBES` | tracing and probes |
| `NNK_PERF=1` / `NNK_PERF=2` | frame = render + guest (plus renderer sections) every 2 s |
| `NNK_FPS_LIMIT=<hz>` | presentation rate (default 59.826; 0 disables pacing) |
| `NNK_IRQ_TRACE=1`, `NNK_IO_TRACE=1`, `NNK_STORE_TRACE=1` | per-event debug lines (off by default) |
| `NNK_DMA_LEGACY_SIZE=1` | A/B the pre-5BF DMA transfer-size bit |
| `NNK_SMC_IMAGE`, `NNK_ROM_PATH`, `NNK_MOVIE_YUV*`, `NNK_MOVIE_MODS*` | A/B overrides for review only |

## Documentation

| File | Description |
|------|-------------|
| BUILD.md | Split-dispatch build and the content-hash object cache |
| CODE_MAP.md | Code-map tables (addresses, sizes, seeds) |
| IO_MAP.md | IO register map and observed reads/writes |
| ROM_MAP.md | ROM layout and header measurement facts |
| INPUT.md | SDL keyboard controls and input proof |
| DECODER_TESTS.md | The regression-class catalogue |
| SAVESTATES.md | Savestate container, regions, determinism proof |
| DSD_OVERLAYS.md | Overlay table, arenas, per-overlay measurement |
| PRELUDE_AUDIT.md | How the build proves the runtime has every feature |
| gxrender/README.md | GX renderer layout and build |
| gxrender/GX_COVERAGE.md | GX command coverage (implemented vs stubbed) |
| PHASE4D…PHASE4Z, PHASE5A…PHASE5BX | The engineering log, one file per investigation: what was measured, how, the fix, and the gates that passed |

## Notes on method

Each `PHASE*.md` is self-contained evidence: the measurement, its method, the fix and
the verification.  The rules that came out of them are enforced by regression classes
in `nnktool/src/decoder_regression_tests.rs`, so a fix cannot be quietly lost — the
instruction-state witness for exception returns, `blx` and taken branches; the ARM9
vs ARM7 DMA field layout (transfer size is bit 26, not bit 30); movie containers
bound by identity through the Nitro filesystem rather than by stream ordinal;
savestate regions identified by name and version.

---

# NNK-DS-Recomp (Русский)

Чистая комната (clean-room) нативный рекомпиляционный рантайм для игры Nintendo DS
*Ni no Kuni: Shikkoku no Madoushi* (B2KJ).  Исследовательский проект по
интероперабельности — никаких игровых данных в репозитории нет.

## Юридическое уведомление

В этом репозитории только наш собственный код и документация.  Здесь НЕТ:

- игрового ROM или любых извлечённых игровых данных
- BIOS / прошивки Nintendo DS
- игрового кода, ассетов, скриншотов, трасс или артефактов melonDS

Для запуска проекта вы должны самостоятельно разместить у себя законно
приобретённый игровой ROM и BIOS.  Публикуемый код — это только рантайм и
инструменты.

## Состояние

Rust-рекомпилятор (`nnktool`) разбирает ARM9 и все оверлеи и выдаёт по одной
C-функции на каждый гостевой PC; сгенерированный диспетчер-образ вместе с
C-рантаймом запускает игру нативно, а GPU DS моделируется нашим рендерером
(`gxrender`).

| веха | состояние |
|---|---|
| загрузка, инициализация железа, первый кадр | работает |
| экран уведомления фанатского перевода, заставка Mobiclip | работает |
| интро Ghibli (`data/movie/op_u.mods` + `op_d`, 1317 кадров) | работает, декод бит-в-бит |
| титульный экран (`New Game`) | работает |
| внутриигровой ролик (`data/movie/s01.mods`, 581 кадр) | работает, читается из ROM |
| окошко *"Skip the cutscene?"* — рамка, текст, **Yes** и **No** | работает |
| ответ на окошко → карта города, диалог, продвигаемый вводом | работает |
| полная 3D-сцена: геометрия, текстуры, strip, клип, чтение матриц GX | работает |
| подача кадров с частотой консоли (59,826 Гц) | работает |
| звук | не реализован |

Прогоны детерминированы: одинаковые бинарь, ROM, лимит шагов и скрипт ввода дают
идентичные счётчики и побайтово одинаковые кадры.  Сейвстейты (`F5`/`F9`) — часть
рантайма, грузятся между сборками (регионы опознаются по имени и версии, при
несовпадении раскладки состояние восстанавливается частично с отчётом по регионам).

## Требования

- Rust + Cargo (собирает `nnktool`)
- компилятор C (`cc`, например `clang` или `gcc`)
- Python 3 (инструменты только на stdlib)
- библиотеки SDL2 (для `NNK_GX=window`)
- `make` (собирает библиотеку `gxrender` и её тесты)

## Сборка

```sh
cd nnktool && cargo build --release && cd ..      # СНАЧАЛА рекомпилятор
NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20 \
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c \
    bash tools/build_split.sh out/p5aj/nnk_runner
```

Сборочная цепочка вызывает `target/release/nnktool` для генерации образа, карты
форм из ROM, таблицы полей сейвстейта и сплайсов.  Устаревший бинарь в том дереве,
из которого вы собираете, роняет сплайсы с сообщением
`expected '…', recompiler emitted '…'` — поэтому `nnktool` собирается первым.

`tools/prelude_audit.py` выполняется внутри сборки и останавливает её, если в
скомпилированном рантайме нет возможности, которая уже есть в дереве
(см. `PRELUDE_AUDIT.md`).

## Запуск

```sh
./run.sh "/путь/к/Ni No Kuni.nds"                      # окно SDL2, F5 сохранить / F9 загрузить
NNK_GX=headless ./nnk_runner "/путь/к/Ni No Kuni.nds"  # headless (воспроизводимо)
```

Единственный вход — образ карты.  На старте рантайм выводит в памяти всё, что
раньше требовалось отдельными файлами:

| что | из чего в ROM |
|---|---|
| гостевой образ 445 304 Б @ `0x02000000` | срез ARM9 `0x4000+0x49328`, добитый нулями |
| распакованный ARM9 | DS-сжатие этого среза |
| окно арены оверлеев @ `0x020C0000` | оверлей 17 (OVT + FAT), распакованный |
| кадры роликов | `data/movie/*.mods`, декодируются в процессе (Mobiclip HLE) |
| файлы, которые открывает игра | Nitro-файловая система ROM (FNT/FAT) |

## Управление (окно)

Стрелки — крестовина · `X` = A · `Z` = B · `S`/`A` = X/Y · `Enter` = Start ·
`Shift` = Select · `Q`/`W` = L/R.

## Скрипты ввода и сейвстейты

```sh
NNK_INPUT_FILE=script.nnk NNK_STEP_LIMIT=4300000000 ./nnk_runner "Ni No Kuni.nds"
NNK_SAVESTATE=state.nnks ./nnk_runner "Ni No Kuni.nds"     # старт с состояния
NNK_SAVESTATE_STRICT=1 ...                                 # строгий режим раскладки
```

Формат скрипта: `<кадр дисплея> <маска KEYINPUT>` на строку (`0x3FF` — ничего не
нажато, активный уровень низкий), последняя строка `end`.  Нажатие надо держать
**≥ 6 кадров** — гость опрашивает `KEYINPUT` раз в кадр, поэтому нажатие в один кадр
невидимо.  `NNK_INPUT_RECORD=out.nnk` записывает живую сессию для точного повтора.

## Производительность

Headless, `/usr/bin/time`, по два прогона:

| шагов | время | скорость |
|---|---|---|
| 200 млн | 9,1 с | ~22 млн шагов/с |
| 600 млн | 24,5 с | ~24 млн шагов/с |

Покадровый тик (DMA/VBlank/таймеры/IPC) событийный: рантайм считает точный
следующий цикл, когда что-то может измениться, и вызывает тик только тогда — на
200 млн шагов приходится ~450 тыс. тиков вместо 200 млн.  Отладочные пробы
скомпилированы, но пропускаются без `NNK_PROBES=1` (ещё ~9%).

`NNK_PERF=1` делит каждый поданный кадр на рендер (растеризатор 3D + сборка 2D +
презентация) и гостя, `NNK_PERF=2` добавляет секции рендерера.  Замер на
3D-сцене города, операторский сейвстейт, headless:

| | кадр | рендер | гость | fps |
|---|---|---|---|---|
| до пропуска idle-петли (Phase-5BX) | 39,6 мс | 10,1 мс | 29,5 мс | 25 |
| после 5BX (петля пропускается до ближайшего события) | 22,4 мс | 10,1 мс | 12,2 мс | 45 |
| после 5BY/5BZ (проходы по рендереру) | 18,7 мс | 7,0 мс | 11,7 мс | 53 |

Окно подаёт кадры с частотой 59,826 Гц (`NNK_FPS_LIMIT=<гц>` меняет частоту,
`NNK_FPS_LIMIT=0` отключает ограничение): эмулятор быстрее консоли, и без
ограничения игра идёт в 5–6 раз быстрее.  Интерактивный прогон больше не
ограничен предохранителем `STEP_LIMIT` в 200 млн шагов; headless-прогоны его
сохраняют, чтобы скрипты не зависали.

## Проверка

```sh
cd nnktool && cargo test                      # 65 регрессионных классов декодера/сборки
SKIP_MELONDS=1 bash tools/verify_phase2a.sh   # трассировочный гейт: 1000/1000 PC против melonDS
```

## Отладочные переменные (никогда не требования)

| переменная | что делает |
|---|---|
| `NNK_STEP_LIMIT=<n>` | остановиться после n гостевых инструкций |
| `NNK_GX=window` / `headless` | окно SDL2 либо headless-рендерер (по умолчанию headless) |
| `NNK_GX_SHOT=<base>`, `NNK_GX_SHOT_EVERY=<n>` | снимки обоих экранов в PPM |
| `NNK_GX_STATE_DUMP=1` | дамп DISPCNT / BGxCNT / OAM / VRAMCNT на выходе |
| `NNK_INPUT_FILE`, `NNK_INPUT_RECORD`, `NNK_INPUT_TRACE` | скрипт ввода, запись, трасса |
| `NNK_SAVESTATE`, `NNK_SAVESTATE_SAVE[_AT]`, `NNK_SAVESTATE_STRICT`, `NNK_STATE_HASH_AT` | сейвстейты и хеши детерминизма |
| `NNK_TRACE_FROM`, `NNK_SMC_TRACE`, `NNK_EXC_TRACE`, `NNK_DEBUG_AT_PC`, `NNK_VRAM_TRACE`, `NNK_AOBJ_TRACE`, `NNK_DMA_TRACE`, `NNK_MOVIE_TRACE`, `NNK_PROBES` | трассировка и пробы |
| `NNK_PERF=1` / `NNK_PERF=2` | кадр = рендер + гость (плюс секции рендерера) раз в 2 с |
| `NNK_FPS_LIMIT=<гц>` | частота подачи кадров (по умолчанию 59,826; 0 отключает) |
| `NNK_IRQ_TRACE=1`, `NNK_IO_TRACE=1`, `NNK_STORE_TRACE=1` | отладочные строки по событиям (по умолчанию выключены) |
| `NNK_DMA_LEGACY_SIZE=1` | A/B бита размера передачи DMA до 5BF |
| `NNK_SMC_IMAGE`, `NNK_ROM_PATH`, `NNK_MOVIE_YUV*`, `NNK_MOVIE_MODS*` | A/B-оверрайды только для ревью |

## Документация

| Файл | Описание |
|------|----------|
| BUILD.md | Сборка сплит-диспетчера и кеш объектов по хешу содержимого |
| CODE_MAP.md | Таблицы карты кода (адреса, размеры, сиды) |
| IO_MAP.md | Карта IO-регистров и наблюдаемые чтения/записи |
| ROM_MAP.md | Раскладка ROM и измерения заголовка |
| INPUT.md | Управление с клавиатуры и доказательство ввода |
| DECODER_TESTS.md | Каталог регрессионных классов |
| SAVESTATES.md | Контейнер сейвстейтов, регионы, доказательство детерминизма |
| DSD_OVERLAYS.md | Таблица оверлеев, арены, измерения по оверлеям |
| PRELUDE_AUDIT.md | Как сборка доказывает наличие всех возможностей рантайма |
| gxrender/README.md | Устройство и сборка рендерера GX |
| gxrender/GX_COVERAGE.md | Покрытие команд GX (реализовано / заглушено) |
| PHASE4D…PHASE4Z, PHASE5A…PHASE5BX | Инженерный журнал: по файлу на исследование — что измерено, как, какой фикс и какие гейты пройдены |
