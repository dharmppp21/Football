"""Proof check for the MotionBricks test pawn (PIE, fixed 60 fps, L_MotionBricksTest): is the visible Footballer driven by
MotionBricks? Jogs with the ball for 3 s while recording the G1 proxy's and the Footballer's left-knee angles, then stops
the MotionBricks component's tick (its pose freezes) for 3 more seconds. MotionBricks drives the Footballer if the two knees
move together and the Footballer's knee stops cycling once MotionBricks is frozen. Results: Saved/MBProbe/<label>.json."""
import json
import math
import os
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'MBProbe')
START = (-1500.0, 0.0)
_state = {}


def _world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()


def _angle(comp, a, b, c):
    """Inner angle (deg) at joint b between a-b and c-b, from world socket positions."""
    pa, pb, pc = comp.get_socket_location(a), comp.get_socket_location(b), comp.get_socket_location(c)
    u, v = pa - pb, pc - pb
    d = (u.x * v.x + u.y * v.y + u.z * v.z) / max(1e-6, u.length() * v.length())
    return math.degrees(math.acos(max(-1.0, min(1.0, d))))


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
            player.set_actor_location(unreal.Vector(START[0], START[1], z), False, True)
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0), True)
            ball.call_method('ResetBall', (unreal.Transform(unreal.Vector(START[0] + 60.0, START[1], 11.0), unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1)), False))
            player.call_method('TestForcePossession')
            ai = mesh.get_anim_instance()
            sai = src.get_anim_instance()
            s.update(step=1, t0=now + 0.5, rows=[], frozen=False,
                     visible_anim=ai.get_class().get_name() if ai else None, source_anim=sai.get_class().get_name() if sai else None,
                     source_mesh=src.get_editor_property('skeletal_mesh_asset').get_name())
            return
        t = now - s['t0']
        if t < 0:
            return
        if t >= 6.0:
            mb.set_component_tick_enabled(True)
            _finish('finished')
            return
        if t >= 3.0 and not s['frozen']:
            mb.set_component_tick_enabled(False)  # MotionBricks' pose stops changing from here on
            s['frozen'] = True
        cmc.max_walk_speed = 430.0
        player.add_movement_input(unreal.Vector(1, 0, 0), 1.0, False)
        ai = mesh.get_anim_instance()
        try:
            ball_alpha = round(ai.get_editor_property('ball'), 3)
        except Exception:
            ball_alpha = None
        s['rows'].append([round(t, 4), int(s['frozen']),
                          round(_angle(src, 'left_hip_pitch_skel', 'left_knee_skel', 'left_ankle_pitch_skel'), 2),
                          round(_angle(mesh, 'LeftUpLeg', 'LeftLeg', 'LeftFoot'), 2),
                          1 if player.get_editor_property('bHasBall') else 0, ball_alpha, mb.get_editor_property('plan_count')])
    except Exception as e:
        _finish('error: %r' % e)


def _stats(xs):
    m = sum(xs) / len(xs)
    return m, (sum((x - m) ** 2 for x in xs) / len(xs)) ** 0.5


def _finish(msg):
    s = _state
    handle = s.pop('handle', None)
    if handle is not None:
        unreal.unregister_slate_post_tick_callback(handle)
    eng = unreal.find_object(None, '/Engine/Transient.UnrealEdEngine_0')
    eng.set_editor_property('FixedFrameRate', 30.0)
    eng.set_editor_property('bUseFixedFrameRate', False)
    out = dict(result=msg, visible_anim=s.get('visible_anim'), source_anim=s.get('source_anim'), source_mesh=s.get('source_mesh'))
    try:
        rows = s.get('rows', [])
        run = [r for r in rows if not r[1] and r[0] >= 0.5]
        frozen = [r for r in rows if r[1] and r[0] >= 3.3]
        if len(run) > 10 and len(frozen) > 10:
            (ms, ss), (mf, sf) = _stats([r[2] for r in run]), _stats([r[3] for r in run])
            cov = sum((r[2] - ms) * (r[3] - mf) for r in run) / len(run)
            out.update(knee_correlation_running=round(cov / max(1e-6, ss * sf), 3),
                       g1_knee_std_running=round(ss, 2), footballer_knee_std_running=round(sf, 2),
                       g1_knee_std_frozen=round(_stats([r[2] for r in frozen])[1], 2),
                       footballer_knee_std_frozen=round(_stats([r[3] for r in frozen])[1], 2),
                       has_ball_running=round(sum(r[4] for r in run) / len(run), 2),
                       ball_blend_alpha_max=max((r[5] for r in rows if r[5] is not None), default=None),
                       plans_at_freeze=frozen[0][6], plans_at_end=rows[-1][6])
        out['rows'] = rows
    except Exception as e:
        out['stats_error'] = repr(e)
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, s.get('label', 'verify') + '.json'), 'w', encoding='utf-8') as f:
        json.dump(out, f)


def start(label):
    if _state.get('handle') is not None:
        return 'already running'
    if _world() is None:
        return 'start PIE first'
    _state.clear()
    _state.update(label=label, step=0)
    eng = unreal.find_object(None, '/Engine/Transient.UnrealEdEngine_0')
    eng.set_editor_property('FixedFrameRate', 60.0)
    eng.set_editor_property('bUseFixedFrameRate', True)
    _state['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'started'
