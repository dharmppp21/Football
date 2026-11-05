"""Sharp-turn dribble probe (PIE): the player dribbles straight, then the stick turns sharply - does he keep the ball?
Load it as a module so its state survives between MCP calls, start PIE, then start('label') and poll status().
Results: Saved/TurnProbe/<label>.json (kept, when it was lost, how far the ball got, the touches after the turn)."""
import json
import math
import os
import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'TurnProbe')
START = (-1500.0, 0.0)  # a clear midfield spot; the player starts facing +X
JOG, SPRINT = 430.0, 660.0  # the CharacterMovement speeds the dribble profiles are keyed on
SCENARIOS = [dict(name='%s_%d' % (n, a), speed=v, turn=a) for n, v in (('jog', JOG), ('sprint', SPRINT)) for a in (90, 180)]
# A thumb sweeping the stick round over `sweep` seconds instead of an instant flip (closer to a real pad).
SCENARIOS += [dict(name='%s_90s' % n, speed=v, turn=90, sweep=0.2) for n, v in (('jog', JOG), ('sprint', SPRINT))]
RUN_IN, AFTER, SETTLE = 1.2, 1.5, 0.3  # seconds: straight dribble, after the turn, after a reset
LOST = 200.0  # cm: SeparationLimit - past it the dribble lets the ball go

_state = {}


def _world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()


def _actors(world):
    ball = unreal.GameplayStatics.get_actor_of_class(world, unreal.load_class(None, '/Game/Football/Blueprints/Ball/BP_Football.BP_Football_C'))
    player = unreal.GameplayStatics.get_actor_of_class(world, unreal.load_class(None, '/Game/Football/Blueprints/Characters/BP_FootballPlayer.BP_FootballPlayer_C'))
    return ball, player


def _guide_point(player):
    return next(c for c in player.get_components_by_class(unreal.SceneComponent) if c.get_name() == 'BallGuidePoint').get_world_location()


def _has_ball(player):
    try:
        return bool(player.get_editor_property('bHasBall'))
    except Exception:
        return None


def _set_fixed_rate(on):
    eng = unreal.find_object(None, '/Engine/Transient.UnrealEdEngine_0')
    eng.set_editor_property('FixedFrameRate', 60.0 if on else 30.0)
    eng.set_editor_property('bUseFixedFrameRate', on)


def _tick(delta):
    s = _state
    try:
        world = _world()
        if world is None:
            raise RuntimeError('PIE stopped')
        now = unreal.GameplayStatics.get_time_seconds(world)
        ball, player = _actors(world)
        cmc = player.get_component_by_class(unreal.CharacterMovementComponent)
        pbi = player.get_component_by_class(unreal.PlayerBallInteractionComponent)
        sc = SCENARIOS[s['index']]
        cmc.max_walk_speed = sc['speed']  # plain attribute set: no construction script rerun in PIE
        step = s['step']
        if step == 0:  # reset: player at the start facing +X, the ball at his feet, possession forced
            cmc.stop_movement_immediately()
            z = player.get_actor_location().z
            player.set_actor_location(unreal.Vector(START[0], START[1], z), False, True)
            player.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0), True)
            pbi.set_agile_dribble(False)
            g = _guide_point(player)
            ball.call_method('ResetBall', (unreal.Transform(unreal.Vector(g.x, g.y, 11.0), unreal.Rotator(0, 0, 0), unreal.Vector(1, 1, 1)), False))
            player.call_method('TestForcePossession')
            s.update(step=1, until=now + SETTLE)
        elif now < s['until']:
            if step == 3:
                since = now - s['turned_at']
                if sc.get('sweep'):
                    a = math.radians(-sc['turn'] * min(1.0, since / sc['sweep']))
                    s['dir'] = unreal.Vector(math.cos(a), math.sin(a), 0.0)
                acc, vel = cmc.get_current_acceleration(), player.get_velocity()
                yaw = lambda v: round(math.degrees(math.atan2(v.y, v.x)), 1) if math.hypot(v.x, v.y) > 1e-3 else None
                # per frame: time since the turn, stick / acceleration / velocity / facing yaw (deg), ball distance (cm)
                s['res']['trace'].append([round(since, 3), yaw(s['dir']), yaw(acc), yaw(vel), round(player.get_actor_rotation().yaw, 1),
                                          round(math.hypot(ball.get_actor_location().x - player.get_actor_location().x,
                                                           ball.get_actor_location().y - player.get_actor_location().y), 1)])
                p, b = player.get_actor_location(), ball.get_actor_location()
                d = math.hypot(b.x - p.x, b.y - p.y)
                r = s['res']
                r['max_dist_cm'] = max(r['max_dist_cm'], round(d, 1))
                if r['lost_after_s'] is None and (_has_ball(player) is False or d > LOST):
                    r['lost_after_s'] = round(now - s['turned_at'], 2)
                n = pbi.get_dribble_touch_count()
                if n != s['touches']:
                    s['touches'] = n
                    r['touches'].append(str(pbi.get_last_dribble_touch().reason).split('.')[-1].split(':')[0])
        elif step == 1:  # settled: dribble straight
            s.update(step=2, until=now + RUN_IN)
        elif step == 2:  # the sharp turn
            a = math.radians(-sc['turn']) if not sc.get('sweep') else 0.0  # a sweep starts from the old direction
            s.update(step=3, until=now + AFTER, turned_at=now, dir=unreal.Vector(math.cos(a), math.sin(a), 0.0),
                     touches=pbi.get_dribble_touch_count(), res=dict(name=sc['name'], max_dist_cm=0.0, lost_after_s=None, touches=[], trace=[]))
        else:  # done: keep the result, next scenario
            r = s['res']
            p, b = player.get_actor_location(), ball.get_actor_location()
            r['end_dist_cm'] = round(math.hypot(b.x - p.x, b.y - p.y), 1)
            r['kept'] = r['lost_after_s'] is None and _has_ball(player) is not False
            s['results'].append(r)
            s['index'] += 1
            if s['index'] >= len(SCENARIOS):
                _finish('finished')
                return
            s.update(step=0, until=now)
        # The stick, every frame of the run - also on the frame a phase starts (a frame without input reads as the stick
        # let go, which fires a stop touch).
        if s['step'] == 2:
            player.add_movement_input(unreal.Vector(1, 0, 0), 1.0, False)
        elif s['step'] == 3:
            player.add_movement_input(s['dir'], 1.0, False)
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
