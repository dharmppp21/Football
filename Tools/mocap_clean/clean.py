"""Tools/mocap_clean (data in Saved/MocapClean). Workflow: in the editor exec ue_export.py, dump_all([(clip, name)])
-> raw/<name>.json; python analyze.py <name>; add the move to configs.py; python clean.py <move>; python verify.py <move>;
in the editor exec ue_import.py, write_logged([move]) -> a copy in /Game/Football/Animations/Mocap_Clean (sources untouched).
Mocap clean-up on the Footballer skeleton: trim / retime, smoothing, cyclic loops, orientation, floor,
foot locking with two-bone leg IK, in-place loops.  Writes clean/<dst>.json for ue_import.py.
python clean.py [config names...]"""
import sys, os, json
import numpy as np
from mocap_lib import (Clip, qmul, qconj, qrot, qnorm, qfrom_to, qaxis_angle, qcontinuous, facing, lowpass, hspeed, ROOT)
from configs import CONFIGS

OUT = os.path.join(ROOT, 'clean')
UP = np.array([0, 0, 1.0])
TOE_TIP_H = 0.97  # toe tip joint height above the sole with the foot flat (reference pose)
SIDES = ('l', 'r')


# ------------------------------------------------------------------ quaternion helpers
def slerp(q0, q1, a):
    d = np.sum(q0 * q1, -1, keepdims=True)
    q1 = np.where(d < 0, -q1, q1)
    d = np.abs(d)
    a = np.asarray(a, float)[..., None] if np.ndim(a) == q0.ndim - 1 else np.asarray(a, float)
    th = np.arccos(np.clip(d, -1, 1))
    s = np.sin(th)
    lin = s < 1e-6
    w0 = np.where(lin, 1 - a, np.sin((1 - a) * th) / np.where(lin, 1, s))
    w1 = np.where(lin, a, np.sin(a * th) / np.where(lin, 1, s))
    return qnorm(w0 * q0 + w1 * q1)


def qpow(q, t):
    """q^t (rotation angle scaled by t) for unit quaternions."""
    q = np.where(q[..., 3:4] < 0, -q, q)
    ang = 2 * np.arccos(np.clip(q[..., 3], -1, 1))
    n = np.linalg.norm(q[..., :3], axis=-1, keepdims=True)
    axis = np.where(n > 1e-9, q[..., :3] / np.maximum(n, 1e-12), np.array([1.0, 0, 0]))
    return qaxis_angle(axis, ang * t)


def qyaw(deg):
    a = np.radians(deg) / 2
    return np.array([0, 0, np.sin(a), np.cos(a)])


def sample(T, Q, s):
    """Pose at fractional key indices s (linear translation, slerp rotation)."""
    s = np.clip(np.asarray(s, float), 0, T.shape[0] - 1)
    i0 = np.minimum(np.floor(s).astype(int), T.shape[0] - 2)
    a = s - i0
    T2 = T[i0] * (1 - a)[:, None, None] + T[i0 + 1] * a[:, None, None]
    Q2 = slerp(Q[i0], Q[i0 + 1], np.repeat(a[:, None], Q.shape[1], 1))
    return T2, Q2


def smoothstep(x):
    x = np.clip(x, 0, 1)
    return x * x * (3 - 2 * x)


def plateau(t, t0, t1, ramp):
    """0 outside [t0 - ramp, t1 + ramp], 1 inside [t0, t1], smooth ramps."""
    return smoothstep((t - (t0 - ramp)) / ramp) * smoothstep(((t1 + ramp) - t) / ramp)


