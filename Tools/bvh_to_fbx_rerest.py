"""Blender batch: BVH -> FBX like bvh_to_fbx.py, but rebuilt on a natural rest pose (frame 0 of the clip).
LaFAN1 and Bandai BVH rest poses (all-zero rotations) carry rotated joint frames, so retargeting them twisted the
Footballer's thighs ~80-170 deg. Here the armature is duplicated, frame 0 is applied as its rest pose, every bone copies
the original's world transform, and the motion is baked onto the copy before export.
Usage: blender -b -P bvh_to_fbx_rerest.py -- <in_dir> <out_dir> <scale> [roll|frame0] [only_these_names_comma_separated]"""
import bpy, glob, os, sys

args = sys.argv[sys.argv.index('--') + 1:]
src, dst, scale = args[:3]
MODE = args[3] if len(args) > 3 else 'roll'
only = set(args[4].split(',')) if len(args) > 4 and args[4] else None
# mode 'ref': every clip gets the SAME rest - the bones whose names contain one of KEYS take the pose of frame 0 of
# REF (one pack-wide reference clip), the rest keep the BVH rest - so one shared skeleton and retargeter fit all clips
REF = args[5] if len(args) > 5 else None
KEYS = tuple(args[6].split(',')) if len(args) > 6 else ()
REF_POSE = {}
if MODE == 'ref':
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_anim.bvh(filepath=REF, global_scale=float(scale), use_fps_scale=False, rotate_mode='NATIVE')
    a0 = bpy.context.selected_objects[0]; bpy.context.scene.frame_set(int(bpy.context.scene.frame_start))
    import math
    from mathutils import Matrix
    if 'Hips' in KEYS:  # the root takes frame 0's orientation with its facing turned to Blender -Y (BVH forward)
        hp = a0.pose.bones['Hips']
        lft = [b for b in a0.pose.bones if b.name in ('LeftUpLeg', 'UpperLeg_L', 'LeftHip')][0].head
        rgt = [b for b in a0.pose.bones if b.name in ('RightUpLeg', 'UpperLeg_R', 'RightHip')][0].head
        L = (lft - rgt); fwd = (L.y, -L.x)  # forward = up x left, in the ground plane
        yaw = math.atan2(fwd[1], fwd[0]) - math.atan2(-1.0, 0.0)
        hp.matrix = Matrix.Rotation(-yaw, 4, 'Z') @ hp.matrix
        bpy.context.view_layer.update()
    for pb in a0.pose.bones:
        if any(k == pb.name if k == 'Hips' else k in pb.name for k in KEYS):
            REF_POSE[pb.name] = pb.matrix_basis.copy()
os.makedirs(dst, exist_ok=True)
for f in sorted(glob.glob(os.path.join(src, '**', '*.bvh'), recursive=True)):
    stem = os.path.splitext(os.path.basename(f))[0]
    if only and stem not in only:
        continue
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_anim.bvh(filepath=f, global_scale=float(scale), use_fps_scale=False, update_scene_fps=True,
                            update_scene_duration=True, rotate_mode='NATIVE')
    ft = float(next(l for l in open(f) if l.startswith('Frame Time')).split(':')[1])
    sc = bpy.context.scene
    sc.render.fps, sc.render.fps_base = round(1.0 / ft), 1.0
    # Blender never shrinks the scene below its default 250 frames: short clips came out padded with a held pose
    sc_ = bpy.context.scene; sc_.frame_end = sc_.frame_start + int(next(l for l in open(f) if l.startswith('Frames')).split(':')[1]) - 1
    src_arm = bpy.context.selected_objects[0]
    f0, f1 = int(sc.frame_start), int(sc.frame_end)
    # copy with frame 0 as rest
    sc.frame_set(f0)
    new = src_arm.copy(); new.data = src_arm.data.copy(); new.animation_data_clear(); sc.collection.objects.link(new)
    new.name = src_arm.name + '_rest'
    bpy.ops.object.select_all(action='DESELECT'); new.select_set(True); bpy.context.view_layer.objects.active = new
    for pb in new.pose.bones:  # pose the copy like frame 0, then make that the rest pose
        c = pb.constraints.new('COPY_TRANSFORMS'); c.target = src_arm; c.subtarget = pb.name
    bpy.ops.object.mode_set(mode='POSE')
    ARM = ('Shoulder', 'Arm', 'Hand', 'Collar', 'Elbow', 'Wrist', 'Finger', 'Thumb', 'Index')
    if MODE == 'ref':
        for pb in new.pose.bones:
            for c in list(pb.constraints):
                pb.constraints.remove(c)
            pb.matrix_basis = REF_POSE.get(pb.name, pb.matrix_basis.__class__.Identity(4))
            pb.select = pb.name in REF_POSE
    if MODE in ('frame0', 'legs'):
        bpy.ops.pose.select_all(action='DESELECT')
        for pb in new.pose.bones:  # 'legs': re-rest hips, spine, neck, head, legs; arms keep the (straight) T-pose rest
            pb.select = MODE == 'frame0' or not any(k in pb.name for k in ARM)  # Blender 5: selection lives on the pose bone
        bpy.ops.pose.visual_transform_apply()
    for pb in new.pose.bones:
        for c in list(pb.constraints):
            pb.constraints.remove(c)
    if MODE in ('frame0', 'legs', 'ref'):
        bpy.ops.pose.armature_apply(selected=MODE != 'frame0')
    else:  # keep the T-pose rest, only give every bone a consistent roll
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.armature.select_all(action='SELECT')
        bpy.ops.armature.calculate_roll(type='GLOBAL_POS_Z')
        bpy.ops.object.mode_set(mode='POSE')
    for pb in new.pose.bones:  # follow the original again and bake
        c = pb.constraints.new('COPY_TRANSFORMS'); c.target = src_arm; c.subtarget = pb.name
    bpy.ops.nla.bake(frame_start=f0, frame_end=f1, only_selected=False, visual_keying=True, clear_constraints=True,
                     use_current_action=False, bake_types={'POSE'})
    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.data.objects.remove(src_arm, do_unlink=True)
    root = new.data.bones[0].name
    bpy.ops.mesh.primitive_cube_add(size=0.02)
    cube = bpy.context.active_object
    cube.vertex_groups.new(name=root).add(range(len(cube.data.vertices)), 1.0, 'REPLACE')
    cube.modifiers.new('Armature', 'ARMATURE').object = new
    cube.parent = new
    out = os.path.join(dst, stem.replace(' ', '_') + '.fbx')
    bpy.ops.export_scene.fbx(filepath=out, use_selection=False, add_leaf_bones=False, bake_anim=True,
                             bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False, bake_anim_simplify_factor=0.0)
    print('OK', out, f0, f1)
