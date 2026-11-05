"""No-ball turn probe (PIE, fixed 60 fps): steady runs that turn like a thumb on a gamepad (sweeps, flicks, a weave, plus
+-1.5 deg of stick noise), traced every frame. Measures facing jitter, facing vs travel (skating), planted-foot slide and the
speed dip during the turn. Load it as a module, start PIE, then start('label'). Results: Saved/TurnFreeProbe/<label>.json."""
import json
import math
import os
import random
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'TurnFreeProbe')
START = (-1500.0, 0.0)
RUN_IN, AFTER = 1.2, 1.3  # s of straight running before the turn starts, s traced after it
NOISE = 1.5  # deg, uniform per frame


def _sweep(deg, dur):
    return lambda t: deg * min(1.0, max(0.0, t) / dur) if dur > 0 else (deg if t >= 0 else 0.0)


SCENARIOS = [dict(name='jog_sweep90', speed=430.0, yaw=_sweep(90.0, 0.35), noise=True),
             dict(name='jog_flick90', speed=430.0, yaw=_sweep(90.0, 0.0), noise=False),
             dict(name='jog_flick180', speed=430.0, yaw=_sweep(180.0, 0.0), noise=False),
             dict(name='sprint_sweep90', speed=660.0, yaw=_sweep(90.0, 0.35), noise=True),
             dict(name='jog_weave', speed=430.0, yaw=lambda t: 25.0 * math.sin(2 * math.pi * 0.6 * max(0.0, t)), noise=True),
             dict(name='jog_straight_noise', speed=430.0, yaw=lambda t: 0.0, noise=True)]

_state = {}


def _world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()


def _actors(world):
    ball = unreal.GameplayStatics.get_actor_of_class(world, unreal.load_class(None, '/Game/Football/Blueprints/Ball/BP_Football.BP_Football_C'))
    player = unreal.GameplayStatics.get_actor_of_class(world, unreal.load_class(None, '/Game/Football/Blueprints/Characters/BP_FootballPlayer.BP_FootballPlayer_C'))
    return ball, player


def _set_fixed_rate(on):
    eng = unreal.find_object(None, '/Engine/Transient.UnrealEdEngine_0')
    eng.set_editor_property('FixedFrameRate', 60.0 if on else 30.0)
    eng.set_editor_property('bUseFixedFrameRate', on)


def _wrap(a):
    return (a + 180.0) % 360.0 - 180.0


