# Trims the retargeted skill/idle clips and bakes skill travel into RootFwd/RootRight curves.
# Re-run after every retarget regeneration:  exec(open(r'<this file>').read())
# Frame indices are keys at each clip's native rate, chosen from per-frame foot/hips analysis.
import unreal

CLIPS = {
    '/Game/Football/Animations/SkillMoves/AS_elastico_Retargeted':   dict(orig_keys=113, start=60, end=106, root_motion=True),
    '/Game/Football/Animations/SkillMoves/AS_heeltoheel_Retargeted': dict(orig_keys=109, start=56, end=103, root_motion=True),
    '/Game/Football/Animations/Player/AS_Idle_Retargeted':           dict(orig_keys=252, start=75, end=238, root_motion=False),
    # Master plan Phase 6 (2026-10-10): the user's second elastico take (copied from
    # /Game/CaptureManager/Imports/Mono_Video_Ingest/Elastico_2_1/AS_elastico_3). f57-f77: right foot steps, outside touch
    # f66, the foot comes down round the ball with the inside at f74, both feet down at f77.
    '/Game/Football/Animations/SkillMoves/AS_elastico3_Retargeted':  dict(orig_keys=100, start=57, end=77, root_motion=True),
}
SMOOTH_SEC = 0.4  # root trajectory = moving average of hips XY; residual keeps natural hip sway

# Mesh sits at yaw -90 in BP_FootballPlayer: mesh +Y = actor forward, mesh -X = actor right.
def to_fwd_right(x, y):
    return y, -x


def smooth(vals, half):
    n = len(vals)
    return [sum(vals[max(0, i - half):min(n, i + half + 1)]) / (min(n, i + half + 1) - max(0, i - half)) for i in range(n)]


def process(path, cfg, only=None):
    seq = unreal.load_asset(path)
    model = seq.get_editor_property('data_model_interface')
    nkeys = model.get_number_of_keys()
    if nkeys != cfg['orig_keys']:
        return f'SKIP {path}: has {nkeys} keys, expected untrimmed {cfg["orig_keys"]} (already trimmed?)'
    s, e = cfg['start'], cfg['end']
    rate = model.get_frame_rate()
    fps = rate.numerator / rate.denominator
    names = [str(n) for n in model.get_bone_track_names()]
    frames = list(range(s, e + 1))
    poses = [unreal.AnimationLibrary.get_bone_poses_for_frame(seq, names, f, False) for f in frames]
    tracks = {n: ([p[i].translation for p in poses], [p[i].rotation for p in poses], [p[i].scale3d for p in poses])
              for i, n in enumerate(names)}

    fwd_curve = right_curve = None
    if cfg['root_motion']:
        hips = next(n for n in names if n.lower() == 'hips')
        pos = tracks[hips][0]
        half = max(1, int(round(SMOOTH_SEC * fps / 2)))
        rx = smooth([p.x for p in pos], half)
        ry = smooth([p.y for p in pos], half)
        # pin trajectory to the hips at first/last frame so hips start and end centred on the capsule
        n = len(pos)
        dx0, dy0, dx1, dy1 = pos[0].x - rx[0], pos[0].y - ry[0], pos[-1].x - rx[-1], pos[-1].y - ry[-1]
        rx = [rx[i] + dx0 * (1 - i / (n - 1)) + dx1 * (i / (n - 1)) for i in range(n)]
        ry = [ry[i] + dy0 * (1 - i / (n - 1)) + dy1 * (i / (n - 1)) for i in range(n)]
        new_pos = [unreal.Vector(p.x - rx[i], p.y - ry[i], p.z) for i, p in enumerate(pos)]
        tracks[hips] = (new_pos, tracks[hips][1], tracks[hips][2])
        fr = [to_fwd_right(rx[i] - rx[0], ry[i] - ry[0]) for i in range(len(frames))]
        fwd_curve = [v[0] for v in fr]
        right_curve = [v[1] for v in fr]

    ctrl = seq.get_editor_property('controller')
    ctrl.open_bracket('prep_skill_anims trim')
    try:
        ctrl.set_number_of_frames(unreal.FrameNumber(e - s))
        ok = all(ctrl.set_bone_track_keys(n, *tracks[n]) for n in names)
    finally:
        ctrl.close_bracket()

    times = [i / fps for i in range(len(frames))]
    for cname, vals in (('RootFwd', fwd_curve), ('RootRight', right_curve)):
        if unreal.AnimationLibrary.does_curve_exist(seq, cname, unreal.RawCurveTrackTypes.RCT_FLOAT):
            unreal.AnimationLibrary.remove_curve(seq, cname, False)
        if vals is not None:
            unreal.AnimationLibrary.add_curve(seq, cname, unreal.RawCurveTrackTypes.RCT_FLOAT, False)
            unreal.AnimationLibrary.add_float_curve_keys(seq, cname, times, vals)

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    travel = f' travel fwd={fwd_curve[-1]:.1f} right={right_curve[-1]:.1f}' if fwd_curve else ''
    return (f'OK {path.split("/")[-1]}: keys {nkeys}->{model.get_number_of_keys()} '
            f'len {seq.get_play_length():.3f}s tracks_set={ok}{travel}')


def run(only=None):
    for path, cfg in CLIPS.items():
        if only and path not in only:
            continue
        print(process(path, cfg))


