# CapstanVar Preset JSON Schema (`.cvpr`)

**Reference for users hand-editing exported preset files and for any future
migration tooling.** The schema is defined in code by `ep_to_json` /
`ep_from_json` in `src/preset_manager.cpp`; the table here is the
authoritative reference.

## Overview

| Property                | Value                                            |
| ----------------------- | ------------------------------------------------ |
| File extension          | `.cvpr` (also `.json` accepted by importer)      |
| Encoding                | UTF-8, JSON                                      |
| Magic (`__app__`)       | `"CapstanVar"` — required for import             |
| Schema version          | `2` (Phase 2; current)                            |
| Phase 4+ compat         | Stale-id guard added; no schema bump             |

`import_preset()` rejects any file whose `j.value("__app__", "")` is not
the literal string `"CapstanVar"` and posts `PRESET_IMPORT_WRONG_APP`
to ErrorLog.

## Reserved keys (3)

These three keys are written by `ep_to_json` and consulted by
`import_preset`. Do not change their semantics without bumping
`__version__`.

| Key           | Type    | Default            | Read by                          |
| ------------- | ------- | ------------------ | -------------------------------- |
| `__version__` | integer | `2`                | Diagnostic only (not gated)      |
| `__app__`     | string  | `"CapstanVar"`     | `import_preset` magic check      |
| `__name__`    | string  | `"Untitled"`       | Used as the human preset label   |

`__version__` is currently a free integer under the writer's control;
the loader does not refuse newer versions. Forward-compat policy keeps
the schema **append-only**: new fields can be added without a version
bump because `ep_from_json` reads every field with a
`if (j.contains(k)) v = j[k].get<float>()` guard.

## Domain fields (32)

`ep_to_json` writes 32 domain fields. Defaults below match
`EngineParams` struct initialisers in `include/dsp_types.hpp`.

### Metadata (6) — Phase 2 fields (since `__version__ = 2`)

| Key             | Type   | Default              | Notes                                           |
| --------------- | ------ | -------------------- | ----------------------------------------------- |
| `oxide_type`    | string | `"Fe2O3"`            | Must match a key in `oxide_presets()`           |
| `eq_curve`      | string | `"Legacy ..."`       | Human-readable, NOT enum int. Use `eq_curve_from_name` to parse. |
| `format_id`     | string | `""` (empty)         | Must match a `TapeFormat.id` if non-empty. Stale-id guard fires `PRESET_FORMAT_UNKNOWN` and clears. |
| `format_locked` | bool   | `false`              | Auto-cleared when `format_id` is empty          |
| `lf_trim_db`    | float  | `0.0`                | LF shelf trim dB; loader accepts any float. The UI slider clamps to ±6 dB. |
| `hf_trim_db`    | float  | `0.0`                | HF shelf trim dB; loader accepts any float. The UI slider clamps to ±6 dB. |

### Input (1)

| Key           | Type  | Default | Range          | Notes                              |
| ------------- | ----- | ------- | -------------- | ---------------------------------- |
| `input_gain`  | float | `1.0`   | `0.0 .. 2.0`   | Pre-DSP volume/trim. Unity = 1.0.  |

### Transport (9)

| Key              | Type  | Default | Range            |
| ---------------- | ----- | ------- | ---------------- |
| `ips_base`       | float | `15.0`  | `0.5 .. 30.0`    |
| `motor_health`   | float | `0.0`   | `0.0 .. 10.0`    |
| `motor_drag`     | float | `0.0`   | `0.0 .. 0.9`     |
| `motor_boost`    | float | `0.0`   | `0.0 .. 0.9`     |
| `wow_dep`        | float | `0.2`   | `0.0 .. 30.0`    |
| `flutter_dep`    | float | `0.05`  | `0.0 .. 10.0`    |
| `scrape_flutter` | float | `0.1`   | `0.0 .. 1.0`     |
| `tension_load`   | float | `0.01`  | `0.0 .. 0.12`    |
| `dropout_rate`   | float | `0.0`   | `0.0 .. 1.0`     |

### Magnetic (10)

| Key               | Type  | Default | Range          |
| ----------------- | ----- | ------- | -------------- |
| `drive`           | float | `1.2`   | `1.0 .. 20.0`  |
| `bias`            | float | `1.0`   | `0.5 .. 3.0`   |
| `replay_diff`     | float | `0.3`   | `0.0 .. 1.0`   |
| `asperities`      | float | `0.0`   | `0.0 .. 0.5`   |
| `barkhausen`      | float | `0.0`   | `0.0 .. 0.1`   |
| `crosstalk`       | float | `0.0`   | `0.0 .. 0.5`   |
| `print_through`   | float | `0.0`   | `0.0 .. 0.1`   |
| `demagnetization` | float | `0.0`   | `0.0 .. 0.99`  |
| `oxide_shedding`  | float | `0.0`   | `0.0 .. 1.0`   |

