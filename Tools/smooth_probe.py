"""Smoothness probe (PIE, fixed 60 fps), no ball: a run that speeds up, sprints, slows down and turns, traced every frame -
the actor (position, yaw), the mesh pose relative to the actor (hips, feet) and the ABP's Speed. Finds what jitters.
Load it as a module, start PIE, then start('label'). Results: Saved/SmoothProbe/<label>.json."""
import json
import math
import os
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'SmoothProbe')
START = (-1500.0, 0.0)
# phases: (seconds, max speed, stick yaw deg, stick amount)
PHASES = [(0.5, 430.0, 0.0, 0.0), (2.0, 430.0, 0.0, 1.0), (2.0, 660.0, 0.0, 1.0), (1.2, 430.0, 0.0, 1.0),
          (1.5, 430.0, 90.0, 1.0), (1.0, 430.0, 90.0, 0.5), (1.0, 430.0, 90.0, 0.0)]

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


def _local(player, p):
    """A world point in the actor's frame (x forward, y right)."""
    a = player.get_actor_location()
    yaw = math.radians(player.get_actor_rotation().yaw)
    dx, dy = p.x - a.x, p.y - a.y
    return [round(dx * math.cos(yaw) + dy * math.sin(yaw), 2), round(-dx * math.sin(yaw) + dy * math.cos(yaw), 2), round(p.z - a.z, 2)]


def _tick(delta):
    s = _state
    try:
        world = _world()
        if world is None:
            raise RuntimeError('PIE stopped')
        now = unreal.GameplayStatics.get_time_seconds(world)
        ball, player = _actors(world)
        cmc = player.get_component_by_class(unreal.CharacterMovementComponent)
        if s['step'] == 0:
            cmc.stop_movement_immediately()
            z = player.get_actor_location().z
            player.set_actor_location(unreal.Vector(START[0], START[1], z), False, True)
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0), True)
            ball.call_method('ResetBall', (unreal.Transform(unreal.Vector(START[0] - 3000.0, START[1], 11.0), unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1)), False))
            s.update(step=1, t0=now + 0.3, rows=[])
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
        mesh = player.get_component_by_class(unreal.SkeletalMeshComponent)
        anim = mesh.get_anim_instance()
        loc, v = player.get_actor_location(), player.get_velocity()
        s['rows'].append([round(t, 4), round(loc.x, 2), round(loc.y, 2), round(player.get_actor_rotation().yaw, 3),
                          round(math.hypot(v.x, v.y), 2), round(anim.get_editor_property('speed'), 2) if anim else -1.0,
                          _local(player, mesh.get_socket_location('Hips')), _local(player, mesh.get_socket_location('LeftFoot')),
                          _local(player, mesh.get_socket_location('RightFoot')), _local(player, mesh.get_socket_location('Head'))])
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
        json.dump(dict(result=msg, cols='t x y yaw vel anim_speed hips lfoot rfoot head (actor frame)', rows=s.get('rows', [])), f)


def start(label):
    if _state.get('handle') is not None:
        return 'already running'
    world = _world()
    if world is None:
        return 'start PIE first'
    player = _actors(world)[1]
    _state.clear()
    _state.update(label=label, step=0, speed0=player.get_component_by_class(unreal.CharacterMovementComponent).max_walk_speed)
    _set_fixed_rate(True)
    _state['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'started'