# ------------------------------------------------------------------ stages
def warp_times(fps, t0, t1, segs, sigma=0.025):
    """Output key times in source seconds: playback speed k inside each (a, b, k) segment, 1 elsewhere,
    the step function smoothed by a Gaussian (sigma s) so speed changes are gradual."""
    g = np.arange(t0 - 0.3, t1 + 0.3, 0.001)
    r = np.ones_like(g)
    for a, b, k in segs:
        r[(g >= a) & (g < b)] = k
    ker = np.exp(-0.5 * (np.arange(-4 * sigma, 4 * sigma + 1e-9, 0.001) / sigma) ** 2)
    r = np.convolve(r, ker / ker.sum(), 'same')
    rate = lambda t: np.interp(t, g, r)
    ts, t, h = [t0], t0, 1.0 / fps
    while True:
        t = t + h * rate(t + 0.5 * h * rate(t))  # midpoint step of dt_src = rate * dt_out
        if t > t1 + 1e-9:
            break
        ts.append(t)
    return np.array(ts)


def smooth(T, Q, fps, hz_body, hz_legs, c):
    Q = Q.copy(); T = T.copy()
    legs = {c.idx[c.K[k]] for k in c.K if k[0] in 'lr' and k[2:] in ('thigh', 'knee', 'ankle', 'ball', 'toe')}
    for b in range(Q.shape[1]):
        hz = hz_legs if b in legs else hz_body
        if hz:
            Q[:, b] = qnorm(lowpass(qcontinuous(Q[:, b]), fps, hz))
    if hz_body:
        T[:, 0] = lowpass(T[:, 0], fps, max(hz_body, hz_legs or 0))
    return T, Q


def make_cyclic(T, Q):
    """Spread the end-start mismatch of every channel over the clip so the last key equals the first.
    The root keeps its translation travel (removed later, after foot locking)."""
    n = T.shape[0]
    u = np.linspace(0, 1, n)
    Q = Q.copy(); T = T.copy()
    d = qmul(Q[-1], qconj(Q[0]))  # residual per bone, (B,4)
    corr = qpow(np.repeat(qconj(d)[None], n, 0), np.repeat(u[:, None], Q.shape[1], 1))
    Q = qnorm(qmul(corr, Q))
    dz = T[-1, 0, 2] - T[0, 0, 2]
    T[:, 0, 2] -= u * dz
    return T, Q


def crossfade_loop(c, T, Q, fps, s, e, blend_s):
    """Seamless cycle from keys s..e of the full clip: the root's turn drift is removed at a constant rate,
    then the last `blend_s` seconds cross-fade into the frames one period earlier (which lead into key s),
    so the end flows into the start with matching pose and velocity. Needs s >= blend keys."""
    n = e - s
    B = max(2, int(round(blend_s * fps)))
    assert s - B >= 0, 'loop start too early for the blend'
    idx = np.arange(s - B, e + 1)
    T, Q = T[idx].copy(), Q[idx].copy()  # local frame k = key s - B + k
    k0 = B  # position of key s
    # root heading drift -> constant-rate counter-turn (applied to rotation and to the root path)
    P, _ = c.fk(T, Q)
    f0 = facing(P, c)[k0]
    loc = qrot(qconj(Q[k0, 0]), f0)
    f = qrot(Q[:, 0], np.repeat(loc[None], len(T), 0))
    yaw = np.unwrap(np.arctan2(f[:, 1], f[:, 0]))
    drift = yaw[k0 + n] - yaw[k0]
    corr = -drift * (np.arange(len(T)) - k0) / n
    qc = qaxis_angle(np.repeat(UP[None], len(T), 0), corr)
    Q[:, 0] = qnorm(qmul(qc, Q[:, 0]))
    base = T[k0, 0] * [1, 1, 0]
    T[:, 0] = qrot(qc, T[:, 0] - base) + base
    travel = T[k0 + n, 0] - T[k0, 0]
    travel[2] = 0
    out_T, out_Q = T[k0:].copy(), Q[k0:].copy()  # keys s..e
    for i in range(n - B, n + 1):
        w = smoothstep((i - (n - B)) / B)
        j = k0 + i - n  # same phase one period earlier
        out_Q[i] = slerp(Q[k0 + i], Q[j], np.full(Q.shape[1], w))
        out_T[i, 0] = (1 - w) * T[k0 + i, 0] + w * (T[j, 0] + travel)
    return out_T, out_Q, float(np.degrees(drift))