### Electronics (6)

| Key             | Type  | Default      | Range            |
| --------------- | ----- | ------------ | ---------------- |
| `hiss`          | float | `0.001`      | `0.0 .. 0.02`    |
| `hiss_color`    | float | `0.0`        | `0.0 .. 1.0`     |
| `mains_hum`     | float | `0.0`        | `0.0 .. 0.05`    |
| `cutoff_base`   | float | `18000.0`    | `500 .. 22000`   |
| `head_bump`     | float | `0.5`        | `0.0 .. 5.0`     |
| `azimuth_drift` | float | `0.05`       | `0.0 .. 1.0`     |
| `sticky_shed`   | float | `0.0`        | `0.0 .. 1.0`     |

> **`cutoff_base` is bypassed** when `eq_curve != Legacy`. The RBJ low/high
> shelf pair in `ElectronicComponents::process` (under the non-Legacy branch)
> takes the LF/HF time constants from `EQSpec` instead. `cutoff_base`
> survives on disk because round-tripping intact was a Phase 4 design
> goal; ecosystem pre-Phase-2 presets benefit from audibility preservation
> when read without coupling.

### Preserved-but-not-exported (4)

These are session-only state and are intentionally **not** written by
`ep_to_json` — they're populated at runtime in `TapeEngine`:

| Key                | Type   | Default | Owned by                              |
| ------------------ | ------ | ------- | ------------------------------------- |
| `is_reversed`      | bool   | `false` | `TapeEngine::set_reverse()`          |
| `motor_engage`     | float  | `1.0`   | `TapeEngine::dsp_process`            |
| `tape_speed_mult`  | float  | `1.0`   | `TransportDynamics`                   |
| `presaturated`     | bool   | `false` | Set by oversampled-saturation path   |

Looking up these keys in a `.cvpr` file should be treated as **stale**;
they will be ignored on load.

## Schema migration

### Versions

| `__version__` | Source                    | Status   |
| ------------- | ------------------------- | -------- |
| (absent) / 0  | Pre-Phase-2 float-only    | Legacy   |
| 1             | Reserved / never released | n/a      |
| 2             | Phase 2 (current)         | Active   |
| ≥3            | Future                    | Not yet defined |

### Migration rules

- **v0 → v2** (forward): All Phase-2 fields default if absent:
  - `oxide_type` defaults to `"Fe2O3"`
  - `eq_curve` defaults to `"Legacy (single LP)"` (pre-refactor LP path)
  - `format_id` defaults to `""` (legacy free-form)
  - `format_locked` defaults to `false`
  - `lf_trim_db`, `hf_trim_db` default to `0.0`
- **v2 → v2** (identity): No-op.
- **v2 → v3+** (future): Loader is currently permissive — newer
  versions load under the v2 reader and any v3-specific fields are
  ignored. A future version bump that adds required fields should add
  explicit `if (j.contains(k))` checks mirroring the existing pattern.

### Stale-id handling (`format_id` migration)

When `format_id` references a catalog id that no longer resolves in
`tape_formats()` (e.g., a `.cvpr` saved under pre-Phase-4 id
`"Ampex_456_30_NAB"` after the catalog rename to `"Ampex_456_30"`),
the loader clears it and posts `PRESET_FORMAT_UNKNOWN`:

**JSON-import path** (`import_preset` → `ep_from_json`):
1. Read `j["format_id"]` as a string.
2. Look up via `tape_format_by_id()`. If miss, post
   `CV_ERR(PRESET_FORMAT_UNKNOWN, "imported preset '" + name +
   "' references unknown format_id '" + fid + "' — clearing")` and
   leave the field empty.
3. If the id cleared, force `format_locked = false` so the UI
   dropdown reads "Custom (no coupling)" instead of a stale locked
   badge.

**Project / session / direct copy paths** (`CapstanApp::_apply_preset`):
The same guard fires for any `EngineParams` arrival path that bypasses
`ep_from_json` (project load via `load_project`, session-preset apply,
etc.). `name` is the project's name or session-preset's name;
fallback `(unnamed)` if blank.

**Audibility preservation under stale clear:** The coupled values
(`oxide_type`, `eq_curve`, `ips_base`, `bias`) are loaded verbatim
from the JSON snapshot in either path. Audibility is unaffected —
only the dropdown label and the format-id read-out change.

## `format_id` → catalog mapping

The 19 canonical keys below are accepted by `tape_format_by_id()`
(see `include/dsp_types.hpp::tape_formats()`). The catalog is the
single source of truth; this table mirrors it for hand-editing.

The table below renders **22 visible rows** (some presets reuse a
catalog id — e.g. `BASF LH Super` shares `Revox_B77_7_5`, and Dolby
presets reuse `Cassette_Type_I`/`_II` — while Studer appears twice
because it has both IEC and NAB entries). The set of **unique
catalog ids is 19**.

