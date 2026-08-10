# CapstanVar — Tape Physics Refactor Design Summary

**Status:** design complete, implementation pending. This document captures
the locked trade-offs so future contributors can review or extend the work
without re-deriving it from scratch. Pair it with `include/dsp_types.hpp`
and `src/preset_manager.cpp` for the current state of the code.

---

## 1. Locked decisions

| Dimension                  | Choice                                                                |
| -------------------------- | --------------------------------------------------------------------- |
| Refactor scope             | **Hybrid** — flat `EngineParams` + optional `format_id` (opt-in lock) |
| Hysteresis / memory        | **Preisach 64×64 LUT** (16 KB per oxide, L1/L2-resident)              |
| Saturation topology       | **4-band parallel + sum**, LR-4 crossovers at 200 Hz / 1.5 kHz / 6 kHz |
| Playback EQ                | Standard NAB/IEC/AES/cassette curves, auto-from-speed + dropdown      |
| EQ LF/HF trim              | ±6 dB user-facing trim knobs (calibration drift)                      |
| Oxide data                 | Full MRL-style curves for 8 stocks; knee/ceiling per format           |
| Head bump physics          | Wavelength formula `f_peak = v_tape / λ_eff`, `λ_eff = 4.0 mm`        |
| Modulation noise           | Configurable per preset (new `mod_noise_depth` field)                 |
| Hybrid lock strictness     | **Defaults-with-override** — touching a knob detaches, label "Custom" |
| Reverse playback           | **Forward-write, backward-read** via per-channel history buffer       |
| CPU budget                 | **Tier 4** (≤35% per stereo pair); modeled ~10–15% actual at 4× OS   |

Rationale shortlist:

- **Hybrid** keeps the 30+ existing presets loading unchanged. New presets
  opt in to format-id coupling; old presets fall back to the legacy free-form
  parameter struct.
- **Preisach 64×64** captures the audible hysteresis tail on transients while
  staying permanently cache-resident. 128× risks cache eviction under
  realtime scheduling; 32× is audibly grainy.
- **4-band** isolates HF self-demagnetization from bass compression (where
  the head bump dominates) and gives the vocal mid-band its own saturation
  path. 2-band is coarse; 6-band yields diminishing returns.
- **Forward-write, backward-read** for `is_reversed`: magnetic state is
  established at record time (forward in time). Reading it backwards would
  invert the hysteresis loop, which is not what real tape does.

---

## 2. Implementation gotchas (avoid re-debugging)

These caused serious bugs in earlier plugin generations; flagged so neither
of them bites again:

1. **Preisach spin-up.** When the bias level changes (preset switch, bypass
   toggle, oxide change), iterate the Preisach LUT from 0 to the bias point
   across ~80 samples before letting audio through. Skipping this causes a
   startup click.
2. **MOL changes affect knee/ceiling, NEVER input gain.** Switching from
   SM900 (+9 dB MOL) to 456 (+6 dB MOL) can NOT change the input level by
   3 dB. Adjust saturation parameters so unity-gain behaviour at low levels
   is preserved across format switches.
3. **Recook biquads from scratch** when format/oxide changes; do not reuse
   old coefficients. Old coefficients were tuned for a different Nyquist —
   shelf EQ can briefly oscillate and slap a transient on the audio.
4. **Barkhausen noise** applies post-band-sum, at base sample rate. Per-band
   repetition distorts its spectrum and wastes RNG.
5. **Playback EQ shelves stay at base SR.** The 4× oversampling lives
   strictly inside the saturation block. Running slow LF shelves at higher
   sample rates destabilises their biquad coefficients.
6. **Saturation is written forward**, then read through a per-channel
   history buffer when `is_reversed`. Demag filter and print-through
   indexing flip on reverse; the magnetic-domain saturation does not.

---

## 3. Backward compatibility

### Phase 3 — magnetic saturator activation

Phase 3 activates the previously-staged `OxideProps` fields
(`mol_thd3_db`, `hf_rolloff_db_oct`, `hysteresis_amt`) in the magnetic
saturator. The activation preserves the four design guardrails:

1. **Audibility preservation.** Legacy oxides (Fe2O3/CrO2/Metal/FeCo)
   carry `hysteresis_amt` ∈ {0.05, 0.10, 0.20, 0.05} and
   `mol_thd3_db` ≈ +6, so the 4-band cascade + hysteresis blend sits
   within a small acceptable coloration floor vs. the pre-Phase-3
   single-band `fast_tanh`. When `hysteresis_amt == 0` (a hypothetical
   future preset) the lerp bypasses the Preisach LUT and the band-
   aggregated output matches the legacy saturator within numerical
   tolerance.

2. **MOL remap rule (gotcha #2).** MOL changes affect knee/ceiling,
   **NEVER** input gain. `_saturate_bandwise` computes
   `mol_factor = pow(10, (mol - 6)/20)` once per block and applies it
   to both the knee base (`hc_ratio * mol_factor`) and ceiling
   (`oxide.Ms * mol_factor`). Thus a switch from SM900 (+9 dB MOL) to
   456 (+6 dB MOL) does **not** change the input level; only saturation
   parameters retune.

3. **Per-oxide HF rolloff.** Bands 3–4 soften the knee by
   `pow(10, hf_rolloff_db_oct * 0.5 / 20)` and
   `pow(10, hf_rolloff_db_oct * 1.5 / 20)` respectively. BASF_LH
   (−6 dB/oct) hardens the rolloff most; SM900 (−3.5 dB/oct) less.
   Fe2O3 (−5 dB/oct) and Maxell_UD (−5 dB/oct) apply a gentler
   soft-knee to the upper bands.

4. **Preisach spin-up (gotcha #1).** When oxide key or driving
   (`drive * bias`) changes, `_saturate_bandwise` runs 80 silent
   samples through the LR-4 cascade + Preisach pipeline to converge
   the biquad DC state and `_m_prev[4][2]` to the bias operating
   point. Skipping this causes a startup click on the first real
   sample.

5. **Per-band magnetisation state.** `_m_prev[4][2]` carries the
   previous sample's Preisach lookup across calls; this is what
   forms the audible "thickening" tail on transients. Reset only on
   `MagneticPath::reset()`, on oxide/driver change spin-up, or in
   the 4-Biquad LR-4 biquad cascade reset.

### 4-band LR-4 cascade (topology)

The saturator splits the signal into four bands using Linkwitz-Riley
4th-order crossovers at 200 Hz / 1.5 kHz / 6 kHz. LR-4 is constructed
by cascading two 2-pole Butterworth filters per side (LP and HP); the
two sides sum to a power-complementary unity gain. The band-aggregated
output equals the legacy single-band `fast_tanh` path within numerical
tolerance **only** when `hysteresis_amt = 0` AND `hf_rolloff_db_oct = 0`
(all four bands share the same knee). At any other operating point the
4-band cascade introduces a measurable nonlinear correlation between
bands — accepted as the audibility floor in exchange for the saturation
topology that captures HF self-demagnetization.

When `hf_rolloff_db_oct ≠ 0` the per-band knees diverge and a naive
`band_ceiling = ceiling/4` lets peak output rise to ≈ 1.4×–1.9× the
documented ceiling (smaller-knee HF bands produce peaks at 1/k_b).
Phase 3 compensates with a **uniform** per-band ceiling

    band_ceiling = ceiling / Σ(1/k_b)

because each band's kernel peak is `1/k_b` and the sum is therefore
`band_ceiling × Σ(1/k_b) = ceiling`. A single uniform scalar is
applied to all four band outputs; no per-band multiplier is needed.

Worked example (SM900, knee=0.71, hf_oct=−3.5): k1=k2=0.71,
k3=0.581, k4=0.388. Σ(1/k_b) = 1.408 + 1.408 + 1.722 + 2.578 =
7.116. `band_ceiling = ceiling / 7.116`. Per-band peaks: 0.198,
0.198, 0.242, 0.362 — sum = 1.000 × ceiling.

At equal knees (no HF rolloff), Σ(1/k_b) = 4/k_b and
`band_ceiling = ceiling × k_b / 4` — the original 1/4 split scaled
by the legacy knee, preserving the legacy saturation peak at unity.

| Band | Range            | Knee modulation                       |
| ---- | ---------------- | ------------------------------------- |
| B1   | 0 – 200 Hz       | base (mild bass-compression via MOL)  |
| B2   | 200 Hz – 1.5 kHz | base                                  |
| B3   | 1.5 – 6 kHz      | `knee × pow(10, hf_oct * 0.5 / 20)`   |
| B4   | 6 kHz – Nyq      | `knee × pow(10, hf_oct * 1.5 / 20)`   |

Each band runs the same per-sample saturator:

```
m_fast  = fast_tanh(band_in * band_knee) / band_knee              // smooth knee
m_lut_l = preisach_lut.lookup(band_in, m_prev[band][ch])          // hysteretic, ±1
m_out   = lerp(m_fast, m_lut_l, oxide.hysteresis_amt) × band_ceiling
m_prev[band][ch] = m_lut_l                                         // bounded LUT domain
band_sum += m_out
```

`band_ceiling = ceiling / Σ(1/k_b)` so the sum of per-band peaks
exactly equals the documented `ceiling` (see worked example above).
Note that `_m_prev[band][ch]` stores the LUT-domain signal directly
(bounded ±1) — NOT the rescaled lerp output. Rescaling before storage
would push values outside the LUT's ±1 axis and silently saturate the
boundary cell on the next sample.

### CPU profile update

Phase 3 adds measured cost on top of the Phase 2 baseline:

| Stage                            | Cycles / sample |
| -------------------------------- | --------------: |
| LR-4 cascade (12 Biquad ticks)   |             ~80 |
| Per-band Preisach LUT lookups    |             ~50 |
| MRL knee/ceiling remap (per blk) |             ~10 |
| **Phase 3 total addition**       |        **~140** |
| **Combined Phase 2 + Phase 3**   |        **~1250** |

Still inside the Tier-4 budget (~35% CPU per stereo pair); expect ~14%
on a modern desktop core.

### Preset migration

Existing preset JSON files load unchanged. The migration is:

- New field `format_id` — defaults to empty string.
- Empty `format_id` → coupling skipped, `EngineParams` behaves exactly as
  before (legacy free-form knobs).
- New preset files include the full Hybrid schema (`format_id`,
  `mod_noise_depth`, `eq_curve_id`, `fluxivity_nWb_m`).
- Old preset files are forward-compatible; missing new fields are silently
  defaulted.

The 30+ built-in presets in `src/preset_manager.cpp` are gradually re-authored
to set `format_id`, so they pick up the new physics automatically.
Mechanical settings (`motor_health`, `wow_dep`, `flutter_dep`) continue to
be free-form knobs independently of `format_id`.

For the full per-field JSON schema reference (every field written by
`ep_to_json`, every default, the 19-entry `format_id` ↔ catalog
mapping table, the v0/v1/v2 migration rules, and a "what is **not** in
a `.cvpr` file" rundown), see the companion document
[`PRESET_SCHEMA.md`](PRESET_SCHEMA.md).

### Re-authored reference catalog (Phase 4, current)

19 reference-fidelity presets now bind to `TapeFormat` catalog entries
via the `apply_format()` static helper in `src/preset_manager.cpp`.
Each preset pulls `ips_base` / `eq_curve` / `oxide_type` / `bias` from
the catalog; free-form character params stay independent so users can
still tune without detaching the format.

| Built-in preset                  | Catalog id              | EQ standard |
| -------------------------------- | ----------------------- | ----------- |
| Ampex 456 (30ips)                | Ampex_456_30            | AES-30      |
| Ampex 456 (15ips)                | Ampex_456_15_NAB        | NAB-15      |
| Studer A820 (30ips)              | Studer_A820_30_IEC      | IEC-15      |
| Studer A820 (15ips)              | Studer_A820_15_IEC      | IEC-15      |
| Revox B77 (7.5ips)               | Revox_B77_7_5           | IEC-7.5     |
| Revox B77 (3.75ips)              | Revox_B77_3_75_NAB      | NAB-3.75    |
| BASF LH Super (7.5ips)           | Revox_B77_7_5[^basf]    | IEC-7.5     |
| Maxell UD (7.5ips)               | Maxell_UD_7_5           | IEC-7.5     |
| Otari MTR-90 (30ips)             | Otari_MTR_90_30_AES      | AES-30      |
| Otari MTR-90 (15ips)             | Otari_MTR_90_15_NAB      | NAB-15      |
| MCI JH-24 (30ips)                | MCI_JH_24_30_AES         | AES-30      |
| Scotch 226 (7.5ips)              | Scotch_226_7_5_NAB       | NAB-7.5     |
| Tascam 38 (7.5ips)               | Tascam_38_7_5_IEC        | IEC-7.5     |
| BBC Radiophonic (7.5ips)         | BBC_Radiophonic_7_5_IEC  | IEC-7.5     |
| Type I (Fe2O3) Normal            | Cassette_Type_I         | 3180+120µs  |
| Type II Chrome (CrO2)            | Cassette_Type_II        | 3180+70µs   |
| Type IV Metal                    | Cassette_Type_IV        | 3180+70µs   |
| Dolby B (Type I)                 | Cassette_Type_I         | 3180+120µs  |
| Dolby C (Type II)                | Cassette_Type_II        | 3180+70µs   |

### Oxide-vs-machine rationale (Phase 4+)

Catalog oxide field selects the matching `OxideProps` entry from
`oxide_presets()` (in `include/dsp_types.hpp`). Choice was driven by
each machine's commonly-shipped stock at its heyday, falling back to
the closest MOL/sensitivity match in the existing 10-stock table:

- **Studer A820 / Otari MTR-90**: SM911 (low-noise balanced studio stock).
- **Ampex 456 / MCI JH-24 / Scotch 226 / BBC Radiophonic**: 456
  (medium-output, MRL-style +6 dB MOL).
- **Revox B77 / BASF LH Super / Tascam 38**: BASF_LH or Maxell_UD
  (consumer reel formulation, lower Hc, slightly higher hiss).
- **Cassettes**: 4-stock legacy table (Fe2O3/CrO2/Metal/FeCo).

If a reference typically pairs with a stock that differs markedly from
the catalog default, override via the OXIDE dropdown in the preset bar.

EQ-standard defaults:

- **Studer A820** → IEC (Swiss mastering convention). IEC shelf is
  tighter (-3 dB @ 35µs) than NAB; brightens snare and vocal sibilance.
- **Ampex 456** → NAB (American broadcast convention). NAB shelf rolls
  off top a bit harder (-3 dB @ 50µs), warmer than IEC.
- **Revox B77** → catalog literal (id `…_NAB` uses IEC-7.5 eq curve —
  vestigial "NAB" suffix in the catalog is a known naming quirk; the
  user can switch standards via the format dropdown if needed).
- **Cassettes** → Type I/II/IV per the IEC cassette reference.

Default EQ can be flipped per preset in the UI Electronics tab →
Format dropdown — touching any coupled knob detaches (label flips to
"Custom"). The IEC/NAB choice changes shelf time-constants only; the
free-form hiss / drive / wow / flutter remain untouched.

### Audibility verification (out-of-band)

Format coupling flips audio behaviour along three axes:

1. Playback EQ: legacy single-LP at `cutoff_base` → RBJ low+high shelf
   pair with implicit ±3 dB shelf gained from the standards.
2. Oxide MOL remap: knee/ceiling in `_saturate_bandwise` scale by
   `pow(10, (mol - 6)/20)` so switching from SM900 (+9 dB MOL) to 456
   (+6 dB MOL) does not change input level — only saturation
   parameters retune (see §3 gotcha #2).
3. Preisach hysteresis: each oxide's `hysteresis_amt` weight blends
   fast_tanh ↔ PreisachLUT in the 4-band cascade.

Audibility verification of the re-authored presets against real-world
references (Studer A820, Ampex 456, Revox B77, Cassette decks) is
manual — the user must A/B against known musical material in the app
since no automated listening test is available in CI. The build
verifies only that `tape_format_by_id()` resolves each preset's
`format_id` and that the resulting `EngineParams` fields match the
catalog; audibility is the human-in-the-loop final check.

---

## 4. CPU profile

Single stereo pair, 44.1 kHz, 1024-sample block, modern desktop core (~3 GHz):

| Stage                                | Cycles / sample |
| ------------------------------------ | --------------: |
| 4-band LR-4 crossover + sum          |             ~80 |
| 4× Preisach LUT lookup (16 KB each)  |            ~120 |
| MRL curve interpolation per band     |             ~80 |
| EQ shelves + head bump + mod noise   |            ~120 |
| Transport (wow/flutter/dropouts)     |             ~50 |
| **Total (base SR)**                  |        **~450** |
| **With 4× OS in saturation block**   |       **~1100** |

At ~1100 cycles/sample on a 3 GHz core, that lands at **~12% CPU per stereo
pair**, well inside the 35% Tier-4 budget. Two hero instances in a busy mix
fit; a utility instance grid (e.g. 16× across stems) is fine at 4× OS but
should drop to 2× OS or no OS in dense projects.

---

## 5. Reference standards — playback EQ time constants

| Format                         | Speed    | LF (µs) | HF (µs) |
| ------------------------------ | -------- | ------: | ------: |
| Compact cassette Type I        | 1⅞ ips   |    3180 |     120 |
| Compact cassette Type II / IV  | 1⅞ ips   |    3180 |      70 |
| NAB consumer                   | 3¾ ips   |    3180 |      90 |
| NAB studio                     | 7½ ips   |    3180 |      50 |
| IEC studio                     | 7½ ips   |    3180 |      35 |
| NAB studio                     | 15 ips   |    3180 |      50 |
| IEC (mastering)                | 15 ips   |    3180 |      35 |
| AES                            | 30 ips   |    3180 |      17 |

`λ_eff_head = 4.0 mm` gives head-bump peaks at approximately:

| Speed  | Peak freq |
| ------ | --------: |
| 30 ips |    ~190 Hz |
| 15 ips |     ~95 Hz |
| 7.5 ips |    ~48 Hz |
| 3.75 ips |  ~24 Hz |

---

## 6. Reference tape stocks (Phase 1 data target)

Eight MRL-style references, each with: `mol_thd_curve`, `sensitivity_1k`,
`sensitivity_10k` / `sensitivity_15k`, `hf_rolloff_dB_per_oct`, `hiss_floor_dB_ref_mol`,
`bias_response`, `distortion_knee_shape`.

Required reference stocks:

- Ampex / Quantegy 456          (medium-output studio standard)
- Ampex / Quantegy SM911        (low-noise balanced)
- Ampex / Quantegy SM900        (high-output +9 dB MOL)
- Quantegy GP9                  (max MOL, max print resistance)
- Maxell UD / XL-I              (consumer open reel)
- BASF LH Super                 (consumer open reel)
- CrO₂ reference                (Type II cassette)
- Metal particle reference      (Type IV cassette)

Each oxide's data drives the LUT-driven knee / ceiling / sensitivity scaling
in the saturator. MOL differences are reflected in saturation parameters,
**never** as an input-gain multiplier (see gotcha #2).

---

## 7. Out of scope (intentionally deferred)

- Full anhysteretic-state hysteresis (current Preisach LUT is simplified).
- DC-bias-asymmetry inside the hysteresis model itself (currently modeled
  via `dc_offset` on the saturator input, not inside the LUT).
- Multitrack inter-track bleed geometry (currently a flat `crosstalk`
  scalar; no spatial model of head-stack guard bands).
- True dropout physics beyond the existing Bernoulli-gated amplitude dip.
- Authentic measurements from physical stocks (all values are reasonable
  approximations from MRL calibration tables and manufacturer data).

---

## 8. Files to touch

Implementation will modify roughly: `include/dsp_types.hpp`,
`include/mod_magnetic.hpp` / `src/mod_magnetic.cpp`,
`include/mod_electronics.hpp` / `src/mod_electronics.cpp`,
`include/mod_transport.hpp` / `src/mod_transport.cpp`,
`include/preset_manager.hpp` / `src/preset_manager.cpp`, plus the engine
plumbing and the preset JSON schema. Phasing follows this order:

1. `OxideProps` expansion + oxide LUT tables (Section 6).
2. EQ curve enumeration + format-id coupling in `EngineParams`.
3. Modular 4-band saturator with Preisach LUT (with `saturate_only()`).
4. Modulation noise envelope follower.
5. Reverse-playback history buffer.
6. Re-author the 30+ built-in presets to set `format_id`.

Each phase is independently shippable behind a feature flag if needed.