def rotate_root(T, Q, deg, about=np.zeros(3)):
    q = qyaw(deg)
    T = T.copy(); Q = Q.copy()
    T[:, 0] = qrot(q[None], T[:, 0] - about) + about
    Q[:, 0] = qnorm(qmul(np.repeat(q[None], len(T), 0), Q[:, 0]))
    return T, Q


def heading(v):
    return np.degrees(np.arctan2(v[1], v[0]))


# ------------------------------------------------------------------ feet
class Feet:
    """Sole points under the ankle (heel proxy) and the toe base, from the reference pose (feet flat on z=0)."""
    def __init__(self, c):
        R = np.asarray(c.ref)
        P, Qg = c.fk(R[None, :, 0:3], qnorm(R[None, :, 3:7]))
        self.c = c
        self.off = {}
        for s in SIDES:
            for k in ('ankle', 'ball'):
                j = c.j(s + '_' + k)
                self.off[s, k] = qrot(qconj(Qg[0, j]), np.array([0, 0, -P[0, j, 2]]))
            jt = c.j(s + '_toe')
            self.off[s, 'toe'] = np.zeros(3)

    def sole(self, P, Qg, s, k):
        j = self.c.j(s + '_' + k)
        return P[:, j] + qrot(Qg[:, j], np.broadcast_to(self.off[s, k], P[:, j].shape))


def contact_mask(h, v, fps, h_tol, v_tol, min_len=0.06, max_gap=0.05):
    m = (h < h_tol) & (v < v_tol)
    n = len(m)
    # fill short gaps, then drop short contacts
    runs = _runs(m)
    for (a, b), (c, d) in zip(runs, runs[1:]):
        if (c - b - 1) / fps <= max_gap:
            m[b + 1:c] = True
    out = np.zeros(n, bool)
    for a, b in _runs(m):
        if (b - a + 1) / fps >= min_len:
            out[a:b + 1] = True
    return out


def _runs(m):
    r, s = [], None
    for i, x in enumerate(m):
        if x and s is None:
            s = i
        if not x and s is not None:
            r.append((s, i - 1)); s = None
    if s is not None:
        r.append((s, len(m) - 1))
    return r


def lock_weights(p, mask, fps, ramp=0.06):
    """Per frame: lock weight (1 inside a contact run, smooth ramps just outside it) and the run's target,
    the median sole position of the run on the floor (z = 0). Overlapping ramps keep the stronger run."""
    n = len(p); t = np.arange(n) / fps
    w = np.zeros(n); tgt = p.copy(); tgt[:, 2] = 0
    for a, b in _runs(mask):
        m = np.median(p[a:b + 1, :2], axis=0)
        wr = plateau(t, a / fps, b / fps, ramp)
        upd = wr > w
        w[upd] = wr[upd]
        tgt[upd, :2] = m
    return w, tgt


def leg_ik(c, T, Q, Pg, Qg, s, ankle_tgt, foot_q):
    """Two-bone IK thigh-knee-ankle to ankle_tgt keeping the knee's swing plane; foot global rotation = foot_q."""
    ih, ik, ia = c.j(s + '_thigh'), c.j(s + '_knee'), c.j(s + '_ankle')
    ip = c.parents[ih]
    H, K, A = Pg[:, ih], Pg[:, ik], Pg[:, ia]
    l1 = np.linalg.norm(K - H, axis=-1, keepdims=True)
    l2 = np.linalg.norm(A - K, axis=-1, keepdims=True)
    D = ankle_tgt - H
    d0 = np.linalg.norm(D, axis=-1, keepdims=True)
    u = D / d0
    d = np.clip(d0, np.abs(l1 - l2) + 1e-3, (l1 + l2) * 0.9999)
    pole = (K - H) - np.sum((K - H) * u, -1, keepdims=True) * u
    pn = np.linalg.norm(pole, axis=-1, keepdims=True)
    pole = pole / np.maximum(pn, 1e-9)
    cosA = np.clip((l1 ** 2 + d ** 2 - l2 ** 2) / (2 * l1 * d), -1, 1)
    sinA = np.sqrt(1 - cosA ** 2)
    K2 = H + l1 * (cosA * u + sinA * pole)
    A2 = H + d * u
    q1 = qfrom_to(K - H, K2 - H)
    qt = qnorm(qmul(q1, Qg[:, ih]))
    q2 = qfrom_to(qrot(q1, A - K), A2 - K2)
    qc = qnorm(qmul(q2, qmul(q1, Qg[:, ik])))
    Q = Q.copy()
    Q[:, ih] = qnorm(qmul(qconj(Qg[:, ip]), qt))
    Q[:, ik] = qnorm(qmul(qconj(qt), qc))
    Q[:, ia] = qnorm(qmul(qconj(qc), foot_q))
    return Q