# ---------------------------------------------------------------- Phase 3: contact bake
# Body regions per frame, with the same maths as UPlayerBallInteractionComponent::GetRegionPoint (socket or bone
# frame x the row's offset / normal), in skill space: F forward, R right, U height above the floor (the mesh
# origin is on the floor: mesh +Y = forward, -X = right, see to_fwd_right).
MESH = '/Game/Football/Models/Footballer/Footballer_latest'
REGION_TABLE = '/Game/Football/Data/DT_BodyRegions'
BALL_RADIUS = 11.0


def _vec(d):
    return unreal.Vector(d['X'], d['Y'], d['Z'])


def region_rows():
    import json
    rows = json.loads(unreal.DataTableFunctionLibrary.export_data_table_to_json_string(unreal.load_asset(REGION_TABLE)))
    return {r['Name']: r for r in rows}


def region_frames(seq, regions, step=None):
    """{region: [(t, point FRU, normal FRU), ...]} for every key of seq (or every `step` seconds)."""
    mesh = unreal.load_asset(MESH)
    rows = region_rows()
    model = seq.get_editor_property('data_model_interface')
    rate = model.get_frame_rate()
    fps = rate.numerator / rate.denominator
    length = seq.get_play_length()
    dt = step or 1.0 / fps
    times = [min(i * dt, length) for i in range(int(round(length / dt)) + 1)]
    opts = unreal.AnimPoseEvaluationOptions()
    opts.set_editor_property('optional_skeletal_mesh', mesh)
    frames = {r: [] for r in regions}
    for t in times:
        pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(seq, t, opts)
        for r in regions:
            row = rows[r]
            if row['Socket'] != 'None':
                socket = mesh.find_socket(row['Socket'])
                rel = unreal.Transform(socket.get_editor_property('relative_location'), socket.get_editor_property('relative_rotation'),
                                       socket.get_editor_property('relative_scale'))
                bone = unreal.AnimPoseExtensions.get_bone_pose(pose, socket.get_editor_property('bone_name'), unreal.AnimPoseSpaces.WORLD)
                frame = unreal.MathLibrary.compose_transforms(rel, bone)
            else:
                frame = unreal.AnimPoseExtensions.get_bone_pose(pose, row['Bone'], unreal.AnimPoseSpaces.WORLD)
            p = unreal.MathLibrary.transform_location(frame, _vec(row['LocalOffset']))
            n = unreal.MathLibrary.transform_direction(frame, _vec(row['LocalNormal'])).normal()
            fp, rp = to_fwd_right(p.x, p.y)
            fn, rn = to_fwd_right(n.x, n.y)
            frames[r].append((t, (fp, rp, p.z), (fn, rn, n.z)))
    return frames


def contact_table(seq_path, regions):
    """Text table per frame and region: point (F R U), normal, speed and the speed along the normal (+ = pushing out)."""
    seq = unreal.load_asset(seq_path)
    frames = region_frames(seq, regions)
    lines = []
    for r in regions:
        lines.append('== %s' % r)
        rows = frames[r]
        for i, (t, p, n) in enumerate(rows):
            if 0 < i < len(rows) - 1:
                (t0, p0, _), (t1, p1, _) = rows[i - 1], rows[i + 1]
                v = [(p1[k] - p0[k]) / (t1 - t0) for k in range(3)]
            else:
                v = [0.0, 0.0, 0.0]
            speed = sum(c * c for c in v) ** 0.5
            push = sum(v[k] * n[k] for k in range(3))
            lines.append('%2d %.3f  p %6.1f %6.1f %5.1f  n %5.2f %5.2f %5.2f  v %4.0f push %5.0f' % (i, t, p[0], p[1], p[2], n[0], n[1], n[2], speed, push))
    return '\n'.join(lines)


