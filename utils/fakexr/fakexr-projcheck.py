#!/usr/bin/env python3
"""Rotation consistency check for fakexr shots: does the app's image really use the projection it declares?

Usage: fakexr-projcheck.py [--declared-scale SX SY] <base.png> <turned.png> [<turned.png> ...]

Every PNG needs its fakexr <name>.json (shot with "world", one eye). The turned shots must differ from the base
only by a head rotation about that eye (fakexr "look"), so there is no parallax: then each pixel of a turned
shot shows the same direction as a computable pixel of the base shot, given the projection. The script fits a
horizontal and vertical scale (sx, sy) of the image's real tangent extents relative to the declared ones: a
correct app gives 1.00 / 1.00; an app that renders one aspect ratio or field of view while declaring another
(world squashed, stretching when looking up/down or rolling) gives e.g. sx 1.00, sy 0.71.
--declared-scale pretends the declared fov was scaled by that much (a self-test: expect 1/SX, 1/SY).
Exit status 1 when either scale is off by more than 2 %.
"""
import json
import sys

import numpy as np
from PIL import Image

N = 256  # analysis size (pixels on the longer side)


def rot(ang):
    """fakexr QuatFromAngles as a matrix: yaw about +Y, then pitch about +X, then roll about +Z (degrees)."""
    p, y, r = np.radians(ang)
    ry = np.array([[np.cos(y), 0, np.sin(y)], [0, 1, 0], [-np.sin(y), 0, np.cos(y)]])
    rx = np.array([[1, 0, 0], [0, np.cos(p), -np.sin(p)], [0, np.sin(p), np.cos(p)]])
    rz = np.array([[np.cos(r), -np.sin(r), 0], [np.sin(r), np.cos(r), 0], [0, 0, 1]])
    return ry @ rx @ rz


def load(png):
    meta = json.load(open(png[:-4] + ".json"))
    if meta["eyes"] == "both":
        sys.exit(f"{png}: shoot one eye (left or right)")
    if not meta.get("world_only"):
        sys.exit(f"{png}: shoot with 'world' (panels would not follow the rotation)")
    size = meta["eye_size"] if isinstance(meta["eye_size"], list) else [meta["eye_size"]] * 2
    if meta["rect"] != [0.0, 0.0, float(size[0]), float(size[1])]:
        sys.exit(f"{png}: shoot the whole eye (no rect)")
    eye = meta["eye_" + meta["eyes"]]
    img = Image.open(png).convert("L")
    w, h = img.size
    scale = N / max(w, h)
    img = img.resize((max(1, round(w * scale)), max(1, round(h * scale))), Image.BOX)
    tl, tr, tu, td = np.tan(np.radians(eye["fov"]))
    return {"img": np.asarray(img, np.float32) / 255.0, "R": rot(eye["pose"]["ang"]),
            "pos": np.array(eye["pose"]["pos"]), "tan": (tl, tr, tu, td), "name": png}


def actual_tan(t, sx, sy):
    """the tangent extents the image really covers, if they are the declared ones scaled about their centre"""
    tl, tr, tu, td = t
    cx, cy = (tl + tr) / 2, (tu + td) / 2
    return cx + (tl - cx) * sx, cx + (tr - cx) * sx, cy + (tu - cy) * sy, cy + (td - cy) * sy


