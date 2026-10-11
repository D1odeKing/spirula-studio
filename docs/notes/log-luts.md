# Built-in log LUTs

A built-in LUT is a formula, not a maker's table. `app/LogLuts.cpp` evaluates
it on a 33^3 lattice the first time a run asks for it
(`cube_lut_cached("builtin:<id>")`); from there it is a `.cube` like any other
to the GPU conversion, ffmpeg's `lut3d` and the CPU preview. Each one is

1. the maker's **published** log curve (code value / 1023 -> scene linear, 18%
   grey at 0.18) and gamut (-> linear Rec.709, D65), and
2. a **look** fitted by `tools/fit_log_luts.py` to what the maker's own
   Rec.709 LUT does, chiefly to natural photographs:

   ```
   out = clamp( M2 * T_w( M1 * lin ) + black )
   ```

   six mixes of linear Rec.709 (`M1`, 6x3), each through its own monotone
   curve `T_w` -- a PCHIP through 15 knots one stop apart, from 2^-8 to 2^6
   times 18% grey, on a log2 axis -- mixed back to RGB (`M2`, 3x6), plus the
   look's lifted black: 129 numbers.

GoPro's two need no fit. The MAX 2's technical LUT is exactly GP-Log
(`(400^x - 1) / 399`, GoPro Labs' log base 400), the BT.709 OETF, then a 3x3
matrix whose entries are whole 512ths; the tool recovers the matrix and checks
it against GoPro's file to its own 5-decimal rounding. GP-Log2 is GoPro Labs'
published generator (gopro.github.io/labs/gplog2, Apache-2.0,
`LICENSES/Apache-2.0-GoPro-Labs.txt`) at its defaults -- +1.8 EV, Rec.709, its
highlight rolloff -- and cube_lut_test holds the C++ port to the `.cube` that
GoPro's own JavaScript writes, run by the tool under QuickJS.

Nothing of a maker's LUT ships: not its samples, not a compressed copy. The
tool downloads each LUT to fit against and keeps only the 129 numbers. That is
also the whole of what Canon's and Nikon's terms come into: their LUTs are
used, not redistributed.

| id | fitted to | curve, gamut | held-out photos, mean / p99 | whole lattice, mean / p99 |
|---|---|---|---|---|
| `dji-dlog-m` | DJI Mavic 3 D-Log M to Rec.709 V1 -- the same file for Mini 4 Pro, Air 3/3S, Avata 2, Flip, O4 Air Unit, Action 4/5 Pro, Pocket 3, Osmo 360, Nano | D-Log, D-Gamut | 0.23 / 1.0 | 9.0 / 44 |
| `dji-dlog-m-2025` | DJI Mavic 4 Pro D-Log M to Rec.709 V1 -- the same for Mini 5 Pro, Avata 360, Lito X1 | D-Log, D-Gamut | 0.23 / 1.2 | 14.4 / 56 |
| `dji-dlog-m-action6` | DJI Osmo Action 6 D-LogM to Rec.709 | D-Log, D-Gamut | 0.53 / 2.2 | 11.0 / 32 |
| `dji-dlog` | DJI Mavic 3 D-Log to Rec.709 V1 -- the Zenmuse X9's carries the same data | D-Log, D-Gamut | 1.03 / 3.6 | 17.8 / 100 |
| `insta360-ilog` | Insta360 X6 I-Log to Rec709 BT1886 (s33, v2) -- Luna's is the same | I-Log, BT.2020 | 1.57 / 9.9 | 25.0 / 75 |
| `insta360-x5-ilog` | Insta360 X5 I-Log To Rec.709 V1.0 | I-Log, BT.2020 | 1.60 / 11.5 | 20.5 / 146 |
| `insta360-acepro2-ilog` | Insta360 Ace Pro 2 I-Log To Rec.709 V1.0 -- GO Ultra's is the same | I-Log, BT.2020 | 0.60 / 2.3 | 9.2 / 55 |
| `gopro-gplog` | exact: GoPro MAX2 GP-Log to Rec.709 Technical | GP-Log | 5e-6 of a code value | the same |
| `gopro-gplog2` | exact: GoPro Labs' GP-Log2 generator | GP-Log2, BT.2020 | 5e-7 | the same |
| `sony-slog3` | Sony S-Log3/S-Gamut3.Cine to s709 V200 | S-Log3, S-Gamut3.Cine | 0.42 / 2.7 | 10.6 / 48 |
| `panasonic-vlog` | Panasonic VLog_to_V709_forV35 | V-Log, V-Gamut | 0.08 / 0.3 | 1.4 / 7 |
| `fujifilm-flog2` | Fujifilm X-H2S F-Log2/F-Gamut to WDR BT.709 -- identical for X-T5 and X-S20 | F-Log2, F-Gamut | 0.75 / 4.0 | 18.4 / 53 |
| `fujifilm-flog` | Fujifilm X-H2S F-Log/F-Gamut to WDR BT.709 | F-Log, F-Gamut | 0.38 / 1.7 | 9.6 / 59 |
| `canon-clog3` | Canon Cinema Gamut / Canon Log 3 to BT.709 Wide DR (Ver.2.0) | Canon Log 3, Cinema Gamut | 1.19 / 8.9 | 47.2 / 127 |
| `nikon-nlog` | Nikon N-Log BT2020 to REC709 BT1886 | N-Log, N-Gamut | 0.29 / 1.6 | 12.3 / 37 |

Errors are in 8-bit levels, the worst channel per colour. "Held-out photos"
are the Kodak images the fit never saw; "whole lattice" is every one of the
33^3 log code triples, most of which no photograph reaches -- a fully
saturated, overexposed primary, a code below the curve's black. For scale,
DJI's own D-Log M LUTs for its 2023 and 2025 cameras differ by 16.6 / 86 over
the whole lattice. Canon's lattice numbers are the largest because Cinema
Gamut's corners are far outside anything a camera records (its green primary
sits at y = 1.14).