# Skills, baked by bake_skill(name). Each contact: the body region, its type (its response mode comes from RESPONSE_MODES,
# spec §122), the window it may happen in (first and last key of the trimmed clip), the planned key of the touch (from
# contact_table: the surface moving into the ball at ball height) and its bounded assist (§14: the share of the gap closed,
# the most it may turn the ball in degrees and change its speed in cm/s; the action's AssistProfile and the player's skill
# scale them). The last contact's exit_deg / exit_speed are the action's exit intent (+ = right); an earlier contact's
# intent is the next contact's spot, worked out at the touch. The touches are physical (UFootballSkillComponent::Touch).
SKILLS = {
    'Elastico': dict(
        tag='Action.Football.Skill.Elastico', category='Action.Football.Skill',
        fallback='Action.Football.Control.Dribble',  # spec §121: too far -> keep dribbling
        # Master plan Phase 6 (2026-10-10): the user's second take, trimmed f57-f77. The outside of the right foot meets the
        # ball at f67 (key 10), the foot comes down round it and its inside cuts it back at f74 (key 17). The player keeps the
        # dribble's speed until the first touch, then it fades fast (the plant). It needs a rolling ball: from standing no
        # touch pair works (the 6.2 study), hence min_speed.
        clip='/Game/Football/Animations/SkillMoves/AS_elastico3_Retargeted',
        mirror_clip='/Game/Football/Animations/SkillMoves/AS_elastico3_Mirrored',
        montage='/Game/Football/Animations/SkillMoves/AM_Skill_Elastico3',
        mirror_montage='/Game/Football/Animations/SkillMoves/AM_Skill_Elastico3_Mirror',
        data='/Game/Football/Data/Skills/DA_Skill_Elastico',
        trajectory='/Game/Football/Data/Skills/DA_Trajectory_Elastico',
        play_rate=1.1, blend_in=0.15, blend_out=0.2, min_speed=100.0,
        # The planned run for the debug trajectory and the report: the agile dribble (UPlayerBallInteractionComponent).
        reference=dict(speed=250.0, guide=30.0),
        # The dribble keeps the ball centred and the outside touch wants it ~22 cm right: a 10 deg turn + ~25 cm move (a
        # 20 deg turn swings the whole move and pushes the ball 32 cm out; 0 deg leaves the move at its 30 cm limit).
        alignment=dict(align_player=True, max_position_correction=30.0, max_rotation_correction=10.0, max_correction_speed=250.0,
                       warp_start_time=0.0, realign_before_each_contact=True, carry_entry_velocity=True, entry_decay_time=0.06),
        recovery=dict(recovery_time=0.0, resume_control_delay=0.25),
        contacts=[
            # Planned at key 10, where the foot starts sweeping outward (f67): the foot comes forward along the ball's side
            # first, and at key 9 it still moves inward, so a touch planned there grazes (+-0.3 cm decided it, and how far
            # the ball went). From key 10 the touch lands at 10.2-10.9 for a ball 30-46 cm ahead (sweep 2026-10-10).
            dict(region='RightFootOutside', type='TOUCH', window=(7, 11), touch=10, assist=0.33, angle=10.0, dv=100.0),
            # Planned at key 17, in the cut back: at key 16 the foot is still swinging out (right) and only catches the ball from
            # behind (it went on at +13 deg); it comes back left at keys 16.5-18 (60-130 cm/s).
            dict(region='RightFootInside', type='REDIRECT', window=(15, 19), touch=17, assist=0.33, angle=10.0, dv=100.0,
                 exit_deg=-40.0, exit_speed=300.0),
        ],
    ),
}
MIRROR_TABLE = '/Game/Football/Animations/MDT_Footballer'
WINDOW_TRACK = 'BallContact'
ATTRIBUTES = '/Game/Football/Data/DA_PlayerAttributes_Default'
# BP_Football's body (the runtime reads it from the ball; these are its values) and UFootballSkillComponent::DefaultFriction.
BALL_MASS, BALL_INERTIA, FRICTION, GRAVITY = 0.43, 0.667, 0.5, -980.0
MAX_ROLLING_LIFT = 50.0  # UFootballSkillComponent's MaxRollingLift

# EBallRegion directions in skill space (forward, right, up), as UBallContactResolver::RegionDirection.
_S = 0.5 ** 0.5
BALL_REGIONS = {
    'FRONT': (1, 0, 0), 'BACK': (-1, 0, 0), 'LEFT': (0, -1, 0), 'RIGHT': (0, 1, 0), 'TOP': (0, 0, 1), 'BOTTOM': (0, 0, -1),
    'FRONT_LEFT': (_S, -_S, 0), 'FRONT_RIGHT': (_S, _S, 0), 'BACK_LEFT': (-_S, -_S, 0), 'BACK_RIGHT': (-_S, _S, 0),
    'TOP_FRONT': (_S, 0, _S), 'TOP_BACK': (-_S, 0, _S), 'TOP_LEFT': (0, -_S, _S), 'TOP_RIGHT': (0, _S, _S),
    'BOTTOM_FRONT': (_S, 0, -_S), 'BOTTOM_BACK': (-_S, 0, -_S), 'BOTTOM_LEFT': (0, -_S, -_S), 'BOTTOM_RIGHT': (0, _S, -_S),
}


def _enum_name(camel):
    import re
    return re.sub(r'(?<!^)(?=[A-Z])', '_', camel).upper()


def _add(a, b, k=1.0):
    return tuple(a[i] + k * b[i] for i in range(3))


def _sub(a, b):
    return tuple(a[i] - b[i] for i in range(3))


def _dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def _len(a):
    return _dot(a, a) ** 0.5


def _unit(a):
    n = _len(a)
    return tuple(c / n for c in a) if n > 1e-9 else (0.0, 0.0, 0.0)


def _heading(v):
    """Degrees from forward, + = right."""
    import math
    return math.degrees(math.atan2(v[1], v[0]))


def _uv(t):
    return unreal.Vector(*t)


def _tv(v):
    return (v.x, v.y, v.z)


def _struct(cls, values):
    s = cls()
    for key, value in values.items():
        s.set_editor_property(key, value)
    return s