def restride(c, T, Q, travel, k):
    """Stride x k along the travel (the mono video squashed depth, and these takes ran toward / away from the camera):
    the root path and the ankle paths stretch by k along the travel about the first key; the legs reach the new ankle
    spots by IK, the feet keep their global rotation, the upper body rides the root. A planted foot stays planted."""
    u = np.array([travel[0], travel[1], 0.0]) / np.linalg.norm(travel[:2])
    o = T[0, 0].copy()
    P, Qg = c.fk(T, Q)
    T = T.copy()
    T[:, 0] += (k - 1) * ((T[:, 0] - o) @ u)[:, None] * u
    P2, Qg2 = c.fk(T, Q)
    for s in SIDES:
        A = P[:, c.j(s + '_ankle')]
        Q = leg_ik(c, T, Q, P2, Qg2, s, A + (k - 1) * ((A - o) @ u)[:, None] * u, Qg[:, c.j(s + '_ankle')])
    return T, Q


def detect(c, T, Q, fps, cfg):
    """Floor height (median planted sole height) and contact masks per (side, heel|ball) sole point."""
    feet = Feet(c)
    P, Qg = c.fk(T, Q)
    h_tol, v_tol = cfg.get('contact_h', 4.0), cfg.get('contact_v', 30.0)
    masks, floor = {}, 0.0
    for it in range(2):
        hs = []
        for s in SIDES:
            for k in ('ankle', 'ball'):
                p = feet.sole(P, Qg, s, k)
                ps = lowpass(p, fps, cfg.get('contact_hz', 6.0))
                m = contact_mask(p[:, 2] - floor, hspeed(ps, fps), fps, h_tol, v_tol)
                masks[s, k] = m
                hs.append(p[m, 2])
        hs = np.concatenate(hs) if any(len(h) for h in hs) else np.zeros(1)
        floor = float(np.median(hs))
    return floor, masks


