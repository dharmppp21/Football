"""Retarget sanity check: compare limb directions (world space) between a source clip and its Footballer copy.
check(src, dst, names) -> worst/mean angle in degrees over 8 sample times, plus hip height of each. names maps the
source bones for (thigh, calf, foot, upperarm, forearm, hand) per side, e.g. UE mannequin or Mixamo names."""
import unreal

FB = dict(l=('LeftUpLeg', 'LeftLeg', 'LeftFoot', 'LeftArm', 'LeftForeArm', 'LeftHand'),
          r=('RightUpLeg', 'RightLeg', 'RightFoot', 'RightArm', 'RightForeArm', 'RightHand'), hips='Hips')
UE = dict(l=('thigh_l', 'calf_l', 'foot_l', 'upperarm_l', 'lowerarm_l', 'hand_l'),
          r=('thigh_r', 'calf_r', 'foot_r', 'upperarm_r', 'lowerarm_r', 'hand_r'), hips='pelvis')
MIXAMO = FB  # this Mixamo export has no 'mixamorig:' prefix
EYES = dict(l=('LeftHip', 'LeftKnee', 'LeftAnkle', 'LeftShoulder', 'LeftElbow', 'LeftWrist'),
            r=('RightHip', 'RightKnee', 'RightAnkle', 'RightShoulder', 'RightElbow', 'RightWrist'), hips='Hips')
MH = dict(l=('thigh_l', 'calf_l', 'foot_l', 'upperarm_l', 'lowerarm_l', 'hand_l'),
          r=('thigh_r', 'calf_r', 'foot_r', 'upperarm_r', 'lowerarm_r', 'hand_r'), hips='pelvis')


def _dirs(anim, names, t):
    p = unreal.AnimPoseExtensions.get_anim_pose_at_time(anim, t, unreal.AnimPoseEvaluationOptions())
    pos = lambda b: unreal.AnimPoseExtensions.get_bone_pose(p, b, unreal.AnimPoseSpaces.WORLD).translation
    out = []
    for s in 'lr':
        th, ca, fo, ua, fa, ha = names[s]
        for a, b in ((th, ca), (ca, fo), (ua, fa), (fa, ha)):
            v = pos(b) - pos(a)
            out.append(v.normal())
    return out, pos(names['hips']).z


def check(src, dst, names):
    s, d = unreal.load_asset(src), unreal.load_asset(dst)
    L = min(s.get_play_length(), d.get_play_length())
    worst, tot, n, hs, hd = 0.0, 0.0, 0, 0.0, 0.0
    seg = [0.0] * 8
    for i in range(8):
        t = L * (i + 0.5) / 8
        a, h1 = _dirs(s, names, t)
        b, h2 = _dirs(d, FB, t)
        hs, hd = hs + h1 / 8, hd + h2 / 8
        for k, (u, v) in enumerate(zip(a, b)):
            ang = unreal.MathLibrary.radians_to_degrees(unreal.MathLibrary.acos(max(-1.0, min(1.0, u.dot(v)))))
            worst, tot, n = max(worst, ang), tot + ang, n + 1
            seg[k] += ang / 8
    # per segment: l thigh, l shin, l upperarm, l forearm, then r
    return dict(worst=round(worst, 1), mean=round(tot / n, 1), hips_src=round(hs), hips_dst=round(hd), seg=[round(x) for x in seg])
