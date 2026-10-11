#!/usr/bin/env python3
"""Fit the built-in log -> Rec.709 looks in src/app/LogLutTable.h to the camera
makers' own LUTs, from how each LUT renders natural photographs. A built-in is
the maker's published log curve and gamut plus 129 fitted numbers;
nothing of a maker's LUT ships. GoPro's are exact formulas instead.

    python tools/fit_log_luts.py            fetch (cached), fit, write
    python tools/fit_log_luts.py --report   fit and print, write nothing

Dev-time only; needs numpy, torch, Pillow, quickjs (GoPro's GP-Log2 page) and
7-Zip (Nikon's installer); about an hour, each fit cached until the LUT or this
script changes. The model and the numbers: docs/notes/log-luts.md. app/LogLuts.cpp evaluates
look() and the curves below -- change one, change both.
"""
import argparse
import hashlib
import io
import math
import pathlib
import re
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

import numpy as np
import torch

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "src" / "app" / "LogLutTable.h"
CHECKS_OUT = ROOT / "src" / "app" / "tests" / "log_lut_checks.h"

SIZE = 33                        # the lattice app::bake_builtin_lut fills
W = 6                            # mixes of linear Rec.709, each with its own curve
U0, U1 = -8, 6                   # curve knots, one per stop about 18% grey

DJI = "https://terra-1-g.djicdn.com/851d20f7b9f64838a34cd02351370894"
I360 = "https://wassets.insta360.com/common"
GOPRO = "https://gopro.my.salesforce.com/sfc/dist/version/download/?oid=00Do0000000HJuF"
FUJI = "https://dl.fujifilm-x.com/lut/x-h2s-3d-lut-v100.zip"
CANON = "https://gdlp01.c-wss.com/gds/2/0200007512/01/canon-lut-202510.zip"
NIKON = "https://download.nikonimglib.com/archive7/ZMwLw00WcWzX064KiDQ66OEy3p80/D-3DLUT-V0200W.exe"
GPLOG2_PAGE = "https://raw.githubusercontent.com/gopro/labs/master/docs/gplog2/index.html"
KODAK = "https://r0k.us/graphics/kodak/kodak/kodim{:02d}.png"

