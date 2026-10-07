"""Ball physics / dribble A-B test (framework Phase 1).

Runs fixed scenarios in PIE and saves every recorded ball frame (UBallMovementComponent recorder) to
Saved/BallParity/<label>_r<run>_<scenario>.json, so two builds can be compared frame by frame.

Use (PIE must be running), from util.execute_python. Load it as a module so its state survives between calls:
    import importlib.util, sys
    spec = importlib.util.spec_from_file_location('ball_parity_test', r'<project>/Tools/ball_parity_test.py')
    m = importlib.util.module_from_spec(spec); sys.modules['ball_parity_test'] = m; spec.loader.exec_module(m)
    m.start('baseline', runs=2)
    ... ~25 s later: sys.modules['ball_parity_test'].status()

Scenarios:
- curl_shot / ground_pass: the ball is reset to an open spot in the left half and kicked through the ball's
  own ApplyKick (the player's tick is paused so its dribble cannot touch the ball).
- dribble_40 / dribble_120: the ball is reset that far to the side of the player's BallGuidePoint and the
  player is given possession (TestForcePossession); the dribble servo pulls it in. The player stands still.
- KICK_SCENARIOS (master plan Phase 1): the six tuned BP kicks at 40/75/100% charge from 20 m at the goal;
  start(label, runs=1, scenarios=KICK_SCENARIOS), then kick_report(label) and compare(a, b, runs, KICK_SCENARIOS).
"""
import json
import os
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'BallParity')

SCENARIOS = [
    dict(name='curl_shot', kind='kick', spot=(-2500.0, -2500.0, 11.0), dir=(0.0, 1.0, 0.0),
         power=2500.0, lift=0.26, spin_right=1500.0, spin_top=0.0, duration=1.6),
    dict(name='ground_pass', kind='kick', spot=(-2500.0, -2500.0, 11.0), dir=(0.0, 1.0, 0.0),
         power=2200.0, lift=0.03, spin_right=0.0, spin_top=-520.0, duration=2.0),
    dict(name='dribble_40', kind='dribble', offset=(0.0, 40.0, 0.0), duration=2.0),
    dict(name='dribble_120', kind='dribble', offset=(0.0, 120.0, 0.0), duration=2.0),
]
SETTLE = 0.3  # seconds between the reset and the start of a scenario

# Phase 2 deflection tests: the ball is thrown from `distance` cm in front of the standing player at a body
# region (ballistic aim, gravity compensated; low targets are rolled along the ground). The player's tick is
# paused so the dribble can't catch the ball, and ResetBall clears the owner so the body really deflects it.
DEFLECT_SCENARIOS = [
    dict(name='throw_chest', kind='throw', region='CHEST', speed=900.0, distance=250.0, duration=0.8),
    dict(name='throw_forehead', kind='throw', region='FOREHEAD', speed=800.0, distance=250.0, duration=0.8),
    dict(name='throw_right_thigh', kind='throw', region='RIGHT_THIGH', speed=800.0, distance=250.0, duration=0.8),
    dict(name='throw_right_shin', kind='throw', region='RIGHT_SHIN', speed=700.0, distance=250.0, duration=0.8),
    dict(name='throw_right_foot', kind='throw', region='RIGHT_FOOT_CENTER', speed=600.0, distance=250.0, duration=0.8),
]
GRAVITY = 980.0

# Master plan Phase 1 baseline: the six BP_FootballPlayer kicks as tuned (2026-10-05), at 40/75/100% charge, from 20 m
# straight at the goal centre (on target, so the aim assist leaves the direction alone). The Blueprint's own formula:
# power = Base * Lerp(Floor, 1, Charge^Exp), finesse spin = SpinRight * the same Lerp (node references in
# Audit_Phase1.txt). Phases 9/10 compare the new strikes against these.
BP_KICKS = {  # name: (base power, floor, exponent, lift, spin right per lerp, spin top)
    'shoot': (3600.0, 0.20, 1.25, 0.26, 0.0, 0.0),
    'pass': (2200.0, 0.20, 1.10, 0.03, 0.0, -520.0),
    'through': (2600.0, 0.20, 1.20, 0.08, 0.0, 0.0),
    'finesse_r': (3300.0, 0.22, 1.35, 0.30, 360.0, 0.0),
    'finesse_l': (3300.0, 0.22, 1.35, 0.30, -360.0, 0.0),
    'lob': (2500.0, 0.19, 1.15, 0.75, 0.0, 0.0),
}
GOAL_LINE_X = 5270.0
KICK_SPOT = (GOAL_LINE_X - 2000.0, 0.0, 11.0)


