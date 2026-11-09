"""Feel probe (PIE, fixed 60 fps, no background throttling): scripted sticks on the arena's player - standing starts and stops, 90 / 180-deg
turns at jog and sprint, without the ball (motion matching) and with it (the dribble, both turn sides) - traced every frame: the capsule,
the visible hips, the ball. The numbers come from RL_Train/feel.py, the same code that measures the training sim (physics player plan
step 0, 2026-11-10):   python feel.py game "<project>/Saved/FeelProbe/<label>.json"
Load it as a module (regress._load pattern) so its state survives between MCP calls; with PIE stopped, start('label') starts PIE itself,
runs every scenario, writes the file and stops PIE; poll status()."""
import importlib.util
import json
import math
import os
import sys
import time
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'FeelProbe')
TOOLS = os.path.dirname(os.path.abspath(__file__))
START = (-1500.0, 0.0)  # a clear midfield spot; the player starts facing +X
JOG, SPRINT = 430.0, 660.0  # CharacterMovement max speeds (cm/s) the game's gaits and the dribble profiles are keyed on
SETTLE = 0.6  # s standing after a reset, before the first stick


def _sc(name, speed, turn=None, ball=False):
    """Phases: (seconds, stick yaw in degrees, or None = let go). turn > 0 = to the left (UE yaw -turn, as Tools/turn_probe.py)."""
    run = 3.5 if speed == JOG else 4.5
    if turn is None:
        ph = [(SETTLE, None), (run, 0.0), (2.5, None)]
    else:
        ph = [(SETTLE, None), (run - 1.0, 0.0), (2.5, -float(turn))] + ([(2.0, None)] if ball else [])
    return dict(name=name, speed=speed, ball=ball, phases=ph)


SCENARIOS = [_sc('jog', JOG), _sc('sprint', SPRINT), _sc('jog_left90', JOG, 90), _sc('jog_left180', JOG, 180),
             _sc('sprint_left90', SPRINT, 90), _sc('sprint_left180', SPRINT, 180), _sc('ball_jog', JOG, ball=True), _sc('ball_sprint', SPRINT, ball=True)]
SCENARIOS += [_sc('ball_%s_%s%d' % (g, side, a), v, a if side == 'left' else -a, True)
              for g, v in (('jog', JOG), ('sprint', SPRINT)) for a in (90, 180) for side in ('left', 'right')]
COLS = 't cap_x cap_y hip_x hip_y ball_x ball_y has_ball speed'

_state = {}


def _load(name):
    m = sys.modules.get(name)
    if m is None:
        spec = importlib.util.spec_from_file_location(name, os.path.join(TOOLS, name + '.py'))
        m = importlib.util.module_from_spec(spec)
        sys.modules[name] = m
        spec.loader.exec_module(m)
    return m


def _world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()


def _actors(world):
    ball = unreal.GameplayStatics.get_actor_of_class(world, unreal.load_class(None, '/Game/Football/Blueprints/Ball/BP_Football.BP_Football_C'))
    player = unreal.GameplayStatics.get_actor_of_class(world, unreal.load_class(None, '/Game/Football/Blueprints/Characters/BP_FootballPlayer.BP_FootballPlayer_C'))
    return ball, player


def _events(phases):
    out, t, prev = [], 0.0, None
    for dur, yaw in phases:
        if yaw is not None and prev is None:
            out.append(dict(kind='start', t=round(t, 4), dir=yaw))
        elif yaw is None and prev is not None:
            out.append(dict(kind='stop', t=round(t, 4)))
        elif yaw is not None and yaw != prev:
            out.append(dict(kind='turn', t=round(t, 4), dir=yaw))
        t, prev = t + dur, yaw
    return out


def _yaw_at(phases, t):
    acc = 0.0
    for dur, yaw in phases:
        if t < acc + dur:
            return yaw, False
        acc += dur
    return None, True


