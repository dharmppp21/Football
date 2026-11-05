"""Joint checks that catch bad retargets which bone-direction checks miss (they caught LaFAN/Bandai twist, 2026-10-29).
Run inside the editor (via ue_bridge.py). hinge: knee/elbow bend-axis spread vs known-good clips (MHA video + Bandai);
stats: per-bone twist about the child direction and total rotation vs the Footballer ref pose (flag twist > 40)."""
import sys, unreal, math
t = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir() + 'Tools')
if t not in sys.path: sys.path.append(t)
sk = unreal.load_asset('/Game/Football/Models/Footballer/Footballer_latest').get_editor_property('skeleton')
ref = unreal.AnimPoseExtensions.get_reference_pose(sk)
L = lambda p, b: unreal.AnimPoseExtensions.get_bone_pose(p, b, unreal.AnimPoseSpaces.LOCAL)
def axes(path, bone):
    an = unreal.load_asset(path); out = []
    for i in range(12):
        p = unreal.AnimPoseExtensions.get_anim_pose_at_time(an, an.get_play_length() * (i + 0.5) / 12, unreal.AnimPoseEvaluationOptions())
        q = L(ref, bone).rotation.inversed() * L(p, bone).rotation
        ang = 2 * math.degrees(math.acos(max(-1, min(1, abs(q.w)))))
        if ang > 15:
            v = unreal.Vector(q.x, q.y, q.z).normal() * (1 if q.w >= 0 else -1); out.append(v)
    return out
def spread(vs, refv):
    return [round(math.degrees(math.acos(max(-1, min(1, abs(v.dot(refv))))))) for v in vs]

CH = {}
for s in ('Left', 'Right'):
    CH.update({s + 'Shoulder': s + 'Arm', s + 'Arm': s + 'ForeArm', s + 'ForeArm': s + 'Hand', s + 'UpLeg': s + 'Leg', s + 'Leg': s + 'Foot', s + 'Foot': s + 'ToeBase'})
CH.update({'Spine': 'Spine1', 'Spine1': 'Spine2', 'Spine2': 'Neck', 'Neck': 'Head'})
def tw(q, axis):
    v = unreal.Vector(q.x, q.y, q.z); d = v.dot(axis.normal()); t = abs(math.degrees(2 * math.atan2(d, q.w))); return round(min(t, 360 - t))
def sw(q, axis):  # swing (bend) angle away from axis
    t = abs(math.degrees(2 * math.acos(max(-1, min(1, abs(q.w)))))); return round(t)
def stats(path):
    an = unreal.load_asset(path); r = {}
    for b, c in CH.items():
        m = 0; s_ = 0
        for f in (0.15, 0.35, 0.55, 0.75, 0.95):
            p = unreal.AnimPoseExtensions.get_anim_pose_at_time(an, an.get_play_length() * f, unreal.AnimPoseEvaluationOptions())
            q = L(ref, b).rotation.inversed() * L(p, b).rotation
            m = max(m, tw(q, L(ref, c).translation)); s_ = max(s_, sw(q, None))
        r[b] = (m, s_)
    return r
