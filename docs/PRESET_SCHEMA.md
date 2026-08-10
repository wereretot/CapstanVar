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
| Phase 5 compat          | 6 env fields appended (domain → 38 fields). `__version__` stays at 2 (append-only). |

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

## Domain fields (38)

`ep_to_json` writes 38 domain fields. Defaults below match
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

### Environment (6) — Phase 5 fields (append-only; `__version__` stays at 2)

These six fields were appended to the `EngineParams` struct in
Phase 5 (`include/dsp_types.hpp`) and are read/written from the same
`F()` macro path as the magnetic/electronic/transport numerics. None
of the 30+ existing pre-Phase-5 presets set these — round-tripping
defaults them at load time, which is what makes the audibility
floor identical to pre-Phase-5 (see "Option (a) instant-override
preservation" below).

| Key                     | Type    | Default | Range             | Persistence | Notes                                            |
| ----------------------- | ------- | ------- | ----------------- | ----------- | ------------------------------------------------ |
| `env_temperature_c`     | float   | `20.0`  | `0.0 .. 60.0`     | Runtime     | Ambient temperature in °C. Reference: 20.       |
| `env_humidity_pct`      | float   | `50.0`  | `0.0 .. 100.0`    | Runtime     | Relative humidity %. Reference: 50.             |
| `env_age_acceleration`  | float   | `1.0`   | `0.1 .. 10000.0`  | Runtime     | Age-mult multiplier. Wall-clock x this = sim-age. |
| `env_age_seconds`       | double  | `0.0`   | n/a               | Persisted   | Running total of `block_secs × α × env_rate`.    |
| `env_tape_health`       | float   | `1.0`   | `0.0 .. 1.0`      | Persisted   | `exp(-age_seconds / half_life)`. Read-only in UI. |
| `env_failure_modes`     | uint32  | `0`     | bit-set           | Persisted   | Bit-set of `TapeFailureMode` flags (see catalog below). |

> **Reference state**: with the four field defaults above (`20°C, 50%`,
> α=1, age=0), `env_derived == 0` for every downstream field, so the
> modified `EngineParams` passed to magnetic / electronic / transport
> modules reproduces the pre-Phase-5 output within numerical
> tolerance. Audibility verification at reference state is therefore
> a no-op (see "Audibility preservation", §X of
> `TAPE_PHYSICS_REFACTOR.md`).

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

## Phase 5 storage presets (4 built-in)

Four bundled storage environments ship with Phase 5 inside
`src/preset_manager.cpp`. They are `.cvpr` files like any other
preset, but they only set the two runtime env knobs (temperature,
humidity) — `env_age_acceleration`, `env_age_seconds`,
`env_tape_health`, and `env_failure_modes` stay at their defaults
and are user-driven from the Environment tab.

| Storage environment  | `env_temperature_c` | `env_humidity_pct` | Aging impact (at α=1.0)                                                       |
| -------------------- | -------------------: | -----------------: | ----------------------------------------------------------------------------- |
| Controlled           |                 20.0 |               50.0 | Reference state — `env_derived ≈ 0` for every downstream field.               |
| Consumer Closet      |                 25.0 |               60.0 | Mild — hiss grows after ~3 years; dropout creeps after ~5.                    |
| Hot Attic            |                 35.0 |               70.0 | Aggressive — sticky shed becomes audible within ~1 year; mould risk.          |
| Cold Warehouse       |                 10.0 |               40.0 | Below reference — hum_rate = 0, temp_rate = 0.5. Wow/flutter drift contributes the only audible change (≈ 0.5 each at T=10).       |

EQ, oxide, format_id, and bias are **untouched** by switching storage
presets — audibility for any non-environment parameter is preserved
across swaps.

**Worked example** (Hot Attic at α=1000.0, 60 wall-clock seconds of
audio playback; `dsp_process` called once per audio block):

```
temp_rate       = pow(2, (35 - 20)/10)                    = 2.83
hum_rate        = max(0, (70-50)/50) + max(0, (70-70)/30)^2 = 0.40
env_age_seconds += 60 × 1.0 × 2.83 × 0.40                 = 67,920 s
env_age_years   ≈ 67,920 / 31,557,600                    ≈ 0.00215 yr
env_tape_health = exp(-0.00215 / kEnvHalfLifeYears)      ≈ 0.9998
hiss add.       ≈ 0.01 × (exp(0.00215/20) - 1.0)         ≈ 1.1e-6
```

The hiss addition is below `hiss`'s default value of 0.001 by four
orders of magnitude — inaudible against the existing hiss floor on
either side. Audible degradation with Hot Attic requires roughly
`env_age_seconds > 3 × 31,557,600` (i.e., > 3 sim-years).
Wall-clock time-to-threshold at Hot Attic
(temp_rate = 2.83, hum_rate = 0.40):

| α       | Wall-clock seconds | Wall-clock days    |
| ------- | ------------------ | ------------------ |
| 1       |       83,576,000   |       ~2.65 yr     |
| 10      |        8,358,000   |       ~96.7 d      |
| 100     |          835,800   |       ~9.68 d      |
| 1000    |           83,580   |       ~23.2 h      |
| 10000   |            8,358   |       ~2.32 h      |
| 100000  |              836   |       ~14 min      |

So at α=1000 the audibility threshold is reached in roughly
23 wall-clock hours; at α=10000 in roughly 2.3 hours; at α=100000
in under 15 minutes. The audibility-preservation audibility floor
(`effective_p == base_p`) still holds at any sim-age the user can
realistically simulate via the Environment tab.

**Variable shorthand** used here (defined fully in
[`TAPE_PHYSICS_REFACTOR.md` §X "Phase 5 aging math"](TAPE_PHYSICS_REFACTOR.md)):

- `T_c`    ≡ `env_temperature_c`
- `RH_pct` ≡ `env_humidity_pct`
- `α`      ≡ `env_age_acceleration`
- `age`    or `age_years` (without a `_seconds` suffix) implicitly
  converts `env_age_seconds` via 1 yr = 31,557,600 s.
- `kEnvHalfLifeYears = 10.0` (compile-time constant in
  `include/mod_environment.hpp`).

## Option (a) instant-override preservation

Phase 5's `EnvironmentModule` (new file `include/mod_environment.hpp`
+ `src/mod_environment.cpp`) sits at the top of
`TapeEngine::dsp_process` and returns an `EngineParams effective_p`
that the downstream modules consume instead of the user's
`base_p`. The recipe is per-field:

