"""Physics kicks from RL_Train on the real Footballer. RL_Train/bake_ue.py writes one JSON per animation (UE local bone
rotations + Hips translation at 50 fps; the physics body's also carries its ball); this builds the AnimSequences and one
Level Sequence per kick: the mocap reference beside the physics body and its ball, the baked playback rates one after
another (ground kicks 0.8x, 1x, 1.25x; aerial kicks 0.9x, 1x, 1.1x).
In the editor: import rl_bake_anims as b; b.start('<bake/ue folder>', ...)  -> rl_collect tick queue, assets in
/Game/RL_Bake; rl_collect.status() for progress. Finished assets are skipped, so start() can be re-run."""
import glob, json, math, os
import unreal
import rl_collect

SRC = {}  # animation name -> its JSON (start() fills it from the bake folders)
DEST = '/Game/RL_Bake'
SKEL = '/Game/Football/Models/Footballer/Footballer_latest_Skeleton'
BALL = ('/Game/Football/Models/Ball/Football', 10.9)  # the game's ball mesh is 2 cm across; x10.9 = the 22 cm ball
EAL = unreal.EditorAssetLibrary


def load(name):
    return json.load(open(SRC[name], encoding='utf-8'))


def rates_of(kick):
    """The playback rates baked for a kick, slow to fast ('x0p8', 'x1', ...)."""
    rs = {n.rsplit('_', 1)[1] for n in SRC if n.startswith('RLK_%s_phys_x' % kick)}
    return sorted(rs, key=lambda r: float(r[1:].replace('p', '.')))


def make_anim(name):
    if EAL.does_asset_exist('%s/%s' % (DEST, name)):
        return 0
    js = load(name)
    assert js['fps'] == 30, 'bake at 30 fps: a new AnimSequence is 30 fps, and changing that crashed compression (2026-11-03)'
    fac = unreal.AnimSequenceFactory()
    fac.set_editor_property('target_skeleton', unreal.load_asset(SKEL))
    anim = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, DEST, unreal.AnimSequence, fac)
    n = js['frames']
    c = anim.controller
    c.open_bracket('RL bake', False)
    try:  # an exception inside an open bracket left the model unable to recompress (2026-11-03)
        c.set_number_of_frames(unreal.FrameNumber(n - 1), False)  # frames count intervals; there are n keys
        one = [unreal.Vector(1.0, 1.0, 1.0)] * n
        for b, bone in enumerate(js['bones']):
            c.add_bone_curve(bone, False)
            pos = [unreal.Vector(*h) for h in js['hips']] if b == 0 else [unreal.Vector(*js['local_t'][b])] * n
            c.set_bone_track_keys(bone, pos, [unreal.Quat(*f[b]) for f in js['rot']], one, False)
    finally:
        c.close_bracket(False)
    EAL.save_loaded_asset(anim)
    return 1


def _key(section, values, frames):
    """values: per frame (x, y, z) location in cm, keyed at display-rate frames."""
    ch = section.get_all_channels()
    for f, v in zip(frames, values):
        for i in range(3):
            ch[i].add_key(unreal.FrameNumber(f), v[i])


def make_sequence(kick):
    name = 'LS_RLK_' + kick
    if EAL.does_asset_exist('%s/%s' % (DEST, name)):
        return 0
    items = [(load('RLK_%s_ref_%s' % (kick, r)), load('RLK_%s_phys_%s' % (kick, r))) for r in rates_of(kick)]
    fps = items[0][1]['fps']
    total = sum(p['frames'] - 1 for _, p in items)  # the segments abut: one's last key is the next one's first frame
    seq = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, DEST, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    seq.set_display_rate(unreal.FrameRate(fps, 1))
    seq.set_playback_start(0)
    seq.set_playback_end(total)
    b0, b1 = items[len(items) // 2][1]['ball'][-6], items[len(items) // 2][1]['ball'][-1]  # the middle rate's ball flight
    # at the end (after the kick; an aerial ball arrives from elsewhere): the reference stands beside it
    d = math.hypot(b1[0] - b0[0], b1[1] - b0[1]) or 1.0
    side = (-(b1[1] - b0[1]) / d * 160.0, (b1[0] - b0[0]) / d * 160.0, 0.0)
    mesh = unreal.load_asset(rl_collect.FOOTBALLER)
    for who, loc in (('mocap reference', side), ('physics policy', (0.0, 0.0, 0.0))):
        bnd = seq.add_spawnable_from_class(unreal.SkeletalMeshActor)
        bnd.set_display_name(who)
        bnd.get_object_template().skeletal_mesh_component.set_skeletal_mesh_asset(mesh)
        tr = bnd.add_track(unreal.MovieScene3DTransformTrack).add_section()
        tr.set_range(0, total)
        _key(tr, [loc], [0])
        track = bnd.add_track(unreal.MovieSceneSkeletalAnimationTrack)
        start = 0
        for ref, phys in items:
            js = ref if who.startswith('mocap') else phys
            sec = track.add_section()
            sec.set_range(start, start + js['frames'] - 1)
            p = sec.get_editor_property('params')
            p.set_editor_property('animation', unreal.load_asset('%s/%s' % (DEST, js['name'])))
            sec.set_editor_property('params', p)
            start += js['frames'] - 1
    ball = seq.add_spawnable_from_class(unreal.StaticMeshActor)
    ball.set_display_name('ball')
    t = ball.get_object_template()
    t.static_mesh_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    t.static_mesh_component.set_static_mesh(unreal.load_asset(BALL[0]))
    t.static_mesh_component.set_editor_property('relative_scale3d', unreal.Vector(BALL[1], BALL[1], BALL[1]))
    sec = ball.add_track(unreal.MovieScene3DTransformTrack).add_section()
    sec.set_range(0, total)
    start = 0
    for i, (_, phys) in enumerate(items):  # a segment's last key is the next segment's first frame: keep the later one
        _key(sec, phys['ball'], range(start, start + phys['frames'] - (i < len(items) - 1)))
        start += phys['frames'] - 1
    for i in (6, 7, 8):  # scale channels: keep the template's ball size
        sec.get_all_channels()[i].set_default(BALL[1])
    EAL.save_loaded_asset(seq)
    return 1


def start(*folders):
    SRC.clear()
    for f in folders:
        for path in glob.glob(os.path.join(f, 'RLK_*.json')):
            SRC[os.path.basename(path)[:-5]] = path
    names = sorted(SRC)
    kicks = sorted({n[4:].rsplit('_', 2)[0] for n in names})
    jobs = [(n, (lambda n=n: make_anim(n))) for n in names] + [('LS_' + k, (lambda k=k: make_sequence(k))) for k in kicks]
    return rl_collect.run(jobs)
