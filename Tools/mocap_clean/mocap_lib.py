"""Offline mocap maths on clips dumped by ue_export.py (numpy only; UE quaternions are (x, y, z, w), Hamilton)."""
import json, os
import numpy as np

# data (raw/ clean/ report/) lives in <project>/Saved/MocapClean, which git ignores
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'Saved', 'MocapClean'))
RAW = os.path.join(ROOT, 'raw')

# Key joints per skeleton (Footballer = Mixamo names, MH = MetaHuman names).
KEYS_FB = dict(hips='hips', chest='spine2', neck='neck', head='head', top='headtop_end',
               l_arm='leftarm', l_fore='leftforearm', l_hand='lefthand', r_arm='rightarm', r_fore='rightforearm', r_hand='righthand',
               l_thigh='leftupleg', l_knee='leftleg', l_ankle='leftfoot', l_ball='lefttoebase', l_toe='lefttoe_end',
               r_thigh='rightupleg', r_knee='rightleg', r_ankle='rightfoot', r_ball='righttoebase', r_toe='righttoe_end')
KEYS_MH = dict(hips='pelvis', chest='spine_05', neck='neck_01', head='head', top='head',
               l_arm='upperarm_l', l_fore='lowerarm_l', l_hand='hand_l', r_arm='upperarm_r', r_fore='lowerarm_r', r_hand='hand_r',
               l_thigh='thigh_l', l_knee='calf_l', l_ankle='foot_l', l_ball='ball_l', l_toe='ball_l',
               r_thigh='thigh_r', r_knee='calf_r', r_ankle='foot_r', r_ball='ball_r', r_toe='ball_r')
BONES = [('hips', 'chest'), ('chest', 'neck'), ('neck', 'head'), ('head', 'top'),
         ('chest', 'l_arm'), ('l_arm', 'l_fore'), ('l_fore', 'l_hand'), ('chest', 'r_arm'), ('r_arm', 'r_fore'), ('r_fore', 'r_hand'),
         ('hips', 'l_thigh'), ('l_thigh', 'l_knee'), ('l_knee', 'l_ankle'), ('l_ankle', 'l_ball'), ('l_ball', 'l_toe'),
         ('hips', 'r_thigh'), ('r_thigh', 'r_knee'), ('r_knee', 'r_ankle'), ('r_ankle', 'r_ball'), ('r_ball', 'r_toe')]


def qmul(a, b):
    ax, ay, az, aw = np.moveaxis(a, -1, 0)
    bx, by, bz, bw = np.moveaxis(b, -1, 0)
    return np.stack([aw * bx + ax * bw + ay * bz - az * by,
                     aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw,
                     aw * bw - ax * bx - ay * by - az * bz], -1)


def qconj(q):
    return q * np.array([-1, -1, -1, 1.0])


def qrot(q, v):
    u, w = q[..., :3], q[..., 3:4]
    t = 2.0 * np.cross(u, v)
    return v + w * t + np.cross(u, t)


def qnorm(q):
    return q / np.linalg.norm(q, axis=-1, keepdims=True)


def qfrom_to(a, b):
    """Shortest rotation taking direction a to direction b (both (...,3))."""
    a = a / np.linalg.norm(a, axis=-1, keepdims=True)
    b = b / np.linalg.norm(b, axis=-1, keepdims=True)
    c = np.cross(a, b)
    d = np.sum(a * b, -1, keepdims=True)
    q = np.concatenate([c, 1.0 + d], -1)
    bad = (1.0 + d[..., 0]) < 1e-8
    if np.any(bad):  # opposite: any perpendicular axis
        perp = np.cross(a[bad], np.array([1.0, 0, 0]))
        small = np.linalg.norm(perp, axis=-1) < 1e-6
        perp[small] = np.cross(a[bad][small], np.array([0, 1.0, 0]))
        q[bad] = np.concatenate([perp, np.zeros((perp.shape[0], 1))], -1)
    return qnorm(q)


def qaxis_angle(axis, ang):
    axis = np.asarray(axis, float)
    axis = axis / np.linalg.norm(axis, axis=-1, keepdims=True)
    s = np.sin(np.asarray(ang)[..., None] / 2)
    return np.concatenate([axis * s, np.cos(np.asarray(ang)[..., None] / 2)], -1)


def qcontinuous(q):
    """Flip signs so consecutive keys sit in the same hemisphere (needed before filtering)."""
    q = q.copy()
    for i in range(1, q.shape[0]):
        flip = np.sum(q[i] * q[i - 1], -1) < 0
        q[i][flip] *= -1
    return q


class Clip:
    def __init__(self, name, data=None):
        if data is None:
            with open(os.path.join(RAW, name + '.json'), encoding='utf-8') as fh:
                data = json.load(fh)
        self.name, self.d = name, data
        self.fps = data['fps']
        self.names = data['names']
        self.parents = data['parents']
        f = np.asarray(data['frames'], float)  # (F, B, 10)
        self.T, self.Q = f[..., 0:3], qnorm(f[..., 3:7])
        self.S = f[..., 7:10] if f.shape[-1] >= 10 else np.ones_like(self.T)
        self.ref = np.asarray(data['ref'], float)
        self.mh = 'pelvis' in self.names
        self.K = KEYS_MH if self.mh else KEYS_FB
        self.idx = {n: i for i, n in enumerate(self.names)}
        assert all(p < i for i, p in enumerate(self.parents)), 'parents must come first'

    @property
    def n(self):
        return self.T.shape[0]

    def j(self, key):
        return self.idx[self.K[key]]

    def fk(self, T=None, Q=None):
        T = self.T if T is None else T
        Q = self.Q if Q is None else Q
        Pg = np.zeros_like(T)
        Qg = np.zeros_like(Q)
        for b, p in enumerate(self.parents):
            if p < 0:
                Pg[:, b], Qg[:, b] = T[:, b], Q[:, b]
            else:
                Qg[:, b] = qmul(Qg[:, p], Q[:, b])
                Pg[:, b] = Pg[:, p] + qrot(Qg[:, p], T[:, b])
        return Pg, Qg

    def curve(self, name):
        if name not in self.d['curves']:
            return None
        t, v = self.d['curves'][name]
        times = np.arange(self.n) / self.fps
        return np.interp(times, t, v) if len(t) else np.zeros(self.n)


def facing(P, clip):
    """Unit forward (horizontal) per frame: (right side - left side) x up, from hips and shoulders."""
    r = (P[:, clip.j('r_thigh')] - P[:, clip.j('l_thigh')]) + (P[:, clip.j('r_arm')] - P[:, clip.j('l_arm')])
    r[:, 2] = 0
    f = np.cross(r, np.array([0, 0, 1.0]))
    return f / np.linalg.norm(f, axis=-1, keepdims=True)


def yaw_deg(f):
    return np.degrees(np.unwrap(np.arctan2(f[:, 1], f[:, 0])))


def lowpass(x, fps, cutoff, axis=0):
    from scipy.signal import butter, filtfilt
    if cutoff >= 0.5 * fps * 0.98:
        return x.copy()
    b, a = butter(2, cutoff / (0.5 * fps))
    pad = min(3 * max(len(a), len(b)), x.shape[axis] - 1)
    return filtfilt(b, a, x, axis=axis, padtype='odd', padlen=pad)


def speed(P, fps):
    v = np.gradient(P, axis=0) * fps
    return np.linalg.norm(v, axis=-1)


def hspeed(P, fps):
    v = np.gradient(P[..., :2], axis=0) * fps
    return np.linalg.norm(v, axis=-1)