## The fit

- **Content.** The 24 Kodak suite images (r0k.us), 128 px, sRGB decoded to
  linear Rec.709 as stand-in scene light, each image's median grey put at 18%
  and one stop either side. Odd images fit, even images are held out. The fit
  takes 20 000 of those colours at full weight; 10 000 more pushed 1.5x
  further from grey in the camera's gamut, across +-2 stops, at half weight,
  for the wider gamut a camera sees than an sRGB photo holds; and 6 000
  lattice points at a tenth, so a colour no photograph reaches stays near the
  maker's rendering instead of wherever the curves extrapolate. Fitted on the
  photographs alone the held-out errors were up to half a level lower, but the
  whole lattice was 16-92 levels off on average -- a saturated LED or sign in
  real footage would have come out garish.
- **Codes.** Each colour goes to the camera's gamut and through the published
  curve's inverse: the code values that camera would write. The target is the
  maker's LUT at those codes, interpolated tetrahedrally as Spirula applies it.
- **Grey.** The grey axis, 257 codes end to end, is in the data at five
  times weight, and a channel falling below its own running maximum along it
  is penalised: the mixes' signed weights otherwise folded the neutral
  response back by up to 18 levels where photographs thin out, near clip and
  just above black. The `black` offset is there because some looks lift black
  (Ace Pro 2's by 14 levels) and every curve starts from zero.
- **Solver.** Levenberg-Marquardt on the squared error, then on the soft-L1
  loss (scale 2 levels) by reweighting, three to five minutes a LUT on a CPU;
  the tool keeps each fit until the LUT or the script changes. A light
  penalty on each curve's second difference, and on knots above 1.25, keeps
  the knots no colour reaches -- far below black, past +4 stops -- in line
  with their neighbours. Without it they ran off to 10^5 where the output
  clamp hid them, and Panasonic's held-out p99 was 41 levels, not 0.3.

Measured instead on frames of two real SfM datasets shot on a phone, every
mean is at most 0.15 of a level above its held-out number (the X5's is lower,
1.25 against 1.60), and every p99 at most 3.3 levels above, except the X6's
I-Log at 16 against 9.9: the Kodak stand-in does not flatter them.

## What else was measured

Mean / p99 in 8-bit levels on those dataset frames, for the open
alternatives found:

| candidate | Sony s709 | DJI D-Log M | Fujifilm WDR | Panasonic V-709 |
|---|---|---|---|---|
| published curve and gamut, BT.709 OETF, clip | 16.0 / 58 | 21.8 / 78 | 17.0 / 67 | 22.3 / 67 |
| best of ACES 1.x fits (Hill, Narkowicz), Hable, Khronos PBR Neutral, GoPro's rolloff | 8.5 / 25 | 12.0 / 38 | 7.4 / 17 | 7.4 / 29 |
| three mixes, one shared curve (28 numbers) | 2.2 / 11 | 2.0 / 13 | 1.5 / 10 | 0.4 / 0.7 |
| this model | 0.37 / 2.9 | 0.25 / 1.7 | 0.74 / 5.8 | 0.10 / 0.4 |

- **OpenOSV** (github.com/Kemerd/OpenOSV, Apache-2.0) is the one open formula
  fitted to a maker's display LUT: DJI's Osmo 360 D-Log M LUT, which is the
  file `dji-dlog-m` is fitted to. Ported and scored on the same held-out
  images it is 1.39 / 5.6, against 0.23 / 1.0 here; on the dataset frames
  1.08 / 5.9, against 0.25 / 1.7.
- **LUTCalc** (GPL-2.0) has Sony's LC-709 and s709, Panasonic's V-709 and
  Canon's Wide DR, but as tables measured from the makers' LUTs -- the same
  thing as shipping them.
- **Community decode curves** under D-Log M (Thatcher Freeman's IDT,
  fitted to footage) instead of DJI's published D-Log and D-Gamut: 2.02
  against 2.07 mean with the three-mix model. The look absorbs the curve, and
  DJI's own document is the one to cite.
- **ACES reference gamut compression** before or after the curve, with fitted
  thresholds: no gain. The six mixes are what follow the makers' saturation
  handling, which the three-mix model gets worst on bright greens.

Why not fit the whole lattice, as an earlier version did: the makers' looks
compress saturation with a hue-dependent knee one or two lattice cells from
grey (D-Log's is sharper than DJI's own 33-point lattice), and no smooth model
follows it everywhere -- a 25^3 tensor B-spline still missed D-Log by 53
levels. Weighting the fit by what photographs contain puts the error where no
image goes.

## Sources

| curve | document | gamut |
|---|---|---|
| S-Log3 | Sony, Technical Summary for S-Gamut3.Cine/S-Log3 and S-Gamut3/S-Log3 | S-Gamut3.Cine |
| V-Log | Panasonic, V-Log/V-Gamut Reference Manual | V-Gamut |
| F-Log, F-Log2 | Fujifilm, F-Log and F-Log2 Data Sheets | F-Gamut (BT.2020) |
| D-Log | DJI, White Paper on D-Log and D-Gamut | D-Gamut |
| I-Log (10-bit) | Insta360, I-Log White Paper | BT.2020 |
| Canon Log 3 | Canon, on legal-range code values, x0.9 to reflectance | Cinema Gamut |
| N-Log | Nikon, N-Log Specification | N-Gamut (BT.2020) |
| GP-Log, GP-Log2 | GoPro Labs, log base 400 and 600 | -- |

DJI publishes no D-Log M curve, so D-Log M's base is D-Log and D-Gamut. The
X5's and the Ace Pro 2 / GO Ultra LUTs put the white paper's 18% grey at
different brightnesses from the X6's, which suggests their I-Log differs; the
white paper's curve is the base for all three and the fitted look absorbs the
difference. Canon's own 1D Canon Log 3 -> linear LUT agrees with the formula
to 4e-5 relative.

Not built in: Apple Log and Samsung Log publish their curves, but their
Rec.709 LUTs are behind a developer or Samsung account, so there is nothing
to fit against or check by. A `.cube` from them still works as a file.

## Detection

Automatic picks a built-in only when the file's own metadata names the profile
(`sfm::video_log_profile`); colour tags never count, because log clips are
tagged BT.709 or BT.2020 like everything else.

| maker | marker | read on |
|---|---|---|
| DJI | `djmd` sample 0, protobuf `ColorMode.color_mode`: 2 D-Log, 19 D-Log M | `dvtm_ac204` (Action 5 Pro) and `dvtm_oq101` (Osmo 360) at 2.4.1; `dvtm_AVATA360` / `dvtm_eagle4_wa530` at 2.2.4.1. Any other proto is not guessed |
| GoPro | `udta/GPMF`, DEVC with DVID 1, `PTCL` = `LOG` | only with a 360 projection: the MAX 2's is the one technical LUT. `FLAT` is a log curve too, not this one |
| Sony | top-level `meta`, NonRealTimeMeta `CaptureGammaEquation` | `s-log3-cine`, or `s-log3` with `S-Gamut3-Cine` primaries |
| Panasonic | `com.panasonic.Semi-Pro.metadata.xml`, `CaptureGamma` | `V-Log` (V-Log L is another curve) |

Insta360 writes `gamma_mode` in its trailer, but the string an I-Log clip
carries has not been seen, so it is not read; Fujifilm's VideoRecordingMode
sits in a makernote under `udta/MVTG` that nothing here parses yet. Canon,
Nikon, GP-Log2 and the Osmo Action 6 are picked by hand. The DJI paths come
from `telemetry-parser`'s protos and Action 5 Pro clips; the Avata 360 one is
confirmed only for Default (an empty ColorMode).
