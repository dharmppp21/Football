"""Blender batch: BVH -> FBX for Unreal. Usage: blender -b -P bvh_to_fbx.py -- <in_dir> <out_dir> <scale>
The Eyes JAPAN soccer BVH is in inches (scale 0.0254). A tiny cube skinned to the root is added because Unreal's
FBX import needs a mesh to create a skeleton."""
import bpy, glob, os, sys

src, dst, scale = sys.argv[sys.argv.index('--') + 1:]
os.makedirs(dst, exist_ok=True)
for f in sorted(glob.glob(os.path.join(src, '**', '*.bvh'), recursive=True)):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    # Unreal's Interchange reads a fractional or non-standard FBX rate (Blender custom, e.g. 90 fps) as 12 fps and drops
    # the animation, so the scene runs at the file's rate if standard, otherwise 60 fps with keys rescaled to it
    ft = float(next(l for l in open(f) if l.startswith('Frame Time')).split(':')[1])
    nf = int(next(l for l in open(f) if l.startswith('Frames')).split(':')[1])
    fps = round(1.0 / ft) if round(1.0 / ft) in (24, 25, 30, 48, 50, 60, 120) else 60
    sc_ = bpy.context.scene
    sc_.render.fps, sc_.render.fps_base = fps, 1.0
    bpy.ops.import_anim.bvh(filepath=f, global_scale=float(scale), use_fps_scale=True, update_scene_fps=False,
                            update_scene_duration=False, rotate_mode='NATIVE')
    # Blender never shrinks the scene below its default 250 frames: short clips came out padded with a held pose
    sc_.frame_start, sc_.frame_end = 1, max(1, round((nf - 1) * ft * fps)) + 1
    arm = bpy.context.selected_objects[0]
    root = arm.data.bones[0].name
    bpy.ops.mesh.primitive_cube_add(size=0.02)
    cube = bpy.context.active_object
    cube.vertex_groups.new(name=root).add(range(len(cube.data.vertices)), 1.0, 'REPLACE')
    cube.modifiers.new('Armature', 'ARMATURE').object = arm
    cube.parent = arm
    stem = os.path.splitext(os.path.basename(f))[0]
    if stem == 'motion_actor':  # OmniContact: every take is <case>/<capture>/motion_actor.bvh
        stem = '%s_%s' % (os.path.basename(os.path.dirname(os.path.dirname(f))), os.path.basename(os.path.dirname(f)).split('_')[0])
    out = os.path.join(dst, stem.replace(' ', '_') + '.fbx')
    bpy.ops.export_scene.fbx(filepath=out, use_selection=False, add_leaf_bones=False, bake_anim=True,
                             bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False, bake_anim_simplify_factor=0.0)
    print('OK', out)