def _metrics(sc, rows):
    """rows: [t, yaw, vel_yaw, speed, stick_yaw, lx, ly, lz, rx, ry, rz, anim_speed]; t < 0 = run-in."""
    turn = [r for r in rows if r[0] >= -0.1]
    t = [r[0] for r in turn]
    yaw = [turn[0][1]]
    for r in turn[1:]:
        yaw.append(yaw[-1] + _wrap(r[1] - yaw[-1]))  # unwrapped facing
    dt = 1.0 / 60.0
    rate = [(b - a) / dt for a, b in zip(yaw, yaw[1:])]
    acc = [abs(b - a) * dt for a, b in zip(rate, rate[1:])]  # deg/frame^2
    rev = sum(1 for a, b in zip(rate, rate[1:]) if abs(a) > 20 and abs(b) > 20 and a * b < 0)
    final = turn[-1][4]
    settle = next((r[0] for r in turn if abs(_wrap(r[1] - final)) < 10 and r[0] >= 0), None)
    skate = [abs(_wrap(r[1] - r[2])) for r in turn if r[3] > 100]
    out = dict(name=sc['name'], yaw_rate_reversals=rev, yaw_acc_max=round(max(acc), 2), yaw_acc_rms=round((sum(a * a for a in acc) / len(acc)) ** 0.5, 3),
               settle_s=settle, skate_max_deg=round(max(skate), 1) if skate else None, skate_mean_deg=round(sum(skate) / len(skate), 1) if skate else None,
               speed_min=round(min(r[3] for r in turn), 1))
    # planted-foot slide while turning (first 1.0 s): each foot's ground speed over its slowest 20 % of frames
    win = [r for r in rows if 0.0 <= r[0] <= 1.0]
    slides = []
    for i0 in (5, 8):
        v = sorted(math.hypot(win[k + 1][i0] - win[k - 1][i0], win[k + 1][i0 + 1] - win[k - 1][i0 + 1]) / (2 * dt) for k in range(1, len(win) - 1))
        slides += v[:max(1, len(v) // 5)]
    out['turn_foot_slide'] = round(sum(slides) / len(slides), 1)
    vis = [turn[0][12]]
    for r in turn[1:]:
        vis.append(vis[-1] + _wrap(r[12] - vis[-1]))
    vrate = [(b - a) / dt for a, b in zip(vis, vis[1:])]
    vacc = [abs(b - a) * dt for a, b in zip(vrate, vrate[1:])]
    out['visual_yaw_acc_rms'] = round((sum(a * a for a in vacc) / len(vacc)) ** 0.5, 3)
    out['visual_yaw_acc_max'] = round(max(vacc), 2)
    out['visual_settle_s'] = next((r[0] for r in turn if abs(_wrap(r[12] - final)) < 10 and r[0] >= 0), None)
    vskate = [abs(_wrap(r[12] - r[2])) for r in turn if r[3] > 100]
    out['visual_skate_mean_deg'] = round(sum(vskate) / len(vskate), 1) if vskate else None
    return out


def _tick(delta):
    s = _state
    try:
        world = _world()
        if world is None:
            raise RuntimeError('PIE stopped')
        now = unreal.GameplayStatics.get_time_seconds(world)
        ball, player = _actors(world)
        cmc = player.get_component_by_class(unreal.CharacterMovementComponent)
        sc = SCENARIOS[s['index']]
        cmc.max_walk_speed = sc['speed']
        if s['step'] == 0:
            cmc.stop_movement_immediately()
            z = player.get_actor_location().z
            player.set_actor_location(unreal.Vector(START[0], START[1], z), False, True)
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0), True)
            ball.call_method('ResetBall', (unreal.Transform(unreal.Vector(START[0] - 3000.0, START[1], 11.0), unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1)), False))
            s.update(step=1, t0=now + 0.3 + RUN_IN, rows=[], rng=random.Random(7))
            return
        t = now - s['t0']
        if now < s['t0'] - RUN_IN:
            return
        if t > AFTER:
            s['results'].append(_metrics(sc, s['rows']))
            s['traces'][sc['name']] = s['rows']
            s['index'] += 1
            if s['index'] >= len(SCENARIOS):
                _finish('finished')
                return
            s['step'] = 0
            return
        stick = sc['yaw'](t) if t >= 0 else 0.0
        if sc['noise']:
            stick += s['rng'].uniform(-NOISE, NOISE)
        r = math.radians(stick)
        player.add_movement_input(unreal.Vector(math.cos(r), math.sin(r), 0), 1.0, False)
        mesh = player.get_editor_property('mesh')
        lt, rt = mesh.get_socket_location('LeftUpLeg'), mesh.get_socket_location('RightUpLeg')
        anim = mesh.get_anim_instance()
        v = player.get_velocity()
        l, rf = mesh.get_socket_location('LeftFoot'), mesh.get_socket_location('RightFoot')
        s['rows'].append([round(t, 4), round(player.get_actor_rotation().yaw, 3), round(math.degrees(math.atan2(v.y, v.x)), 3), round(math.hypot(v.x, v.y), 2),
                          round(stick, 3), round(l.x, 2), round(l.y, 2), round(l.z, 2), round(rf.x, 2), round(rf.y, 2), round(rf.z, 2),
                          round(anim.get_editor_property('speed'), 2) if anim else -1.0,
                          round(math.degrees(math.atan2(-(rt.x - lt.x), rt.y - lt.y)), 3)])
    except Exception as e:  # never leave a broken callback running
        _finish('error: %r' % e)


def _finish(msg):
    s = _state
    handle = s.pop('handle', None)
    if handle is not None:
        unreal.unregister_slate_post_tick_callback(handle)
    _set_fixed_rate(False)
    try:
        world = _world()
        if world is not None and 'speed0' in s:
            _actors(world)[1].get_component_by_class(unreal.CharacterMovementComponent).max_walk_speed = s['speed0']
    except Exception:
        pass
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, s.get('label', 'probe') + '.json'), 'w', encoding='utf-8') as f:
        json.dump(dict(result=msg, results=s.get('results', []), traces=s.get('traces', {}),
                       cols='t yaw vel_yaw speed stick_yaw lx ly lz rx ry rz anim_speed'), f)


def start(label, stiffness=None, sample_stiffness=None):
    if _state.get('handle') is not None:
        return 'already running'
    world = _world()
    if world is None:
        return 'start PIE first'
    player = _actors(world)[1]
    _state.clear()
    cmc = player.get_component_by_class(unreal.CharacterMovementComponent)
    _state.update(label=label, index=0, step=0, results=[], traces={}, speed0=cmc.max_walk_speed)
    if stiffness is not None:  # this PIE instance only, no asset changes
        cmc.set_editor_property('turn_stiffness', float(stiffness))
    if sample_stiffness is not None:
        cmc.set_editor_property('sample_turn_stiffness', float(sample_stiffness))
    _set_fixed_rate(True)
    _state['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'started'