# kind "look": fitted to the LUT at `url`/`member`; "bt709": GoPro's technical
# LUT, a matrix after the BT.709 OETF; "gplog2": GoPro Labs' generator.
LUTS = [
    dict(id="dji-dlog-m", name="DJI D-Log M", curve="dlog", gamut="dgamut", kind="look",
         url=DJI + "/M3/DJI%20Mavic%203%20D-Log%20M%20to%20Rec.709%20V1.cube", member=None),
    dict(id="dji-dlog-m-2025", name="DJI D-Log M (Mavic 4 Pro, Mini 5 Pro, Avata 360)", curve="dlog",
         gamut="dgamut", kind="look",
         url=DJI + "/WA341%20LUT/DJI%20Mavic%204%20Pro%20D-Log%20M%20to%20Rec.709%20V1.cube", member=None),
    dict(id="dji-dlog-m-action6", name="DJI D-Log M (Osmo Action 6)", curve="dlog", gamut="dgamut",
         kind="look", url=DJI + "/AC206%20LUT/DJI%20OSMO%20Action%206%20D-LogM%20to%20Rec.709%20LUT-11.17.cube",
         member=None),
    dict(id="dji-dlog", name="DJI D-Log", curve="dlog", gamut="dgamut", kind="look",
         url=DJI + "/260%20downloads/DJI%20Mavic%203%20D-Log%20to%20Rec.709%20V1.cube", member=None),
    dict(id="insta360-ilog", name="Insta360 I-Log (X6, Luna)", curve="ilog", gamut="bt2020", kind="look",
         url=I360 + "/8d7c0e7275dd49baa6a2295197f46744/X6_I-Log_LUTs.zip",
         member="X6_I-Log_to_Rec709_BT1886_s33_v2.cube"),
    dict(id="insta360-x5-ilog", name="Insta360 I-Log (X5)", curve="ilog", gamut="bt2020", kind="look",
         url=I360 + "/056218ae92504c73965e6f40eb394d79/Insta360%20X5%20LUT.zip",
         member="X5_I-Log_To_Rec.709_V1.0.cube"),
    dict(id="insta360-acepro2-ilog", name="Insta360 I-Log (Ace Pro 2, GO Ultra)", curve="ilog",
         gamut="bt2020", kind="look",
         url=I360 + "/2484dd9895654b66a64a9e025f6c1e75/Insta360%20Ace%20Pro%202%20LUT.zip",
         member="AcePro2_I-Log_To_Rec.709_V1.0.cube"),
    dict(id="gopro-gplog", name="GoPro GP-Log (MAX 2)", curve="gplog", gamut="rec709", kind="bt709",
         url=GOPRO + "&ids=068Uo00000Uu6eAIAR&d=%2Fa%2FUo000002QHSH%2FN9hFoWmhb2AlJsFlExRELISr3VqHiKKiZK8eyvA.d3E&asPdf=false",
         member=None),
    dict(id="gopro-gplog2", name="GoPro GP-Log2", curve="gplog2", gamut="bt2020", kind="gplog2",
         url=GPLOG2_PAGE, member=None),
    dict(id="sony-slog3", name="Sony S-Log3 / S-Gamut3.Cine (s709)", curve="slog3", gamut="sgamut3cine",
         kind="look", url="https://77snszqv.media.zestyio.com/Slog3-S-Gamut3.Cine_To_s709_V200.zip",
         member="Slog3-S-Gamut3.Cine_To_s709_V200.cube"),
    dict(id="panasonic-vlog", name="Panasonic V-Log (V-709)", curve="vlog", gamut="vgamut", kind="look",
         url="https://av.jpn.support.panasonic.com/support/share2/eww/en/dsc/lut/VLog_to_V709_forV35_EN.zip",
         member="VLog_to_V709_forV35_ver100.cube"),
    dict(id="fujifilm-flog2", name="Fujifilm F-Log2 (WDR)", curve="flog2", gamut="bt2020", kind="look",
         url=FUJI, member="XH2S_FLog2_FGamut_to_WDR_BT.709_33grid_V.1.00.cube"),
    dict(id="fujifilm-flog", name="Fujifilm F-Log (WDR)", curve="flog", gamut="bt2020", kind="look",
         url=FUJI, member="XH2S_FLog_FGamut_to_WDR_BT.709_33grid_V.1.00.cube"),
    dict(id="canon-clog3", name="Canon Log 3 / Cinema Gamut (BT.709 Wide DR)", curve="clog3",
         gamut="cinemagamut", kind="look", url=CANON,
         member="CinemaGamut_CanonLog3-to-BT709_WideDR_33_FF_Ver.2.0.cube"),
    dict(id="nikon-nlog", name="Nikon N-Log", curve="nlog", gamut="bt2020", kind="look", url=NIKON,
         member="N-Log_BT2020_to_REC709_BT1886_size_33.cube"),
]


# ================
# Fetching and .cube files
# ================

def fetch_bytes(url, cache):
    path = cache / hashlib.sha256(url.encode()).hexdigest()[:16]
    if not path.exists():
        print(f"  fetching {url[:90]}", flush=True)
        with urllib.request.urlopen(url, timeout=120) as r:
            path.write_bytes(r.read())
    return path


def seven_zip():
    for c in (shutil.which("7z"), shutil.which("7za"), r"C:\Program Files\7-Zip\7z.exe"):
        if c and pathlib.Path(c).exists():
            return c
    raise RuntimeError("Nikon ships its LUTs in a RAR self-extractor; install 7-Zip")


