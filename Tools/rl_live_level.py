"""The test level for ARLPhysicsPlayer: a floor at z = 0 (MuJoCo's ground), a sun, a sky and one physics footballer.
In the editor: import rl_live_level as l; l.start()   -> rl_collect tick queue (a level must not be made from the TCP
handler); rl_collect.status() for progress. Press Play in /Game/RL_Live/L_RLLiveTest to watch the kick."""
import unreal
import rl_collect

LEVEL = '/Game/RL_Live/L_RLLiveTest'


def make():
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL):
        les.load_level(LEVEL)
        return 0
    assert les.new_level(LEVEL), 'new_level failed'
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    spawn = lambda cls, loc=(0, 0, 0), rot=(0, 0, 0): eas.spawn_actor_from_class(cls, unreal.Vector(*loc), unreal.Rotator(*rot))
    floor = spawn(unreal.StaticMeshActor)
    floor.static_mesh_component.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Plane'))
    floor.set_actor_scale3d(unreal.Vector(200, 200, 1))
    floor.set_actor_label('Floor')
    sun = spawn(unreal.DirectionalLight, (0, 0, 500), (0, -50, 40))
    sun.set_actor_label('Sun')
    sun.light_component.set_editor_property('atmosphere_sun_light', True)
    spawn(unreal.SkyAtmosphere).set_actor_label('Sky')
    sky = spawn(unreal.SkyLight, (0, 0, 300))
    sky.light_component.set_editor_property('real_time_capture', True)
    sky.set_actor_label('SkyLight')
    player = spawn(unreal.load_class(None, '/Script/FootballArena.RLPhysicsPlayer'))
    player.set_actor_label('RLPlayer')
    player.get_component_by_class(unreal.CameraComponent).set_editor_property('field_of_view', 50.0)
    spawn(unreal.PlayerStart, (0, 0, -3000)).set_actor_label('PawnStart')  # the default pawn's sphere, out of the shot
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    world.get_world_settings().set_editor_property('default_game_mode', unreal.GameModeBase)  # no footballer pawn of the game's
    assert les.save_current_level(), 'save failed'
    return 1


def start():
    return rl_collect.run([('rl live level', make)])
