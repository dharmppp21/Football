"""Motion-matching check (PIE, fixed 60 fps), no ball: stand, run, sprint, a 90-degree turn, a stop - traced every frame on the
visible footballer (retargeted from the hidden mannequin) and on the mannequin. Reports whether the sample's AnimBP gets the
player's data, the feet's height (floating / sinking), planted-foot slide, and the visible facing's smoothness.
Load it as a module, start PIE, then start('label'). Results: Saved/MMCheck/<label>.json."""
import json
import math
import os
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'MMCheck')
START = (-1500.0, 0.0)
# (seconds, max speed, stick yaw, stick amount)
PHASES = [(0.8, 430.0, 0.0, 0.0), (2.2, 430.0, 0.0, 1.0), (2.0, 660.0, 0.0, 1.0), (1.2, 430.0, 0.0, 1.0), (1.6, 430.0, 90.0, 1.0), (1.6, 430.0, 90.0, 0.0)]
_state = {}
SHOTS = [0.6, 1.0, 1.25, 2.4, 4.4, 6.0, 6.3, 7.9, 8.3, 9.2]  # s: idle, start, run, sprint, turn, stop, idle


def _shot(world, player, name):
    """The game camera's view (HighResShot -> Saved/Screenshots/...); the names are listed in the result, in order."""
    unreal.SystemLibrary.execute_console_command(world, 'HighResShot 960x540')
    _state.setdefault('shots', []).append(name)


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
            s.update(step=1, t0=now + 0.3, rows=[], half=player.get_component_by_class(unreal.CapsuleComponent).get_scaled_capsule_half_height())
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
        mesh, src = player.get_editor_property('mesh'), player.get_editor_property('motion_source')
        if phase is None:
            ai = src.get_anim_instance()
            info = dict(source_anim=str(ai.get_class().get_name()) if ai else None)
            for name in ('CharacterProperties', 'Gait', 'MovementState', 'CurrentDatabase', 'CurrentSelectedDatabase'):
                try:
                    info[name] = str(ai.get_editor_property(name))[:400]
                except Exception as e:
                    info[name] = 'n/a'
            s['info'] = info
            _finish('finished')
            return
        dur, vmax, yaw, amount = phase
        cmc.max_walk_speed = vmax
        if amount > 0:
            r = math.radians(yaw)
            player.add_movement_input(unreal.Vector(math.cos(r), math.sin(r), 0), amount, False)
        while s.get('shot', 0) < len(SHOTS) and t >= SHOTS[s.get('shot', 0)]:
            _shot(world, player, '%s_%04.1f' % (s['label'], SHOTS[s.get('shot', 0)]))
            s['shot'] = s.get('shot', 0) + 1
        hips = mesh.get_socket_transform('Hips', unreal.RelativeTransformSpace.RTS_WORLD)
        v = player.get_velocity()
        s['rows'].append([round(t, 4), round(math.hypot(v.x, v.y), 1), round(player.get_actor_rotation().yaw, 2), round(hips.rotation.rotator().yaw, 2)]
                         + _p(mesh.get_socket_location('LeftToeBase')) + _p(mesh.get_socket_location('RightToeBase'))
                         + _p(mesh.get_socket_location('LeftFoot')) + _p(mesh.get_socket_location('RightFoot'))
                         + _p(src.get_socket_location('ball_l')) + _p(src.get_socket_location('ball_r')) + _p(player.get_actor_location()))
    except Exception as e:  # never leave a broken callback running
        _finish('error: %r' % e)


def _metrics(rows):
    out = {}
    if not rows:
        return out
    dt = 1.0 / 60.0
    ground = rows[0][-1] - _state.get('half', 88.0)  # the floor: the capsule's bottom
    # feet height above the floor (toe and ankle of the visible mesh; the mannequin's balls of the feet)
    out['visible_toe_min_cm'] = round(min(min(r[6], r[9]) for r in rows) - ground, 1)
    out['visible_ankle_min_cm'] = round(min(min(r[12], r[15]) for r in rows) - ground, 1)
    out['mannequin_ball_min_cm'] = round(min(min(r[18], r[21]) for r in rows) - ground, 1)
    # planted-foot slide of the visible mesh, per phase (slowest 20 % of a toe's ground speed)
    def slide(t0, t1):
        win = [r for r in rows if t0 <= r[0] <= t1]
        vals = []
        for i in (4, 7):
            v = sorted(math.hypot(win[k + 1][i] - win[k - 1][i], win[k + 1][i + 1] - win[k - 1][i + 1]) / (2 * dt) for k in range(1, len(win) - 1))
            vals += v[:max(1, len(v) // 5)]
        return round(sum(vals) / len(vals), 1) if vals else None
    out['slide_run430'] = slide(1.6, 3.0)
    out['slide_sprint660'] = slide(3.6, 5.0)
    out['slide_turn'] = slide(6.2, 7.2)
    out['slide_stop'] = slide(7.9, 9.2)
    # visible facing (the hips' yaw): reversals of its turning direction and its jerk; the capsule's yaw for comparison
    def wrap(a):
        return (a + 180.0) % 360.0 - 180.0
    for key, col in (('visible', 3), ('capsule', 2)):
        yaw = [rows[0][col]]
        for r in rows[1:]:
            yaw.append(yaw[-1] + wrap(r[col] - yaw[-1]))
        rate = [(b - a) / dt for a, b in zip(yaw, yaw[1:])]
        accs = [abs(b - a) * dt for a, b in zip(rate, rate[1:])]
        out[key + '_yaw_acc_max'] = round(max(accs), 2)
        out[key + '_yaw_acc_rms'] = round((sum(a * a for a in accs) / len(accs)) ** 0.5, 3)
        out[key + '_yaw_reversals'] = sum(1 for a, b in zip(rate, rate[1:]) if abs(a) > 30 and abs(b) > 30 and a * b < 0)
    out['speed_at'] = {str(tt): next((r[1] for r in rows if r[0] >= tt), None) for tt in (1.0, 1.3, 1.6, 2.5, 3.5, 4.5, 5.5, 6.5, 7.6, 8.0, 8.5)}
    return out


def _finish(msg):
    s = _state
    handle = s.pop('handle', None)
    if handle is not None:
        unreal.unregister_slate_post_tick_callback(handle)
    _set_fixed_rate(False)
    os.makedirs(OUT_DIR, exist_ok=True)
    res = dict(result=msg, info=s.get('info'), shots=s.get('shots', []), metrics=_metrics(s.get('rows', [])),
               cols='t speed capsule_yaw hips_yaw ltoe rtoe lankle rankle mq_ball_l mq_ball_r actor', rows=s.get('rows', []))
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
