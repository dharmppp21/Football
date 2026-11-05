"""MotionBricks with-ball check (PIE, fixed 60 fps) in L_MotionBricksTest: the test pawn (BP_MBPlayer, the Footballer
retargeted from the MotionBricks-driven G1 proxy) is given the ball and run through idle, jog 430, sprint 660, a 90-degree
turn and a stop. Records MotionBricks' plans, ball possession and distance, the visible feet's slide and height, and takes
HighResShot screenshots. Load it as a module, start PIE, then start('label'). Results: Saved/MBProbe/<label>.json."""
import json
import math
import os
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'MBProbe')
START = (-1500.0, 0.0)
# (seconds, max speed cm/s, stick yaw deg, stick amount)
PHASES = [(1.0, 430.0, 0.0, 0.0), (3.0, 430.0, 0.0, 1.0), (3.0, 660.0, 0.0, 1.0), (2.0, 660.0, 90.0, 1.0), (1.5, 430.0, 90.0, 0.0)]
SHOTS = [0.8, 2.5, 3.6, 5.5, 6.6, 7.6, 8.4, 9.9]
_state = {}


def _world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()


def _actors(world):
    ball = unreal.GameplayStatics.get_actor_of_class(world, unreal.load_class(None, '/Game/Football/Blueprints/Ball/BP_Football.BP_Football_C'))
    player = unreal.GameplayStatics.get_actor_of_class(world, unreal.load_class(None, '/Game/Football/Blueprints/Characters/BP_MBPlayer.BP_MBPlayer_C'))
    return ball, player


def _set_fixed_rate(on):
    eng = unreal.find_object(None, '/Engine/Transient.UnrealEdEngine_0')
    eng.set_editor_property('FixedFrameRate', 60.0 if on else 30.0)
    eng.set_editor_property('bUseFixedFrameRate', on)


def _p(v):
    return [round(v.x, 2), round(v.y, 2), round(v.z, 2)]


def _tick(delta):
    s = _state
    try:
        world = _world()
        if world is None:
            raise RuntimeError('PIE stopped')
        now = unreal.GameplayStatics.get_time_seconds(world)
        ball, player = _actors(world)
        if player is None:
            raise RuntimeError('no BP_MBPlayer in this level')
        cmc = player.get_component_by_class(unreal.CharacterMovementComponent)
        mb = player.get_component_by_class(unreal.load_class(None, '/Script/FootballArena.MotionBricksComponent'))
        if s['step'] == 0:  # place the player, put the ball at his feet and give it to him
            cmc.stop_movement_immediately()
            z = player.get_actor_location().z
            player.set_actor_location(unreal.Vector(START[0], START[1], z), False, True)
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0), True)
            ball.call_method('ResetBall', (unreal.Transform(unreal.Vector(START[0] + 60.0, START[1], 11.0), unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1)), False))
            player.call_method('TestForcePossession')
            s.update(step=1, half=player.get_component_by_class(unreal.CapsuleComponent).get_scaled_capsule_half_height(), wait_from=now)
            return
        if s['step'] == 1:  # wait for MotionBricks' first plan (model load + plan)
            if mb is None or mb.get_editor_property('plan_count') < 1:
                if now - s['wait_from'] > 20.0:
                    raise RuntimeError('MotionBricks produced no plan in 20 s (component %s)' % ('missing' if mb is None else 'present'))
                return
            s.update(step=2, t0=now + 0.3, rows=[], ready_after=round(now - s['wait_from'], 2))
            return
        t = now - s['t0']
        if t < 0:
            return
        acc, phase = 0.0, None
        for ph in PHASES:
            if t < acc + ph[0]:
                phase = ph
                break
            acc += ph[0]
        if phase is None:
            _finish('finished')
            return
        dur, vmax, yaw, amount = phase
        cmc.max_walk_speed = vmax
        if amount > 0:
            r = math.radians(yaw)
            player.add_movement_input(unreal.Vector(math.cos(r), math.sin(r), 0), amount, False)
        while s.get('shot', 0) < len(SHOTS) and t >= SHOTS[s.get('shot', 0)]:
            unreal.SystemLibrary.execute_console_command(world, 'HighResShot 960x540')
            s.setdefault('shots', []).append('%s_%04.1f' % (s['label'], SHOTS[s.get('shot', 0)]))
            s['shot'] = s.get('shot', 0) + 1
        mesh = player.get_editor_property('mesh')
        v = player.get_velocity()
        bs = ball.get_component_by_class(unreal.BallStateComponent)
        s['rows'].append([round(t, 4), round(math.hypot(v.x, v.y), 1), round(player.get_actor_rotation().yaw, 2)]
                         + _p(mesh.get_socket_location('LeftToeBase')) + _p(mesh.get_socket_location('RightToeBase'))
                         + _p(mesh.get_socket_location('LeftFoot')) + _p(mesh.get_socket_location('RightFoot'))
                         + _p(ball.get_actor_location()) + _p(player.get_actor_location())
                         + [1 if player.get_editor_property('bHasBall') else 0, 1 if bs and bs.get_ball_owner() else 0,
                            mb.get_editor_property('plan_count'), round(mb.get_editor_property('last_plan_ms'), 1)])
    except Exception as e:  # never leave a broken callback running
        _finish('error: %r' % e)


