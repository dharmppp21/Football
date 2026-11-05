"""Rotation capture for the MotionBricks test pawn (PIE, fixed 60 fps): 1 s idle + 3 s jog with the ball, recording the
component-space rotations of the G1 proxy's left shoulder/elbow joints and the Footballer's left arm bones every frame, to
see which G1 joint the Footballer's upper arm follows. start(label, freeze=False); freeze keeps the G1 in its rest pose.
Results: Saved/MBProbe/<label>.json."""
import json
import os
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'MBProbe')
G1_BONES = ['pelvis_skel', 'waist_pitch_skel', 'left_shoulder_pitch_skel', 'left_shoulder_roll_skel', 'left_shoulder_yaw_skel', 'left_elbow_skel', 'left_wrist_roll_skel']
FB_BONES = ['Hips', 'Spine2', 'LeftShoulder', 'LeftArm', 'LeftForeArm', 'LeftHand']
_state = {}


def _world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()


def _q(t):
    r = t.rotation
    return [round(r.x, 6), round(r.y, 6), round(r.z, 6), round(r.w, 6)]


def _tick(delta):
    s = _state
    try:
        world = _world()
        if world is None:
            raise RuntimeError('PIE stopped')
        now = unreal.GameplayStatics.get_time_seconds(world)
        ball = unreal.GameplayStatics.get_actor_of_class(world, unreal.load_class(None, '/Game/Football/Blueprints/Ball/BP_Football.BP_Football_C'))
        player = unreal.GameplayStatics.get_actor_of_class(world, unreal.load_class(None, '/Game/Football/Blueprints/Characters/BP_MBPlayer.BP_MBPlayer_C'))
        mb = player.get_component_by_class(unreal.load_class(None, '/Script/FootballArena.MotionBricksComponent'))
        cmc = player.get_component_by_class(unreal.CharacterMovementComponent)
        mesh, src = player.get_editor_property('mesh'), player.get_editor_property('motion_source')
        if s['step'] == 0:
            cmc.stop_movement_immediately()
            z = player.get_actor_location().z
            player.set_actor_location(unreal.Vector(-1500.0, 0.0, z), False, True)
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0), True)
            ball.call_method('ResetBall', (unreal.Transform(unreal.Vector(-1440.0, 0.0, 11.0), unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1)), False))
            player.call_method('TestForcePossession')
            if s.get('freeze') and mb:
                mb.set_component_tick_enabled(False)
            s.update(step=1, t0=now + 0.5, rows=[])
            return
        t = now - s['t0']
        if t < 0:
            return
        if t >= 4.0:
            _finish('finished')
            return
        cmc.max_walk_speed = 430.0
        if t >= 1.0:
            player.add_movement_input(unreal.Vector(1, 0, 0), 1.0, False)
        C = unreal.RelativeTransformSpace.RTS_COMPONENT
        s['rows'].append(dict(t=round(t, 4), g1=[_q(src.get_socket_transform(b, C)) for b in G1_BONES], fb=[_q(mesh.get_socket_transform(b, C)) for b in FB_BONES]))
    except Exception as e:
        _finish('error: %r' % e)


def _finish(msg):
    s = _state
    handle = s.pop('handle', None)
    if handle is not None:
        unreal.unregister_slate_post_tick_callback(handle)
    eng = unreal.find_object(None, '/Engine/Transient.UnrealEdEngine_0')
    eng.set_editor_property('FixedFrameRate', 30.0)
    eng.set_editor_property('bUseFixedFrameRate', False)
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, s.get('label', 'rotcap') + '.json'), 'w', encoding='utf-8') as f:
        json.dump(dict(result=msg, g1_bones=G1_BONES, fb_bones=FB_BONES, rows=s.get('rows', [])), f)


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
