"""Checks a cleaned clip (clean/<name>.json): foot slide in world space, floor contact, penetration, loop seam,
pops and knees.  python verify.py [names...]   (also writes report/<name>_clean_poses.png)"""
import sys, os, json
import numpy as np
from mocap_lib import Clip, BONES, ROOT, facing, lowpass, hspeed
from clean import Feet, SIDES, _runs
import analyze

CLEAN = os.path.join(ROOT, 'clean')


def load(name):
    with open(os.path.join(CLEAN, name + '.json'), encoding='utf-8') as fh:
        d = json.load(fh)
    d.setdefault('path', d['dst'])
    return Clip(name + '_clean', d), d


def check(name, sheet=True):
    c, d = load(name)
    fps, n = c.fps, c.n
    P, Qg = c.fk()
    rep = d['report']
    v = np.array(rep.get('travel_cms', [0, 0, 0]), float)
    t = np.arange(n) / fps
    Pw = P + (t[:, None, None] * v[None, None, :]) if d['loop'] else P  # in-place loops back to world space
    feet = Feet(c)
    out = dict(name=name, keys=n, length_s=round((n - 1) / fps, 3))
    worst_slide, worst_h, lows = 0.0, 0.0, []
    for s in SIDES:
        for k in ('ankle', 'ball'):
            p = feet.sole(Pw, Qg, s, k)
            ps = lowpass(p, fps, 6.0)
            m = (p[:, 2] < 1.5) & (hspeed(ps, fps) < 20)
            for a, b in _runs(m):
                if b - a + 1 >= max(2, int(0.06 * fps)):
                    seg = p[a:b + 1]
                    worst_slide = max(worst_slide, float(np.max(np.linalg.norm(seg[:, :2] - seg[0, :2], axis=1))))
                    worst_h = max(worst_h, float(np.max(np.abs(seg[:, 2]))))
            lows.append(p[:, 2].min())
        lows.append(P[:, c.j(s + '_toe')][:, 2].min() - 0.97)  # toe tip joint sits 0.97 cm above the sole
    out['contact_slide_max_cm'] = round(worst_slide, 2)
    out['contact_height_max_cm'] = round(worst_h, 2)
    out['lowest_sole_cm'] = round(float(min(lows)), 2)
    if d['loop']:
        q = np.asarray(d['frames'], float)
        dq = np.abs(q[-1] - q[0])[:, 3:].max()
        out['seam_rot_diff'] = round(float(dq), 6)
        # velocity continuity across the seam vs inside the clip (body-relative joint positions)
        rel = P - P[:, :1]
        jump = np.linalg.norm((rel[1] - rel[0]) - (rel[-1] - rel[-2]), axis=-1).max()
        out['seam_vel_jump_cm_per_key'] = round(float(jump), 2)
    acc = np.linalg.norm(np.diff(P, 2, axis=0), axis=-1)
    out['max_accel_cm_per_key2'] = round(float(acc.max()), 2)
    for s in SIDES:
        a, b, k = P[:, c.j(s + '_thigh')], P[:, c.j(s + '_ankle')], P[:, c.j(s + '_knee')]
        u, w = a - k, b - k
        ang = np.degrees(np.arccos(np.clip(np.sum(u * w, -1) / np.linalg.norm(u, axis=-1) / np.linalg.norm(w, axis=-1), -1, 1)))
        out['knee_max_deg_' + s] = round(float(ang.max()), 1)
    if sheet:
        L = np.stack([P[..., 1], -P[..., 0], P[..., 2]], -1)  # mesh +Y forward, -X right
        analyze.draw(c, L, 0.0, t, np.zeros(n), dict(skel='FB clean'))
    return out


if __name__ == '__main__':
    names = sys.argv[1:] or sorted(f[:-5] for f in os.listdir(CLEAN) if f.endswith('.json'))
    for nm in names:
        print(json.dumps(check(nm)))
