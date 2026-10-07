"""Builds the body contact regions (framework Phase 2, doc §4 / §21).

From the Footballer skeleton's reference pose it computes every EBodyRegion's surface point and outward
normal, converts them to bone space, then:
- creates the 12 foot sockets on the mesh (foot_<l|r>_<inside|outside|heel|toe|instep|sole>), the
  "carefully placed sockets" of doc §21; the rest of the regions are bone + offset rows;
- writes /Game/Football/Data/DT_BodyRegions (FBodyRegionRow, row name = EBodyRegion name).
Safe to re-run: its own sockets are replaced and the table is rewritten.

Use from util.execute_python:
    g = {}; exec(open(r'<project>/Tools/build_body_regions.py').read(), g); print(g['build']())
Mesh space of this character: +Y = forward, +X = the character's left, +Z = up.
"""
import json
import unreal

MESH = '/Game/Football/Models/Footballer/Footballer_latest'
SKELETON = '/Game/Football/Models/Footballer/Footballer_latest_Skeleton'
TABLE_DIR, TABLE_NAME = '/Game/Football/Data', 'DT_BodyRegions'

V = unreal.Vector
UP, FWD = V(0, 0, 1), V(0, 1, 0)

# Deflection response per surface type (starting tunables): restitution, tangent damping, spin transfer.
RESPONSE = {
    'foot': (0.55, 0.2, 0.3), 'knee': (0.6, 0.2, 0.1), 'thigh': (0.35, 0.3, 0.1), 'chest': (0.25, 0.4, 0.0),
    'back': (0.35, 0.3, 0.0), 'head': (0.65, 0.15, 0.1), 'shoulder': (0.45, 0.25, 0.0), 'hand': (0.3, 0.3, 0.0),
}


def _n(v):
    return v.normal()


def _lerp(a, b, t):
    return a + (b - a) * t


def _flat(v, z):
    return V(v.x, v.y, z)


def regions(bone_cs):
    """{region: (bone, socket_or_None, point_cs, normal_cs, radius, response_key)} in mesh component space."""
    out = {}
    P = bone_cs  # bone -> component-space location
    for side, s in (('Left', 1.0), ('Right', -1.0)):
        foot, toe, toe_end = P[side + 'Foot'], P[side + 'ToeBase'], P[side + 'Toe_End']
        f = _n(V(toe.x - foot.x, toe.y - foot.y, 0))                 # foot forward (toe-out included)
        l = _n(f.cross(UP)) * s                                       # outward, away from the body
        sole = min(toe.z, toe_end.z) - 0.5
        fb, tb, sk = side + 'Foot', side + 'ToeBase', 'foot_%s_' % ('l' if s > 0 else 'r')
        mid = lambda t, z: _flat(_lerp(foot, toe, t), sole + z)
        out[side + 'Toe'] = (tb, sk + 'toe', _flat(toe_end, sole + 3.5) + f * 1.5, f, 4, 'foot')
        out[side + 'ToeInside'] = (tb, None, _flat(toe, sole + 3) + f * 2 - l * 4.5, l * -1.0, 4, 'foot')
        out[side + 'ToeOutside'] = (tb, None, _flat(toe, sole + 3) + f * 2 + l * 4.5, l, 4, 'foot')
        out[side + 'Instep'] = (fb, sk + 'instep', mid(0.55, 7.5), _n(f * 0.7 + UP), 4, 'foot')
        out[side + 'FootInside'] = (fb, sk + 'inside', mid(0.35, 4) - l * 5, l * -1.0, 4, 'foot')
        out[side + 'FootOutside'] = (fb, sk + 'outside', mid(0.35, 4) + l * 5, l, 4, 'foot')
        out[side + 'FootCenter'] = (fb, None, mid(0.3, 5), f, 4, 'foot')
        out[side + 'Heel'] = (fb, sk + 'heel', _flat(foot, sole + 4) - f * 7, f * -1.0, 4, 'foot')
        out[side + 'HeelInside'] = (fb, None, _flat(foot, sole + 4) - f * 4 - l * 4.5, l * -1.0, 4, 'foot')
        out[side + 'HeelOutside'] = (fb, None, _flat(foot, sole + 4) - f * 4 + l * 4.5, l, 4, 'foot')
        out[side + 'Sole'] = (fb, sk + 'sole', mid(0.45, 0), UP * -1.0, 4, 'foot')
        up_leg, leg = P[side + 'UpLeg'], P[side + 'Leg']
        out[side + 'Thigh'] = (side + 'UpLeg', None, _lerp(up_leg, leg, 0.5) + FWD * 8, FWD, 8, 'thigh')
        out[side + 'Knee'] = (side + 'Leg', None, leg + FWD * 5, FWD, 5, 'knee')
        out[side + 'Shin'] = (side + 'Leg', None, _lerp(leg, foot, 0.5) + FWD * 4.5, FWD, 5, 'knee')
        out[side + 'Shoulder'] = (side + 'Shoulder', None, P[side + 'Arm'] + UP * 5 + V(s, 0, 0) * 2, _n(V(s, 0, 0) + UP * 0.5), 6, 'shoulder')
        palm = _lerp(P[side + 'Hand'], P[side + 'HandMiddle1'], 0.5) - UP * 2   # T-pose: palms face down
        out[side + 'Hand'] = (side + 'Hand', None, palm, UP * -1.0, 5, 'hand')
    spine, spine1, spine2 = P['Spine'], P['Spine1'], P['Spine2']
    out['Chest'] = ('Spine2', None, spine2 + FWD * 13 + UP * 2, FWD, 10, 'chest')
    out['ChestLeft'] = ('Spine2', None, spine2 + FWD * 11 + V(9, 0, 0) + UP * 2, _n(FWD + V(0.6, 0, 0)), 8, 'chest')
    out['ChestRight'] = ('Spine2', None, spine2 + FWD * 11 + V(-9, 0, 0) + UP * 2, _n(FWD + V(-0.6, 0, 0)), 8, 'chest')
    out['Abdomen'] = ('Spine', None, _lerp(spine, spine1, 0.5) + FWD * 11, FWD, 10, 'chest')
    out['Back'] = ('Spine1', None, spine1 - FWD * 9, FWD * -1.0, 10, 'back')
    head = _lerp(P['Head'], P['HeadTop_End'], 0.5)
    out['Head'] = ('Head', None, head, FWD, 10, 'head')
    out['Forehead'] = ('Head', None, head + FWD * 10 + UP * 3, _n(FWD + UP * 0.3), 6, 'head')
    out['HeadLeft'] = ('Head', None, head + V(8, 0, 0), V(1, 0, 0), 6, 'head')
    out['HeadRight'] = ('Head', None, head + V(-8, 0, 0), V(-1, 0, 0), 6, 'head')
    out['HeadTop'] = ('Head', None, P['HeadTop_End'] - UP * 1, UP, 6, 'head')
    return out


