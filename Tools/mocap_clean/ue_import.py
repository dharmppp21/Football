# Run inside the editor: writes clean/<name>.json into a copy of its source clip (never the source itself).
import unreal, json, os, time

LOG = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), 'MocapClean', 'import_log.txt')
CLEAN = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), 'MocapClean', 'clean')
E = unreal.EditorAssetLibrary
L = unreal.AnimationLibrary
FLOAT = unreal.RawCurveTrackTypes.RCT_FLOAT


def log(msg):
    with open(LOG, 'a', encoding='utf-8') as fh:
        fh.write(time.strftime('%H:%M:%S ') + msg + '\n')


def write_logged(names):
    """One clip after another, each result (and its key check) appended to LOG, so a long call can be polled."""
    for nm in names:
        t = time.time()
        try:
            r = write(nm)
        except Exception as e:
            r = '%s: ERROR %r' % (nm, e)
        log('%s  (%.1f s)' % (r, time.time() - t))
        try:
            log(check(nm))
        except Exception as e:
            log('%s: check ERROR %r' % (nm, e))
    log('DONE')


def write(name):
    t0 = time.time()
    with open(os.path.join(CLEAN, name + '.json'), encoding='utf-8') as fh:
        d = json.load(fh)
    src, dst = d['src_path'], d['dst']
    if not dst.startswith('/Game/Football/Animations/Mocap_Clean/'):
        return '%s: refused, destination outside Mocap_Clean' % name
    if not E.does_asset_exist(dst):
        if src.startswith('/Game/Football/Animations/Mocap_Clean/') or not E.duplicate_asset(src, dst):
            return '%s: duplicate failed' % name
    seq = unreal.load_asset(dst)
    model = seq.get_editor_property('data_model_interface')
    tracks = [str(x) for x in model.get_bone_track_names()]
    names, frames = d['names'], d['frames']
    if sorted(tracks) != sorted(names):
        return '%s: bone tracks differ' % name
    n = len(frames)
    one = unreal.Vector(1, 1, 1)
    ctrl = seq.get_editor_property('controller')
    ctrl.open_bracket('mocap clean ' + name)
    try:
        ctrl.set_number_of_frames(unreal.FrameNumber(n - 1))
        ok = True
        for b, bone in enumerate(names):
            pos = [unreal.Vector(f[b][0], f[b][1], f[b][2]) for f in frames]
            rot = [unreal.Quat(f[b][3], f[b][4], f[b][5], f[b][6]) for f in frames]
            ok &= bool(ctrl.set_bone_track_keys(bone, pos, rot, [one] * n))
    finally:
        ctrl.close_bracket()
    log('  %s tracks written %.1f s' % (name, time.time() - t0))
    for cname, (times, vals) in d['curves'].items():
        if L.does_curve_exist(seq, cname, FLOAT):
            L.remove_curve(seq, cname, False)
        L.add_curve(seq, cname, FLOAT, False)
        L.add_float_curve_keys(seq, cname, times, vals)
    log('  %s curves written %.1f s' % (name, time.time() - t0))
    E.save_asset(dst, only_if_is_dirty=False)
    log('  %s saved %.1f s' % (name, time.time() - t0))
    return '%s: keys %d -> %d, %.3f s, tracks ok=%s' % (name, n, model.get_number_of_keys(), seq.get_play_length(), ok)


def check(name):
    """Max difference between the asset's keys and the JSON (cm, quaternion dot)."""
    with open(os.path.join(CLEAN, name + '.json'), encoding='utf-8') as fh:
        d = json.load(fh)
    seq = unreal.load_asset(d['dst'])
    names, frames = d['names'], d['frames']
    dp, dq = 0.0, 1.0
    for f in range(len(frames)):
        ps = L.get_bone_poses_for_frame(seq, names, f, False)
        for b, t in enumerate(ps):
            g = frames[f][b]
            dp = max(dp, abs(t.translation.x - g[0]), abs(t.translation.y - g[1]), abs(t.translation.z - g[2]))
            dot = abs(t.rotation.x * g[3] + t.rotation.y * g[4] + t.rotation.z * g[5] + t.rotation.w * g[6])
            dq = min(dq, dot)
    return '%s: max pos diff %.4f cm, min quat dot %.6f, skeleton %s' % (name, dp, dq, seq.get_editor_property('skeleton').get_name())
