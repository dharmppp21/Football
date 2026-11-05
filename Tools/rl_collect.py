"""Collect motion clips for RL training into /Game/RL_Data, every clip on the Footballer skeleton. Jobs run one per editor
tick (deferred, never inside the TCP handler); finished outputs are skipped, so a job list can be re-run.
Progress: Saved/MHAQueue/rl_collect_log.json. Load as a module in the editor, then run(jobs); status() reports progress."""
import json
import os
import time
import unreal

RL = '/Game/RL_Data'
FOOTBALLER = '/Game/Football/Models/Footballer/Footballer_latest'
LOG = os.path.join(unreal.Paths.project_saved_dir(), 'MHAQueue', 'rl_collect_log.json')
EAL = unreal.EditorAssetLibrary
AR = unreal.AssetRegistryHelpers.get_asset_registry()
# Queue state survives importlib.reload (a reload mid-run once wiped it and left the tick callback erroring).
_q = globals().get('_q', {})
_s = {'busy': True}  # the old state name: keeps a tick callback orphaned by that reload inert


def anims(path, recursive=True):
    f = unreal.ARFilter(package_paths=[path], recursive_paths=recursive,
                        class_paths=[unreal.TopLevelAssetPath('/Script/Engine', 'AnimSequence')])
    return sorted(AR.get_assets(f), key=lambda a: str(a.package_name))


def retarget(assets, source_mesh, retargeter, out_dir, overwrite=False):
    """Retarget a list of AssetData onto the Footballer into out_dir, same names; existing outputs are skipped unless
    overwrite (overwrite in place, never delete + re-create: see delete_dir)."""
    todo = [a for a in assets if overwrite or not EAL.does_asset_exist('%s/%s' % (out_dir, a.asset_name))]
    if todo:
        unreal.IKRetargetBatchOperation.duplicate_and_retarget(
            todo, unreal.load_asset(source_mesh), unreal.load_asset(FOOTBALLER), unreal.load_asset(retargeter),
            '', '', '', '', out_dir, False, False, overwrite)
    for a in todo:
        p = '%s/%s' % (out_dir, a.asset_name)
        o = unreal.load_asset(p)
        o.set_editor_property('force_root_lock', False)  # GAS clips inherit True, which pins our root (Hips)
        EAL.save_asset(p, only_if_is_dirty=False)
    return len(todo)


def copy(assets, out_dir):
    n = 0
    for a in assets:
        p = '%s/%s' % (out_dir, a.asset_name)
        if not EAL.does_asset_exist(p):
            EAL.duplicate_asset(str(a.package_name), p)
            EAL.save_asset(p, only_if_is_dirty=False)
            n += 1
    return n


def _save():
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'w', encoding='utf-8') as f:
        json.dump(dict(left=len(_q.get('jobs', [])), results=_q.get('results'), started=_q.get('started'), finished=_q.get('finished')), f, indent=1)


def _tick(dt):
    if _q.get('busy') or 'jobs' not in _q:
        return
    _q['busy'] = True
    try:
        if _q['jobs']:
            label, fn = _q['jobs'].pop(0)
            t0 = time.time()
            try:
                _q['results'].append(dict(job=label, ok=True, n=fn(), s=round(time.time() - t0, 1)))
            except Exception as e:
                _q['results'].append(dict(job=label, ok=False, info=repr(e)))
        else:
            h = _q.pop('handle', None)
            if h is not None:
                unreal.unregister_slate_post_tick_callback(h)
            _q['finished'] = time.strftime('%H:%M:%S')
        _save()
    finally:
        _q['busy'] = False