def plan_skill(name, trace=None):
    """Works out a skill's contacts from its clip (no assets written) and plays a reference run through the game's own
    maths. Per contact: the ball region (the side facing the body), the expected ball offset (spec §71: the ball centre
    relative to the capsule's ground point at the planned touch, skill space: the region point pushed out along its ground
    normal by the ball's and the region's radius) and the window. The reference run (cfg['reference']: the dribble's speed
    and how far ahead the ball rolls) steps the runtime's flow at 240 Hz: the start alignment (CalculateAlignmentTarget),
    the entry momentum, each touch where the region meets the ball (ComputeResponse, then the player's BoundedBlend toward
    the next contact's spot or the exit), the re-alignment after it and PredictFreeMotion's roll in between. The
    turn is taken as done at once (it takes the blend-in, done before the first window) and the ball as rolling at each
    touch. trace: a list to fill with the per-step gap in the windows. Returns the descriptors, the debug trajectory samples
    and the report."""
    import math
    cfg = SKILLS[name]
    seq = unreal.load_asset(cfg['clip'])
    rate = cfg['play_rate']
    table = region_rows()
    regions = sorted({c['region'] for c in cfg['contacts']})
    frames = region_frames(seq, regions)
    n = len(frames[regions[0]])
    times = [frames[regions[0]][k][0] for k in range(n)]
    length = seq.get_play_length()
    _, root_f = unreal.AnimationLibrary.get_float_keys(seq, 'RootFwd')
    _, root_r = unreal.AnimationLibrary.get_float_keys(seq, 'RootRight')
    if len(root_f) != n:
        raise RuntimeError('root curves have %d keys, the clip %d' % (len(root_f), n))
    root = [(root_f[k], root_r[k], 0.0) for k in range(n)]

    contacts = []
    for index, spec in enumerate(cfg['contacts']):
        k = spec['touch']
        rows = frames[spec['region']]
        normal = _unit(tuple(sum(rows[j][2][i] for j in range(max(k - 1, 0), min(k + 2, n))) for i in range(3)))
        # The touched side of the ball faces the body: the region direction closest to -normal.
        ball_region = max(BALL_REGIONS, key=lambda r: _dot(BALL_REGIONS[r], tuple(-c for c in normal)))
        reach = BALL_RADIUS + table[spec['region']]['Radius']  # SweepRegionToBall: they meet at this centre distance
        out = _unit((normal[0], normal[1], 0.0))
        point = rows[k][1]
        away = reach + spec.get('clearance', 0.0)
        contacts.append(dict(index=index, spec=spec, k=k, ball_region=ball_region, reach=reach, tolerance=spec.get('tolerance', 6.0),
                             approach_dot=spec.get('approach_dot', 0.0), expected=(point[0] + out[0] * away, point[1] + out[1] * away, BALL_RADIUS),
                             restitution=table[spec['region']]['Restitution'], closest=1e9))

    # ---- the reference run: start skill space (X forward, Y right, Z up, the ground at 0), real seconds ----
    ref, al = cfg['reference'], cfg['alignment']
    profile = _struct(unreal.ActionAlignmentProfile, al)
    decel, drag_k = unreal.get_default_object(unreal.BallMovementComponent).get_roll_model()
    attributes = unreal.load_asset(ATTRIBUTES)
    skill_level = unreal.BallAssist.get_attribute(attributes.get_editor_property('attributes'), 'SkillMoves')
    skill_curve = attributes.get_editor_property('skill_to_assist_scale')
    track = {r: [_add(frames[r][k][1], root[k]) for k in range(n)] for r in regions}
    normals = {r: [frames[r][k][2] for k in range(n)] for r in regions}
    entry = (ref['speed'], 0.0, 0.0) if al['carry_entry_velocity'] else (0.0, 0.0, 0.0)
    hold = times[contacts[0]['k']] / rate  # EntryHoldTime: the first contact's ContactTime
    tau = max(al['entry_decay_time'], 0.01)
    yaw = [0.0]
    warp = dict(base=(0.0, 0.0, 0.0), a=0.0, b=hold, move=(0.0, 0.0, 0.0))

    def lerp(keys, t):  # a per-key track at real time t
        kf = min(max(t * rate / times[1], 0.0), n - 1.0)
        k = min(int(kf), n - 2)
        return _add(keys[k], _sub(keys[k + 1], keys[k]), kf - k)

    def turn(v, sign=1.0):
        c, s = math.cos(math.radians(yaw[0] * sign)), math.sin(math.radians(yaw[0] * sign))
        return (v[0] * c - v[1] * s, v[0] * s + v[1] * c, v[2])

    def entry_at(t):  # EntryDisplacement from the start: held to the first contact, then fading
        s = t if t <= hold else hold + tau * (1.0 - math.exp(-(t - hold) / tau))
        return tuple(c * s for c in entry)

    def warp_at(t):  # ApplyAlignment: the move spread over its window with a smoothstep
        x = min(max((t - warp['a']) / (warp['b'] - warp['a']), 0.0), 1.0) if warp['b'] > warp['a'] else 1.0
        return _add(warp['base'], warp['move'], x * x * (3.0 - 2.0 * x))

    def place(p, t):  # a clip point (with the clip's travel) in the world at t: turned, carried and warped
        return _add(_add(turn(p), entry_at(t)), warp_at(t))

    def region_at(r, t):
        return place(lerp(track[r], t), t), _unit(turn(lerp(normals[r], t)))

    def free(p, v, t):  # PredictFreeMotion
        if t <= 0.0:
            return p, v
        loc, vel = unreal.BallMovementComponent.predict_free_motion(_uv(p), _uv(v), v[2] > 1.0 or p[2] > BALL_RADIUS + 0.5,
                                                                      BALL_RADIUS, GRAVITY, decel, drag_k, t)
        return _tv(loc), _tv(vel)

    def spot(index):  # PredictContactSpot: where contact index wants the ball, the current warp done
        c = contacts[index]
        return _add(_add(_add(turn(root[c['k']]), entry_at(times[c['k']] / rate)), _add(warp['base'], warp['move'])), turn(c['expected']))

    def align(index, t, ball, pivot, predicted_root, entry_offset, rotate):
        c = contacts[index]
        basis = unreal.BallContactResolver.make_skill_basis(_uv((0.0, 0.0, 0.0)), _uv(turn((1.0, 0.0, 0.0))), False)
        return unreal.FootballActionComponent.calculate_alignment_target(_struct(unreal.AlignmentQuery, dict(
            predicted_ball=_uv(ball), pivot=_uv(pivot), predicted_root=_uv(predicted_root), entry_offset=_uv(entry_offset), basis=basis,
            expected_ball_offset=_uv(c['expected']), tolerance=c['tolerance'], time_available=times[c['k']] / rate - t,
            window_start=t * rate, window_end=times[c['k']], contact_index=index, allow_rotation=rotate, profile=profile)))

    report = ['reference run: dribble %.0f cm/s, ball %.0f cm ahead; SkillMoves %.0f; roll %.0f cm/s2 + %.2e v2' % (
        ref['speed'], ref['guide'], skill_level, decel, drag_k)]
    # PrepareSkill: the bounded turn, then the bounded move, for the first contact.
    events = [(0.0, (ref['guide'], 0.0, BALL_RADIUS), (ref.get('ball_speed', ref['speed']), 0.0, 0.0))]  # the free ball: (time, centre, velocity)
    ball0, _ = free(events[0][1], events[0][2], hold)
    a = align(0, 0.0, ball0, (0.0, 0.0, 0.0), root[contacts[0]['k']], entry_at(hold), True)
    yaw[0] = a.rotation_correction
    warp['move'] = _tv(a.position_correction)
    report.append('start alignment: turn %+.1f deg, move %.1f cm (needed %.1f), residual %.1f cm%s' % (
        a.rotation_correction, a.position_correction.length(), a.required_correction, a.residual, '' if a.reachable else ' - UNREACHABLE'))

    def gap_at(c, t):  # the region and the free ball at real time t
        ev_t, ev_p, ev_v = events[-1]
        ball, ball_v = free(ev_p, ev_v, t - ev_t)
        point, facing = region_at(c['spec']['region'], t)
        return _len(_sub(ball, point)) - c['reach'], ball, ball_v, point, facing

    def attempt(c, t):  # ResolveContact (T4 the side the region faces, T2 the body closing) + Touch's guard at moment t
        gap, ball, ball_v, point, facing = gap_at(c, t)
        if gap > 0.0:
            return None
        normal = _unit(_sub(ball, point))
        body_v = tuple(x * 480.0 for x in _sub(point, region_at(c['spec']['region'], t - 1.0 / 480.0)[0]))
        raw = None
        if _dot(facing, normal) >= 0.5 and _dot(_unit(_sub(body_v, ball_v)), normal) > c['approach_dot']:
            raw = unreal.BallPhysicsResponse.compute_response(_struct(unreal.BallResponseInput, dict(
                mode=RESPONSE_MODES[c['spec']['type']], ball_center=_uv(ball), ball_velocity=_uv(ball_v),
                ball_angular_velocity=_uv((-ball_v[1] / BALL_RADIUS, ball_v[0] / BALL_RADIUS, 0.0)), ball_radius=BALL_RADIUS,
                ball_mass=BALL_MASS, inertia_factor=BALL_INERTIA, contact_point=_uv(_sub(ball, tuple(x * BALL_RADIUS for x in normal))),
                contact_normal=_uv(normal), body_velocity=_uv(body_v), restitution=c['restitution'], friction=FRICTION)))
        if raw is None or not raw.contact:
            c['refused'] = c.get('refused', 0) + 1
            return None
        return t, ball, ball_v, body_v, raw

    def search(c):  # UFootballSkillComponent::EvaluateWindow: a 1/240 s grid, the window's ends, the first meeting bisected
        w0, w1 = times[c['spec']['window'][0]] / rate, times[c['spec']['window'][1]] / rate
        prev_t, prev_g = w0, gap_at(c, w0)[0]
        c['closest'] = prev_g
        if prev_g <= 0.0:
            hit = attempt(c, w0)
            if hit:
                return hit
        j = int(math.floor(w0 * 240.0)) + 1
        while True:
            t = min(j / 240.0, w1)
            g = gap_at(c, t)[0]
            c['closest'] = min(c['closest'], g)
            if trace is not None:
                _, tb, _, tp, _ = gap_at(c, t)
                rel = turn(_sub(tb, tp), -1.0)
                trace.append('c%d key %5.2f gap %6.2f ball-foot (%6.1f %6.1f %5.1f)' % (c['index'], t * rate / times[1], g, rel[0], rel[1], rel[2]))
            if g <= 0.0:
                if prev_g > 0.0:
                    lo, hi = prev_t, t
                    for _ in range(12):
                        mid = 0.5 * (lo + hi)
                        if gap_at(c, mid)[0] > 0.0:
                            lo = mid
                        else:
                            hi = mid
                    hit = attempt(c, hi)
                    if hit:
                        return hit
                hit = attempt(c, t)
                if hit:
                    return hit
            if t >= w1:
                return None
            prev_t, prev_g = t, g
            j += 1

    for nxt, c in enumerate(contacts):
        spec = c['spec']
        hit = search(c)
        if hit is None:
            report.append('contact %d %s MISSED: closest %.1f cm off' % (nxt, spec['region'], c['closest']))
            break  # the runtime carries on without the ball (first contact) or lets it roll on
        t, ball, ball_v, body_v, raw = hit
        # A grounded ball that isn't lifted leaves the touch rolling (UFootballSkillComponent::Touch).
        physical = _tv(raw.velocity_after)
        if physical[2] < MAX_ROLLING_LIFT:
            physical = _tv(unreal.BallPhysicsResponse.rolling_velocity(raw.velocity_after, raw.angular_velocity_after, BALL_RADIUS, BALL_INERTIA))
        last = nxt == len(contacts) - 1
        if last:
            want = tuple(x * spec['exit_speed'] for x in turn((math.cos(math.radians(spec['exit_deg'])), math.sin(math.radians(spec['exit_deg'])), 0.0)))
        else:
            target = spot(nxt + 1)
            chord = (target[0] - ball[0], target[1] - ball[1], 0.0)
            time_left = max(times[contacts[nxt + 1]['k']] / rate - t, 0.05)
            want = tuple(x * unreal.BallMovementComponent.speed_to_roll(_len(chord), time_left, decel, drag_k) for x in _unit(chord))
        assist = _struct(unreal.BallAssistInput, dict(raw_velocity=_uv(physical), intended_direction=_uv(_unit(want)), intended_speed=_len(want),
                                                      strength=spec['assist'], max_angle_degrees=spec['angle'], max_speed_change=spec['dv']))
        if skill_level >= 0.0:
            assist = unreal.BallAssist.scale_for_skill(assist, skill_level, skill_curve)
        applied = _tv(unreal.BallAssist.bounded_blend(assist).velocity)
        events.append((t, ball, applied))
        report.append('contact %d %s at key %.1f (planned %d, window %d-%d, %d refused): ball %.0f cm/s, body %.0f cm/s; raw %.0f cm/s at %+.0f deg (up %.0f), '
                      'wanted %.0f at %+.0f -> %.0f at %+.0f deg' % (
                          nxt, spec['region'], t * rate / times[1], c['k'], spec['window'][0], spec['window'][1], c.get('refused', 0), _len(ball_v), _len(body_v),
                          _len(physical), _heading(turn(physical, -1.0)), physical[2], _len(want), _heading(turn(want, -1.0)), _len(applied),
                          _heading(turn(applied, -1.0))))
        if not last and al['align_player'] and al['realign_before_each_contact']:
            # Realign: position only, for the next contact, from where the ball will really be; it takes the rest of the warp over.
            c1 = contacts[nxt + 1]
            t1 = times[c1['k']] / rate
            done = warp_at(t)
            remaining = _sub(_add(warp['base'], warp['move']), done)
            pivot = place(lerp(root, t), t)
            ball1, _ = free(ball, applied, t1 - t)
            a = align(nxt + 1, t, ball1, pivot, _add(_add(pivot, _sub(turn(root[c1['k']]), turn(lerp(root, t)))), remaining),
                      _sub(entry_at(t1), entry_at(t)), False)
            warp.update(base=done, a=t, b=t1, move=_add(_tv(a.position_correction), remaining))
            side = max(turn(_sub(free(ball, applied, (t1 - t) * f / 10.0)[0], ball), -1.0)[1] for f in range(11))
            report.append('re-align for contact %d: move %.1f cm (needed %.1f), residual %.1f cm; the ball goes %.1f cm out' % (
                nxt + 1, a.position_correction.length(), a.required_correction, a.residual, side))
    if len(events) > 1:
        exit_v = events[-1][2]
        report.append('exit %.0f cm/s at %+.0f deg from the run-up (%+.0f from the facing after the turn)' % (
            _len(exit_v), _heading(exit_v), _heading(turn(exit_v, -1.0))))

    # The debug trajectory: the ball at every key, relative to the start in the (turned) skill space.
    samples = []
    for j in range(n):
        tj = times[j] / rate
        ev_t, ev_p, ev_v = [e for e in events if e[0] <= tj][-1]
        samples.append((times[j] / length, turn(free(ev_p, ev_v, tj - ev_t)[0], -1.0)))

    descriptors = []
    for c in contacts:
        spec = c['spec']
        exit_deg = spec.get('exit_deg', 0.0)
        k0, k1 = max(spec['window'][0] - 1, 0), min(spec['window'][1] + 1, n - 1)  # the region's path, one key past the window
        rows = frames[spec['region']]
        descriptors.append(dict(body=spec['region'], ball=c['ball_region'], type=spec['type'], time=times[c['k']],
                                path=[rows[k][1] for k in range(k0, k1 + 1)], path_normal=[rows[k][2] for k in range(k0, k1 + 1)],
                                path_start=times[k0], path_step=times[1] - times[0],
                                window=(times[spec['window'][0]], times[spec['window'][1]]), expected=c['expected'],
                                tolerance=c['tolerance'], approach_dot=c['approach_dot'], assist=spec['assist'], angle=spec['angle'], dv=spec['dv'],
                                direction=(math.cos(math.radians(exit_deg)), math.sin(math.radians(exit_deg)), 0.0),
                                exit_speed=spec.get('exit_speed', 300.0)))
    return dict(cfg=cfg, length=length, times=times, contacts=contacts, descriptors=descriptors, samples=samples, report=report)