def kick_baseline_scenarios(charges=(0.4, 0.75, 1.0), duration=3.0):
    out = []
    for name, (base, floor, exp, lift, spin_right, spin_top) in BP_KICKS.items():
        for charge in charges:
            k = floor + (1.0 - floor) * charge ** exp
            out.append(dict(name='%s_%d' % (name, round(charge * 100)), kind='kick', spot=KICK_SPOT, dir=(1.0, 0.0, 0.0),
                            power=base * k, lift=lift, spin_right=spin_right * k, spin_top=spin_top, duration=duration, charge=charge))
    return out


KICK_SCENARIOS = kick_baseline_scenarios()

_state = {}


def _world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()


def _actors(world):
    ball_cls = unreal.load_class(None, '/Game/Football/Blueprints/Ball/BP_Football.BP_Football_C')
    player_cls = unreal.load_class(None, '/Game/Football/Blueprints/Characters/BP_FootballPlayer.BP_FootballPlayer_C')
    ball = unreal.GameplayStatics.get_actor_of_class(world, ball_cls)
    player = unreal.GameplayStatics.get_actor_of_class(world, player_cls)
    return ball, player


def _guide_point(player):
    for c in player.get_components_by_class(unreal.SceneComponent):
        if c.get_name() == 'BallGuidePoint':
            return c.get_world_location()
    raise RuntimeError('BallGuidePoint not found')


def _reset_ball(ball, loc):
    xf = unreal.Transform(unreal.Vector(*loc), unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1))
    ball.call_method('ResetBall', (xf, False))  # (SpawnTransform, bKeepSpin)


# The player's placed start in Arena, facing +X: where `baseline` / `baseline60` were recorded, clear of every kick lane.
RIG_PLAYER = (3430.0, 2320.0, 0.0)  # x, y, yaw


def _place_player(player):
    """Every scenario starts from the same player pose and dribble mode, whatever earlier tests left behind (a player
    left in a kick lane deflects the ball; a leftover agile dribble moves the guide point)."""
    z = player.get_actor_location().z
    player.set_actor_location(unreal.Vector(RIG_PLAYER[0], RIG_PLAYER[1], z), False, True)
    player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=RIG_PLAYER[2]), True)  # Python Rotator is (roll, pitch, yaw)
    player.get_component_by_class(unreal.PlayerBallInteractionComponent).set_agile_dribble(False)


