"""Per-clip quality report + contact sheets.  python analyze.py [names...]"""
import sys, os, json
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from mocap_lib import Clip, BONES, facing, yaw_deg, lowpass, speed, hspeed, RAW, ROOT

OUT = os.path.join(ROOT, 'report')
os.makedirs(OUT, exist_ok=True)
FOOT = [('L', 'l_ankle', 'l_ball'), ('R', 'r_ankle', 'r_ball')]


def contacts(h, hs, h_floor, fps, h_tol=4.0, v_tol=35.0):
    """Boolean per frame: point within h_tol of the floor and slower than v_tol (cm/s)."""
    c = (h < h_floor + h_tol) & (hs < v_tol)
    # drop 1-frame flickers
    out = c.copy()
    for i in range(1, len(c) - 1):
        if c[i - 1] == c[i + 1] != c[i]:
            out[i] = c[i - 1]
    return out


def runs(mask):
    r, s = [], None
    for i, m in enumerate(mask):
        if m and s is None:
            s = i
        if not m and s is not None:
            r.append((s, i - 1)); s = None
    if s is not None:
        r.append((s, len(mask) - 1))
    return r


def analyze(name, sheet=True):
    c = Clip(name)
    P, _ = c.fk()
    n, fps = c.n, c.fps
    t = np.arange(n) / fps
    fw = facing(P, c)
    yaw = yaw_deg(fw)
    f0 = fw[:max(3, int(0.2 * fps))].mean(0); f0[2] = 0; f0 /= np.linalg.norm(f0)
    r0 = np.cross(np.array([0, 0, 1.0]), f0)  # right = up x fwd (UE left-handed: fwd x right = up)
    # express positions in the clip's initial facing frame: x fwd, y right, z up
    def loc(p):
        return np.stack([p @ f0, p @ r0, p[..., 2]], -1)
    L = loc(P)
    hips = L[:, c.j('hips')]
    rep = dict(name=name, skel='MH' if c.mh else 'FB', fps=round(fps, 3), keys=n, sec=round(n / fps, 3))
    solved = c.curve('BodyFrameSolved')
    if solved is not None:
        rep['unsolved_frames'] = int(np.sum(solved < 0.5))
    d = hips[-1] - hips[0]
    rep['hips_travel_fwd_right'] = [round(d[0], 1), round(d[1], 1)]
    rep['hips_path_cm'] = round(float(np.sum(np.linalg.norm(np.diff(hips[:, :2], axis=0), axis=1))), 1)
    hsp = hspeed(lowpass(hips, fps, 3.0), fps)
    rep['hips_speed_mean_max'] = [round(float(hsp.mean()), 0), round(float(hsp.max()), 0)]
    rep['hips_z_min_max'] = [round(float(hips[:, 2].min()), 1), round(float(hips[:, 2].max()), 1)]
    rep['yaw_change_deg'] = round(float(yaw[-1] - yaw[0]), 1)
    rep['yaw_min_max'] = [round(float(yaw.min() - yaw[0]), 1), round(float(yaw.max() - yaw[0]), 1)]
    # floor: low percentile of toe/ankle heights
    lows = np.concatenate([L[:, c.j(k)][:, 2] for _, a, b in FOOT for k in (b,)])
    floor = float(np.percentile(lows, 3))
    rep['floor_toe_p3'] = round(floor, 2)
    rep['min_point_z'] = round(float(L[..., 2].min()), 1)
    feet = {}
    for side, ank, ball in FOOT:
        pa, pb = L[:, c.j(ank)], L[:, c.j(ball)]
        hb = pb[:, 2]; sb = hspeed(pb, fps); sa = hspeed(pa, fps)
        cm = contacts(hb, sb, floor, fps)
        rr = [x for x in runs(cm) if x[1] - x[0] + 1 >= max(2, int(0.06 * fps))]
        slides = []
        for s0, s1 in rr:
            seg = pb[s0:s1 + 1, :2]
            slides.append(float(np.max(np.linalg.norm(seg - seg[0], axis=1))))
        feet[side] = dict(contacts=[(round(s0 / fps, 2), round(s1 / fps, 2)) for s0, s1 in rr],
                          slide_cm=[round(x, 1) for x in slides], ball_h_max=round(float(hb.max() - floor), 1),
                          ankle_speed_max=round(float(sa.max()), 0))
    rep['feet'] = feet
    # jitter: residual from a 6 Hz low-pass (cm RMS), per region
    jit = {}
    for grp, keys in dict(head=['head'], hands=['l_hand', 'r_hand'], feet=['l_ball', 'r_ball', 'l_ankle', 'r_ankle'], hips=['hips']).items():
        res = []
        for k in keys:
            x = L[:, c.j(k)]
            res.append(np.sqrt(np.mean(np.sum((x - lowpass(x, fps, 6.0)) ** 2, -1))))
        jit[grp] = round(float(np.mean(res)), 2)
    rep['jitter_rms_cm'] = jit
    # spikes: max per-frame acceleration-like jump on torso joints (cm per frame^2)
    acc = np.linalg.norm(np.diff(L[:, [c.j('hips'), c.j('chest'), c.j('head')]], 2, axis=0), axis=-1)
    rep['torso_jerk_max_cm'] = round(float(acc.max()), 2)
    # knees: min angle (180 = straight)
    kn = {}
    for side in 'lr':
        a, b, k = L[:, c.j(side + '_thigh')], L[:, c.j(side + '_ankle')], L[:, c.j(side + '_knee')]
        u, v = a - k, b - k
        ang = np.degrees(np.arccos(np.clip(np.sum(u * v, -1) / np.linalg.norm(u, axis=-1) / np.linalg.norm(v, axis=-1), -1, 1)))
        kn[side] = [round(float(ang.min()), 0), round(float(ang.max()), 0)]
    rep['knee_deg_min_max'] = kn
    if sheet:
        draw(c, L, floor, t, yaw, rep)
    return rep