def _load_or_create(path, cls, factory):
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return unreal.load_asset(path)
    folder, name = path.rsplit('/', 1)
    return unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, cls, factory)


def _data_asset(path, cls):
    factory = unreal.DataAssetFactory()
    factory.set_editor_property('data_asset_class', cls)
    return _load_or_create(path, cls, factory)


def _sections(plan):
    """Doc §9: Start | Approach | [touch window] Contact | Redirection | [release window] Release | Recovery."""
    cfg, d = plan['cfg'], plan['descriptors']
    release = next((x for x in d if x['type'] == 'RELEASE'), d[-1])
    first = d[0]['window']
    starts = [0.0, min(cfg['blend_in'] * cfg['play_rate'], first[0] * 0.5), first[0], first[1], release['window'][0], release['window'][1]]
    return ['Start', 'Approach', 'Contact', 'Redirection', 'Release', 'Recovery'], starts


def _montage(path, clip, plan):
    """Montage of clip with the plan's contact windows, the sections and the blends. Re-runnable: windows and sections are rebuilt."""
    import json
    cfg = plan['cfg']
    factory = unreal.AnimMontageFactory()
    factory.set_editor_property('source_animation', clip)
    factory.set_editor_property('target_skeleton', clip.get_editor_property('skeleton'))
    montage = _load_or_create(path, unreal.AnimMontage, factory)
    lib = unreal.AnimationLibrary
    lib.remove_animation_notify_events_by_track(montage, WINDOW_TRACK)
    if not lib.is_valid_anim_notify_track_name(montage, WINDOW_TRACK):
        lib.add_animation_notify_track(montage, WINDOW_TRACK)
    for i, d in enumerate(plan['descriptors']):
        start, end = d['window']
        notify = lib.add_animation_notify_state_event(montage, WINDOW_TRACK, start, end - start, unreal.AnimNotifyState_BallContactWindow)
        notify.set_editor_property('contact_index', i)
    for prop, value in (('blend_in', cfg['blend_in']), ('blend_out', cfg['blend_out'])):
        blend = montage.get_editor_property(prop)
        blend.set_editor_property('blend_time', value)
        montage.set_editor_property(prop, blend)
    # Blend out after the last frame: the release window runs close to the end and the ball follows the rendered foot.
    montage.set_editor_property('blend_out_trigger_time', 0.0)
    names, starts = _sections(plan)
    sections = json.loads(unreal.FootballEditorLibrary.set_montage_sections(montage, names, starts))
    unreal.EditorAssetLibrary.save_loaded_asset(montage, False)
    return montage, sections