| Built-in preset                  | `format_id`                  | EQ         | Reference machine                  |
| -------------------------------- | ---------------------------- | ---------- | ---------------------------------- |
| Ampex 456 (30 ips)               | `Ampex_456_30`               | AES-30     | Mastering machine post-1980        |
| Ampex 456 (15 ips)               | `Ampex_456_15_NAB`           | NAB-15     | American broadcast convention      |
| Ampex 456 (15 ips)               | `Ampex_456_15_IEC`           | IEC-15     | (dropdown-only; no built-in binds this id) |
| Studer A820 (30 ips)             | `Studer_A820_30_IEC`         | IEC-15     | Swiss mastering convention         |
| Studer A820 (30 ips)             | `Studer_A820_30_NAB`         | NAB-15     | (dropdown-only; no built-in binds this id) |
| Studer A820 (15 ips)             | `Studer_A820_15_IEC`         | IEC-15     | Swiss mastering convention         |
| Studer A820 (15 ips)             | `Studer_A820_15_NAB`         | NAB-15     | (dropdown-only; no built-in binds this id) |
| Otari MTR-90 (30 ips)            | `Otari_MTR_90_30_AES`        | AES-30     | Japanese mastering deck            |
| Otari MTR-90 (15 ips)            | `Otari_MTR_90_15_NAB`        | NAB-15     | Japanese mastering deck            |
| MCI JH-24 (30 ips)               | `MCI_JH_24_30_AES`           | AES-30     | American MCI/Quantegy mastering    |
| Revox B77 (7.5 ips)              | `Revox_B77_7_5`              | IEC-7.5    | Swiss consumer reel                |
| Revox B77 (3.75 ips)             | `Revox_B77_3_75_NAB`         | NAB-3.75   | Swiss consumer reel                |
| BASF LH Super (7.5 ips)          | `Revox_B77_7_5` (shared)     | IEC-7.5    | European consumer reel stock       |
| Maxell UD (7.5 ips)              | `Maxell_UD_7_5`              | IEC-7.5    | Japanese consumer reel              |
| Scotch 226 (7.5 ips)             | `Scotch_226_7_5_NAB`         | NAB-7.5    | American 3M medium-output stock    |
| Tascam 38 (7.5 ips)              | `Tascam_38_7_5_IEC`          | IEC-7.5    | Japanese consumer/semi-pro deck    |
| BBC Radiophonic (7.5 ips)        | `BBC_Radiophonic_7_5_IEC`    | IEC-7.5    | British mastering workshop         |
| Type I (Fe2O3) Normal            | `Cassette_Type_I`            | 3180+120 µs | Compact cassette Type I          |
| Type II Chrome (CrO2)            | `Cassette_Type_II`           | 3180+70 µs  | Compact cassette Type II         |
| Type IV Metal                    | `Cassette_Type_IV`           | 3180+70 µs  | Compact cassette Type IV         |
| Dolby B (Type I)                 | `Cassette_Type_I`            | 3180+120 µs | Same EQ as Type I                |
| Dolby C (Type II)                | `Cassette_Type_II`           | 3180+70 µs  | Same EQ as Type II               |

Naming convention: a `_NAB` / `_IEC` / `_AES` suffix makes the EQ
curve explicit. No-suffix entries (`Ampex_456_30`, `Revox_B77_7_5`,
`Maxell_UD_7_5`) encode the only canonical EQ variant for that
machine/speed. The three Studer alternates (`_30_NAB`, `_15_NAB`,
`Ampex_456_15_IEC`) and other entries without a dedicated built-in
preset are still valid `format_id` values — users can pick them via
the format dropdown even if no preset binds them by default.

## `eq_curve` string encoding

`ep_to_json` writes the human-readable string of the `EQCurve` enum
via `eq_curve_name()`. `ep_from_json` re-parses via
`eq_curve_from_name()`. Both functions live in
`include/dsp_types.hpp`.

| Enum value               | `eq_curve_name()` output              |
| ----------------------- | ------------------------------------- |
| `Legacy`                | `"Legacy (single LP)"`                |
| `Cassette_I`            | `"Cassette I (3180+120 µs)"`          |
| `Cassette_II_IV`        | `"Cassette II/IV (3180+70 µs)"`       |
| `NAB_3_75`              | `"NAB 3.75 ips (3180+90 µs)"`         |
| `NAB_7_5`               | `"NAB 7.5 ips (3180+50 µs)"`          |
| `IEC_7_5`               | `"IEC 7.5 ips (3180+35 µs)"`          |
| `NAB_15`                | `"NAB 15 ips (3180+50 µs)"`           |
| `IEC_15`                | `"IEC 15 ips (3180+35 µs)"`           |
| `AES_30`                | `"AES 30 ips (3180+17 µs)"`           |