def draw(c, L, floor, t, yaw, rep):
    n = c.n
    K = 16
    fr = np.linspace(0, n - 1, K).round().astype(int)
    fig = plt.figure(figsize=(24, 13))
    gs = fig.add_gridspec(4, K // 2 + 0, height_ratios=[1, 1, 1, 1])
    lims = []
    for row, (ax_i, label) in enumerate([((0, 2), 'side (fwd, up)'), ((1, 2), 'front (right, up)')]):
        for k, f in enumerate(fr):
            ax = fig.add_subplot(gs[row * 2 + k // (K // 2), k % (K // 2)])
            hx = L[f, c.j('hips'), ax_i[0]]
            for a, b in BONES:
                pa, pb = L[f, c.j(a)], L[f, c.j(b)]
                col = 'tab:blue' if a.startswith('l_') or b.startswith('l_') else ('tab:red' if a.startswith('r_') or b.startswith('r_') else 'k')
                ax.plot([pa[ax_i[0]] - hx, pb[ax_i[0]] - hx], [pa[2], pb[2]], color=col, lw=2)
            ax.axhline(floor, color='g', lw=0.8)
            ax.set_xlim(-90, 90); ax.set_ylim(floor - 5, floor + 200); ax.set_aspect('equal')
            ax.set_xticks([]); ax.set_yticks([])
            ax.set_title('%s f%d %.2fs' % ('S' if row == 0 else 'F', f, f / c.fps), fontsize=8)
    fig.suptitle('%s  (%s, %.2f fps, %d keys)  blue=left red=right' % (c.name, rep['skel'], c.fps, n))
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, c.name + '_poses.png'), dpi=60)
    plt.close(fig)
    # time plots
    fig, axs = plt.subplots(4, 1, figsize=(14, 10), sharex=True)
    for side, ank, ball in FOOT:
        col = 'tab:blue' if side == 'L' else 'tab:red'
        axs[0].plot(t, L[:, c.j(ball), 2] - floor, color=col, label=side + ' ball h')
        axs[0].plot(t, L[:, c.j(ank), 2] - floor, color=col, ls='--', label=side + ' ankle h')
        axs[1].plot(t, hspeed(L[:, c.j(ball)], c.fps), color=col, label=side + ' ball hspeed')
        axs[2].plot(t, L[:, c.j(ball), 0], color=col, label=side + ' ball fwd')
        axs[2].plot(t, L[:, c.j(ball), 1], color=col, ls=':', label=side + ' ball right')
    axs[2].plot(t, L[:, c.j('hips'), 0], 'k', label='hips fwd')
    axs[2].plot(t, L[:, c.j('hips'), 1], 'k:', label='hips right')
    axs[3].plot(t, yaw - yaw[0], 'k', label='facing yaw')
    ax2 = axs[3].twinx()
    ax2.plot(t, L[:, c.j('hips'), 2], 'g', label='hips z')
    s = c.curve('BodyFrameSolved')
    if s is not None:
        ax2.plot(t, s * 10 + L[:, c.j('hips'), 2].min() - 15, 'm', lw=0.8, label='solved')
    for a in axs:
        a.grid(alpha=0.3); a.legend(fontsize=7, loc='upper right')
    axs[0].set_ylabel('cm'); axs[1].set_ylabel('cm/s'); axs[2].set_ylabel('cm'); axs[3].set_ylabel('deg')
    axs[1].set_ylim(0, 800)
    fig.suptitle(c.name)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, c.name + '_curves.png'), dpi=60)
    plt.close(fig)


if __name__ == '__main__':
    names = sys.argv[1:] or sorted(f[:-5] for f in os.listdir(RAW) if f.endswith('.json'))
    reps = []
    for nm in names:
        reps.append(analyze(nm))
        print(json.dumps(reps[-1]))
    with open(os.path.join(OUT, 'report.json'), 'w', encoding='utf-8') as fh:
        json.dump(reps, fh, indent=1)