def fetch(entry, cache):
    path = fetch_bytes(entry["url"], cache)
    if entry["member"] is None:
        return path.read_text("utf-8", "replace")
    data = path.read_bytes()
    if data[:2] == b"MZ":
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run([seven_zip(), "e", "-y", f"-o{tmp}", str(path), entry["member"], "-r"],
                           check=True, stdout=subprocess.DEVNULL)
            return (pathlib.Path(tmp) / entry["member"]).read_text("utf-8", "replace")
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        # Some makers' zips store Windows separators in the names.
        for info in z.infolist():
            name = info.filename.replace("\\", "/")
            if name.split("/")[-1] == entry["member"] and "__MACOSX" not in name:
                return z.read(info).decode("utf-8", "replace")
    raise RuntimeError(f"{entry['member']} not in {entry['url']}")


def parse_cube(text, clip=True):
    """[r][g][b][3]."""
    size, rows = None, []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        w = line.split()
        k = w[0].upper()
        if k == "LUT_3D_SIZE":
            size = int(w[1])
        elif k in ("DOMAIN_MIN", "DOMAIN_MAX"):
            if [float(v) for v in w[1:4]] != ([0.0] * 3 if k == "DOMAIN_MIN" else [1.0] * 3):
                raise RuntimeError("not a [0,1] LUT")
        elif not k[0].isalpha():
            rows.append([float(v) for v in w[:3]])
    a = np.array(rows)
    if size is None or len(a) != size ** 3:
        raise RuntimeError("not a 3D LUT")
    a = a.reshape(size, size, size, 3).transpose(2, 1, 0, 3)
    return np.clip(a, 0, 1) if clip else a


def tetra(t, p):
    """Tetrahedral lookup, the interpolation app::apply_cube_lut uses."""
    n = t.shape[0]
    q = np.clip(p, 0, 1) * (n - 1)
    i = np.clip(np.floor(q).astype(int), 0, n - 2)
    f = q - i
    r, g, b = f[..., 0], f[..., 1], f[..., 2]

    def at(o):
        return t[i[..., 0] + o[0], i[..., 1] + o[1], i[..., 2] + o[2]]

    c0, c1 = at((0, 0, 0)), at((1, 1, 1))
    out = np.zeros(p.shape)
    done = np.zeros(p.shape[:-1], bool)
    for m, a, bb, x, y, z in (((r >= g) & (g >= b), (1, 0, 0), (1, 1, 0), r, g, b),
                              ((r >= b) & (b > g), (1, 0, 0), (1, 0, 1), r, b, g),
                              ((b > r) & (r >= g), (0, 0, 1), (1, 0, 1), b, r, g),
                              ((g > r) & (r >= b), (0, 1, 0), (1, 1, 0), g, r, b),
                              ((g >= b) & (b > r), (0, 1, 0), (0, 1, 1), g, b, r),
                              ((b > g) & (g > r), (0, 0, 1), (0, 1, 1), b, g, r)):
        m = m & ~done
        done |= m
        v = ((1 - x)[..., None] * c0 + (x - y)[..., None] * at(a) + (y - z)[..., None] * at(bb) +
             z[..., None] * c1)
        out[m] = v[m]
    return out


# ================
# The makers' published curves and gamuts -- app/LogLuts.cpp decode()
# ================