def foot_lock(c, T, Q, fps, cfg, report, masks=None, floor=None, ramp=0.06):
    """Floor shift, contact locks (heel / toe-base sole points), flat stance, toe clamp, hips drop for reach, leg IK.
    masks / floor: precomputed (loops detect on the source take, before the seam cross-fade); ramp: lock blend-in (s)."""
    feet = Feet(c)
    if masks is None:
        floor, masks = detect(c, T, Q, fps, cfg)
    masks = {k: m.copy() for k, m in masks.items()}
    sole = {}
    T = T.copy()
    T[:, 0, 2] -= floor
    report['floor_shift_cm'] = round(-floor, 2)
    P, Qg = c.fk(T, Q)
    for key in masks:
        sole[key] = feet.sole(P, Qg, *key)
    if cfg.get('lock_all'):
        for key in masks:
            masks[key][:] = True
    # 2) per foot: translation of the whole foot + flattening rotation
    ankle_tgt, foot_q, toe_local = {}, {}, {}
    drop = np.zeros(len(T))
    for s in SIDES:
        ja = c.j(s + '_ankle')
        heel, ball = sole[s, 'ankle'], sole[s, 'ball']
        mh, mb = masks[s, 'ankle'], masks[s, 'ball']
        (wh, th), (wb, tb) = lock_weights(heel, mh, fps, ramp), lock_weights(ball, mb, fps, ramp)
        ch, cb = th - heel, tb - ball
        delta = (wh[:, None] * ch + wb[:, None] * cb) / np.maximum(1, wh + wb)[:, None]
        heel2, ball2, ank2 = heel + delta, ball + delta, P[:, ja] + delta
        wf = np.minimum(wh, wb)
        u = ball2 - heel2
        uf = u.copy(); uf[:, 2] = 0
        R = slerp(np.repeat(np.array([[0, 0, 0, 1.0]]), len(T), 0), qfrom_to(u, uf), wf)
        ank2 = heel2 + qrot(R, ank2 - heel2)
        fq = qnorm(qmul(R, Qg[:, ja]))
        # heel / toe-base sole below the floor outside a lock: lift the foot
        ball3 = heel2 + qrot(R, ball2 - heel2)
        lift = np.maximum(0, -np.minimum(heel2[:, 2], ball3[:, 2]))
        lift = np.maximum(lift, np.convolve(lift, np.ones(5) / 5, 'same'))
        ank2[:, 2] += lift
        # toe tip below the floor (the solve barely bends the toes at push-off): bend the toe bone up
        jb, jt = c.j(s + '_ball'), c.j(s + '_toe')
        B = heel2 + qrot(R, P[:, jb] + delta - heel2); B[:, 2] += lift
        tip = heel2 + qrot(R, P[:, jt] + delta - heel2); tip[:, 2] += lift
        v = tip - B
        L = np.linalg.norm(v, axis=-1)
        rise = np.maximum(0, TOE_TIP_H - tip[:, 2])
        ang = np.arcsin(np.clip((v[:, 2] + rise) / L, -1, 1)) - np.arcsin(np.clip(v[:, 2] / L, -1, 1))
        axis = np.cross(v, UP)
        axis /= np.maximum(np.linalg.norm(axis, axis=-1, keepdims=True), 1e-9)
        toe_g = qnorm(qmul(qaxis_angle(axis, ang), qmul(R, Qg[:, jb])))
        toe_local[s] = qnorm(qmul(qconj(fq), toe_g))
        report.setdefault('feet', {})[s] = dict(
            heel_contacts=len(_runs(mh)), ball_contacts=len(_runs(mb)),
            max_lock_shift_cm=round(float(np.max(np.linalg.norm(delta * np.maximum(wh, wb)[:, None], axis=-1))), 1),
            max_lift_cm=round(float(lift.max()), 1), max_toe_bend_deg=round(float(np.degrees(ang.max())), 1))
        ankle_tgt[s], foot_q[s] = ank2, fq
        # reach: hips drop needed so the leg is <= 99.5 % straight
        H = P[:, c.j(s + '_thigh')]
        l = (np.linalg.norm(P[:, c.j(s + '_knee')] - H, axis=-1) + np.linalg.norm(P[:, ja] - P[:, c.j(s + '_knee')], axis=-1)) * 0.995
        D = ank2 - H
        dxy2 = np.sum(D[:, :2] ** 2, -1)
        need = np.linalg.norm(D, axis=-1) > l
        dz = np.where(need & (l ** 2 > dxy2), -np.sqrt(np.maximum(l ** 2 - dxy2, 0)) - D[:, 2], 0)
        drop = np.maximum(drop, np.maximum(dz, 0))
    if drop.max() > 0:
        k = max(1, int(0.12 * fps))
        grown = np.array([drop[max(0, i - k):i + k + 1].max() for i in range(len(drop))])
        drop = np.maximum(lowpass(grown, fps, 2.0), drop)
        T[:, 0, 2] -= drop
        P, Qg = c.fk(T, Q)
    report['max_hips_drop_cm'] = round(float(drop.max()), 1)
    for s in SIDES:
        Q = leg_ik(c, T, Q, P, Qg, s, ankle_tgt[s], foot_q[s])
        Q[:, c.j(s + '_ball')] = toe_local[s]
    return T, Q, masks