def residual(a, b, sx, sy):
    """mean |A(warped) - B| over the overlap, with the images' true extents = declared * (sx, sy)"""
    ha, wa = a["img"].shape
    hb, wb = b["img"].shape
    al, ar, au, ad = actual_tan(a["tan"], sx, sy)
    bl, br, bu, bd = actual_tan(b["tan"], sx, sy)
    u = (np.arange(wb) + 0.5) / wb
    v = (np.arange(hb) + 0.5) / hb
    tx = bl + u[None, :] * (br - bl)
    ty = bu - v[:, None] * (bu - bd)
    d = np.stack([np.broadcast_to(tx, (hb, wb)), np.broadcast_to(ty, (hb, wb)), -np.ones((hb, wb))], -1)
    d = d @ (a["R"].T @ b["R"]).T  # B eye -> world -> A eye
    front = d[..., 2] < -1e-6
    z = np.where(front, -d[..., 2], 1.0)
    ua = (d[..., 0] / z - al) / (ar - al) * wa - 0.5
    va = (au - d[..., 1] / z) / (au - ad) * ha - 0.5
    ok = front & (ua >= 0) & (ua <= wa - 1.001) & (va >= 0) & (va <= ha - 1.001)
    if ok.mean() < 0.25:
        return np.inf
    x0 = np.floor(ua[ok]).astype(int)
    y0 = np.floor(va[ok]).astype(int)
    fx = ua[ok] - x0
    fy = va[ok] - y0
    img = a["img"]
    s = (img[y0, x0] * (1 - fx) * (1 - fy) + img[y0, x0 + 1] * fx * (1 - fy) +
         img[y0 + 1, x0] * (1 - fx) * fy + img[y0 + 1, x0 + 1] * fx * fy)
    return float(np.abs(s - b["img"][ok]).mean())


def total(base, turned, sx, sy):
    return sum(residual(base, t, sx, sy) for t in turned)


def main():
    args = sys.argv[1:]
    declared = (1.0, 1.0)
    if args[:1] == ["--declared-scale"]:
        declared = (float(args[1]), float(args[2]))
        args = args[3:]
    if len(args) < 2:
        sys.exit(__doc__)
    shots = [load(p) for p in args]
    for s in shots:
        s["tan"] = actual_tan(s["tan"], *declared)
        if np.abs(s["pos"] - shots[0]["pos"]).max() > 1e-3:
            sys.exit(f"{s['name']}: the eye moved ({s['pos']} vs {shots[0]['pos']}); turn with 'look'")
    base, turned = shots[0], shots[1:]
    # a view that did not change at all means the scene is not live (menu, loading screen, stale image), not a
    # projection error: say so instead of fitting noise
    for t in turned:
        if t["img"].shape == base["img"].shape and float(np.abs(t["img"] - base["img"]).mean()) * 255 < 0.5:
            print(f"projection check: CANNOT TELL, {t['name'].rsplit('/', 1)[-1]} is the same image as the base "
                  "(the view did not turn: not in a map, paused, or a stale image)")
            sys.exit(2)

    best = (np.inf, 1.0, 1.0)
    for step, span, centre in ((0.05, 0.4, None), (0.01, 0.05, True), (0.002, 0.01, True)):
        cx, cy = (best[1], best[2]) if centre else (1.0, 1.0)
        for sx in np.arange(cx - span, cx + span + 1e-9, step):
            for sy in np.arange(cy - span, cy + span + 1e-9, step):
                e = total(base, turned, sx, sy)
                if e < best[0]:
                    best = (e, sx, sy)
    err0 = total(base, turned, 1.0, 1.0)
    _, sx, sy = best
    print(f"projection check: horizontal scale {sx:.3f}, vertical {sy:.3f} (1.000 = as declared); "
          f"mismatch {err0 * 255:.1f} at the declared projection, {best[0] * 255:.1f} at the best fit "
          f"({len(turned)} turned shot{'s' if len(turned) > 1 else ''})")
    for t in turned:
        print(f"  {t['name'].rsplit('/', 1)[-1]}: mismatch {residual(base, t, 1.0, 1.0) * 255:.1f} declared, "
              f"{residual(base, t, sx, sy) * 255:.1f} fitted")
    if abs(sx - 1) > 0.02 or abs(sy - 1) > 0.02:
        print("projection check: FAILED (the image does not use the projection it declares)")
        sys.exit(1)
    print("projection check: OK")


if __name__ == "__main__":
    main()