def _tick(delta):
    s = _state
    try:
        world = _world()
        if world is None:
            raise RuntimeError('PIE stopped')
        now = unreal.GameplayStatics.get_time_seconds(world)
        if s['step'] == 2 and s['scenarios'][s['index']]['kind'] == 'throw':
            # The ball keeps only its last contact; collect every one (a deflected ball can brush a second region).
            # one poll per frame sees only the frame's last contact (two overlaps in one physics frame keep the
            # second); deflect_report therefore also measures the deflection from the ball's own trajectory.
            c = _actors(world)[0].get_component_by_class(unreal.BallInteractionComponent).get_last_contact()
            if c.instigator and c.time >= s['rec_world_start'] and (not s['contacts'] or s['contacts'][-1]['time'] != c.time):
                s['contacts'].append(_contact_dict(c))
        if now < s['wait_until']:
            return
        ball, player = _actors(world)
        movement = ball.get_component_by_class(unreal.BallMovementComponent)
        scenarios = s['scenarios']
        sc = scenarios[s['index']]
        step = s['step']
        if step == 0:  # reset
            _place_player(player)
            if sc['kind'] == 'kick':
                player.set_actor_tick_enabled(False)
                _reset_ball(ball, sc['spot'])
            elif sc['kind'] == 'throw':
                player.set_actor_tick_enabled(False)
                body = player.get_component_by_class(unreal.PlayerBallInteractionComponent)
                # Python returns (point, normal), or None when the C++ bool is false.
                res = body.get_region_point(getattr(unreal.BodyRegion, sc['region']))
                if res is None:
                    raise RuntimeError('no region point for %s' % sc['region'])
                point, normal = res
                fwd = player.get_actor_forward_vector()
                roll = point.z < 25.0
                start_pt = unreal.Vector(point.x + fwd.x * sc['distance'], point.y + fwd.y * sc['distance'], 11.0 if roll else point.z)
                s['target'], s['start'], s['roll'] = [point.x, point.y, point.z], [start_pt.x, start_pt.y, start_pt.z], roll
                _reset_ball(ball, (start_pt.x, start_pt.y, start_pt.z))
            else:
                player.set_actor_tick_enabled(True)
                g = _guide_point(player)
                s['guide'] = [g.x, g.y, g.z]
                _reset_ball(ball, (g.x + sc['offset'][0], g.y + sc['offset'][1], 11.0))
                # Possession from the reset frame on, as when the baselines were recorded: until 2026-10-06 a ball reset
                # did not end possession, so the dribble already pulled the ball during SETTLE.
                player.call_method('TestForcePossession')
            s['step'], s['wait_until'] = 1, now + SETTLE
        elif step == 1:  # go
            movement.start_recording(sc['duration'])
            s['rec_world_start'] = now
            if sc['kind'] == 'kick':
                # ApplyKick(KickDirection, Power, Lift, SpinRight, Kicker, SpinTop) - Kicker comes before SpinTop.
                ball.call_method('ApplyKick', (unreal.Vector(*sc['dir']), sc['power'], sc['lift'], sc['spin_right'], None, sc['spin_top']))
            elif sc['kind'] == 'throw':
                s['contacts'] = []
                # Aim from where the ball actually is now (it falls a little during SETTLE).
                here = ball.get_actor_location()
                t, p = s['target'], [here.x, here.y, here.z]
                s['start'] = p
                dx, dy = t[0] - p[0], t[1] - p[1]
                horizontal = (dx * dx + dy * dy) ** 0.5
                flight = horizontal / sc['speed']
                vz = 0.0 if s['roll'] else (t[2] - p[2]) / flight + 0.5 * GRAVITY * flight
                ball.root_component.set_physics_linear_velocity(unreal.Vector(dx / flight, dy / flight, vz))
                ball.root_component.set_physics_angular_velocity_in_degrees(unreal.Vector(0, 0, 0))
            else:
                player.call_method('TestForcePossession')
            s['step'], s['wait_until'] = 2, now + sc['duration'] + 0.05
        else:  # save
            rec = [dict(t=r.time, p=list(r.position.to_tuple()), v=list(r.velocity.to_tuple()),
                        w=list(r.angular_velocity_deg.to_tuple()), state=str(r.state), grounded=r.grounded)
                   for r in movement.get_recording()]
            os.makedirs(OUT_DIR, exist_ok=True)
            path = os.path.join(OUT_DIR, '%s_r%d_%s.json' % (s['label'], s['run'], sc['name']))
            contacts = s.get('contacts', []) if sc['kind'] == 'throw' else []
            # The deflection the throw is about: the first contact that was applied, else the first one.
            contact = next((c for c in contacts if c['applied']), contacts[0] if contacts else None)
            with open(path, 'w', encoding='utf-8') as f:
                json.dump(dict(scenario=sc, guide=s.get('guide'), target=s.get('target'), start=s.get('start'), contact=contact,
                               contacts=contacts, record_start=s.get('rec_world_start'),
                               extras=movement.get_editor_property('apply_physics_extras'), samples=rec), f)
            s['done'].append(os.path.basename(path))
            s['index'] += 1
            if s['index'] >= len(scenarios):
                s['index'] = 0
                s['run'] += 1
            if s['run'] > s['runs']:
                player.set_actor_tick_enabled(True)
                _finish('finished')
                return
            s['step'], s['wait_until'] = 0, now
    except Exception as e:  # never leave a broken callback running
        _finish('error: %r' % e)


def _contact_dict(c):
    return dict(time=c.time, instigator=c.instigator, region=str(c.region), priority=str(c.priority), applied=c.applied,
                note=c.note, body_point=list(c.body_point.to_tuple()), normal=list(c.normal.to_tuple()),
                v_in=list(c.velocity_in.to_tuple()), v_out=list(c.velocity_out.to_tuple()))


def _finish(msg):
    _state['result'] = msg
    handle = _state.pop('handle', None)
    if handle is not None:
        unreal.unregister_slate_post_tick_callback(handle)
    try:  # never leave the player frozen
        world = _world()
        if world is not None:
            _actors(world)[1].set_actor_tick_enabled(True)
    except Exception:
        pass


