"""Dribble look probe (PIE, fixed 60 fps): straight dribbles at fixed speeds, a turning one and an agile one, traced every
frame. Per touch: how far the touching foot's toe is from the ball and whether that foot is swinging forward or planted - how
much the ball looks kicked by the foot rather than moving by itself. Load it as a module, start PIE, then start('label').
Results: Saved/DribbleProbe/<label>.json."""
import json
import math
import os
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'DribbleProbe')
START = (-1500.0, 0.0)
SETTLE, RUN, SKIP = 0.4, 3.4, 1.0  # s: ball at the feet before running, the run, the run's start left out of the metrics
SCENARIOS = [dict(name='ball_250', speed=250.0), dict(name='ball_430', speed=430.0), dict(name='ball_660', speed=660.0),
             dict(name='ball_430_turn90', speed=430.0, turn=(1.6, 90.0, 0.35)), dict(name='agile_250', speed=430.0, agile=True)]

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


def _p(v):
    return [round(v.x, 2), round(v.y, 2), round(v.z, 2)]


def _metrics(sc, frames, touches):
    """frames: [t, actor xyz, yaw, ball xyz, ltoe xyz, rtoe xyz]; touches: [t, reason, foot, frame index]."""
    dt = 1.0 / 60.0
    out = dict(name=sc['name'])
    steady = [k for k, f in enumerate(frames) if f[0] >= SKIP]
    ts = [tc for tc in touches if tc[0] >= SKIP]
    out['touches'] = len(ts)
    out['interval'] = round((ts[-1][0] - ts[0][0]) / (len(ts) - 1), 3) if len(ts) > 1 else None
    reasons = {}
    for tc in ts:
        reasons[tc[1]] = reasons.get(tc[1], 0) + 1
    out['reasons'] = reasons
    d_toe, swing, planted, h_toe, details = [], 0, 0, [], []
    for t, reason, foot, k in ts:
        k -= 1  # the frame before the touch: the ball not yet sent, the pose the touch was made in
        if k < 1 or k + 1 >= len(frames):
            continue
        f, fp, fn = frames[k], frames[k - 1], frames[k + 1]
        i = 8 if foot == 'L' else 11  # the touching foot's toe
        toe, ball = f[i:i + 3], f[5:8]
        d_toe.append(math.hypot(toe[0] - ball[0], toe[1] - ball[1]))
        h_toe.append(toe[2])
        yaw = math.radians(f[4])
        fwd = (math.cos(yaw), math.sin(yaw))
        v_toe = ((fn[i] - fp[i]) / (2 * dt), (fn[i + 1] - fp[i + 1]) / (2 * dt))
        v_body = ((fn[1] - fp[1]) / (2 * dt), (fn[2] - fp[2]) / (2 * dt))
        rel = (v_toe[0] - v_body[0]) * fwd[0] + (v_toe[1] - v_body[1]) * fwd[1]  # toe speed forward relative to the body
        if math.hypot(*v_toe) < 60.0:
            planted += 1
        elif rel > 100.0:
            swing += 1
        a = f[1:3]  # player frame: forward, right of the actor (cm)
        loc = lambda p: [round((p[0] - a[0]) * fwd[0] + (p[1] - a[1]) * fwd[1], 1), round(-(p[0] - a[0]) * fwd[1] + (p[1] - a[1]) * fwd[0], 1)]
        details.append(dict(t=t, reason=reason, foot=foot, toe=loc(toe) + [round(toe[2], 1)], ball=loc(ball), toe_speed=round(math.hypot(*v_toe)),
                            toe_rel_fwd=round(rel)))
    out['details'] = details
    n = max(1, len(d_toe))
    out['toe_ball_cm_mean'] = round(sum(d_toe) / n, 1) if d_toe else None
    out['toe_ball_cm_max'] = round(max(d_toe), 1) if d_toe else None
    out['toe_height_cm_mean'] = round(sum(h_toe) / n, 1) if h_toe else None
    out['touch_on_swinging_foot'] = round(swing / n, 2)
    out['touch_on_planted_foot'] = round(planted / n, 2)
    # where the ball sits: ahead / to the right of the player (cm), mean and spread over the steady part
    ah, rt = [], []
    for k in steady:
        f = frames[k]
        yaw = math.radians(f[4])
        dx, dy = f[5] - f[1], f[6] - f[2]
        ah.append(dx * math.cos(yaw) + dy * math.sin(yaw))
        rt.append(-dx * math.sin(yaw) + dy * math.cos(yaw))
    if ah:
        m = sum(ah) / len(ah)
        out['ball_ahead_mean'] = round(m, 1)
        out['ball_ahead_sd'] = round((sum((a - m) ** 2 for a in ah) / len(ah)) ** 0.5, 1)
        out['ball_right_mean'] = round(sum(rt) / len(rt), 1)
    out['kept'] = bool(frames) and math.hypot(frames[-1][5] - frames[-1][1], frames[-1][6] - frames[-1][2]) < 250.0
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
        dribble = player.get_component_by_class(unreal.PlayerBallInteractionComponent)
        sc = SCENARIOS[s['index']]
        if s['step'] == 0:
            dribble.set_agile_dribble(False)
            cmc.max_walk_speed = sc['speed']
            cmc.stop_movement_immediately()
            z = player.get_actor_location().z
            player.set_actor_location(unreal.Vector(START[0], START[1], z), False, True)
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0), True)
            g = next(c for c in player.get_components_by_class(unreal.SceneComponent) if c.get_name() == 'BallGuidePoint').get_world_location()
            ball.call_method('ResetBall', (unreal.Transform(unreal.Vector(g.x, g.y, 11.0), unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1)), False))
            player.call_method('TestForcePossession')
            if sc.get('agile'):
                dribble.set_agile_dribble(True)
            s.update(step=1, t0=now + SETTLE, frames=[], touches=[], count=dribble.get_dribble_touch_count())
            return
        t = now - s['t0']
        if t < 0:
            return
        if t > RUN:
            s['results'].append(_metrics(sc, s['frames'], s['touches']))
            s['index'] += 1
            dribble.set_agile_dribble(False)
            if s['index'] >= len(SCENARIOS):
                _finish('finished')
                return
            s['step'] = 0
            return
        yaw = 0.0
        if sc.get('turn'):
            t1, deg, dur = sc['turn']
            yaw = deg * min(1.0, max(0.0, (t - t1) / dur))
        r = math.radians(yaw)
        player.add_movement_input(unreal.Vector(math.cos(r), math.sin(r), 0), 1.0, False)
        mesh = player.get_component_by_class(unreal.SkeletalMeshComponent)
        s['frames'].append([round(t, 4)] + _p(player.get_actor_location()) + [round(player.get_actor_rotation().yaw, 3)] + _p(ball.get_actor_location())
                           + _p(mesh.get_socket_location('LeftToeBase')) + _p(mesh.get_socket_location('RightToeBase')))
        c = dribble.get_dribble_touch_count()
        if c != s['count']:
            last = dribble.get_last_dribble_touch()
            s['touches'].append([round(t, 4), str(last.reason).split('.')[-1], 'L' if 'LEFT' in str(last.foot).upper() else 'R', len(s['frames']) - 1])
            s['count'] = c
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
            _, player = _actors(world)
            player.get_component_by_class(unreal.CharacterMovementComponent).max_walk_speed = s['speed0']
            player.get_component_by_class(unreal.PlayerBallInteractionComponent).set_agile_dribble(False)
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
    _state.update(label=label, index=0, step=0, results=[], speed0=player.get_component_by_class(unreal.CharacterMovementComponent).max_walk_speed)
    _set_fixed_rate(True)
    _state['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'started'