`eq_curve_from_name()` is tolerant: it accepts both the canonical
`eq_curve_name()` output and historical short forms (`"Legacy"`,
`"Cassette_I"`, `"NAB_15"`, etc.). Unknown or empty strings default
to `Legacy`.

## Example minimal preset

The smallest valid `.cvpr` file with all defaults:

```json
{
    "__version__": 2,
    "__app__": "CapstanVar",
    "__name__": "Minimal",
    "oxide_type": "Fe2O3",
    "eq_curve": "Legacy (single LP)",
    "format_id": "",
    "format_locked": false,
    "lf_trim_db": 0.0,
    "hf_trim_db": 0.0,
    "input_gain": 1.0,
    "ips_base": 15.0,
    "motor_health": 0.0,
    "motor_drag": 0.0,
    "motor_boost": 0.0,
    "wow_dep": 0.2,
    "flutter_dep": 0.05,
    "scrape_flutter": 0.1,
    "tension_load": 0.01,
    "dropout_rate": 0.0,
    "drive": 1.2,
    "bias": 1.0,
    "replay_diff": 0.3,
    "asperities": 0.0,
    "barkhausen": 0.0,
    "crosstalk": 0.0,
    "print_through": 0.0,
    "demagnetization": 0.0,
    "oxide_shedding": 0.0,
    "hiss": 0.001,
    "hiss_color": 0.0,
    "mains_hum": 0.0,
    "cutoff_base": 18000.0,
    "head_bump": 0.5,
    "azimuth_drift": 0.05,
    "sticky_shed": 0.0
}
```

With format_id binding (e.g., exporting the Studer A820 preset):

```json
{
    "...": "<same 30 keys as above>",
    "oxide_type": "SM911",
    "eq_curve": "IEC 15 ips (3180+35 µs)",
    "format_id": "Studer_A820_15_IEC",
    "format_locked": true,
    "lf_trim_db": 0.0,
    "hf_trim_db": 0.0,
    "drive": 1.04,
    "bias": 1.0
}
```

## Forward-compat policy

1. **New fields are append-only.** Add at the end of `ep_to_json`'s
   output and gate reads with `if (j.contains(k))` so older files
   still load via struct defaults.
2. **Do not reuse semantic meaning for a renamed field.** Add a new
   key under a new name; leave the old key as a dead-code alias with
   a loader-time deprecation warning. *(policy prescription — not
   yet exercised)*
3. **Bump `__version__` only for breaking changes** (mandatory new
   required fields, or removal of legacy fields). Phase 4 introduced
   the stale-id guard without a version bump because the schema bytes
   didn't change — only runtime-load behaviour did.
4. **Catalog renames are runtime-only.** They touch
   `tape_formats()` and the `apply_format()` call sites, but they
   never require a `.cvpr` rewrite because the stale-id guard
   normalises on load.

## File-system layout

`.cvpr` files are searched under `AppDirs::presets()` (per
`include/app_dirs.hpp`) and show up in the preset bar's import/export
dialog (`src/ui.cpp`). Project files (`.cvproject`) embed a separate
schema that holds `EngineParams` directly via
`nlohmann_json`-style serialization; that schema is unrelated to
`.cvpr` and is **not** covered here.

## What is **not** in a `.cvpr` file

The following are session- or project-scoped state that lives in
`.cvproject` files or in `TapeEngine`'s runtime, **not** in a preset.
Exporting a preset that uses any of these will silently drop them.
The 9 categories below are grouped by where they live:

| Missing field / state              | Where it lives                                                |
| ---------------------------------- | ------------------------------------------------------------- |
| `ParamAnim` keyframes              | Project file (`.cvproject`) only — animation state per DSP param |
| `play_head` position               | Project file only                                              |
| `loaded_file` audio path           | Project file (`d.audio_path_abs` / `d.audio_path_rel`)         |
| Audio buffer / stream state        | `TapeEngine::stream`, `audio_data` (runtime, never exported)  |
| `is_reversed`, `motor_engage`      | Set by transport when the preset is applied (runtime)          |
| `tape_speed_mult`, `presaturated`  | Set by transport / over-sampling paths (runtime)               |
| Timeline scroll/zoom              | Project file (animation timeline cursor, view range)           |
| Render job history                 | `src/render_engine.cpp` (session-scoped)                       |
| Session presets                    | `PresetManager::_session` (in-memory only, not persisted)      |

Implication: if a user has keyframed automation on a preset (e.g.,
sweeping `ips_base` from 7.5 to 15 across a song), exporting then
importing that preset will preserve the static engine parameters
(`format_id`, `oxide_type`, etc.) but **lose all keyframes**. The
correct workflow for sharing an animated preset is to save the
`.cvproject` file instead.
