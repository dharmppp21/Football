"""Epic Game Animation Sample loops (already seamless over the whole clip): in place + on the floor, nothing else.
python gas_inplace.py  -> clean/<name>.json for ue_import.write (reuses CONFIGS' src/dst)."""
import os, json, sys
import numpy as np
from mocap_lib import Clip, ROOT
from clean import Feet, SIDES, contact_mask, _runs, OUT
from configs import CONFIGS

NAMES = ['GAS_Idle', 'GAS_Walk', 'GAS_Jog', 'GAS_Run', 'GAS_Sprint']


def process(name):
    cfg = CONFIGS[name]
    c = Clip(cfg['src'])
    fps, T, Q = c.fps, c.T.copy(), c.Q.copy()
    rep = dict(name=name, keys=c.n, length_s=round((c.n - 1) / fps, 3))
    rep['seam_rot_dot_min'] = round(float(np.abs((Q[0] * Q[-1]).sum(-1)).min()), 5)  # 1 = the last key repeats the first
    travel = T[-1, 0, :2] - T[0, 0, :2]
    rep['speed_cms'] = round(float(np.linalg.norm(travel)) / ((c.n - 1) / fps), 1)
    u = np.linspace(0, 1, c.n)
    T[:, 0, :2] -= u[:, None] * travel[None]
    T[:, 0, :2] -= T[:, 0, :2].mean(0)
    feet = Feet(c)
    P, Qg = c.fk(T, Q)
    soles = {(s, k): feet.sole(P, Qg, s, k) for s in SIDES for k in ('ankle', 'ball')}
    low = np.minimum.reduce([p[:, 2] for p in soles.values()])  # lowest sole point per frame
    dz = -float(low.min())
    T[:, 0, 2] += dz
    rep['floor_shift_cm'] = round(dz, 2)
    # foot sync markers: each foot's plant = the frame its sole reaches the floor after a swing
    for s in SIDES:
        h = np.minimum(soles[s, 'ankle'][:, 2], soles[s, 'ball'][:, 2]) + dz
        on = h < 2.5
        plants = [a for a, b in _runs(on) if a > 0]
        rep['plant_%s' % s] = [round(a / fps, 3) for a in plants]
    curves = {nm: [list(map(float, ts)), [float(v) for v in vs]] for nm, (ts, vs) in c.d['curves'].items()}
    out = dict(src_path=c.d['path'], dst=cfg['dst'], fps=fps, rate=c.d['rate'], names=c.names, parents=c.parents, ref=c.d['ref'], loop=True,
               frames=np.concatenate([T, Q], -1).round(6).tolist(), curves=curves, report=rep)
    with open(os.path.join(OUT, name + '.json'), 'w', encoding='utf-8') as fh:
        json.dump(out, fh)
    return rep


if __name__ == '__main__':
    for nm in sys.argv[1:] or NAMES:
        print(json.dumps(process(nm)))
