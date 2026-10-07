"""Skill framework PIE tests (framework Phase 3).

Load as a module so its state survives between util.execute_python calls (PIE must be running):
    import importlib.util, sys
    spec = importlib.util.spec_from_file_location('skill_tests', r'<project>/Tools/skill_tests.py')
    m = importlib.util.module_from_spec(spec); sys.modules['skill_tests'] = m; spec.loader.exec_module(m)
    m.skill_start(label)     ... a few seconds later: sys.modules['skill_tests'].status(), then m.skill_report(label)
    m.reset_start()          ... then m.reset_report(): goal / out-of-bounds resets end possession (master plan Phase 1)

Run with the engine's fixed frame rate (set_fixed_rate(True), 59.9988 fps = the baseline60 substeps) so
results don't depend on how fast the editor happens to run; set_fixed_rate(False) restores it.
"""
import json
import os
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'SkillTests')
SPOT = (-2500.0, -2500.0, 11.0)  # open ground in the left half (same as the ball parity tests)

_state = {}


# ---------------------------------------------------------------- rig

def set_fixed_rate(on, fps=59.9988):
    """Engine fixed frame rate + no background throttling while testing; off restores the defaults (False / 30, throttle on).
    These settings need their raw C++ property names."""
    engine = unreal.find_object(None, '/Engine/Transient.UnrealEdEngine_0')
    engine.set_editor_property('FixedFrameRate', fps if on else 30.0)
    engine.set_editor_property('bUseFixedFrameRate', bool(on))
    perf = unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))
    perf.set_editor_property('bThrottleCPUWhenNotForeground', not on)
    return dict(fixed=engine.get_editor_property('bUseFixedFrameRate'), fps=engine.get_editor_property('FixedFrameRate'),
                throttle=perf.get_editor_property('bThrottleCPUWhenNotForeground'))


def _world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()


def _actors(world):
    ball_cls = unreal.load_class(None, '/Game/Football/Blueprints/Ball/BP_Football.BP_Football_C')
    player_cls = unreal.load_class(None, '/Game/Football/Blueprints/Characters/BP_FootballPlayer.BP_FootballPlayer_C')
    return unreal.GameplayStatics.get_actor_of_class(world, ball_cls), unreal.GameplayStatics.get_actor_of_class(world, player_cls)


def _reset_ball(ball, loc):
    xf = unreal.Transform(unreal.Vector(*loc), unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1))
    ball.call_method('ResetBall', (xf, False))  # (SpawnTransform, bKeepSpin)


def _v(vec):
    return [round(vec.x, 3), round(vec.y, 3), round(vec.z, 3)]


def _short(enum_value):
    return str(enum_value).split('.')[-1].split(':')[0].rstrip('>')


def _run(steps, **initial):
    """Runs steps(state, world, now) once per frame until it returns a result. `initial` seeds the state first."""
    if _state.get('handle') is not None:
        return 'already running'
    if _world() is None:
        return 'start PIE first'
    _state.clear()
    _state.update(steps=steps, result='running', log=[], **initial)

    def tick(delta):
        try:
            world = _world()
            if world is None:
                raise RuntimeError('PIE stopped')
            out = _state['steps'](_state, world, unreal.GameplayStatics.get_time_seconds(world))
            if out is not None:
                _finish(out)
        except Exception as e:  # never leave a broken callback running
            import traceback
            _finish('error: %r %s' % (e, traceback.format_exc()[-400:]))

    _state['handle'] = unreal.register_slate_post_tick_callback(tick)
    return 'started'


def _finish(result):
    _state['result'] = result
    handle = _state.pop('handle', None)
    if handle is not None:
        unreal.unregister_slate_post_tick_callback(handle)
    try:  # never leave the player frozen
        world = _world()
        if world is not None:
            _actors(world)[1].set_actor_tick_enabled(True)
    except Exception:
        pass


def status():
    return dict(result=_state.get('result'))


