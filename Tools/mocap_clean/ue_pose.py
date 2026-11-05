# Run inside the editor: poses transient Footballer meshes in a row (one per (clip, time)) for a visual check.
# The hips travel of each pose is cancelled so the row stays tidy. clear() removes them.
import unreal, json, os

CLEAN = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), 'MocapClean', 'clean')
D = '/Game/Football/Animations/Mocap_Clean/'
ORIGIN = unreal.Vector(3000, 1500, 0)
STEP = 130.0


def _actors():
    w = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    return sorted([a for a in unreal.GameplayStatics.get_all_actors_of_class(w, unreal.SkeletalMeshActor)
                   if a.get_actor_label().startswith('ZZ_MocapCheck')], key=lambda a: int(a.get_actor_label().split('_')[-1]))


def pose(items, yaw=0.0):
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    mesh = unreal.load_asset('/Game/Football/Models/Footballer/Footballer_latest')
    acts = _actors()
    while len(acts) < len(items):
        a = eas.spawn_actor_from_class(unreal.SkeletalMeshActor, ORIGIN, unreal.Rotator(roll=0, pitch=0, yaw=0), transient=True)
        a.set_actor_label('ZZ_MocapCheck_%d' % len(acts))
        a.skeletal_mesh_component.set_skinned_asset_and_update(mesh, True)
        acts = _actors()
    for i, a in enumerate(acts):
        a.set_actor_hidden_in_game(i >= len(items))
        a.skeletal_mesh_component.set_visibility(i < len(items))
    for i, (name, t) in enumerate(items):
        with open(os.path.join(CLEAN, name + '.json'), encoding='utf-8') as fh:
            d = json.load(fh)
        k = min(len(d['frames']) - 1, int(round(t * d['fps'])))
        hx, hy = d['frames'][k][0][0], d['frames'][k][0][1]
        a = acts[i]
        a.set_actor_location(unreal.Vector(ORIGIN.x + i * STEP - hx, ORIGIN.y - hy, 0), False, True)
        a.set_actor_rotation(unreal.Rotator(roll=0, pitch=0, yaw=yaw), True)
        data = unreal.SingleAnimationPlayData()
        data.set_editor_property('anim_to_play', unreal.load_asset(d['dst']))
        data.set_editor_property('saved_position', t)
        data.set_editor_property('saved_play_rate', 0.0)
        data.set_editor_property('saved_playing', False)
        data.set_editor_property('saved_looping', False)
        comp = a.skeletal_mesh_component
        comp.set_editor_property('animation_mode', unreal.AnimationMode.ANIMATION_SINGLE_NODE)
        comp.set_editor_property('animation_data', data)
    return len(items)


def clear():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    n = 0
    for a in _actors():
        eas.destroy_actor(a); n += 1
    return n