def _mirrored_clip(cfg):
    """The left-foot clip: baked once from the right-foot one with MDT_Footballer (delete it to re-bake after a re-trim)."""
    import json
    if unreal.EditorAssetLibrary.does_asset_exist(cfg['mirror_clip']):
        return unreal.load_asset(cfg['mirror_clip']), 'existing'
    clip = unreal.load_asset(cfg['clip'])
    if not unreal.EditorAssetLibrary.does_asset_exist(MIRROR_TABLE):
        made = json.loads(unreal.FootballEditorLibrary.create_mirror_data_table(clip.get_editor_property('skeleton'), MIRROR_TABLE))
        if not made.get('success'):
            raise RuntimeError('mirror table: %s' % made)
        unreal.EditorAssetLibrary.save_asset(MIRROR_TABLE, False)
    baked = json.loads(unreal.FootballEditorLibrary.bake_mirrored_sequence(clip, unreal.load_asset(MIRROR_TABLE), cfg['mirror_clip']))
    if not baked.get('success'):
        raise RuntimeError('mirror bake: %s' % baked)
    unreal.EditorAssetLibrary.save_asset(cfg['mirror_clip'], False)
    return unreal.load_asset(cfg['mirror_clip']), baked


# Spec §122 response mode per contact type (written to the descriptors; UFootballSkillComponent::Touch plays them).
RESPONSE_MODES = {
    'TOUCH': unreal.BallResponseMode.PHYSICAL_TOUCH, 'REDIRECT': unreal.BallResponseMode.PHYSICAL_REDIRECT,
    'RELEASE': unreal.BallResponseMode.PHYSICAL_REDIRECT, 'CONTROL': unreal.BallResponseMode.CONTROLLED_CONTACT,
    'TRAP': unreal.BallResponseMode.ENERGY_ABSORPTION, 'STRIKE': unreal.BallResponseMode.PHYSICAL_STRIKE,
    'PASS': unreal.BallResponseMode.PHYSICAL_STRIKE, 'SHOT': unreal.BallResponseMode.PHYSICAL_STRIKE,
    'HEADER': unreal.BallResponseMode.PHYSICAL_STRIKE, 'DEFLECTION': unreal.BallResponseMode.DEFLECTION,
}


