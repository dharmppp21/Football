# Trims the retargeted skill/idle clips and bakes skill travel into RootFwd/RootRight curves.
# Re-run after every retarget regeneration:  exec(open(r'<this file>').read())
# Frame indices are keys at each clip's native rate, chosen from per-frame foot/hips analysis.
import unreal

CLIPS = {
    '/Game/Football/Animations/SkillMoves/AS_elastico_Retargeted':   dict(orig_keys=113, start=60, end=106, root_motion=True),
    '/Game/Football/Animations/SkillMoves/AS_heeltoheel_Retargeted': dict(orig_keys=109, start=56, end=103, root_motion=True),
    '/Game/Football/Animations/Player/AS_Idle_Retargeted':           dict(orig_keys=252, start=75, end=238, root_motion=False),
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


# Skills, baked by bake_skill(name). Each contact: the body region, its type, the window it may happen in (first and
# last key of the trimmed clip), the planned key of the touch (from contact_table: the surface moving into the ball at
# ball height), its AssistStrength (0 = pure physics, 1 = placed exactly) and its T1 tolerance. The touches are physical
# (UFootballSkillComponent::Touch); everything else is worked out from the mocap and the ball's roll.
SKILLS = {
    'Elastico': dict(
        tag='Skill.Football.Dribble.Elastico',
        clip='/Game/Football/Animations/SkillMoves/AS_elastico_Retargeted',
        mirror_clip='/Game/Football/Animations/SkillMoves/AS_elastico_Mirrored',
        montage='/Game/Football/Animations/SkillMoves/AM_Skill_Elastico',
        mirror_montage='/Game/Football/Animations/SkillMoves/AM_Skill_Elastico_Mirror',
        data='/Game/Football/Data/Skills/DA_Skill_Elastico',
        trajectory='/Game/Football/Data/Skills/DA_Trajectory_Elastico',
        play_rate=1.25, blend_in=0.15, blend_out=0.2,
        # The outside of the right foot touches the ball as the foot comes through (one collision, placed so it rolls
        # to where the inside will meet it); the inside then sweeps it back across (a carry: in this clip one push, as
        # the foot turns forward and slides past the ball right after; measured in PIE 2026-10-05).
        contacts=[
            dict(region='RightFootOutside', type='TOUCH', window=(6, 12), touch=8, aim=1.0, tolerance=4.0),
            dict(region='RightFootInside', type='REDIRECT', window=(35, 39), touch=36, aim=0.0, tolerance=8.0),
        ],
    ),
}
MIRROR_TABLE = '/Game/Football/Animations/MDT_Footballer'
WINDOW_TRACK = 'BallContact'
# Carries (UFootballSkillComponent): the body pushes the ball along; the other types are one collision.
CARRIES = ('CONTROL', 'TRAP', 'REDIRECT', 'RELEASE')

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


def _deflect(ball_vel, body_vel, normal, restitution, damping):
    """UBallContactResolver::ComputeDeflection, velocity part: the ball against a moving surface."""
    n = _unit(normal)
    rel = _sub(ball_vel, body_vel)
    approach = _dot(rel, n)
    if approach >= 0.0:
        return ball_vel
    normal_part = tuple(c * approach for c in n)
    slide = _sub(rel, normal_part)
    return _add(body_vel, tuple(slide[i] * (1.0 - damping) - normal_part[i] * restitution for i in range(3)))


def _heading(v):
    """Degrees from forward, + = right."""
    import math
    return math.degrees(math.atan2(v[1], v[0]))


def plan_skill(name):
    """Works out a skill's touches and the ball's path from its clip (no assets written), the way the runtime makes them
    (UFootballSkillComponent::Touch): each touch is a collision with the moving body region (DT_BodyRegions restitution
    and damping) moved by its AssistStrength toward what is needed (rolling to the next touch's spot in time; for the last
    touch the reference collision itself becomes the authored exit), and the ball rolls freely between touches,
    slowing as e^(-k t) (BallRollDecay). Returns the descriptors, windows, trajectory samples and a report."""
    import math
    cfg = SKILLS[name]
    seq = unreal.load_asset(cfg['clip'])
    rate = cfg['play_rate']
    k_roll = unreal.get_default_object(unreal.FootballSkillComponent).get_editor_property('ball_roll_decay')
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

    def ground_point(region, k):  # skill space relative to the start, ground = 0
        return _add(frames[region][k][1], root[k])

    def ground_velocity(region, k):  # cm per real second at the play rate
        a, b = max(k - 1, 0), min(k + 1, n - 1)
        return tuple(c * rate / (times[b] - times[a]) for c in _sub(ground_point(region, b), ground_point(region, a)))

    contacts, report = [], []
    for index, spec in enumerate(cfg['contacts']):
        k = spec['touch']
        rows = frames[spec['region']]
        normal = _unit(tuple(sum(rows[j][2][i] for j in range(max(k - 1, 0), min(k + 2, n))) for i in range(3)))
        # The touched side of the ball faces the body: the region direction closest to -normal.
        ball_region = max(BALL_REGIONS, key=lambda r: _dot(BALL_REGIONS[r], tuple(-c for c in normal)))
        centre = _sub(rows[k][1], tuple(c * BALL_RADIUS for c in BALL_REGIONS[ball_region]))
        row = table[spec['region']]
        contacts.append(dict(index=index, spec=spec, k=k, ball_region=ball_region, normal=normal,
                             offset_z=BALL_RADIUS - centre[2],  # the ball is on the ground when it is touched
                             spot=(centre[0] + root[k][0], centre[1] + root[k][1], BALL_RADIUS),
                             point=ground_point(spec['region'], k), body_vel=ground_velocity(spec['region'], k),
                             restitution=row['Restitution'], damping=row['TangentDamping']))

    def roll(origin, velocity, k_from, k_to):  # a free rolling ball, keys k_from..k_to
        speed, direction = _len(velocity), _unit(velocity)
        for j in range(k_from, k_to + 1):
            dist = speed * (1.0 - math.exp(-k_roll * (times[j] - times[k_from]) / rate)) / k_roll
            world[j] = (origin[0] + direction[0] * dist, origin[1] + direction[1] * dist, BALL_RADIUS)

    # The touches, carries and rolls, in ground space relative to the start.
    world = {j: contacts[0]['spot'] for j in range(contacts[0]['k'] + 1)}  # it waits on the first touch's spot
    ball_vel = (0.0, 0.0, 0.0)
    for i, c in enumerate(contacts):
        spec = c['spec']
        geo_normal = _unit(_sub(c['spot'], c['point']))  # body point -> ball centre
        c['t2_dot'] = _dot(_unit(_sub(c['body_vel'], ball_vel)), geo_normal)
        c['carry'] = spec['type'] in CARRIES
        c['last'] = i == len(contacts) - 1 or spec['type'] == 'RELEASE'
        if c['carry']:
            # As the runtime: the ball takes the body's velocity (no bounce, full grip) on every tick the body presses on
            # it, and rolls on when it stops pressing.
            # planned as one push at the touch key; simulate key by key when a skill's carry keeps pressing (a sole drag).
            physical = needed = applied = (c['body_vel'][0], c['body_vel'][1], 0.0)
            leave_key = c['k']
            report.append('carry %d %s %s key %d (window %d-%d): ball %s; body %.0f cm/s at %+.0f deg pushes it off at that velocity; T2 dot %.2f' % (
                i, spec['type'], spec['region'], c['k'], spec['window'][0], spec['window'][1], c['ball_region'],
                _len(c['body_vel']), _heading(c['body_vel']), c['t2_dot']))
        else:
            physical = _deflect(ball_vel, c['body_vel'], geo_normal, c['restitution'], c['damping'])
            physical = (physical[0], physical[1], 0.0)
            if c['last']:
                needed = physical  # the reference collision is the authored exit
            else:
                nxt = contacts[i + 1]
                chord = _sub(nxt['spot'], c['spot'])
                span = (times[nxt['k']] - times[c['k']]) / rate
                needed = tuple(x * (k_roll / (1.0 - math.exp(-k_roll * span))) for x in (chord[0], chord[1], 0.0))
            aim = spec['aim']
            applied = tuple(physical[j] + (needed[j] - physical[j]) * aim for j in range(3))
            leave_key = c['k']
            report.append('touch %d %s %s key %d (%.3f s, window %d-%d): ball %s; body %.0f cm/s at %+.0f deg; collision %.0f cm/s at %+.0f deg, '
                          'needed %.0f at %+.0f, aim %.1f -> %.0f cm/s at %+.0f deg; T2 dot %.2f' % (
                              i, spec['type'], spec['region'], c['k'], times[c['k']], spec['window'][0], spec['window'][1],
                              c['ball_region'], _len(c['body_vel']), _heading(c['body_vel']), _len(physical), _heading(physical),
                              _len(needed), _heading(needed), aim, _len(applied), _heading(applied), c['t2_dot']))
        c.update(physical=physical, needed=needed, applied=applied, ball_in=ball_vel)
        if c['last']:
            roll(world[leave_key], applied, leave_key, n - 1)
            break
        nxt = contacts[i + 1]
        if nxt['k'] > leave_key:
            roll(world[leave_key], applied, leave_key, nxt['k'])
            span = (times[nxt['k']] - times[leave_key]) / rate
            ball_vel = tuple(x * math.exp(-k_roll * span) for x in applied)
            report.append('roll %d -> %d: %.2f s, arrives %.1f cm from the next spot at %.0f cm/s' % (
                leave_key, nxt['k'], span, _len(_sub(world[nxt['k']], nxt['spot'])), _len(ball_vel)))
            world[nxt['k']] = nxt['spot']
        else:
            ball_vel = applied  # back-to-back contacts: the next one takes the ball over as it is

    # The path relative to the skill's start (UBallTrajectoryData): the runtime swaps the baked travel for the player's real position.
    samples = [(times[j] / length, world[j]) for j in range(n)]

    descriptors = []
    for i, c in enumerate(contacts):
        if 'applied' not in c:
            break  # after the last contact
        spec = c['spec']
        # T2 (doc §7): let it pass where the body grazes the ball, never above the default 0.3. A carry keeps it too:
        # right after a push the ball moves with the body, so only a body clearly pressing on again pushes again (at 0
        # the direction of that near-zero relative velocity is noise, and runs split between one push and many).
        desc = dict(body=spec['region'], ball=c['ball_region'], type=spec['type'], time=times[c['k']],
                    window=(times[spec['window'][0]], times[spec['window'][1]]), offset=(0.0, 0.0, c['offset_z']),
                    tolerance=spec['tolerance'], aim=spec['aim'], threshold=min(0.3, c['t2_dot'] - 0.15),
                    direction=(1.0, 0.0, 0.0), exit_speed=300.0, inherit_player=1.0)
        if c['last']:
            # The exit is a ground velocity in skill space (it already holds the player's travel through the foot).
            desc.update(direction=_unit(c['applied']), exit_speed=_len(c['applied']), inherit_player=0.0)
        descriptors.append(desc)
    steps = [_len(_sub(world[j + 1], world[j])) for j in range(n - 1)]
    report.append('ball path: max step %.1f cm per clip frame (%.1f per 60 fps frame at the play rate); roll decay %.2f/s' % (
        max(steps), max(steps) * rate * (1.0 / 60.0) / (times[1] - times[0]), k_roll))
    return dict(cfg=cfg, length=length, times=times, contacts=contacts, descriptors=descriptors, samples=samples, world=world,
                root=root, report=report)


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


# Spec §122 response mode per contact type (written to the descriptors; the responses come in master plan Phase 3).
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
    tag = unreal.GameplayTag()
    tag.import_text('(TagName="%s")' % cfg['tag'])
    skill.set_editor_property('skill_tag', tag)
    skill.set_editor_property('montage', montage)
    skill.set_editor_property('mirrored_montage', mirror_montage)
    skill.set_editor_property('play_rate', cfg['play_rate'])
    skill.set_editor_property('root_motion_mode', unreal.SkillRootMotion.BAKED_TRACK)
    skill.set_editor_property('trajectory_profile', trajectory)
    descriptors = []
    for d in plan['descriptors']:
        desc = unreal.BallContactDescriptor()
        desc.set_editor_property('body_contact_region', getattr(unreal.BodyRegion, _enum_name(d['body'])))
        desc.set_editor_property('ball_contact_region', getattr(unreal.BallRegion, d['ball']))
        desc.set_editor_property('contact_type', getattr(unreal.ContactType, d['type']))
        desc.set_editor_property('response_mode', RESPONSE_MODES[d['type']])
        desc.set_editor_property('contact_time', d['time'])
        desc.set_editor_property('window_start', d['window'][0])
        desc.set_editor_property('window_end', d['window'][1])
        desc.set_editor_property('local_ball_offset', unreal.Vector(*d['offset']))
        desc.set_editor_property('preferred_direction', unreal.Vector(*d['direction']))
        desc.set_editor_property('assist_strength', d['aim'])
        desc.set_editor_property('contact_tolerance', d['tolerance'])
        desc.set_editor_property('min_approach_dot', d['threshold'])
        desc.set_editor_property('preferred_speed', d['exit_speed'])
        desc.set_editor_property('inherit_player_velocity', d['inherit_player'])
        descriptors.append(desc)
    skill.set_editor_property('contact_descriptors', descriptors)
    unreal.EditorAssetLibrary.save_loaded_asset(skill, False)
    return dict(report=plan['report'], sections=sections, mirror=mirror_info, written=[cfg['montage'], cfg['mirror_montage'], cfg['trajectory'], cfg['data']])


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

    montage, mirror = skill.get_editor_property('montage'), skill.get_editor_property('mirrored_montage')
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
        # Spec §14: above 0.40 only for an arcade-like action. Every skill runs the frozen legacy touch until master plan
        # Phase 6, which uses AssistStrength the old way (1 = the baked path exactly).
        if d.get_editor_property('assist_strength') > 0.4:
            problems.append('legacy until Phase 6: descriptor %d AssistStrength %.2f > 0.40' % (i, d.get_editor_property('assist_strength')))
    return problems


if __name__ == '__main__':
    run()