def decode(curve, x):
    """Code value / 1023 -> scene linear, 18% grey = 0.18 (GoPro's: 1 = clip)."""
    if curve == "slog3":     # Sony, Technical Summary for S-Gamut3.Cine/S-Log3
        c = x * 1023
        return np.where(c >= 171.2102946929, 10 ** ((c - 420) / 261.5) * 0.19 - 0.01,
                        (c - 95) * 0.01125 / (171.2102946929 - 95))
    if curve == "vlog":      # Panasonic, V-Log/V-Gamut Reference Manual
        return np.where(x < 0.181, (x - 0.125) / 5.6, 10 ** ((x - 0.598206) / 0.241514) - 0.00873)
    if curve == "flog":      # Fujifilm, F-Log Data Sheet
        return np.where(x < 0.100537775223865, (x - 0.092864) / 8.735631,
                        (10 ** ((x - 0.790453) / 0.344676) - 0.009468) / 0.555556)
    if curve == "flog2":     # Fujifilm, F-Log2 Data Sheet
        return np.where(x < 0.100686685370811, (x - 0.092864) / 8.799461,
                        (10 ** ((x - 0.384316) / 0.245281) - 0.064829) / 5.555556)
    if curve == "dlog":      # DJI, White Paper on D-Log and D-Gamut
        return np.where(x <= 0.14, (x - 0.0929) / 6.025, (10 ** (3.89616 * x - 2.27752) - 0.0108) / 0.9892)
    if curve == "ilog":      # Insta360, I-Log White Paper (10-bit)
        return np.where(x < 0.154402, (x - 0.09055934) / 5.77837328, 10 ** ((x - 0.623992) / 0.280055) - 0.01)
    if curve == "clog3":     # Canon Log 3, on legal-range code values, x0.9 to reflectance
        t = (x * 1023 - 64) / 876
        return 0.9 * np.where(t < 0.04076162, -(10 ** ((0.07623209 - t) / 0.42889912) - 1) / 14.98325,
                              np.where(t <= 0.105357102, (t - 0.073059361) / 2.3069815,
                                       (10 ** ((t - 0.069886632) / 0.42889912) - 1) / 14.98325))
    if curve == "nlog":      # Nikon, N-Log Specification
        return np.where(x < 452 / 1023, (x / (650 / 1023)) ** 3 - 0.0075, np.exp((x - 619 / 1023) / (150 / 1023)))
    if curve == "gplog":     # GoPro Labs, log base 400
        return (400.0 ** x - 1) / 399
    if curve == "gplog2":    # GoPro Labs, GP-Log2 white paper, log base 600
        return (600.0 ** x - 1) / 599
    raise KeyError(curve)


def encode(curve, lin):
    xs = np.linspace(-0.25, 1.25, 300001)
    ys = decode(curve, xs)
    k = np.argmax(np.minimum.accumulate(np.diff(ys)[::-1])[::-1] > 0)  # N-Log's cube root dips below 0
    return np.clip(np.interp(lin, ys[k:], xs[k:]), 0, 1)


D65 = (0.3127, 0.3290)
PRIMARIES = {
    "rec709": ((0.64, 0.33), (0.30, 0.60), (0.15, 0.06)),
    "bt2020": ((0.708, 0.292), (0.170, 0.797), (0.131, 0.046)),     # also F-Gamut, N-Gamut, I-Log's
    "sgamut3cine": ((0.766, 0.275), (0.225, 0.800), (0.089, -0.087)),
    "vgamut": ((0.730, 0.280), (0.165, 0.840), (0.100, -0.030)),
    "dgamut": ((0.71, 0.31), (0.21, 0.88), (0.09, -0.08)),
    "cinemagamut": ((0.74, 0.27), (0.17, 1.14), (0.08, -0.10)),
}


def to_xyz(p):
    P = np.array([[x / y, 1, (1 - x - y) / y] for x, y in p]).T
    w = np.array([D65[0] / D65[1], 1, (1 - D65[0] - D65[1]) / D65[1]])
    return P * np.linalg.solve(P, w)


def to709(e):
    if e["kind"] == "gplog2":  # the generator's own rounded matrix, normalised as it does
        m = np.array([1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187])
        return (m / (m.sum() / 3)).reshape(3, 3)
    return np.linalg.inv(to_xyz(PRIMARIES["rec709"])) @ to_xyz(PRIMARIES[e["gamut"]])


# ================
# The look -- app/LogLuts.cpp map()
# ================

def oetf709(v):
    v = np.maximum(v, 0)
    return np.where(v < 0.018, 4.5 * v, 1.099 * np.maximum(v, 0.018) ** 0.45 - 0.099)