# ---------------------------------------------------------------- resets (master plan Phase 1, BUG 1)

# A possessed ball dropped into these triggers: the out-of-bounds slab behind the far goal, and the goal mouth.
RESET_TARGETS = {'out_of_bounds': (5700.0, 0.0, 30.0), 'goal': (5300.0, 0.0, 60.0)}
RESET_WATCH = 1.5


def _reset_snapshot(ball, player, t):
    bs = ball.get_component_by_class(unreal.BallStateComponent)
    owner = bs.get_ball_owner()
    return dict(t=round(t, 4), p=_v(ball.get_actor_location()), v=_v(ball.root_component.get_physics_linear_velocity()),
                state=_short(bs.get_state()), owner=owner.get_name() if owner else None,
                has_ball=player.get_editor_property('bHasBall'), controlled=player.get_editor_property('ControlledBall') is not None)


def reset_start(label='reset'):
    """The dribbling player's ball goes out of bounds, then into the goal. After each reset the ball must be Free with no
    owner, the player must have lost possession, and the ball must stay at the spawn point (until 2026-10-06 the dribble
    dragged it back across the pitch). Results: reset_report()."""
    def steps(s, world, now):
        ball, player = _actors(world)
        kind = s['kinds'][s['index']]
        phase = s.setdefault('phase', 'place')
        if phase == 'place':
            player.set_actor_tick_enabled(True)
            z = player.get_actor_location().z
            player.set_actor_location(unreal.Vector(PLAYER_SPOT[0], PLAYER_SPOT[1], z), False, True)
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0), True)
            _reset_ball(ball, (PLAYER_SPOT[0] + 100.0, PLAYER_SPOT[1], 11.0))
            player.call_method('TestForcePossession')
            s.update(phase='settle', until=now + 1.0)
        elif phase == 'settle':
            if now < s['until']:
                return None
            s['before'] = _reset_snapshot(ball, player, 0.0)
            ball.set_actor_location(unreal.Vector(*RESET_TARGETS[kind]), False, True)  # into the trigger
            s.update(phase='watch', t0=now, rows=[], until=now + RESET_WATCH)
        elif phase == 'watch':
            s['rows'].append(_reset_snapshot(ball, player, now - s['t0']))
            if now < s['until']:
                return None
            s['results'][kind] = dict(before=s['before'], rows=s['rows'])
            s['index'] += 1
            if s['index'] >= len(s['kinds']):
                os.makedirs(OUT_DIR, exist_ok=True)
                with open(os.path.join(OUT_DIR, '%s.json' % s['label']), 'w', encoding='utf-8') as f:
                    json.dump(s['results'], f)
                return 'finished'
            s['phase'] = 'place'
        return None
    return _run(steps, label=label, kinds=list(RESET_TARGETS), index=0, results={})


def reset_report(label='reset'):
    """Per reset: the state, owner and possession after it, and how far the ball moved from the spawn point afterwards."""
    import math
    with open(os.path.join(OUT_DIR, '%s.json' % label), encoding='utf-8') as f:
        results = json.load(f)
    out = {}
    for kind, r in results.items():
        rows = r['rows']
        spawn = next((x for x in rows if math.dist(x['p'][:2], (4140.0, 0.0)) < 50.0), None)
        last = rows[-1]
        moved = round(math.dist(spawn['p'], last['p']), 1) if spawn else None
        out[kind] = dict(respawned=spawn is not None, state=last['state'], owner=last['owner'], has_ball=last['has_ball'],
                         controlled=last['controlled'], moved_after_cm=moved,
                         passed=bool(spawn) and last['state'] in ('FREE', 'AIRBORNE') and last['owner'] is None
                         and not last['has_ball'] and not last['controlled'] and moved is not None and moved < 5.0)
    return out


# ---------------------------------------------------------------- skills: A1-A8 (doc §40)

