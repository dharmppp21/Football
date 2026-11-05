"""Export every /Game/RL_Data clip as raw per-key bone data for the RL trainer (read-only on the assets).
Per clip: D:/Football Game/RL_Train/data/ue_export/<folder>/<name>.f32 = float32 [keys, 91]:
22 local rotation quats (x, y, z, w; order BONES) + the Hips local translation (cm). UE component space
(cm, Z up, left-handed, character faces +Y). Keys sampled at the clip's native rate (kept, not resampled).
Run in the editor: import rl_export_motion as m; m.start()  -> rl_collect tick queue; m.status() for progress."""
import json, os, array
import unreal
import rl_collect

OUT = 'D:/Football Game/RL_Train/data/ue_export'
INV = 'D:/Football Game/RL_Train/data/rl_clip_inventory.json'
BONES = ['Hips', 'Spine', 'Spine1', 'Spine2', 'Neck', 'Head', 'LeftShoulder', 'LeftArm', 'LeftForeArm', 'LeftHand',
         'RightShoulder', 'RightArm', 'RightForeArm', 'RightHand', 'LeftUpLeg', 'LeftLeg', 'LeftFoot', 'LeftToeBase',
         'RightUpLeg', 'RightLeg', 'RightFoot', 'RightToeBase']
A = unreal.AnimPoseExtensions
LOCAL = unreal.AnimPoseSpaces.LOCAL


def out_path(r):
    return os.path.join(OUT, r['folder'], r['name'] + '.f32')


def export_clip(r):
    anim = unreal.load_asset(r['path'])
    opts = unreal.AnimPoseEvaluationOptions()
    opts.evaluation_type = unreal.AnimDataEvalType.RAW
    keys = r['frames'] + 1  # 'Number of Frames' counts intervals; keys include both ends
    buf = array.array('f')
    for k in range(keys):
        p = A.get_anim_pose_at_time(anim, min(k / r['fps'], r['length']), opts)
        for b in BONES:
            q = A.get_bone_pose(p, b, LOCAL).rotation
            buf.extend((q.x, q.y, q.z, q.w))
        t = A.get_bone_pose(p, 'Hips', LOCAL).translation
        buf.extend((t.x, t.y, t.z))
    f = out_path(r)
    os.makedirs(os.path.dirname(f), exist_ok=True)
    with open(f + '.tmp', 'wb') as h:  # temp + rename: a hang mid-write never leaves a half file that resume would skip
        buf.tofile(h)
    os.replace(f + '.tmp', f)
    return keys


def jobs(max_keys=6000):
    rows = [r for r in json.load(open(INV, encoding='utf-8')) if not os.path.exists(out_path(r))]
    out, cur, n = [], [], 0
    for r in rows:
        cur.append(r); n += r['frames'] + 1
        if n >= max_keys:
            out.append(cur); cur, n = [], 0
    if cur:
        out.append(cur)
    return [('%s +%d' % (c[0]['name'], len(c) - 1), (lambda c=c: sum(export_clip(r) for r in c))) for c in out]


def start():
    j = jobs()
    return rl_collect.run(j) if j else 'nothing to export'


def status():
    return rl_collect.status()