def _vec(v):
    return {'X': round(v.x, 4), 'Y': round(v.y, 4), 'Z': round(v.z, 4)}


def build():
    skel = unreal.load_asset(SKELETON)
    mesh = unreal.load_asset(MESH)
    pose = unreal.AnimPoseExtensions.get_reference_pose(skeleton=skel)
    names = [str(n) for n in unreal.AnimPoseExtensions.get_bone_names(pose)]
    bone_xf = {n: unreal.AnimPoseExtensions.get_bone_pose(pose, n, unreal.AnimPoseSpaces.WORLD) for n in names}
    defs = regions({n: t.translation for n, t in bone_xf.items()})

    enum_names = [r.name for r in unreal.BodyRegion]  # python names, e.g. LEFT_FOOT_INSIDE
    missing = [e for e in enum_names if e.replace('_', '').lower() not in {k.lower() for k in defs}]
    if missing:
        return {'error': 'no definition for %s' % missing}

    rows, sockets = [], []
    for region, (bone, socket, point, normal, radius, kind) in defs.items():
        xf = bone_xf[bone]
        local_p = xf.inverse_transform_location(point)
        local_n = xf.rotation.unrotate_vector(normal).normal()
        rest, damp, spin = RESPONSE[kind]
        row = {'Name': region, 'Socket': 'None', 'Bone': bone, 'LocalOffset': _vec(local_p), 'LocalNormal': _vec(local_n),
               'Radius': radius, 'Restitution': rest, 'TangentDamping': damp, 'SpinTransfer': spin}
        if socket:
            rot = unreal.MathLibrary.make_rot_from_x(local_n)
            sockets.append((socket, bone, local_p, rot))
            row.update(Socket=socket, Bone='None', LocalOffset=_vec(V(0, 0, 0)), LocalNormal=_vec(V(1, 0, 0)))
        rows.append(row)

    # Sockets: replace ours.
    socket_results = []
    for socket, bone, p, rot in sockets:
        unreal.MCPythonHelper.remove_skeletal_mesh_socket(mesh, socket)
        r = json.loads(unreal.MCPythonHelper.add_skeletal_mesh_socket(mesh, socket, bone, p.x, p.y, p.z, rot.pitch, rot.yaw, rot.roll))
        socket_results.append((socket, r.get('success')))

    # Table: create once, then (re)fill.
    path = TABLE_DIR + '/' + TABLE_NAME
    table = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if table is None:
        factory = unreal.DataTableFactory()
        factory.set_editor_property('struct', unreal.BodyRegionRow.static_struct())
        table = unreal.AssetToolsHelpers.get_asset_tools().create_asset(TABLE_NAME, TABLE_DIR, unreal.DataTable, factory)
    filled = unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(table, json.dumps(rows))

    saved = [unreal.EditorAssetLibrary.save_loaded_asset(mesh, False), unreal.EditorAssetLibrary.save_loaded_asset(table, False)]
    return {'regions': len(rows), 'sockets': socket_results, 'table_filled': filled,
            'table_rows': len(unreal.DataTableFunctionLibrary.get_data_table_row_names(table)), 'saved': saved}