def start(label, runs=2, scenarios=None):
    """scenarios: SCENARIOS (default, the ball/dribble parity set) or DEFLECT_SCENARIOS."""
    if _state.get('handle') is not None:
        return 'already running'
    if _world() is None:
        return 'start PIE first'
    _state.clear()
    _state.update(label=label, runs=runs, run=1, index=0, step=0, wait_until=0.0, done=[], result='running',
                  scenarios=scenarios or SCENARIOS)
    _state['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'started'


def status():
    return dict(result=_state.get('result'), done=_state.get('done'))


# ---- comparison (plain Python, no PIE needed) ----

KICK_TIMES = (0.25, 0.5, 0.75, 1.0, 1.25, 1.5)
DRIBBLE_TIMES = (0.1, 0.25, 0.5, 1.0, 1.5, 1.9)


def _at(samples, t):
    """Linear interpolation of position / velocity at time t (seconds since the scenario started)."""
    for a, b in zip(samples, samples[1:]):
        if a['t'] <= t <= b['t']:
            k = 0.0 if b['t'] == a['t'] else (t - a['t']) / (b['t'] - a['t'])
            return ([a['p'][i] + k * (b['p'][i] - a['p'][i]) for i in range(3)],
                    [a['v'][i] + k * (b['v'][i] - a['v'][i]) for i in range(3)])
    return None, None


def metrics(path):
    """Named numbers for one recording: kick = position xyz at fixed times + apex height,
    dribble = horizontal distance ball -> guide point at fixed times."""
    with open(path, encoding='utf-8') as f:
        d = json.load(f)
    s, sc, out = d['samples'], d['scenario'], {}
    dts = [b['t'] - a['t'] for a, b in zip(s, s[1:])]
    out['frame_dt_mean_ms'] = 1000.0 * sum(dts) / max(len(dts), 1)
    if sc['kind'] == 'kick':
        for t in KICK_TIMES:
            p, _ = _at(s, t)
            if p:
                for axis, v in zip('xyz', p):
                    out['%s@%.2f' % (axis, t)] = v
        out['apex_z'] = max(r['p'][2] for r in s)
    else:
        g = d['guide']
        for t in DRIBBLE_TIMES:
            p, _ = _at(s, t)
            if p:
                out['dist@%.2f' % t] = ((p[0] - g[0]) ** 2 + (p[1] - g[1]) ** 2) ** 0.5
    return out


def deflect_report(label, runs=1):
    """Per throw: region aimed at / hit, whether the deflection applied, speeds in/out, the formula check
    (applied velocity == ComputeDeflection with the hit region's row) and the ball's next recorded velocity."""
    rows = {r['Name']: r for r in json.loads(unreal.DataTableFunctionLibrary.export_data_table_to_json_string(
        unreal.load_asset('/Game/Football/Data/DT_BodyRegions')))}
    report = {}
    for run in range(1, runs + 1):
        for sc in DEFLECT_SCENARIOS:
            with open(os.path.join(OUT_DIR, '%s_r%d_%s.json' % (label, run, sc['name'])), encoding='utf-8') as f:
                d = json.load(f)
            c, item = d['contact'], {}
            # From the ball's own trajectory: the frame it became Deflected, and its speed just before / after.
            states = [x['state'].split('.')[-1].split(':')[0] for x in d['samples']]
            k = next((i for i in range(1, len(states)) if states[i] == 'DEFLECTED' and states[i - 1] != 'DEFLECTED'), None)
            if k is not None:
                vb, va = d['samples'][k - 1]['v'], d['samples'][k]['v']
                item['trajectory'] = dict(t=round(d['samples'][k]['t'], 3), speed_before=round(sum(x * x for x in vb) ** 0.5, 1),
                                          speed_after=round(sum(x * x for x in va) ** 0.5, 1), v_after=[round(x, 1) for x in va])
            if not c or not c['instigator']:
                item['contact'] = 'none recorded'
                report['%s_r%d' % (sc['name'], run)] = item
                continue
            hit = c['region'].split('.')[-1].split(':')[0]
            row_name = ''.join(w.capitalize() for w in hit.split('_'))
            v_in, v_out = unreal.Vector(*c['v_in']), unreal.Vector(*c['v_out'])
            item.update(aimed=sc['region'], hit=hit, priority=c['priority'].split('.')[-1].split(':')[0], applied=c['applied'], note=c['note'],
                        speed_in=round(v_in.length(), 1), speed_out=round(v_out.length(), 1))
            if c['applied'] and row_name in rows:
                row = rows[row_name]
                expected, _ = unreal.BallContactResolver.compute_deflection(v_in, unreal.Vector(0, 0, 0), unreal.Vector(*c['normal']),
                                                                            row['Restitution'], row['TangentDamping'], unreal.Vector(0, 0, 0), 0.0, 11.0)
                item['formula_error'] = round((expected - v_out).length(), 2)
                # The first recorded frame after the contact: did physics carry the deflection on?
                t_rel = c['time'] - d['record_start']
                nxt = next((s for s in d['samples'] if s['t'] > t_rel + 1e-4), None)
                if nxt:
                    item['next_frame_velocity_diff'] = round((unreal.Vector(*nxt['v']) - v_out).length(), 1)
            seq, last = [], None
            for s in d['samples']:
                st = s['state'].split('.')[-1].split(':')[0]
                if st != last:
                    seq.append('%s@%.2f' % (st, s['t']))
                    last = st
            item['states'] = seq
            report['%s_r%d' % (sc['name'], run)] = item
    return report


def kick_report(label, runs=1, scenarios=None):
    """Per kick: launch speed / elevation / spin, apex, where it passes x = 5240 (30 cm before the goal line, just in front
    of the goal trigger: a scoring ball is teleported away by the goal reset before it reaches the line) - time, lateral
    offset (curl) and ball centre height - and its speed after 1 s. The recording is cut at a goal / out-of-bounds reset."""
    import math
    line_x = GOAL_LINE_X - 30.0
    report = {}
    for run in range(1, runs + 1):
        for sc in scenarios or KICK_SCENARIOS:
            with open(os.path.join(OUT_DIR, '%s_r%d_%s.json' % (label, run, sc['name'])), encoding='utf-8') as f:
                s = json.load(f)['samples']
            cut = next((i for i in range(1, len(s)) if s[i]['p'][0] < s[i - 1]['p'][0] - 200.0), len(s))
            reset = cut < len(s)
            s = s[:cut]
            launch = next((r for r in s if math.hypot(*r['v']) > 50.0), None)
            item = dict(power=round(sc['power'], 1), lift=sc['lift'], spin_right=round(sc['spin_right'], 1), spin_top=sc['spin_top'],
                        reset=reset)
            if launch:
                v = launch['v']
                item.update(launch_speed=round(math.hypot(*v), 1), launch_elev_deg=round(math.degrees(math.atan2(v[2], math.hypot(v[0], v[1]))), 2),
                            launch_spin_dps=[round(c, 1) for c in launch['w']])
            item['apex_z'] = round(max(r['p'][2] for r in s), 1)
            for a, b in zip(s, s[1:]):
                if a['p'][0] < line_x <= b['p'][0]:
                    k = (line_x - a['p'][0]) / (b['p'][0] - a['p'][0])
                    item['at_goal'] = dict(t=round(a['t'] + k * (b['t'] - a['t']), 3), y=round(a['p'][1] + k * (b['p'][1] - a['p'][1]), 1),
                                           z=round(a['p'][2] + k * (b['p'][2] - a['p'][2]), 1))
                    break
            if 'at_goal' not in item and reset and s and s[-1]['v'][0] > 1.0:
                # Reset within the last frame before the line (a fast ball moves up to ~60 cm a frame): carry the last
                # sample on ballistically for that fraction of a frame.
                p, v = s[-1]['p'], s[-1]['v']
                dt = (line_x - p[0]) / v[0]
                if 0.0 <= dt <= 0.1:
                    item['at_goal'] = dict(t=round(s[-1]['t'] + dt, 3), y=round(p[1] + v[1] * dt, 1),
                                           z=round(p[2] + v[2] * dt - 0.5 * GRAVITY * dt * dt, 1), extrapolated_s=round(dt, 4))
            _, v1 = _at(s, (launch['t'] if launch else 0.0) + 1.0)
            if v1:
                item['speed_after_1s'] = round(math.hypot(*v1), 1)
            report['%s_r%d' % (sc['name'], run)] = item
    return report


def compare(label_a='baseline', label_b='after', runs=2, scenarios=None):
    """For every scenario and metric: run-to-run spread inside A, and the difference of the means B - A."""
    report = {}
    for sc in scenarios or SCENARIOS:
        def load(label):
            return [metrics(os.path.join(OUT_DIR, '%s_r%d_%s.json' % (label, r, sc['name']))) for r in range(1, runs + 1)]
        a, b = load(label_a), load(label_b)
        rows = {}
        for key in a[0]:
            if key not in b[0] or key == 'frame_dt_mean_ms':
                continue
            va, vb = [m[key] for m in a], [m[key] for m in b]
            mean_a, mean_b = sum(va) / len(va), sum(vb) / len(vb)
            rows[key] = dict(spread_a=round(max(va) - min(va), 2), spread_b=round(max(vb) - min(vb), 2), diff=round(mean_b - mean_a, 2))
        report[sc['name']] = dict(dt_ms_a=[round(m['frame_dt_mean_ms'], 2) for m in a], dt_ms_b=[round(m['frame_dt_mean_ms'], 2) for m in b], rows=rows)
    return report