SKILL = '/Game/Football/Data/Skills/DA_Skill_Elastico'
PLAYER_SPOT = (-2500.0, -1800.0)  # open ground (the parity tests kick from (-2500, -2500))
SETTLE_DRIBBLE = 1.2              # the dribble parks the ball at the guide point before the skill starts
# The elastico gesture needs RB held, so the dribble is agile (ball close) when it starts; one run without for comparison.
SKILL_SCENARIOS = [
    dict(name='right_yaw0', yaw=0.0, mirrored=False), dict(name='right_yaw90', yaw=90.0, mirrored=False),
    dict(name='right_yaw180', yaw=180.0, mirrored=False), dict(name='right_yaw270', yaw=270.0, mirrored=False),
    dict(name='left_yaw0', yaw=0.0, mirrored=True), dict(name='left_yaw180', yaw=180.0, mirrored=True),
    dict(name='right_yaw0_not_agile', yaw=0.0, mirrored=False, agile=False),
]


def _sample(r):
    return dict(t=round(r.time, 4), mt=round(r.montage_time, 4), skill=_short(r.skill_state), ball=_short(r.ball_state), driven=r.ball_driven,
                contact=r.contact_index, valid=_short(r.validation), dist=round(r.contact_distance, 2), body=_v(r.body_point),
                bp=_v(r.ball_pos), bv=_v(r.ball_vel), target=_v(r.target_pos), touch_phys=_v(r.touch_physical),
                touch_need=_v(r.touch_needed), touch=_v(r.touch_applied), actor=_v(r.actor_pos),
                yaw=round(r.actor_yaw, 2), hips=_v(r.hips_pos), lf=_v(r.left_foot_pos), rf=_v(r.right_foot_pos))


def skill_start(label='p3', scenarios=None):
    """Per scenario: player at PLAYER_SPOT facing `yaw`, given the ball, dribble settles, then RequestSkill (forward, the
    scenario's foot). Records the skill recorder + the ball recorder to Saved/SkillTests/<label>_<scenario>.json."""
    def steps(s, world, now):
        ball, player = _actors(world)
        skill_comp = player.get_component_by_class(unreal.FootballSkillComponent)
        movement = ball.get_component_by_class(unreal.BallMovementComponent)
        sc = s['scenarios'][s['index']]
        phase = s.setdefault('phase', 'place')
        if phase == 'place':
            player.set_actor_tick_enabled(True)
            z = player.get_actor_location().z
            player.set_actor_location(unreal.Vector(PLAYER_SPOT[0], PLAYER_SPOT[1], z), False, True)
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=sc['yaw']), True)  # Python Rotator is (roll, pitch, yaw)
            fwd = player.get_actor_forward_vector()
            _reset_ball(ball, (PLAYER_SPOT[0] + fwd.x * 100.0, PLAYER_SPOT[1] + fwd.y * 100.0, 11.0))
            player.get_component_by_class(unreal.PlayerBallInteractionComponent).set_agile_dribble(sc.get('agile', True))
            player.call_method('TestForcePossession')
            s.update(phase='settle', until=now + SETTLE_DRIBBLE)
        elif phase == 'settle':
            if now < s['until']:
                return None
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=sc['yaw']), True)  # the dribble must not have turned it
            start_ball, start_actor = ball.get_actor_location(), player.get_actor_location()
            movement.start_recording(3.0)
            ok = skill_comp.request_skill(unreal.load_asset(SKILL), ball, unreal.Vector(0, 0, 0), sc['mirrored'])
            s.update(phase='run', requested=ok, t0=now, start_ball=_v(start_ball), start_actor=_v(start_actor), start_yaw=sc['yaw'],
                     state_at_start=_short(ball.get_component_by_class(unreal.BallStateComponent).get_state()))
            if not ok:
                s['phase'] = 'save'
        elif phase == 'run':
            if skill_comp.is_skill_active() or now < s['t0'] + 0.2:
                return None
            s.update(phase='tail', until=now + skill_comp.get_editor_property('record_tail') + 0.15)
        elif phase == 'tail':
            if now < s['until']:
                return None
            s['phase'] = 'save'
        if s.get('phase') == 'save':
            rec = [_sample(r) for r in skill_comp.get_last_skill_record()] if s.get('requested') else []
            balls = [dict(t=round(r.time, 4), p=_v(r.position), v=_v(r.velocity), state=_short(r.state)) for r in movement.get_recording()]
            os.makedirs(OUT_DIR, exist_ok=True)
            path = os.path.join(OUT_DIR, '%s_%s.json' % (s['label'], sc['name']))
            with open(path, 'w', encoding='utf-8') as f:
                json.dump(dict(scenario=sc, requested=s.get('requested'), start_ball=s.get('start_ball'), start_actor=s.get('start_actor'),
                               state_at_start=s.get('state_at_start'), samples=rec, ball=balls,
                               loaded_skills=len(skill_comp.get_editor_property('loaded_skills'))), f)
            s['done'].append(os.path.basename(path))
            s['index'] += 1
            if s['index'] >= len(s['scenarios']):
                return 'finished'
            s['phase'] = 'place'
        return None
    return _run(steps, label=label, scenarios=scenarios or SKILL_SCENARIOS, index=0, done=[])