# ------------------------------------------------------------------ loops
def find_loop(c, T, Q, fps, t0, t1, pmin, pmax, min_start=0):
    """Best (start, end) keys in [t0, t1] with period in [pmin, pmax] s: pose + velocity match, body-relative."""
    P, Qg = c.fk(T, Q)
    keys = [c.j(k) for k in ('l_hand', 'r_hand', 'head', 'l_knee', 'r_knee', 'l_ankle', 'r_ankle', 'l_ball', 'r_ball', 'chest')]
    hips = P[:, 0]
    fw = facing(P, c)
    rel = P[:, keys] - hips[:, None]
    # rotate into each frame's facing so a slight turn does not spoil the match
    yaw = np.arctan2(fw[:, 1], fw[:, 0])
    cs, sn = np.cos(-yaw), np.sin(-yaw)
    x = rel[..., 0] * cs[:, None] - rel[..., 1] * sn[:, None]
    y = rel[..., 0] * sn[:, None] + rel[..., 1] * cs[:, None]
    rel = np.stack([x, y, rel[..., 2]], -1)
    hz = np.zeros((len(P), 1, 3))
    hz[:, 0, 2] = hips[:, 2] - hips[:, 2].mean()
    rel = np.concatenate([rel, hz], 1)
    vel = np.gradient(rel, axis=0) * fps
    best = None
    a0, a1 = max(int(round(t0 * fps)), min_start), int(round(t1 * fps))
    for s in range(a0, a1):
        for e in range(s + int(pmin * fps), min(a1, s + int(pmax * fps)) + 1):
            cost = np.mean(np.sum((rel[e] - rel[s]) ** 2, -1)) + 0.01 * np.mean(np.sum((vel[e] - vel[s]) ** 2, -1))
            if best is None or cost < best[0]:
                best = (cost, s, e)
    return best


