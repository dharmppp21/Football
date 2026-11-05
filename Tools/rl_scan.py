"""Scan every clip of a pack: hips height (source vs Footballer copy) and limb-direction error at 3 times.
scan(name, names) -> writes Saved/MHAQueue/scan_<name>.json and returns a summary of bad clips."""
import json, os, unreal
import rl_check as K, rl_collect as C


def scan(name, names):
    S, D = C.SRC + '/' + name, C.RL + '/Downloaded/' + name
    rows = []
    for a in C.anims(S, False):
        src, dst = str(a.package_name), '%s/%s' % (D, a.asset_name)
        s = unreal.load_asset(src)
        row = dict(clip=str(a.asset_name), len=round(s.get_play_length(), 1))
        if not unreal.EditorAssetLibrary.does_asset_exist(dst):
            row['missing'] = True
        else:
            d = unreal.load_asset(dst)
            L = min(s.get_play_length(), d.get_play_length())
            errs, hs, hd = [], [], []
            for f in (0.2, 0.5, 0.8):
                u, h1 = K._dirs(s, names, L * f)
                v, h2 = K._dirs(d, K.FB, L * f)
                hs.append(h1); hd.append(h2)
                errs += [unreal.MathLibrary.radians_to_degrees(unreal.MathLibrary.acos(max(-1.0, min(1.0, x.dot(y))))) for x, y in zip(u, v)]
            # stretched bones (deformed mesh) and frozen clips (look empty): thigh+shin length and hips travel in the copy
            P = lambda t: unreal.AnimPoseExtensions.get_anim_pose_at_time(d, t, unreal.AnimPoseEvaluationOptions())
            pos = lambda p, b: unreal.AnimPoseExtensions.get_bone_pose(p, b, unreal.AnimPoseSpaces.WORLD).translation
            p0, p1 = P(L * 0.25), P(L * 0.75)
            leg = (pos(p0, 'LeftLeg') - pos(p0, 'LeftUpLeg')).length() + (pos(p0, 'LeftFoot') - pos(p0, 'LeftLeg')).length()
            row.update(leg=round(leg), move=round((pos(p1, 'Hips') - pos(p0, 'Hips')).length() + (pos(p1, 'LeftHand') - pos(p0, 'LeftHand')).length()))
            row.update(err=round(sum(errs) / len(errs), 1), worst=round(max(errs), 1), hs=round(sum(hs) / 3), hd=round(sum(hd) / 3),
                       dlen=round(d.get_play_length(), 1))
        rows.append(row)
    json.dump(rows, open(os.path.join(unreal.Paths.project_saved_dir(), 'MHAQueue', 'scan_%s.json' % name), 'w'), indent=0)
    bad = [r for r in rows if r.get('missing') or r['err'] > 8 or not 40 < r['hs'] < 130 or not 40 < r['hd'] < 140 or abs(r['dlen'] - r['len']) > 0.2]
    return len(rows), len(bad)