def _metrics(rows):
    out = {}
    if not rows:
        return out
    dt = 1.0 / 60.0
    # columns: t speed yaw | ltoe 3-5 rtoe 6-8 lankle 9-11 rankle 12-14 ball 15-17 actor 18-20 | has_ball 21 owned 22 plans 23 ms 24
    ground = rows[0][20] - _state.get('half', 88.0)
    names = ['idle', 'jog430', 'sprint660', 'turn90', 'stop']
    acc = 0.0
    for name, ph in zip(names, PHASES):
        win = [r for r in rows if acc + 0.3 <= r[0] < acc + ph[0]]
        acc += ph[0]
        if len(win) < 4:
            continue
        slides = []
        for i in (3, 6):  # each toe's ground speed over its slowest 20 % of frames = the planted-foot slide
            sp = sorted(math.hypot(win[k + 1][i] - win[k - 1][i], win[k + 1][i + 1] - win[k - 1][i + 1]) / (2 * dt) for k in range(1, len(win) - 1))
            slides += sp[:max(1, len(sp) // 5)]
        dist = [math.hypot(r[15] - r[18], r[16] - r[19]) for r in win]
        out[name] = dict(speed=round(sum(r[1] for r in win) / len(win), 1), toe_slide=round(sum(slides) / len(slides), 1),
                         toe_min_cm=round(min(min(r[5], r[8]) for r in win) - ground, 1), ankle_min_cm=round(min(min(r[11], r[14]) for r in win) - ground, 1),
                         ball_dist_mean=round(sum(dist) / len(dist), 1), ball_dist_max=round(max(dist), 1),
                         has_ball=round(sum(r[21] for r in win) / len(win), 2))
    out['plans'] = rows[-1][23]
    out['last_plan_ms'] = rows[-1][24]
    return out


def _finish(msg):
    s = _state
    handle = s.pop('handle', None)
    if handle is not None:
        unreal.unregister_slate_post_tick_callback(handle)
    _set_fixed_rate(False)
    os.makedirs(OUT_DIR, exist_ok=True)
    res = dict(result=msg, ready_after_s=s.get('ready_after'), shots=s.get('shots', []), metrics=_metrics(s.get('rows', [])),
               cols='t speed yaw ltoe rtoe lankle rankle ball actor has_ball ball_owned plans plan_ms', rows=s.get('rows', []))
    with open(os.path.join(OUT_DIR, s.get('label', 'probe') + '.json'), 'w', encoding='utf-8') as f:
        json.dump(res, f)


def start(label):
    if _state.get('handle') is not None:
        return 'already running'
    if _world() is None:
        return 'start PIE first'
    _state.clear()
    _state.update(label=label, step=0)
    _set_fixed_rate(True)
    _state['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'started'