def tile(T, Q, travel, copies=3):
    """Repeat a cyclic clip (last key == first) with the root moved by `travel` per cycle."""
    body_T, body_Q = T[:-1], Q[:-1]
    Ts, Qs = [], []
    for k in range(copies):
        t = body_T.copy()
        t[:, 0, :2] += (k - copies // 2) * travel[:2]
        Ts.append(t); Qs.append(body_Q)
    return np.concatenate(Ts), np.concatenate(Qs)


# ------------------------------------------------------------------ edits (authored corrections)
def edit_twist(c, T, Q, fps, bone, deg, t0, t1, ramp, keep_ankle=False):
    """Rotate a bone about the axis to its child (thigh: about hip->knee), smooth plateau envelope.
    keep_ankle: for a thigh, IK the leg back so the ankle keeps its original path (knee and foot turn only)."""
    j = c.idx[bone]
    child = [i for i, p in enumerate(c.parents) if p == j][0]
    axis = T[0, child] / np.linalg.norm(T[0, child])
    t = np.arange(len(T)) / fps
    w = plateau(t, t0, t1, ramp)
    q = qaxis_angle(np.repeat(axis[None], len(T), 0), np.radians(deg) * w)
    P0, _ = c.fk(T, Q)
    Q = Q.copy()
    Q[:, j] = qnorm(qmul(Q[:, j], q))
    if keep_ankle:
        side = 'l' if bone.startswith('left') else 'r'
        P, Qg = c.fk(T, Q)
        Q = leg_ik(c, T, Q, P, Qg, side, P0[:, c.j(side + '_ankle')], Qg[:, c.j(side + '_ankle')])
    return Q


def edit_yaw_scale(c, T, Q, fps, k):
    """Scale the root's turn about the vertical axis by k (the mono solve under-reads turns seen side-on)."""
    P, Qg = c.fk(T, Q)
    f0 = facing(P, c)[:3].mean(0)
    loc = qrot(qconj(Q[0, 0]), f0)  # facing in the root's frame at key 0
    f = qrot(Q[:, 0], np.repeat(loc[None], len(T), 0))
    yaw = np.unwrap(np.arctan2(f[:, 1], f[:, 0]))
    extra = (k - 1) * (yaw - yaw[0])
    Q = Q.copy()
    Q[:, 0] = qnorm(qmul(qaxis_angle(np.repeat(UP[None], len(T), 0), extra), Q[:, 0]))
    return Q


def edit_ankle_pitch(c, T, Q, fps, side, deg, t0, t1, ramp):
    """Raise the toes (deg > 0) about the foot's lateral axis, smooth plateau envelope."""
    P, Qg = c.fk(T, Q)
    ja = c.j(side + '_ankle')
    jb = c.j(side + '_ball')
    t = np.arange(len(T)) / fps
    w = plateau(t, t0, t1, ramp)
    fwd = P[:, jb] - P[:, ja]
    lat = np.cross(fwd, UP)  # foot's right; rotating +about it lifts the toes? sign checked by caller
    lat /= np.linalg.norm(lat, axis=-1, keepdims=True)
    qw = qaxis_angle(lat, np.radians(deg) * w)
    newg = qnorm(qmul(qw, Qg[:, ja]))
    Q = Q.copy()
    Q[:, ja] = qnorm(qmul(qconj(Qg[:, c.parents[ja]]), newg))
    return Q


# ------------------------------------------------------------------ main
def process(name):
    cfg = CONFIGS[name]
    c = Clip(cfg['src'])
    fps = c.fps
    rep = dict(name=name, src=cfg['src'], fps=round(fps, 3))
    T, Q = c.T.copy(), c.Q.copy()
    T, Q = smooth(T, Q, fps, cfg.get('hz_body', 8.0), cfg.get('hz_legs', 10.0), c)
    for e in cfg.get('edits', []):
        if e[0] == 'twist':
            Q = edit_twist(c, T, Q, fps, *e[1:])
        elif e[0] == 'pitch':
            Q = edit_ankle_pitch(c, T, Q, fps, *e[1:])
        elif e[0] == 'yawscale':
            Q = edit_yaw_scale(c, T, Q, fps, *e[1:])
    loop = cfg['kind'] == 'loop'
    if loop:
        t0, t1, pmin, pmax = cfg['search']
        blend = cfg.get('blend', 0.25)
        cost, s, e = find_loop(c, T, Q, fps, t0, t1, pmin, pmax, min_start=int(round(blend * fps)) + 1)
        rep['loop_keys'] = [int(s), int(e)]
        rep['loop_cost'] = round(float(cost), 2)
        src_t = np.arange(s, e + 1) / fps
        # contacts from the source take: the seam cross-fade may slide a planted foot, which must still lock
        floor, msrc = detect(c, T, Q, fps, cfg)
        n, B = e - s, max(2, int(round(blend * fps)))
        lm = {}
        for k, m in msrc.items():
            ml = m[s:e + 1].copy()
            for i in range(n - B, n + 1):
                ml[i] |= m[s + i - n]
            lm[k] = np.concatenate([ml[:-1]] * 3)
        T, Q, drift = crossfade_loop(c, T, Q, fps, s, e, blend)
        rep['turn_drift_removed_deg'] = round(drift, 1)
        travel = T[-1, 0] - T[0, 0]
        period = (len(T) - 1) / fps
        rep['period_s'] = round(period, 3)
        rep['speed_cms'] = round(float(np.linalg.norm(travel[:2]) / period), 1)
        # orientation: the travel (or facing) goes to the configured direction
        P0, _ = c.fk(T, Q)
        fw = facing(P0, c).mean(0)
        ref = travel if cfg.get('align', 'travel') == 'travel' else fw
        target = cfg.get('dir', 90.0)
        rot = target - heading(ref)
        T, Q = rotate_root(T, Q, rot, about=T[0, 0] * [1, 1, 0])
        travel = T[-1, 0] - T[0, 0]
        P0, _ = c.fk(T, Q)
        rep['facing_vs_travel_deg'] = round(float(heading(facing(P0, c).mean(0)) - heading(travel)), 1)
        rep['travel_cms'] = [round(float(x), 3) for x in travel / period]
        Tt, Qt = tile(T, Q, travel)
        Tt, Qt, masks = foot_lock(c, Tt, Qt, fps, cfg, rep, masks=lm, floor=floor)
        if cfg.get('speed'):  # restride the locked loop to this travel speed (cm/s at play rate 1), then lock again
            k = rep['stride'] = round(cfg['speed'] / rep['speed_cms'], 3)
            Tt, Qt = restride(c, Tt, Qt, travel, k)
            travel = travel + (k - 1) * np.array([travel[0], travel[1], 0.0])
            # the feet are planted already: lock without the blend-in, which at these speeds drags a foot 20+ cm
            Tt, Qt, masks = foot_lock(c, Tt, Qt, fps, cfg, rep, masks=masks, floor=0.0, ramp=1e-6)
            rep['speed_cms'] = round(float(np.linalg.norm(travel[:2]) / period), 1)
            rep['travel_cms'] = [round(float(x), 3) for x in travel / period]
        m = len(T) - 1
        # per foot: stance time and the plant time in the loop (the contact after the longest swing) - the foot sync marker
        for s in SIDES:
            on = masks[s, 'ankle'] | masks[s, 'ball']
            runs = _runs(on)
            plants = [(a - pb, a) for (pa, pb), (a, b) in zip(runs, runs[1:]) if m <= a < 2 * m]
            rep['feet'][s].update(stance_s=round(float(on[m:2 * m].sum()) / fps, 3),
                                  plant_s=round((max(plants)[1] - m) / fps, 3) if plants else None)
        T = np.concatenate([Tt[m:2 * m], Tt[m:m + 1]])
        Q = np.concatenate([Qt[m:2 * m], Qt[m:m + 1]])
        T[-1, 0, :2] += travel[:2]
        # in place: remove the linear travel, centre the root
        u = np.linspace(0, 1, len(T))
        T[:, 0, :2] -= u[:, None] * travel[None, :2]
        T[:, 0, :2] -= T[:, 0, :2].mean(0)
    else:
        t0, t1 = cfg['window']
        if cfg.get('retime'):
            src_t = warp_times(fps, t0, t1, cfg['retime'])
        else:
            src_t = np.arange(int(round(t0 * fps)), int(round(t1 * fps)) + 1) / fps
        T, Q = sample(T, Q, src_t * fps)
        P0, _ = c.fk(T, Q)
        k = max(2, int(0.2 * fps))
        rot = cfg.get('dir', 90.0) - heading(facing(P0, c)[:k].mean(0))
        T, Q = rotate_root(T, Q, rot)
        T[:, 0, :2] -= T[0, 0, :2]
        T, Q, masks = foot_lock(c, T, Q, fps, cfg, rep)
    rep['keys'] = len(T)
    rep['length_s'] = round((len(T) - 1) / fps, 3)
    curves = {}
    for nm, (ts, vs) in c.d['curves'].items():
        vals = np.interp(src_t, ts, vs) if len(ts) else np.zeros(len(src_t))
        curves[nm] = [list(np.arange(len(src_t)) / fps), [float(v) for v in vals]]
    out = dict(src_path=c.d['path'], dst=cfg['dst'], fps=fps, rate=c.d['rate'], names=c.names, parents=c.parents, ref=c.d['ref'], loop=loop,
               frames=np.concatenate([T, Q], -1).round(6).tolist(), curves=curves, report=rep)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name + '.json'), 'w', encoding='utf-8') as fh:
        json.dump(out, fh)
    return rep


if __name__ == '__main__':
    for nm in sys.argv[1:] or list(CONFIGS):
        print(json.dumps(process(nm)))