def run(jobs):
    """jobs: list of (label, zero-arg callable returning a count)."""
    if _q.get('handle') is not None:
        return 'already running'
    _q.clear()
    _q.update(jobs=list(jobs), results=[], started=time.strftime('%H:%M:%S'))
    _q['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'queued %d' % len(_q['jobs'])


def status():
    r = _q.get('results', [])
    return dict(done=len(r), failed=[x for x in r if not x['ok']], left=len(_q.get('jobs', [])), finished=_q.get('finished'))


# --- job lists ---------------------------------------------------------------------------------------------------------
GAS = '/Game/Characters/UEFN_Mannequin/Animations'
GAS_SKIP = ('AimOffset', 'Poses', 'ExperimentalStateMachineData')  # single-frame / additive poses, not motion


def gas_jobs(chunk=40):
    groups = {}
    for a in anims(GAS):
        rel = str(a.package_path)[len(GAS) + 1:]
        if rel.split('/')[0] in GAS_SKIP:
            continue
        groups.setdefault(rel, []).append(a)
    jobs = []
    for rel, lst in sorted(groups.items()):
        for i in range(0, len(lst), chunk):
            part = lst[i:i + chunk]
            jobs.append(('GAS/%s[%d]' % (rel, i), lambda part=part, rel=rel: retarget(
                part, '/Game/Characters/UEFN_Mannequin/Meshes/SKM_UEFN_Mannequin',
                RTG_UEFN_FK, '%s/GameAnimSample_RL/%s' % (RL, rel))))
    return jobs


def project_jobs():
    """Clips already on the Footballer skeleton in the project."""
    fb = unreal.load_asset(FOOTBALLER).get_editor_property('skeleton').get_path_name()
    jobs = []
    for sub in ('Mocap_Clean', 'Player', 'SkillMoves', 'GK', 'GAS_FB', 'AIGen/ARDY', 'AIGen/Kimodo', 'AIGen/MotionBricks'):
        src = '/Game/Football/Animations/' + sub
        lst = [a for a in anims(src, False)
               if unreal.load_asset(a.package_name).get_editor_property('skeleton').get_path_name() == fb]
        jobs.append(('Project/' + sub, lambda lst=lst, sub=sub: copy(lst, '%s/Project/%s' % (RL, sub))))
    return jobs


PACK1 = '/Game/Mocap_Pack_05_Soccer/mocap'


def pack1_ue5_jobs():
    """Anderson Rohr free pack 05 (UE5 mannequin variant): each clip was imported with its own mesh and skeleton."""
    jobs = []
    for a in anims(PACK1 + '/UE5', False):
        mesh = '%s/UE5/%s' % (PACK1, str(a.asset_name)[:-len('_Anim')])
        jobs.append(('Pack05/' + str(a.asset_name), lambda a=a, mesh=mesh: retarget(
            [a], mesh, RTG_PACK05_FK, RL + '/Downloaded/AndersonRohr_Pack05_FK')))
    return jobs


# RL copies of the retargeters: FK only (the runtime UEFN one pins the feet to the mannequin's feet with leg IK, which
# bends the legs 30-48 deg away from the source) and a target retarget pose auto-aligned to each source's rest pose.
RTG_DIR = RL + '/_Retargeters'
RTG_UEFN_FK = RTG_DIR + '/RTG_UEFN_To_Footballer_FK'
RTG_PACK05_FK = RTG_DIR + '/RTG_Pack05_To_Footballer_FK'
SIDE = unreal.RetargetSourceOrTarget


def make_fk_rtg(base, dst, source_mesh=None, pose='Pose_MatchSource'):
    if not EAL.does_asset_exist(dst):
        EAL.duplicate_asset(base, dst)
    rc = unreal.IKRetargeterController.get_controller(unreal.load_asset(dst))
    for i in range(rc.get_num_retarget_ops()):
        if str(rc.get_op_name(i)) in ('Retarget IK Goals', 'Run IK Rig'):
            rc.set_retarget_op_enabled(i, False)
    if source_mesh:
        rc.set_preview_mesh(SIDE.SOURCE, unreal.load_asset(source_mesh))
    if str(rc.get_current_retarget_pose_name(SIDE.TARGET)) != pose:
        rc.create_retarget_pose(pose, SIDE.TARGET)
        rc.set_current_retarget_pose(pose, SIDE.TARGET)
    rc.auto_align_all_bones(SIDE.TARGET, unreal.RetargetAutoAlignMethod.CHAIN_TO_CHAIN)
    EAL.save_asset(dst, only_if_is_dirty=False)
    return 1


def rtg_jobs():
    return [('RTG UEFN FK', lambda: make_fk_rtg('/Game/Football/Models/Footballer/RTG_UEFN_To_Footballer', RTG_UEFN_FK)),
            ('RTG Pack05 FK', lambda: make_fk_rtg('/Game/Football/Models/Footballer/RTG_UEFN_To_Footballer', RTG_PACK05_FK,
                                                   PACK1 + '/UE5/01_Dribble_01_ue5'))]


# Never delete a folder and re-create the same asset names in one editor session: on 2026-10-29 that crashed the editor
# (fatal "package name containing double slashes" from Duplicate and Retarget). Write new outputs to new names instead.
def delete_dir(path):
    return int(EAL.delete_directory(path)) if EAL.does_directory_exist(path) else 0


SRC = RL + '/_Source'  # raw imports of downloaded packs (their own skeletons), before retargeting


def import_fbx(files, dest, replace=False):
    """FBX import on the deferred tick (never in the TCP handler). Default Interchange pipeline: mesh, skeleton, anims."""
    tasks = []
    for f in files:
        t = unreal.AssetImportTask()
        t.set_editor_property('filename', f)
        t.set_editor_property('destination_path', dest)
        t.set_editor_property('automated', True)
        t.set_editor_property('save', True)
        t.set_editor_property('replace_existing', replace)
        tasks.append(t)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    return sum(len(t.get_editor_property('imported_object_paths')) for t in tasks)


# Source rigs for the downloaded packs, with chain names matching IKRig_Footballer so chains map by name.
MIXAMO_CHAINS = [('Spine', 'Spine', 'Spine2'), ('Neck', 'Neck', 'Neck'), ('Head', 'Head', 'Head'),
                 ('LeftClavicle', 'LeftShoulder', 'LeftShoulder'), ('LeftArm', 'LeftArm', 'LeftHand'),
                 ('RightClavicle', 'RightShoulder', 'RightShoulder'), ('RightArm', 'RightArm', 'RightHand'),
                 ('LeftLeg', 'LeftUpLeg', 'LeftFoot'), ('LeftFoot', 'LeftToeBase', 'LeftToeBase'),
                 ('RightLeg', 'RightUpLeg', 'RightFoot'), ('RightFoot', 'RightToeBase', 'RightToeBase')]
EYES_CHAINS = [('Spine', 'Chest', 'Chest2'), ('Neck', 'Neck', 'Neck'), ('Head', 'Head', 'Head'),
               ('LeftClavicle', 'LeftCollar', 'LeftCollar'), ('LeftArm', 'LeftShoulder', 'LeftWrist'),
               ('RightClavicle', 'RightCollar', 'RightCollar'), ('RightArm', 'RightShoulder', 'RightWrist'),
               ('LeftLeg', 'LeftHip', 'LeftAnkle'), ('RightLeg', 'RightHip', 'RightAnkle')]  # no toes in this data


def make_ik_rig(path, mesh, root, chains, prefix=''):
    if not EAL.does_asset_exist(path):
        d, n = path.rsplit('/', 1)
        unreal.AssetToolsHelpers.get_asset_tools().create_asset(n, d, unreal.IKRigDefinition, unreal.IKRigDefinitionFactory())
    c = unreal.IKRigController.get_controller(unreal.load_asset(path))
    c.set_skeletal_mesh(unreal.load_asset(mesh))
    c.set_retarget_root(prefix + root)
    for ch in list(c.get_retarget_chains()):
        c.remove_retarget_chain(ch.get_editor_property('chain_name'))
    for name, a, b in chains:
        c.add_retarget_chain(name, prefix + a, prefix + b, '')
    EAL.save_asset(path, only_if_is_dirty=False)
    return len(chains)


def make_pack_rtg(dst, ik_rig, source_mesh, hips_height=0.0):
    """FK-only retargeter from a pack's rig to the Footballer, target pose aligned to the pack's rest pose.
    hips_height: for BVH whose rest pose has the hips on the floor (CMU, Xsens), a source retarget pose lifting them to
    standing height - the pelvis op scales hip travel by target/source rest height, and a 0 source height gave 100x."""
    if not EAL.does_asset_exist(dst):
        EAL.duplicate_asset(RTG_PACK05_FK, dst)
    rt = unreal.load_asset(dst)
    rc = unreal.IKRetargeterController.get_controller(rt)
    rig = unreal.load_asset(ik_rig)
    rc.set_ik_rig(SIDE.SOURCE, rig)
    rc.assign_ik_rig_to_all_ops(SIDE.SOURCE, rig)
    rc.set_preview_mesh(SIDE.SOURCE, unreal.load_asset(source_mesh))
    if hips_height:
        if str(rc.get_current_retarget_pose_name(SIDE.SOURCE)) != 'Pose_HipsUp':
            rc.create_retarget_pose('Pose_HipsUp', SIDE.SOURCE)
            rc.set_current_retarget_pose('Pose_HipsUp', SIDE.SOURCE)
        rc.set_root_offset_in_retarget_pose(unreal.Vector(0, 0, hips_height), SIDE.SOURCE)
    rc.auto_map_chains(unreal.AutoMapChainType.EXACT, True)
    rc.auto_align_all_bones(SIDE.TARGET, unreal.RetargetAutoAlignMethod.CHAIN_TO_CHAIN)
    EAL.save_asset(dst, only_if_is_dirty=False)
    return 1


def pack_jobs(src_dir, rtg_path, out_dir, fallback_mesh=None, overwrite=False):
    """Clips imported with their own mesh (<name> + <name>_Anim); anim-only imports use fallback_mesh."""
    jobs = []
    for a in anims(src_dir, False):
        mesh = '%s/%s' % (src_dir, str(a.asset_name)[:-len('_Anim')])
        mesh = mesh if EAL.does_asset_exist(mesh) else fallback_mesh
        jobs.append((str(a.asset_name), lambda a=a, mesh=mesh: retarget([a], mesh, rtg_path, out_dir, overwrite)))
    return jobs


def import_anim_only(files, dest, skeleton, scale=1.0, replace=False, rate=0):
    """Animation-only FBX import onto an existing skeleton (legacy FBX path, no mesh build). Importing a new skinned mesh
    per file ran a 6 GB GPU out of memory after ~15 files (editor crash 2026-10-29)."""
    tasks = []
    for f in files:
        ui = unreal.FbxImportUI()
        ui.set_editor_property('import_mesh', False)
        ui.set_editor_property('import_as_skeletal', True)
        ui.set_editor_property('import_animations', True)
        ui.set_editor_property('import_materials', False)
        ui.set_editor_property('import_textures', False)
        ui.set_editor_property('skeleton', unreal.load_asset(skeleton))
        ui.set_editor_property('mesh_type_to_import', unreal.FBXImportType.FBXIT_ANIMATION)
        ui.set_editor_property('automated_import_should_detect_type', False)  # else it re-detects a skeletal mesh
        # Blender FBX (metres) came in 100x too big on this legacy path; pass scale=0.01
        ad = ui.get_editor_property('anim_sequence_import_data')
        ad.set_editor_property('import_uniform_scale', scale)
        if rate:  # keep a non-standard native rate (OmniContact 90 fps; Interchange misreads it as 12 fps)
            ad.set_editor_property('use_default_sample_rate', False)
            ad.set_editor_property('custom_sample_rate', rate)
            ad.set_editor_property('snap_to_closest_frame_boundary', True)  # Blender's 90 fps FBX end half a frame late
        t = unreal.AssetImportTask()
        for k, v in (('filename', f), ('destination_path', dest), ('automated', True), ('save', True), ('options', ui),
                     ('replace_existing', replace),
                     ('destination_name', os.path.splitext(os.path.basename(f))[0] + '_Anim')):
            t.set_editor_property(k, v)
        tasks.append(t)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    return sum(len(t.get_editor_property('imported_object_paths')) for t in tasks)


def chains(spine, neck, head, clav, arm, leg, foot=None, left='Left', right='Right'):
    """Footballer-named chains; side-dependent bone names use {S}, filled with left / right."""
    out = [('Spine',) + spine, ('Neck', neck, neck), ('Head', head, head)]
    for side, tok in (('Left', left), ('Right', right)):
        f = lambda s: s.format(S=tok)
        out += [(side + 'Clavicle', f(clav), f(clav)), (side + 'Arm', f(arm[0]), f(arm[1])), (side + 'Leg', f(leg[0]), f(leg[1]))]
        if foot:
            out.append((side + 'Foot', f(foot), f(foot)))
    return out


# downloaded BVH packs, converted to FBX by bvh_to_fbx.py (folders under D:/Football Game/RL_Downloads/<key>/fbx)
PACKS = {
    'cmu_soccer': ('CMU_Kicks', chains(('LowerBack', 'Spine1'), 'Neck', 'Head', '{S}Shoulder', ('{S}Arm', '{S}Hand'), ('{S}UpLeg', '{S}Foot'), '{S}ToeBase'), 97.0),
    'lafan1': ('LaFAN1', chains(('Spine', 'Spine2'), 'Neck', 'Head', '{S}Shoulder', ('{S}Arm', '{S}Hand'), ('{S}UpLeg', '{S}Foot'), '{S}Toe')),
    'xsens_emm': ('Xsens_EMM', chains(('Chest', 'Chest4'), 'Neck', 'Head', '{S}Collar', ('{S}Shoulder', '{S}Wrist'), ('{S}Hip', '{S}Ankle'), '{S}Toe'), 95.0),
    'bandai': ('BandaiNamco', chains(('Spine', 'Chest'), 'Neck', 'Head', 'Shoulder_{S}', ('UpperArm_{S}', 'Hand_{S}'), ('UpperLeg_{S}', 'Foot_{S}'), 'Toes_{S}', 'L', 'R')),
}
# re-converted with bvh_to_fbx_rerest.py (shared reference rest; the plain conversion twisted thighs 80-170 deg)
PACKS['bandai_fixed'] = ('Bandai',) + PACKS['bandai'][1:]
PACKS['lafan1_fixed'] = ('LaFAN',) + PACKS['lafan1'][1:]
# second fix: LaFAN's hips rest faces a rotated direction too - re-rest hips (facing straightened) + legs + clavicles
PACKS['omnicontact'] = ('OmniContact', chains(('Spine', 'Spine4'), 'Neck', 'Head', '{S}Shoulder', ('{S}Arm', '{S}Hand'), ('{S}UpLeg', '{S}Foot'), '{S}ToeBase'), 92.0)
PACKS['lafan1_fixed2'] = ('LaFAN1_Fixed',) + PACKS['lafan1'][1:]
DL = 'D:/Football Game/RL_Downloads'


def pack_import_jobs(key, chunk=8, redo=False):
    """First clip with its mesh (creates the shared skeleton), the rest animation-only onto it at 1/100 scale."""
    import glob
    name = PACKS[key][0]
    files = [f.replace(chr(92), '/') for f in sorted(glob.glob('%s/%s/fbx/*.fbx' % (DL, key)))]
    dest = '%s/%s' % (SRC, name)
    first = os.path.splitext(os.path.basename(files[0]))[0]
    have = lambda f: not redo and EAL.does_asset_exist('%s/%s_Anim' % (dest, os.path.splitext(os.path.basename(f))[0]))
    jobs = [(name + ' mesh', lambda: 0 if have(files[0]) else import_fbx(files[:1], dest, redo))]
    rest = files[1:]
    for i in range(0, len(rest), chunk):
        jobs.append(('%s anim %d' % (name, i), lambda part=rest[i:i + chunk]: import_anim_only(
            [f for f in part if not have(f)], dest, '%s/%s_Skeleton' % (dest, first), 0.01, redo) if any(not have(f) for f in part) else 0))
    return jobs


def pack_retarget_jobs(key, redo=False):
    import glob
    name, ch = PACKS[key][:2]
    hips = PACKS[key][2] if len(PACKS[key]) > 2 else 0.0
    dest = '%s/%s' % (SRC, name)
    first = os.path.splitext(os.path.basename(sorted(glob.glob('%s/%s/fbx/*.fbx' % (DL, key)))[0]))[0]
    mesh, rig, rtg = '%s/%s' % (dest, first), '%s/IK_%s' % (RTG_DIR, name), '%s/RTG_%s_To_Footballer_FK' % (RTG_DIR, name)
    jobs = [(name + ' rig', lambda: make_ik_rig(rig, mesh, 'Hips', ch)), (name + ' rtg', lambda: make_pack_rtg(rtg, rig, mesh, hips))]
    def queue_clips():  # source clips exist only after the import jobs ran, so list them now and queue in chunks
        lst = pack_jobs(dest, rtg, '%s/Downloaded/%s' % (RL, name), mesh, redo)
        for i in range(0, len(lst), 25):
            _q['jobs'].append(('%s rt %d' % (name, i), lambda part=lst[i:i + 25]: sum(fn() for _, fn in part)))
        return len(lst)
    return jobs + [(name + ' queue', queue_clips)]


# bone names per pack for rl_check.check (thigh, shin, foot, upperarm, forearm, hand per side)
CHECK_NAMES = {
    'Xsens_EMM': dict(l=('LeftHip', 'LeftKnee', 'LeftAnkle', 'LeftShoulder', 'LeftElbow', 'LeftWrist'),
                      r=('RightHip', 'RightKnee', 'RightAnkle', 'RightShoulder', 'RightElbow', 'RightWrist'), hips='Hips'),
    'BandaiNamco': dict(l=('UpperLeg_L', 'LowerLeg_L', 'Foot_L', 'UpperArm_L', 'LowerArm_L', 'Hand_L'),
                        r=('UpperLeg_R', 'LowerLeg_R', 'Foot_R', 'UpperArm_R', 'LowerArm_R', 'Hand_R'), hips='Hips'),
}
CHECK_NAMES['Bandai'] = CHECK_NAMES['BandaiNamco']


def import_mesh_legacy(f, dest, rate):
    """First clip of a pack through the legacy FBX path with its mesh, at a custom sample rate."""
    ui = unreal.FbxImportUI()
    for k, v in (('import_mesh', True), ('import_as_skeletal', True), ('import_animations', True), ('import_materials', False),
                 ('import_textures', False), ('create_physics_asset', False), ('mesh_type_to_import', unreal.FBXImportType.FBXIT_SKELETAL_MESH)):
        ui.set_editor_property(k, v)
    ad = ui.get_editor_property('anim_sequence_import_data')
    ad.set_editor_property('use_default_sample_rate', False)
    ad.set_editor_property('custom_sample_rate', rate)
    t = unreal.AssetImportTask()
    for k, v in (('filename', f), ('destination_path', dest), ('automated', True), ('save', True), ('options', ui)):
        t.set_editor_property(k, v)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([t])
    return len(t.get_editor_property('imported_object_paths'))


def import_anim_interchange(files, dest, skeleton, rate):
    """Animation-only import through Interchange onto an existing skeleton at a fixed sample rate, snapping the length to
    the closest frame (Blender's 90 fps FBX are half a frame long, which Interchange otherwise rejects)."""
    tasks = []
    for f in files:
        p = unreal.InterchangeGenericAssetsPipeline()
        c = p.get_editor_property('common_skeletal_meshes_and_animations_properties')
        c.set_editor_property('import_only_animations', True)
        c.set_editor_property('skeleton', unreal.load_asset(skeleton))
        a = p.get_editor_property('animation_pipeline')
        a.set_editor_property('custom_bone_animation_sample_rate', rate)
        a.set_editor_property('frame_alignment', unreal.InterchangeFrameAlignment.SNAP_TO_CLOSEST)
        t = unreal.AssetImportTask()
        for k, v in (('filename', f), ('destination_path', dest), ('automated', True), ('save', True), ('options', p)):
            t.set_editor_property(k, v)
        tasks.append(t)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    return sum(len(t.get_editor_property('imported_object_paths')) for t in tasks)
