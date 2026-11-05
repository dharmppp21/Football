"""Minimal BVH forward kinematics, the ground truth for checking imports: joint world positions at a frame.
positions(path, frame) -> {joint: (x, y, z)} in the file's own units and axes (BVH is Y-up)."""
import math


def _rot(axis, deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return {'X': [[1, 0, 0], [0, c, -s], [0, s, c]], 'Y': [[c, 0, s], [0, 1, 0], [-s, 0, c]], 'Z': [[c, -s, 0], [s, c, 0], [0, 0, 1]]}[axis]


def _mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def _app(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) for i in range(3)]


def frames(path):
    l = open(path).read().split(chr(10))
    i = next(k for k, x in enumerate(l) if x.strip().startswith("MOTION"))
    return int(l[i + 1].split(":")[1])


def positions(path, frame):
    lines = open(path).read().split('\n')
    joints, stack, i = [], [], 0
    while not lines[i].strip().startswith('MOTION'):
        t = lines[i].split()
        if t and t[0] in ('ROOT', 'JOINT'):
            joints.append(dict(name=t[1], parent=stack[-1] if stack else None, chans=[]))
        elif t and t[0] == 'End':
            joints.append(dict(name=None, parent=stack[-1], chans=[]))
        elif t and t[0] == '{':
            stack.append(len(joints) - 1)
        elif t and t[0] == '}':
            stack.pop()
        elif t and t[0] == 'OFFSET':
            joints[-1]['off'] = [float(x) for x in t[1:4]]
        elif t and t[0] == 'CHANNELS':
            joints[-1]['chans'] = t[2:]
        i += 1
    n = int(lines[i + 1].split(':')[1])
    vals = [float(x) for x in lines[i + 3 + min(frame, n - 1)].split()]
    out, k, R, P = {}, 0, {}, {}
    for j, J in enumerate(joints):
        loc, rot = list(J['off']), [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
        for c in J['chans']:
            v = vals[k]; k += 1
            if c.endswith('position'):
                loc['XYZ'.index(c[0])] = v if J['parent'] is None else loc['XYZ'.index(c[0])] + v
            else:
                rot = _mul(rot, _rot(c[0], v))
        if J['parent'] is None:
            R[j], P[j] = rot, loc
        else:
            p = J['parent']
            R[j], P[j] = _mul(R[p], rot), [a + b for a, b in zip(P[p], _app(R[p], loc))]
        if J['name']:
            out[J['name']] = P[j]
    return out