def pchip_slopes(y):
    """Fritsch-Butland slopes (SciPy's PCHIP) on unit-spaced, rising knots [W, K]."""
    d = y[:, 1:] - y[:, :-1]
    d0, d1 = d[:, :-1], d[:, 1:]
    inner = torch.where(d0 * d1 > 0, 2 * d0 * d1 / (d0 + d1 + 1e-300), torch.zeros_like(d0))

    def edge(a, b):
        e = (3 * a - b) / 2
        return torch.where(e * a <= 0, torch.zeros_like(e),
                           torch.where((a * b <= 0) & (e.abs() > 3 * a.abs()), 3 * a, e))

    return torch.cat([edge(d[:, 0], d[:, 1])[:, None], inner, edge(d[:, -1], d[:, -2])[:, None]], 1)


def curves(y, x):
    """Column w of x [N, W] through the Hermite curve on knot row w of y [W, K]."""
    K = y.shape[1]
    m = pchip_slopes(y)
    u = torch.log2(torch.clamp(x, min=1e-30) / 0.18) - U0
    k = torch.clamp(torch.floor(u), 0, K - 2).long()
    t = torch.clamp(u - k, 0, 1)
    cols = torch.arange(y.shape[0]).expand_as(k)
    y0, y1, m0, m1 = y[cols, k], y[cols, k + 1], m[cols, k], m[cols, k + 1]
    t2, t3 = t * t, t * t * t
    v = (2 * t3 - 3 * t2 + 1) * y0 + (t3 - 2 * t2 + t) * m0 + (3 * t2 - 2 * t3) * y1 + (t3 - t2) * m1
    toe = y[:, 0] * torch.clamp(x, min=0) / (0.18 * 2.0 ** U0)
    return torch.where(u <= 0, toe, torch.where(u >= K - 1, y[:, -1].expand_as(v), v))


def unpack(p):
    K = U1 - U0 + 1
    return dict(m1=p[:3 * W].reshape(W, 3), m2=p[3 * W:6 * W].reshape(3, W),
                knots=torch.cumsum(torch.nn.functional.softplus(p[6 * W:6 * W + W * K].reshape(W, K)), 1),
                black=p[6 * W + W * K:])


def look(prm, lin):
    """Scene-linear Rec.709 [N, 3] -> display [N, 3]."""
    return torch.clamp(curves(prm["knots"], lin @ prm["m1"].T) @ prm["m2"].T + prm["black"], 0, 1)


def initial():
    K = U1 - U0 + 1
    Y = np.array([0.2126, 0.7152, 0.0722])
    m1 = np.array([np.eye(3)[i] * s + (1 - s) * Y for s in (1.0, 0.5) for i in range(3)])
    m2 = np.hstack([np.eye(3), np.eye(3)]) / 2
    y = np.maximum.accumulate(np.clip(oetf709(0.18 * 2.0 ** np.arange(U0, U1 + 1)), 0, 0.999))
    d = np.diff(np.concatenate([[0], y + np.arange(K) * 1e-3]))
    return torch.tensor(np.concatenate([m1.ravel(), m2.ravel(), np.tile(np.log(np.expm1(d)), W), np.zeros(3)]))


# ================
# Fitting, on natural photographs
# ================

def natural(cache):
    """Scene-linear Rec.709 from the Kodak suite, each image's median grey at
    18% and one stop either side; odd images fit, even ones test."""
    from PIL import Image
    fit, test = [], []
    for i in range(1, 25):
        im = Image.open(fetch_bytes(KODAK.format(i), cache)).convert("RGB")
        im.thumbnail((128, 128))
        a = np.asarray(im, dtype=np.float64).reshape(-1, 3) / 255
        lin = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
        lin *= 0.18 / np.median(lin @ np.array([0.2126, 0.7152, 0.0722]))
        (fit if i % 2 else test).extend(lin * 2.0 ** e for e in (-1, 0, 1))
    return np.concatenate(fit), np.concatenate(test)


