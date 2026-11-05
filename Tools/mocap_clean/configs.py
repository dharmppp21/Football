"""Per-move clean-up settings. Times are seconds in the source take. dir = heading in mesh space
(90 = mesh +Y = the player's forward in BP_FootballPlayer; 0 = mesh +X = the player's left).
loop:    search = (from, to, min period, max period) for the best seamless cycle; align = travel | facing.
oneshot: window = (from, to); retime = [(from, to, speed x)]; edits applied before trimming."""
D = '/Game/Football/Animations/Mocap_Clean/'

CONFIGS = {
    # ---- loops (in place)
    'Idle': dict(src='Idle', dst=D + 'AS_Idle_Clean', kind='loop', search=(0.15, 4.15, 2.0, 3.6),
                 align='facing', hz_body=3.0, hz_legs=3.0, lock_all=True, blend=0.5),
    'Walk': dict(src='Walk', dst=D + 'AS_Walk_Clean', kind='loop', search=(2.4, 5.15, 1.25, 1.55), blend=0.3),
    'Jog': dict(src='Run', dst=D + 'AS_Jog_Clean', kind='loop', search=(1.3, 2.88, 0.5, 0.75), blend=0.15, contact_hz=12.0, contact_v=50.0, hz_legs=None, hz_body=10.0),
    'WalkWithBall': dict(src='WalkWithBall', dst=D + 'AS_WalkWithBall_Clean', kind='loop', search=(5.0, 8.4, 0.95, 1.2), blend=0.25),
    'JogWithBall': dict(src='JogWithBall', dst=D + 'AS_JogWithBall_Clean', kind='loop', search=(2.6, 5.5, 0.62, 0.78), blend=0.2),
    'RunWithBall': dict(src='RunWithBall', dst=D + 'AS_RunWithBall_Clean', kind='loop', search=(4.0, 5.42, 0.45, 0.56), blend=0.15,
                        contact_hz=12.0, contact_v=50.0, hz_legs=None, hz_body=10.0),
    'Sidewalk': dict(src='Sidewalk', dst=D + 'AS_Sidewalk_Clean', kind='loop', search=(1.0, 3.05, 0.5, 0.62), blend=0.15,
                     dir=0.0, hz_legs=12.0),
    # ---- one-shots (travel kept in the hips)
    'FrontPassBall': dict(src='FrontPassBall', dst=D + 'AS_FrontPassBall_Clean', kind='oneshot', window=(0.6, 2.35),
                          retime=[(1.23, 1.45, 2.2), (1.45, 1.62, 1.5), (1.62, 2.1, 1.3)]),
    'InsideFootPass': dict(src='InsideFootPass', dst=D + 'AS_InsideFootPass_Clean', kind='oneshot', window=(0.0, 2.25),
                           retime=[(0.80, 0.95, 1.4), (1.2, 1.7, 1.5)],
                           edits=[('twist', 'rightupleg', -60.0, 0.78, 1.2, 0.15, True)]),
    'FrontStopBall': dict(src='FrontStopBall', dst=D + 'AS_FrontStopBall_Clean', kind='oneshot', window=(1.9, 3.75),
                          edits=[('pitch', 'r', 20.0, 2.45, 2.95, 0.15)]),
    'HeelToHeel': dict(src='HeelToHeel', dst=D + 'AS_HeelToHeel_Clean', kind='oneshot', window=(1.6, 3.6)),
    'StepOver': dict(src='StepOver', dst=D + 'AS_StepOver_Clean', kind='oneshot', window=(1.1, 2.75)),
    'Elastico': dict(src='Elastico2', dst=D + 'AS_Elastico_Clean', kind='oneshot', window=(1.6, 2.9)),
    'TurnLeft': dict(src='TurnLeft', dst=D + 'AS_TurnLeft_Clean', kind='oneshot', window=(0.0, 1.1),
                     edits=[('yawscale', 90.0 / 52.8)]),
}

# ---- the loops restrided to the game speeds (the video squashed the strides, see restride in clean.py):
# speed = cm/s at play rate 1; BS_Locomotion plays each at its sample speed / speed
for _name, _base, _speed in (('Walk_100', 'Walk', 87.0), ('Jog_250', 'Jog', 192.0),
                             ('WalkWithBall_100', 'WalkWithBall', 84.0), ('JogWithBall_250', 'JogWithBall', 178.0),
                             ('RunWithBall_430', 'RunWithBall', 384.0)):
    CONFIGS[_name] = dict(CONFIGS[_base], dst=D + 'AS_%s_Clean' % _name, speed=_speed)
# ---- side-on re-shoot 2026-10-21 (60 fps, markers 4.5 m: true speed ~350 cm/s)
CONFIGS['Jog_350'] = dict(src='JogPass1FB', dst=D + 'AS_Jog_350_Clean', kind='loop', search=(0.5, 1.75, 0.68, 0.82), blend=0.12, contact_h=7.0, contact_hz=12.0, contact_v=60.0, hz_legs=None, hz_body=10.0, speed=350.0)
# ---- SprongMotion-14M text-to-motion (Blender add-on on the Footballer rig, 2026-10-24): /Game/Football/Animations/SprongMotion
CONFIGS['SM_Idle'] = dict(src='SM_Idle', dst=D + 'AS_SM_Idle_Clean', kind='loop', search=(0.5, 5.8, 2.0, 3.6),
                          align='facing', hz_body=3.0, hz_legs=3.0, lock_all=True, blend=0.5)
CONFIGS['SM_Walk'] = dict(src='SM_Walk', dst=D + 'AS_SM_Walk_Clean', kind='loop', search=(1.0, 5.8, 0.9, 1.4), blend=0.3)
for _n in ('SM_TurnLeft90', 'SM_TurnRight90', 'SM_TurnAround'):
    CONFIGS[_n] = dict(src=_n, dst=D + 'AS_%s_Clean' % _n, kind='oneshot', window=(0.0, 2.0 if '90' in _n else 2.5), contact_h=8.0)
# ---- Epic Game Animation Sample loops (retargeted by RTG_UEFN_To_Footballer, 30 fps, 2026-10-24): free locomotion
_R = dict(contact_hz=12.0, contact_v=50.0, hz_legs=None, hz_body=10.0)
CONFIGS['GAS_Idle'] = dict(src='GAS_Neutral_Stand_Idle_Loop', dst=D + 'AS_GAS_Idle_Clean', kind='loop', search=(0.3, 9.7, 3.0, 6.0),
                           align='facing', hz_body=3.0, hz_legs=3.0, lock_all=True, blend=0.5)
CONFIGS['GAS_Walk'] = dict(src='GAS_Relaxed_Walk_Loop_F', dst=D + 'AS_GAS_Walk_Clean', kind='loop', search=(0.2, 4.0, 0.9, 1.3), blend=0.2)
CONFIGS['GAS_Jog'] = dict(src='GAS_Relaxed_Run_Loop_F', dst=D + 'AS_GAS_Jog_Clean', kind='loop', search=(0.2, 2.8, 0.6, 0.85), blend=0.12, **_R)
CONFIGS['GAS_Run'] = dict(src='GAS_Neutral_Run_Loop_F', dst=D + 'AS_GAS_Run_Clean', kind='loop', search=(0.2, 2.4, 0.55, 0.8), blend=0.12, **_R)
CONFIGS['GAS_Sprint'] = dict(src='GAS_Neutral_Sprint_Loop_F', dst=D + 'AS_GAS_Sprint_Clean', kind='loop', search=(0.1, 1.95, 0.5, 0.75), blend=0.1, **_R)