def bake_skill(name):
    """Writes everything for a skill from plan_skill: the left-foot clip, both montages (windows, sections, blends),
    the trajectory asset and the skill asset. Re-runnable. Returns the plan's report plus what was written."""
    cfg = SKILLS[name]
    plan = plan_skill(name)
    clip = unreal.load_asset(cfg['clip'])
    mirror_clip, mirror_info = _mirrored_clip(cfg)
    montage, sections = _montage(cfg['montage'], clip, plan)
    mirror_montage, _ = _montage(cfg['mirror_montage'], mirror_clip, plan)

    trajectory = _data_asset(cfg['trajectory'], unreal.BallTrajectoryData)
    samples = []
    for t, offset in plan['samples']:
        sample = unreal.BallTrajectorySample()
        sample.set_editor_property('t', t)
        sample.set_editor_property('local_offset', unreal.Vector(*offset))
        samples.append(sample)
    trajectory.set_editor_property('samples', samples)
    trajectory.rebuild_curves_from_samples()
    unreal.EditorAssetLibrary.save_loaded_asset(trajectory, False)

    skill = _data_asset(cfg['data'], unreal.FootballSkillData)
    for prop, key in (('action_tag', 'tag'), ('action_category', 'category'), ('fallback_action_tag', 'fallback')):
        tag = unreal.GameplayTag()
        tag.import_text('(TagName="%s")' % cfg[key])
        skill.set_editor_property(prop, tag)
    skill.set_editor_property('montage', montage)
    skill.set_editor_property('alternate_montage', mirror_montage)
    skill.set_editor_property('play_rate', cfg['play_rate'])
    skill.set_editor_property('root_motion_mode', unreal.SkillRootMotion.BAKED_TRACK)
    skill.set_editor_property('trajectory_profile', trajectory)
    skill.set_editor_property('min_speed', cfg['min_speed'])
    skill.set_editor_property('alignment_profile', _struct(unreal.ActionAlignmentProfile, cfg['alignment']))
    skill.set_editor_property('recovery_profile', _struct(unreal.ActionRecoveryProfile, cfg['recovery']))
    descriptors = [_struct(unreal.BallContactDescriptor, dict(
        body_contact_region=getattr(unreal.BodyRegion, _enum_name(d['body'])), ball_contact_region=getattr(unreal.BallRegion, d['ball']),
        contact_type=getattr(unreal.ContactType, d['type']), response_mode=RESPONSE_MODES[d['type']], contact_time=d['time'],
        window_start=d['window'][0], window_end=d['window'][1], expected_ball_offset=unreal.Vector(*d['expected']),
        contact_tolerance=d['tolerance'], min_approach_dot=d['approach_dot'],
        region_path=[unreal.Vector(*x) for x in d['path']], region_path_normal=[unreal.Vector(*x) for x in d['path_normal']],
        region_path_start=d['path_start'], region_path_step=d['path_step'],
        preferred_direction=unreal.Vector(*d['direction']), preferred_speed=d['exit_speed'],
        assist_strength=d['assist'], max_assist_angle_degrees=d['angle'], max_assist_speed=d['dv'])) for d in plan['descriptors']]
    skill.set_editor_property('contact_descriptors', descriptors)
    unreal.EditorAssetLibrary.save_loaded_asset(skill, False)
    tuning = {k: skill.get_editor_property(k) for k in ('exit_speed', 'exit_angle', 'contact_tolerance')}
    return dict(report=plan['report'], sections=sections, mirror=mirror_info, tuning=tuning,
                written=[cfg['montage'], cfg['mirror_montage'], cfg['trajectory'], cfg['data']])


