"""Locomotion probe (PIE): straight runs at fixed speeds, with and without the ball - what ABP_Footballer's blend space gets
(Speed, Ball), the step rate, and how far the planted foot slides (the mocap loops' strides are shorter than the game speeds).
Load it as a module, start PIE, then start('label') and poll status(). Results: Saved/LocoProbe/<label>.json."""
import json
import math
import os
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'LocoProbe')
START = (-1500.0, 0.0)
SCENARIOS = [dict(name='idle_ball', speed=0.0, ball=True), dict(name='ball_250', speed=250.0, ball=True),
             dict(name='ball_430', speed=430.0, ball=True), dict(name='ball_660', speed=660.0, ball=True),
             dict(name='free_100', speed=100.0, ball=False), dict(name='free_250', speed=250.0, ball=False),
             dict(name='free_430', speed=430.0, ball=False), dict(name='free_660', speed=660.0, ball=False)]
SETTLE, WARM, RUN = 0.3, 0.6, 2.0  # s: after the reset, before measuring (reach the speed, blend in), measured

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


def _metrics(sc, rows):
    """rows: [t, px, py, lx, ly, lz, rx, ry, rz, speed, ball]. The run goes along +X."""
    out = dict(name=sc['name'], anim_speed=round(sum(r[9] for r in rows) / len(rows), 1), anim_ball=round(sum(r[10] for r in rows) / len(rows), 2))
    run = math.hypot(rows[-1][1] - rows[0][1], rows[-1][2] - rows[0][2]) / max(rows[-1][0] - rows[0][0], 1e-6)
    out['player_speed'] = round(run, 1)
    # stance slide: a foot's ground speed over its slowest 20 % of frames (the stance; 0 = planted), no height threshold
    slides = []
    for i0 in (3, 6):  # left foot, right foot
        v = sorted(math.hypot(rows[k + 1][i0] - rows[k - 1][i0], rows[k + 1][i0 + 1] - rows[k - 1][i0 + 1]) / (rows[k + 1][0] - rows[k - 1][0])
                   for k in range(1, len(rows) - 1))
        slides += v[:max(1, len(v) // 5)]
    out['planted_foot_slide'] = round(sum(slides) / len(slides), 1)
    out['slide_vs_speed'] = round(out['planted_foot_slide'] / run, 2) if run > 1 else None
    # steps: the feet pass each other once per step
    gap = [r[3] - r[6] for r in rows]
    steps = sum(1 for a, b in zip(gap, gap[1:]) if a * b < 0)
    out['steps_per_min'] = round(steps / (rows[-1][0] - rows[0][0]) * 60.0)
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
        if sc['speed'] > 0:
            cmc.max_walk_speed = sc['speed']
        step = s['step']
        if step == 0:  # reset: the player at the start facing +X; the ball at his feet (possession) or 20 m behind him
            cmc.stop_movement_immediately()
            z = player.get_actor_location().z
            player.set_actor_location(unreal.Vector(START[0], START[1], z), False, True)
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0), True)
            g = next(c for c in player.get_components_by_class(unreal.SceneComponent) if c.get_name() == 'BallGuidePoint').get_world_location()
            spot = unreal.Vector(g.x, g.y, 11.0) if sc['ball'] else unreal.Vector(START[0] - 2000.0, START[1], 11.0)
            ball.call_method('ResetBall', (unreal.Transform(spot, unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1)), False))
            if sc['ball']:
                player.call_method('TestForcePossession')
            s.update(step=1, until=now + SETTLE, rows=[])
        elif step == 1 and now >= s['until']:
            s.update(step=2, until=now + WARM + RUN, t0=now)
        elif step == 2:
            if now - s['t0'] >= WARM:
                mesh = player.get_component_by_class(unreal.SkeletalMeshComponent)
                anim = mesh.get_anim_instance()
                p, l, r = player.get_actor_location(), mesh.get_socket_location('LeftFoot'), mesh.get_socket_location('RightFoot')
                s['rows'].append([now - s['t0'], p.x, p.y, l.x, l.y, l.z, r.x, r.y, r.z,
                                  anim.get_editor_property('speed') if anim else -1.0, anim.get_editor_property('ball') if anim else -1.0])
            if now >= s['until']:
                s['results'].append(_metrics(sc, s['rows']))
                s['index'] += 1
                if s['index'] >= len(SCENARIOS):
                    _finish('finished')
                    return
                s.update(step=0)
        if s['step'] == 2 and sc['speed'] > 0:
            player.add_movement_input(unreal.Vector(1, 0, 0), 1.0, False)
    except Exception as e:  # never leave a broken callback running
        _finish('error: %r' % e)


def _finish(msg):
    s = _state
    s['result'] = msg
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
        json.dump(dict(result=msg, results=s.get('results', [])), f, indent=1)


def start(label):
    if _state.get('handle') is not None:
        return 'already running'
    world = _world()
    if world is None:
        return 'start PIE first'
    player = _actors(world)[1]
    _state.clear()
    _state.update(label=label, index=0, step=0, until=0.0, results=[], result='running',
                  speed0=player.get_component_by_class(unreal.CharacterMovementComponent).max_walk_speed)
    _set_fixed_rate(True)
    _state['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'started'


def status():
    return dict(result=_state.get('result'), results=_state.get('results'))
