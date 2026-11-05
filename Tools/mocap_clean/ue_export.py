# Run inside the editor: dumps every key of a clip (local bone transforms + float curves) to JSON.
import unreal, json, os

OUT = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), 'MocapClean', 'raw')
SRC = '/Game/CaptureManager/Imports/Mono_Video_Ingest/'


def dump(path, out_name):
    seq = unreal.load_asset(path)
    model = seq.get_editor_property('data_model_interface')
    n = model.get_number_of_keys()
    rate = model.get_frame_rate()
    names = [str(x) for x in model.get_bone_track_names()]
    parents = []
    for b in names:
        p = [str(x) for x in unreal.AnimationLibrary.find_bone_path_to_root(seq, b)]
        parents.append(names.index(p[1].lower()) if len(p) > 1 and p[1].lower() in names else -1)
    frames = []
    for f in range(n):
        ps = unreal.AnimationLibrary.get_bone_poses_for_frame(seq, names, f, False)
        frames.append([[t.translation.x, t.translation.y, t.translation.z, t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
                        t.scale3d.x, t.scale3d.y, t.scale3d.z] for t in ps])
    ref = unreal.AnimPoseExtensions.get_reference_pose(seq.get_editor_property('skeleton'))
    refl = []
    for b in names:
        t = unreal.AnimPoseExtensions.get_bone_pose(ref, b, unreal.AnimPoseSpaces.LOCAL)
        refl.append([t.translation.x, t.translation.y, t.translation.z, t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w])
    curves = {}
    for c in unreal.AnimationLibrary.get_animation_curve_names(seq, unreal.RawCurveTrackTypes.RCT_FLOAT):
        times, vals = unreal.AnimationLibrary.get_float_keys(seq, c)
        curves[str(c)] = [list(times), list(vals)]
    data = dict(path=path, fps=rate.numerator / rate.denominator, rate=[rate.numerator, rate.denominator], keys=n,
                length=seq.get_play_length(), names=names, parents=parents, ref=refl, frames=frames, curves=curves)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, out_name + '.json'), 'w', encoding='utf-8') as fh:
        json.dump(data, fh)
    return '%s keys=%d fps=%.3f bones=%d curves=%s' % (out_name, n, data['fps'], len(names), list(curves))


def dump_all(items):
    res = []
    for path, name in items:
        try:
            res.append(dump(path, name))
        except Exception as e:
            res.append('%s ERROR %r' % (name, e))
    return res
