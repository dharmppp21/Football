"""Posture capture for the MotionBricks test pawn (PIE, fixed 60 fps, L_MotionBricksTest): idle, jog 430 and sprint 660
with the ball, recording every frame the world positions of the Footballer's main bones and of the G1 proxy's joints, and
taking side-on close-up HighResShots (the level's PlayerStart is moved alongside the player and used as the view target).
Load it as a module, start PIE, then start('label'). Results: Saved/MBProbe/<label>.json."""
import json
import math
import os
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'MBProbe')
START = (-1500.0, 0.0)
PHASES = [(1.5, 430.0, 0.0), (2.5, 430.0, 1.0), (2.5, 660.0, 1.0)]  # seconds, max speed, stick amount (straight +X)
SHOTS = [1.0, 2.4, 3.2, 4.6, 5.4, 6.2]
FB_BONES = ['Hips', 'Spine', 'Spine1', 'Spine2', 'Neck', 'Head', 'LeftShoulder', 'LeftArm', 'LeftForeArm', 'LeftHand', 'RightShoulder',
            'RightArm', 'RightForeArm', 'RightHand', 'LeftUpLeg', 'LeftLeg', 'LeftFoot', 'LeftToeBase', 'RightUpLeg', 'RightLeg',
            'RightFoot', 'RightToeBase']
_state = {}


def _world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()


def _p(v):
    return [round(v.x, 2), round(v.y, 2), round(v.z, 2)]


def _tick(delta):
    s = _state
    try:
        world = _world()
        if world is None:
            raise RuntimeError('PIE stopped')
        now = unreal.GameplayStatics.get_time_seconds(world)
        ball = unreal.GameplayStatics.get_actor_of_class(world, unreal.load_class(None, '/Game/Football/Blueprints/Ball/BP_Football.BP_Football_C'))
        player = unreal.GameplayStatics.get_actor_of_class(world, unreal.load_class(None, '/Game/Football/Blueprints/Characters/BP_MBPlayer.BP_MBPlayer_C'))
        cam = unreal.GameplayStatics.get_actor_of_class(world, unreal.PlayerStart)
        pc = unreal.GameplayStatics.get_player_controller(world, 0)
        mb = player.get_component_by_class(unreal.load_class(None, '/Script/FootballArena.MotionBricksComponent'))
        cmc = player.get_component_by_class(unreal.CharacterMovementComponent)
        mesh, src = player.get_editor_property('mesh'), player.get_editor_property('motion_source')
        if s['step'] == 0:
            cmc.stop_movement_immediately()
            z = player.get_actor_location().z
            player.set_actor_location(unreal.Vector(START[0], START[1], z), False, True)
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0), True)
            ball.call_method('ResetBall', (unreal.Transform(unreal.Vector(START[0] + 60.0, START[1], 11.0), unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1)), False))
            player.call_method('TestForcePossession')
            if cam and pc:
                pc.set_view_target_with_blend(cam, 0.0)
            if s.get('freeze') and mb:  # no plan ever arrives: the G1 proxy stays in its rest pose, the Footballer in the retarget pose
                mb.set_component_tick_enabled(False)
            s.update(step=1, t0=now + 0.5, rows=[], g1_names=[str(n) for n in src.get_all_socket_names()])
            return
        # side-on close-up camera: 380 cm to the player's right, slightly above, looking at the hips
        loc = player.get_actor_location()
        if cam:
            right = player.get_actor_right_vector()
            eye = unreal.Vector(loc.x + right.x * 380.0, loc.y + right.y * 380.0, loc.z + 25.0)
            look = unreal.MathLibrary.find_look_at_rotation(eye, unreal.Vector(loc.x, loc.y, loc.z - 15.0))
            cam.set_actor_location_and_rotation(eye, look, False, True)
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
        cmc.max_walk_speed = phase[1]
        if phase[2] > 0:
            player.add_movement_input(unreal.Vector(1, 0, 0), phase[2], False)
        while s.get('shot', 0) < len(SHOTS) and t >= SHOTS[s.get('shot', 0)]:
            unreal.SystemLibrary.execute_console_command(world, 'HighResShot 1280x720')
            s.setdefault('shots', []).append(SHOTS[s.get('shot', 0)])
            s['shot'] = s.get('shot', 0) + 1
        v = player.get_velocity()
        g1 = [n for n in s['g1_names'] if n.endswith('_skel') or n.endswith('_base')]
        s.setdefault('g1', g1)
        s['rows'].append(dict(t=round(t, 4), speed=round(math.hypot(v.x, v.y), 1), actor=_p(loc), yaw=round(player.get_actor_rotation().yaw, 2),
                              fb=[_p(mesh.get_socket_location(b)) for b in FB_BONES], g1=[_p(src.get_socket_location(b)) for b in g1],
                              ball=_p(ball.get_actor_location()), plans=mb.get_editor_property('plan_count') if mb else -1))
    except Exception as e:
        _finish('error: %r' % e)


def _finish(msg):
    s = _state
    handle = s.pop('handle', None)
    if handle is not None:
        unreal.unregister_slate_post_tick_callback(handle)
    try:
        world = _world()
        if world:
            pc = unreal.GameplayStatics.get_player_controller(world, 0)
            pawn = pc.get_controlled_pawn() if pc else None
            if pawn:
                pc.set_view_target_with_blend(pawn, 0.0)
    except Exception:
        pass
    eng = unreal.find_object(None, '/Engine/Transient.UnrealEdEngine_0')
    eng.set_editor_property('FixedFrameRate', 30.0)
    eng.set_editor_property('bUseFixedFrameRate', False)
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, s.get('label', 'capture') + '.json'), 'w', encoding='utf-8') as f:
        json.dump(dict(result=msg, fb_bones=FB_BONES, g1_joints=s.get('g1', []), shots=s.get('shots', []), rows=s.get('rows', [])), f)


def start(label, freeze=False):
    if _state.get('handle') is not None:
        return 'already running'
    if _world() is None:
        return 'start PIE first'
    _state.clear()
    _state.update(label=label, step=0, freeze=freeze)
    eng = unreal.find_object(None, '/Engine/Transient.UnrealEdEngine_0')
    eng.set_editor_property('FixedFrameRate', 60.0)
    eng.set_editor_property('bUseFixedFrameRate', True)
    _state['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'started'