```
effective_p.x = max(base_p.x, env_derived_x)        // additive damage fields
effective_p.bias *= max(0.5, 1.0 - (T_c - 20) * 0.005)  // thermal drift (multiplicative)
effective_p.wow_dep   += |T_c - 20| × 0.05          // thermal-expansion-driven mechanical
effective_p.flutter_dep += |T_c - 20| × 0.05
effective_p.tension_load  += (max(0, T_c - 20) / 100) × (max(0, T_c - 20) / 100)  // ((T_c-20)/100)²; cold has no contrib
effective_p.head_bump *= env_tape_health           // decays as tape dies
effective_p.eq_curve     = base_p.eq_curve         // IMMUNE (preserves standard playback EQ)
effective_p.format_id    = base_p.format_id        // IMMUNE (preserves catalog coupling)
effective_p.oxide_type   = base_p.oxide_type       // IMMUNE (preserves stock identity)
effective_p.input_gain   = base_p.input_gain       // IMMUNE (unity gain path)
```

Consequences:

1. **Reference state is silent.** At 20°C / 50% RH / α=1.0 / age=0,
   every `env_derived_x == 0` and every thermal shift is 0, so
   `effective_p == base_p` within numerical tolerance. Existing
   presets sound identical to pre-Phase-5.
2. **Hand-tuned damage survives.** A user who sets
   `sticky_shed = 0.6` keeps 0.6 as the floor; env only adds **on
   top of** their entry, never replaces it.
3. **Append-only schema.** No field is renamed or removed, so the
   loader's `if (j.contains(k))` pattern defaults new keys on read.
   `__version__` stays at 2.
4. **EQ shelves never drift.** Even at Hot Attic temperature with 10
   simulated years of age, the playback EQ shelves stay at the
   catalog-derived time constants — audibility changes come from
   hiss/oxide/dropout/sticky-shed growth, not from EQ drift.
5. **`oxide_type` is immune to env.** Switching 456 → SM911 with the
   same env state changes audible character via the magnetic
   saturator's MOL remap (Phase 3 gotcha #2), not via env.

If any future Phase-N (N ≥ 6) ever needs to bend one of the IMMUNE
rules above — e.g., let env drift EQ shelves — that work must add a
new `env_*` field and bump `__version__` to 3 (per the "Phase 5
audibility-preservation" policy in §X of
`TAPE_PHYSICS_REFACTOR.md`).

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
    "sticky_shed": 0.0,
    "env_temperature_c": 20.0,
    "env_humidity_pct": 50.0,
    "env_age_acceleration": 1.0,
    "env_age_seconds": 0.0,
    "env_tape_health": 1.0,
    "env_failure_modes": 0
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