def _skill_space(point, origin, yaw, mirrored):
    """World point -> (forward, right, up) of the skill started at origin facing yaw (right flipped when mirrored)."""
    import math
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    dx, dy = point[0] - origin[0], point[1] - origin[1]
    f, r = dx * c + dy * s, -dx * s + dy * c
    return (f, -r if mirrored else r, point[2])


def skill_report(label='p3', scenarios=None):
    """A1-A8 and the approach numbers per scenario, plus A6/A7 cross-checks (skill-space ball paths across facings and feet)."""
    import math
    out, paths = {}, {}
    for sc in scenarios or SKILL_SCENARIOS:
        with open(os.path.join(OUT_DIR, '%s_%s.json' % (label, sc['name'])), encoding='utf-8') as f:
            d = json.load(f)
        s, item = d['samples'], dict(requested=d['requested'], state_at_start=d['state_at_start'], loaded_skills=d['loaded_skills'])
        if not d['requested'] or not s:
            out[sc['name']] = item
            continue
        active = [x for x in s if x['skill'] not in ('NONE', 'COMPLETED', 'CANCELLED')]
        origin, yaw, mir = d['start_actor'], sc['yaw'], sc['mirrored']
        # Approach: how far the ball had to come back, how fast, and when it was taken.
        start_local = _skill_space(d['start_ball'], origin, yaw, mir)
        captured = next((x for x in s if x['ball'] == 'SKILL_CONTROLLED'), None)
        approach = [x for x in active if x['ball'] == 'CONTROLLED']
        item['approach'] = dict(ball_start_fwd_right=[round(start_local[0], 1), round(start_local[1], 1)],
                                max_ball_speed=round(max((math.hypot(x['bv'][0], x['bv'][1]) for x in approach), default=0.0), 1),
                                captured_at=captured and captured['t'], captured_montage_time=captured and captured['mt'])
        # A2: each contact's check when it was validated (or its best distance).
        contacts = {}
        for x in s:
            if x['contact'] >= 0:
                c = contacts.setdefault(x['contact'], dict(first_valid=None, best=1e9, results=set()))
                c['results'].add(x['valid'])
                if x['valid'] == 'VALID' and c['first_valid'] is None:
                    c['first_valid'] = dict(t=x['t'], dist=x['dist'])
                c['best'] = min(c['best'], x['dist'])
        item['A2_contacts'] = {k: dict(first_valid=v['first_valid'], best_dist=round(v['best'], 2), results=sorted(v['results'])) for k, v in contacts.items()}
        # Touches: what the collision gave, what the skill needed, what was applied (speed, heading in skill space: + = right),
        # and (A4) the ball's physics velocity on the next recorded frame.
        def heading(v):
            f, r, _ = _skill_space((v[0], v[1], 0.0), (0.0, 0.0, 0.0), yaw, mir)
            return round(math.degrees(math.atan2(r, f)), 1)
        touches = []
        for x in s:
            if any(abs(c) > 1e-3 for c in x['touch']):
                nxt = next((b for b in d['ball'] if b['t'] > x['t'] + 1e-3), None)
                touches.append(dict(t=x['t'], mt=x['mt'], contact=x['contact'], dist=x['dist'],
                                    physical=[round(math.hypot(*x['touch_phys'][:2]), 1), heading(x['touch_phys'])],
                                    needed=[round(math.hypot(*x['touch_need'][:2]), 1), heading(x['touch_need'])],
                                    applied=[round(math.hypot(*x['touch'][:2]), 1), heading(x['touch'])],
                                    next_frame=[round(math.hypot(*nxt['v'][:2]), 1), heading(nxt['v'])] if nxt else None,
                                    ball_before=[round(math.hypot(*x['bv'][:2]), 1), heading(x['bv'])]))
        item['touches'] = touches
        # A3: the ball's direction change at the last touch (velocity before it vs applied).
        if len(touches) >= 2:
            before = next((y for y in reversed(s) if y['t'] < touches[-1]['t'] - 1e-4), None)
            if before:
                item['A3_release_turn_deg'] = round(touches[-1]['applied'][1] - heading(before['bv']), 1)
        # A5: biggest single-frame ball move while the skill drives it.
        steps = [math.dist(a['bp'], b['bp']) for a, b in zip(s, s[1:]) if a['driven'] or b['driven']]
        item['A5_max_ball_step_cm'] = round(max(steps), 2) if steps else None
        # A1: pops at the start/end transitions = spikes in the bones' frame-to-frame acceleration.
        def spikes(lo, hi):
            worst = 0.0
            for key in ('hips', 'lf', 'rf'):
                pts = [x[key] for x in s[lo:hi]]
                for a, b, c in zip(pts, pts[1:], pts[2:]):
                    worst = max(worst, math.dist([b[i] - a[i] for i in range(3)], [c[i] - b[i] for i in range(3)]))
            return round(worst, 2)
        n = len(s)
        item['A1_bone_accel_cm'] = dict(start=spikes(0, min(n, 20)), middle=spikes(20, max(20, n - 30)), end=spikes(max(0, n - 30), n))
        # A8: the hand-back: states in the tail.
        item['A8_tail'] = sorted({(x['skill'], x['ball']) for x in s[-6:]})
        item['states'] = []
        for x in s:
            if not item['states'] or item['states'][-1][0] != x['skill']:
                item['states'].append((x['skill'], x['t']))
        # From the first touch on (the approach before it depends on where the dribble left the ball), while the montage
        # time still advances (it stops at the end during the blend-out, and those samples can't be matched by time).
        first_touch = touches[0]['t'] if touches else 1e9
        path = []
        for x in active:
            if x['t'] >= first_touch and (not path or x['mt'] > path[-1][0] + 1e-4):
                path.append((x['mt'], _skill_space(x['bp'], origin, yaw, mir)))
        paths[sc['name']] = path
        out[sc['name']] = item

    # A6 / A7: the same skill-space ball path (driven part, matched by montage time) across facings and feet.
    def compare(a, b):
        if a not in paths or b not in paths:
            return None
        pb = {round(t, 2): p for t, p in paths[b]}
        diffs = [math.dist(p, pb[round(t, 2)]) for t, p in paths[a] if round(t, 2) in pb]
        return dict(matched=len(diffs), max_cm=round(max(diffs), 2) if diffs else None)
    out['A6_facings'] = {k: compare('right_yaw0', k) for k in ('right_yaw90', 'right_yaw180', 'right_yaw270')}
    out['A7_mirror'] = {'left_yaw0': compare('right_yaw0', 'left_yaw0'), 'left_yaw180': compare('right_yaw0', 'left_yaw180')}
    return out