def jacobian(res, p, chunk=16):
    """Forward mode, a few parameter directions at a time to bound memory."""
    eye = torch.eye(len(p), dtype=p.dtype)
    jvp = torch.func.vmap(lambda v: torch.func.jvp(res, (p,), (v,))[1])
    return torch.cat([jvp(eye[i:i + chunk]) for i in range(0, len(p), chunk)]).T


def levenberg_marquardt(res, p, iters, soft=None):
    """Minimise |res(p)|^2, or the soft-L1 loss of scale `soft` by IRLS."""
    lam = 1e-3
    for _ in range(iters):
        r = res(p)
        w = torch.ones_like(r) if soft is None else (1 + (r / soft) ** 2) ** -0.25
        J = jacobian(res, p) * w[:, None]
        r = r * w
        A, g = J.T @ J, J.T @ r
        cost = float(r @ r)
        while True:
            step = torch.linalg.solve(A + lam * torch.diag(torch.diag(A) + 1e-12), -g)
            rn = res(p + step)
            wn = torch.ones_like(rn) if soft is None else (1 + (rn / soft) ** 2) ** -0.25
            if float((rn * wn) @ (rn * wn)) < cost:
                p, lam = p + step, max(lam / 3, 1e-9)
                break
            lam *= 4
            if lam > 1e9:
                return p
    return p


def errors(pred, ref):
    e = np.abs(pred - ref).max(-1) * 255
    return e.mean(), np.percentile(e, 99)


def fit_look(e, lut, fit_lin, test_lin, rng):
    """Mostly natural colours; photographs pushed 1.5x more saturated across
    +-2 stops at half weight, for the wider gamut a camera sees; the grey axis
    end to end at five times; and a tenth weight on the whole lattice, for
    colours no photograph reaches."""
    M = to709(e)
    grid = np.stack(np.meshgrid(*[np.linspace(0, 1, SIZE)] * 3, indexing="ij"), -1).reshape(-1, 3)

    def codes(lin709):
        return encode(e["curve"], lin709 @ np.linalg.inv(M).T)

    def pick(a, n):
        return a[rng.choice(len(a), n, replace=False)]

    cam = pick(fit_lin, 10000) @ np.linalg.inv(M).T
    grey = cam.mean(-1, keepdims=True)
    cam = (grey + (cam - grey) * 1.5) * 2.0 ** rng.uniform(-2, 2, (len(cam), 1))
    grey = np.repeat(np.linspace(0, 1, 257)[:, None], 3, 1)
    c = np.concatenate([codes(pick(fit_lin, 20000)), encode(e["curve"], cam), grey, pick(grid, 6000)])
    weight = torch.tensor(np.concatenate([np.ones(20000), np.full(10000, 0.5), np.full(257, 5.0),
                                          np.full(6000, 0.1)]))[:, None]
    x = torch.tensor(decode(e["curve"], c) @ M.T)
    xg = torch.tensor(decode(e["curve"], grey) @ M.T)
    tref = torch.tensor(tetra(lut, c))

    def res(q):
        # Knots few colours reach -- far below black, above +4 stops -- follow
        # their neighbours instead of running off where the clamp hides them.
        prm = unpack(q)
        y = prm["knots"]
        bend = (y[:, 2:] - 2 * y[:, 1:-1] + y[:, :-2]).reshape(-1) * 0.1
        cap = torch.relu(y - 1.25).reshape(-1)
        g = look(prm, xg)
        fall = (torch.cummax(g, 0).values - g).reshape(-1) * 10  # brighter grey never comes out darker
        return torch.cat([((look(prm, x) - tref) * weight).reshape(-1), bend, cap, fall]) * 255

    p = levenberg_marquardt(res, initial(), 60)
    p = levenberg_marquardt(res, p, 60, soft=2.0)
    ct = codes(pick(test_lin, 200000))
    with torch.no_grad():
        prm = unpack(p)
        held = errors(look(prm, torch.tensor(decode(e["curve"], ct) @ M.T)).numpy(), tetra(lut, ct))
        whole = errors(look(prm, torch.tensor(decode(e["curve"], grid) @ M.T)).numpy(), tetra(lut, grid))
        return {k: v.numpy() for k, v in prm.items()}, held, whole