def validate_skill(name):
    """The doc's data checks (design: ValidateSkillData): windows <-> descriptors, both montages alike, body regions in the
    table, curves present, trajectory matching the descriptors' contacts. Returns a list of problems (empty = fine)."""
    cfg = SKILLS[name]
    problems = []
    skill = unreal.load_asset(cfg['data'])
    if skill is None:
        return ['no skill asset %s' % cfg['data']]
    descriptors = list(skill.get_editor_property('contact_descriptors'))
    rows = region_rows()

    def window_indices(montage):
        out = []
        for e in unreal.AnimationLibrary.get_animation_notify_events(montage):
            state = e.get_editor_property('notify_state_class')
            if isinstance(state, unreal.AnimNotifyState_BallContactWindow):
                out.append(state.get_editor_property('contact_index'))
        return sorted(out)

    montage, mirror = skill.get_editor_property('montage'), skill.get_editor_property('alternate_montage')
    if montage is None or mirror is None:
        problems.append('montage or mirrored montage missing')
    else:
        a, b = window_indices(montage), window_indices(mirror)
        if a != list(range(len(descriptors))):
            problems.append('montage windows %s do not cover descriptors 0..%d' % (a, len(descriptors) - 1))
        if a != b:
            problems.append('mirrored montage windows differ: %s vs %s' % (a, b))
        for m in (montage, mirror):
            for curve in ('RootFwd', 'RootRight'):
                clip = m.get_editor_property('slot_anim_tracks')[0].get_editor_property('anim_track').get_editor_property('anim_segments')[0].get_editor_property('anim_reference')
                if not unreal.AnimationLibrary.does_curve_exist(clip, curve, unreal.RawCurveTrackTypes.RCT_FLOAT):
                    problems.append('%s has no %s curve' % (clip.get_name(), curve))
    for i, d in enumerate(descriptors):
        region = str(d.get_editor_property('body_contact_region')).split('.')[-1].split(':')[0]
        camel = ''.join(w.capitalize() for w in region.split('_'))
        for side in (camel, camel.replace('Right', '@').replace('Left', 'Right').replace('@', 'Left')):
            if side not in rows:
                problems.append('descriptor %d: body region %s not in DT_BodyRegions' % (i, side))
    if skill.get_editor_property('trajectory_profile') is None:
        problems.append('no trajectory profile')
    for i, d in enumerate(descriptors):
        # Spec §71: player alignment needs every contact's expected ball offset from the bake.
        if d.get_editor_property('expected_ball_offset').length() < 1e-3:
            problems.append('descriptor %d has no ExpectedBallOffset (re-bake)' % i)
        # The touch sweeps the baked region path through the whole window (master plan 6.7: frame-rate independent).
        path, start, step = d.get_editor_property('region_path'), d.get_editor_property('region_path_start'), d.get_editor_property('region_path_step')
        if len(path) < 2 or len(d.get_editor_property('region_path_normal')) != len(path) or start > d.get_editor_property('window_start') + 1e-4                 or start + (len(path) - 1) * step < d.get_editor_property('window_end') - 1e-4:
            problems.append('descriptor %d: the region path does not cover its window (re-bake)' % i)
    tag = str(skill.get_editor_property('action_tag').export_text())
    category = str(skill.get_editor_property('action_category').export_text())
    if cfg['tag'] not in tag or cfg['category'] not in category:
        problems.append('tags %s / %s, expected %s / %s' % (tag, category, cfg['tag'], cfg['category']))
    for i, d in enumerate(descriptors):
        # Spec §14: above 0.40 only for an arcade-like action (UBallAssist caps it otherwise).
        if d.get_editor_property('assist_strength') > 0.4 and not d.get_editor_property('arcade_assist'):
            problems.append('descriptor %d AssistStrength %.2f > 0.40 without arcade assist' % (i, d.get_editor_property('assist_strength')))
    return problems


if __name__ == '__main__':
    run()
