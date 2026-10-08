"""Master plan U.4: fit the BP kicks (power / lift / spin) to their tuned goal-line results under the ball's physics.

fly() integrates a kick the way Chaos does at 60 fps: two 1/120 s substeps a frame, semi-implicit Euler, the air force
from UBallMovementComponent.compute_aero_force (the game's own C++) at the start of each step, gravity, spin decaying
with the body's AngularDamping. It starts on the kick spot and stops where the ball centre crosses x = 5240, 30 cm before
the goal line, as ball_parity_test.kick_report measures it. Airborne kicks only: a ball that lands first needs PIE
(bounce, skid, roll).

Use from util.execute_python (no PIE needed):
    import importlib.util, sys
    spec = importlib.util.spec_from_file_location('kick_fit', r'<project>/Tools/kick_fit.py')
    m = importlib.util.module_from_spec(spec); sys.modules['kick_fit'] = m; spec.loader.exec_module(m)
    m.fit('shoot', {0.75: dict(z=225.9), 1.0: dict(z=352.3)}, ['base', 'lift'])
"""
import importlib.util
import math
import os
import unreal

DT = 1.0 / 120.0
GRAVITY = 980.0
MASS = 0.43
RADIUS = 11.0
ANGULAR_DAMPING = 0.02
CONTACT_HEIGHT = 1.5
LINE_X = 5240.0
SPOT = (3270.0, 0.0, 11.0)

# The six BP_FootballPlayer kicks, from ball_parity_test.BP_KICKS (the one mirror of the Blueprint values).
# power = base * k, k = floor + (1 - floor) * charge^exp; ApplyKick launches at power * normalize(aim + (0, 0, lift)) with
# spin (deg/s) = 10 * (spin_top [* k] * (aim x up) + spin_right * k * up).
_spec = importlib.util.spec_from_file_location('_bp_kicks', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ball_parity_test.py'))
_bpt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_bpt)
KICKS = {name: dict(base=b, floor=f, exp=e, lift=l, spin_right=sr, spin_top=st, top_scales=ts)
         for name, (b, f, e, l, sr, st, ts) in _bpt.BP_KICKS.items()}


def launch(kick, charge):
    """Launch velocity (cm/s) and spin (deg/s) of a kick along +X at this charge."""
    k = kick['floor'] + (1.0 - kick['floor']) * charge ** kick['exp']
    power, lift = kick['base'] * k, kick['lift']
    n = math.sqrt(1.0 + lift * lift)
    top = kick['spin_top'] * (k if kick.get('top_scales') else 1.0)
    return (power / n, 0.0, power * lift / n), (0.0, -10.0 * top, 10.0 * kick['spin_right'] * k)


def fly(kick, charge, ball=None, t_max=4.0):
    """Where the kick crosses the goal-line plane: dict(t, y, z), or None if it comes down first."""
    ball = ball or unreal.get_default_object(unreal.BallMovementComponent)
    v, w = launch(kick, charge)
    v, w, p = list(v), list(w), list(SPOT)
    t, left = 0.0, False
    while t < t_max:
        airborne = p[2] - RADIUS > CONTACT_HEIGHT
        f = ball.compute_aero_force(unreal.Vector(*v), unreal.Vector(*w), airborne)
        v = [v[0] + f.x / MASS * DT, v[1] + f.y / MASS * DT, v[2] + (f.z / MASS - GRAVITY) * DT]
        q = [p[i] + v[i] * DT for i in range(3)]
        w = [c * (1.0 - ANGULAR_DAMPING * DT) for c in w]
        left = left or q[2] - RADIUS > CONTACT_HEIGHT
        if p[0] < LINE_X <= q[0]:
            k = (LINE_X - p[0]) / (q[0] - p[0])
            return dict(t=t + k * DT, y=p[1] + k * (q[1] - p[1]), z=p[2] + k * (q[2] - p[2]))
        if left and q[2] < RADIUS:
            return None
        p, t = q, t + DT
    return None


def fit(name, targets, unknowns, start=None, iterations=12, tol=0.5):
    """Newton fit of `unknowns` (keys of the kick: base, lift, spin_right, spin_top) so the kick hits `targets`
    {charge: {'y': cm, 'z': cm}} at the goal line. Returns (kick values, residuals in cm)."""
    kick = dict(start or KICKS[name])
    goals = [(c, axis, value) for c, want in sorted(targets.items()) for axis, value in sorted(want.items())]
    steps = dict(base=10.0, lift=0.005, spin_right=5.0, spin_top=5.0)

    def residuals(kk):
        out = []
        for c, axis, value in goals:
            hit = fly(kk, c)
            out.append(1e4 if hit is None else hit[axis] - value)
        return out
    r = residuals(kick)
    for _ in range(iterations):
        if max(abs(x) for x in r) < tol:
            break
        cols = []
        for u in unknowns:
            kk = dict(kick)
            kk[u] += steps[u]
            cols.append([(a - b) / steps[u] for a, b in zip(residuals(kk), r)])
        # Least squares J d = -r (normal equations, small and well conditioned here).
        n = len(unknowns)
        jtj = [[sum(cols[i][k] * cols[j][k] for k in range(len(r))) for j in range(n)] for i in range(n)]
        jtr = [sum(cols[i][k] * r[k] for k in range(len(r))) for i in range(n)]
        d = _solve(jtj, [-x for x in jtr])
        for u, x in zip(unknowns, d):
            kick[u] += x
        r = residuals(kick)
    return kick, [round(x, 2) for x in r]


def _solve(a, b):
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for i in range(n):
        piv = max(range(i, n), key=lambda k: abs(m[k][i]))
        m[i], m[piv] = m[piv], m[i]
        for k in range(i + 1, n):
            f = m[k][i] / m[i][i]
            for j in range(i, n + 1):
                m[k][j] -= f * m[i][j]
    x = [0.0] * n
    for i in range(n - 1, -1, -1):
        x[i] = (m[i][n] - sum(m[i][j] * x[j] for j in range(i + 1, n))) / m[i][i]
    return x