def fit_bt709(e, raw):
    """GoPro's technical LUT: a matrix after the BT.709 OETF, in 512ths."""
    x = np.stack(np.meshgrid(*[np.linspace(0, 1, raw.shape[0])] * 3, indexing="ij"), -1).reshape(-1, 3)
    a = oetf709(decode(e["curve"], x))
    t = raw.reshape(-1, 3)
    ok = (a < 1.2).all(1) & (np.abs(t) < 0.999).all(1)
    m = np.round(np.linalg.lstsq(a[ok], t[ok], rcond=None)[0].T * 512) / 512
    worst = float(np.abs(np.clip(a @ m.T, 0, 1) - np.clip(t, 0, 1)).max())
    assert worst < 1e-4, (e["id"], worst)
    return m


def gplog2_reference(page):
    """GoPro Labs' own generator, run as published: EV +1.8, Rec.709, rolloff."""
    import quickjs
    js = page[page.index("// COLOUR MATH"):page.index("function cubeFilename")]
    ctx = quickjs.Context()
    ctx.eval(js)
    return parse_cube(ctx.eval(f"makeCube({SIZE}, 1.8, 'rec709', true, false)"))


def lattice_points(n):
    return [(0, 0, 0), (n - 1, n - 1, n - 1), (n // 2, n // 2, n // 2), (1, n // 3, n - 2),
            (n // 4, 3 * n // 4, n // 2), (n - 1, 0, n // 5), (n // 7, n // 2 + 1, n // 3), (n // 2, n // 3, n // 4)]


# ================
# Output
# ================

def cf(v):
    s = repr(float(v))
    return s if ("." in s or "e" in s) else s + ".0"


def arr(a):
    return "{" + ", ".join(cf(v) for v in np.ravel(a)) + "}"


def render(done):
    K = U1 - U0 + 1
    out = ["// GENERATED by tools/fit_log_luts.py -- do not edit. Each built-in's",
           "// published curve and gamut, and its look; read only by app/LogLuts.cpp,",
           "// after the types. The sources and the fit: docs/notes/log-luts.md.",
           "#pragma once", "", "namespace app {", "namespace {", "",
           f"static_assert(kLogLutSize == {SIZE} && kLogMixes == {W} && kLogKnot0 == {U0} && "
           f"kLogKnots == {K},",
           '              "app/LogLuts.cpp and tools/fit_log_luts.py disagree on the model");', "",
           "const LogLutParams kLogLuts[] = {"]
    for e, prm in done:
        curve = {"slog3": "SLog3", "vlog": "VLog", "flog": "FLog", "flog2": "FLog2", "dlog": "DLog",
                 "ilog": "ILog", "clog3": "CanonLog3", "nlog": "NLog", "gplog": "GpLog",
                 "gplog2": "GpLog2"}[e["curve"]]
        kind = {"look": "Fitted", "bt709": "Bt709", "gplog2": "GpLog2"}[e["kind"]]
        out.append(f'    {{{{"{e["id"]}", "{e["name"]}"}}, LogCurve::{curve}, LogLook::{kind},')
        out.append(f"     {arr(to709(e))},")
        out.append(f"     {arr(prm['m1'])},")
        out.append(f"     {arr(prm['m2'])},")
        out.append(f"     {arr(prm['knots'])},")
        out.append(f"     {arr(prm['black'])}}},")
    out += ["};", "", "}  // namespace", "}  // namespace app", ""]
    return "\n".join(out)


def render_checks(checks):
    out = ["// GENERATED by tools/fit_log_luts.py -- do not edit. What each built-in",
           "// gives at a few lattice points: tools/fit_log_luts.py's look() for a",
           "// fitted one, GoPro's own LUT or generator for theirs.",
           "#pragma once", "", "struct LogLutCheck {", "    const char* id;", "    int r, g, b;",
           "    double rgb[3];", "};", "inline const LogLutCheck kLogLutChecks[] = {"]
    for id_, r, g, b, v in checks:
        out.append(f'    {{"{id_}", {r}, {g}, {b}, {arr(v)}}},')
    out += ["};", ""]
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--report", action="store_true", help="fit and print, write nothing")
    ap.add_argument("--cache", default=str(pathlib.Path.home() / ".cache" / "spirula-luts"))
    ap.add_argument("ids", nargs="*", help="only these, and write nothing")
    args = ap.parse_args()
    args.report |= bool(args.ids)
    cache = pathlib.Path(args.cache)
    cache.mkdir(parents=True, exist_ok=True)
    fit_lin, test_lin = natural(cache)
    K = U1 - U0 + 1
    grid = np.stack(np.meshgrid(*[np.linspace(0, 1, SIZE)] * 3, indexing="ij"), -1)
    done, checks = [], []
    print("| id | kind | held-out photos mean / p99 | whole lattice mean / p99 (8-bit levels) |")
    print("|---|---|---|---|")
    for e in [e for e in LUTS if not args.ids or e["id"] in args.ids]:
        rng = np.random.default_rng(0)
        prm = dict(m1=np.zeros((W, 3)), m2=np.zeros((3, W)), knots=np.zeros((W, K)), black=np.zeros(3))
        if e["kind"] == "look":
            text = fetch(e, cache)
            # A fit is minutes; keep it until the LUT or this script changes.
            key = hashlib.sha256((text + pathlib.Path(__file__).read_text()).encode()).hexdigest()[:16]
            saved = cache / f"fit-{e['id']}-{key}.npz"
            if saved.exists():
                z = np.load(saved)
                prm, (hm, h99), (wm, w99) = {k: z[k] for k in ("m1", "m2", "knots", "black")}, z["held"], z["whole"]
            else:
                prm, (hm, h99), (wm, w99) = fit_look(e, parse_cube(text), fit_lin, test_lin, rng)
                np.savez(saved, **prm, held=(hm, h99), whole=(wm, w99))
            with torch.no_grad():
                lin = torch.tensor(decode(e["curve"], grid.reshape(-1, 3)) @ to709(e).T)
                ref = look({k: torch.tensor(v) for k, v in prm.items()}, lin).numpy().reshape(grid.shape)
            print(f"| {e['id']} | fitted | {hm:.2f} / {h99:.1f} | {wm:.1f} / {w99:.0f} |", flush=True)
        elif e["kind"] == "bt709":
            raw = parse_cube(fetch(e, cache), clip=False)
            prm["m2"][:, :3] = fit_bt709(e, raw)
            ref = np.clip(raw, 0, 1)
            print(f"| {e['id']} | exact, matrix x512 {np.round(prm['m2'][:, :3] * 512).astype(int).tolist()} | | |")
        else:
            ref = gplog2_reference(fetch(e, cache))
            print(f"| {e['id']} | exact, GoPro Labs' generator | | |")
        assert ref.shape[0] == SIZE
        diag = ref[np.arange(SIZE), np.arange(SIZE), np.arange(SIZE)]
        assert (np.diff(diag, axis=0) > -1 / 255).all(), (e["id"], "grey axis falls")
        for r, g, b in lattice_points(SIZE):
            checks.append((e["id"], r, g, b, ref[r, g, b]))
        done.append((e, prm))
    if not args.report:
        OUT.write_text(render(done), encoding="utf-8", newline="\n")
        CHECKS_OUT.write_text(render_checks(checks), encoding="utf-8", newline="\n")
        print(f"wrote {OUT.relative_to(ROOT)}, {CHECKS_OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