def _tick(delta):
    s = _state
    try:
        world = _world()
        if s['step'] == 'pie':  # wait for BeginPlay
            if world is None:
                if time.time() - s['asked'] > 120.0:
                    raise RuntimeError('PIE did not start')
                return
            s.update(step='reset', speed0=_actors(world)[1].get_component_by_class(unreal.CharacterMovementComponent).max_walk_speed)
            return
        if world is None:
            raise RuntimeError('PIE stopped')
        now = unreal.GameplayStatics.get_time_seconds(world)
        ball, player = _actors(world)
        cmc = player.get_component_by_class(unreal.CharacterMovementComponent)
        sc = SCENARIOS[s['index']]
        cmc.max_walk_speed = sc['speed']  # a plain attribute set, every frame
        if s['step'] == 'reset':  # the player at START facing +X; the ball at his feet (possession forced) or parked 30 m away
            cmc.stop_movement_immediately()
            z = player.get_actor_location().z
            player.set_actor_location(unreal.Vector(START[0], START[1], z), False, True)
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0), True)
            if sc['ball']:
                player.get_component_by_class(unreal.PlayerBallInteractionComponent).set_agile_dribble(False)
                g = next(c for c in player.get_components_by_class(unreal.SceneComponent) if c.get_name() == 'BallGuidePoint').get_world_location()
                ball.call_method('ResetBall', (unreal.Transform(unreal.Vector(g.x, g.y, 11.0), unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1)), False))
                player.call_method('TestForcePossession')
            else:
                ball.call_method('ResetBall', (unreal.Transform(unreal.Vector(START[0] - 3000.0, START[1], 11.0), unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1)), False))
            s.update(step='run', t0=now, rows=[])
            return
        t = now - s['t0']
        yaw, over = _yaw_at(sc['phases'], t)
        if over:
            s['scenarios'].append(dict(name=sc['name'], speed=sc['speed'], ball=sc['ball'], phases=sc['phases'], events=_events(sc['phases']), cols=COLS, rows=s['rows']))
            s['index'] += 1
            if s['index'] >= len(SCENARIOS):
                _finish('finished')
                return
            s['step'] = 'reset'
            return
        if yaw is not None:  # the stick every frame of a phase (a frame without input reads as the stick let go)
            r = math.radians(yaw)
            player.add_movement_input(unreal.Vector(math.cos(r), math.sin(r), 0.0), 1.0, False)
        p, b, v = player.get_actor_location(), ball.get_actor_location(), player.get_velocity()
        h = player.get_editor_property('mesh').get_socket_location('Hips')
        try:
            has = 1 if player.get_editor_property('bHasBall') else 0
        except Exception:
            has = -1
        s['rows'].append([round(t, 4), round(p.x, 2), round(p.y, 2), round(h.x, 2), round(h.y, 2), round(b.x, 2), round(b.y, 2), has, round(math.hypot(v.x, v.y), 1)])
    except Exception as e:  # never leave a broken callback running
        _finish('error: %r' % e)


def _finish(msg):
    s = _state
    s['result'] = msg
    handle = s.pop('handle', None)
    if handle is not None:
        unreal.unregister_slate_post_tick_callback(handle)
    try:
        world = _world()
        if world is not None and 'speed0' in s:
            _actors(world)[1].get_component_by_class(unreal.CharacterMovementComponent).max_walk_speed = s['speed0']
    except Exception:
        pass
    try:
        _load('skill_tests').set_fixed_rate(False)
    except Exception:
        pass
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, s.get('label', 'probe') + '.json')
    with open(path + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(dict(label=s.get('label'), result=msg, fps=s.get('fps'), scenarios=s.get('scenarios', [])), f)
    os.replace(path + '.tmp', path)
    try:
        if _world() is not None:
            unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_request_end_play()
    except Exception:
        pass


def start(label):
    if _state.get('handle') is not None:
        return 'already running'
    if _world() is not None:
        return 'stop PIE first (the probe starts its own session)'
    _state.clear()
    rate = _load('skill_tests').set_fixed_rate(True)
    _state.update(label=label, index=0, step='pie', asked=time.time(), scenarios=[], result='running', fps=rate.get('fps'))
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_request_begin_play()
    _state['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'started: %d scenarios, rate %s' % (len(SCENARIOS), rate)


def status():
    return dict(result=_state.get('result'), done=len(_state.get('scenarios', [])), of=len(SCENARIOS),
                scenario=SCENARIOS[_state['index']]['name'] if _state.get('index', 0) < len(SCENARIOS) and _state.get('result') == 'running' else None)
