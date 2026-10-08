"""Self-test of the contact framework (Phase 2): pure functions and data, no PIE.

Use from util.execute_python:
    g = {}; exec(open(r'<project>/Tools/framework_selftest.py').read(), g); print(g['run']())
Returns {'passed': n, 'failed': [messages]}; an empty 'failed' list is the gate.
"""
import json
import unreal

R = unreal.BallContactResolver
V = unreal.Vector
TABLE = '/Game/Football/Data/DT_BodyRegions'
MESH = '/Game/Football/Models/Footballer/Footballer_latest'

_results = {'passed': 0, 'failed': []}


def check(name, ok, detail=''):
    if ok:
        _results['passed'] += 1
    else:
        _results['failed'].append('%s %s' % (name, detail))


def near(a, b, tol=0.01):
    return abs(a.x - b.x) <= tol and abs(a.y - b.y) <= tol and abs(a.z - b.z) <= tol


def query(ball, body=V(0, 0, 0), normal=V(1, 0, 0), ball_region=unreal.BallRegion.BACK, ball_vel=V(0, 0, 0), body_vel=V(0, 0, 0),
          window=True, contact_type=unreal.ContactType.TOUCH, min_speed=0.0, player_vel=V(0, 0, 0), **desc):
    d = unreal.BallContactDescriptor()
    d.set_editor_property('ball_contact_region', ball_region)
    d.set_editor_property('contact_type', contact_type)
    d.set_editor_property('min_body_speed', min_speed)
    for k, v in desc.items():
        d.set_editor_property(k, v)
    q = unreal.ContactQuery()
    q.set_editor_property('ball_center', ball)
    q.set_editor_property('ball_velocity', ball_vel)
    q.set_editor_property('ball_radius', 11.0)
    q.set_editor_property('body_point', body)
    q.set_editor_property('body_normal', normal)
    q.set_editor_property('body_velocity', body_vel)
    q.set_editor_property('player_velocity', player_vel)
    q.set_editor_property('descriptor', d)
    q.set_editor_property('basis', R.make_skill_basis(V(0, 0, 0), V(1, 0, 0), False))
    q.set_editor_property('window_open', window)
    return R.resolve_contact(q)


def run():
    _results['passed'], _results['failed'] = 0, []
    ball_regions, body_regions = list(unreal.BallRegion), list(unreal.BodyRegion)
    check('enum sizes', (len(ball_regions), len(body_regions), len(list(unreal.ContactType))) == (19, 42, 10))

    # Skill basis: facing +Y -> right is -X; mirroring flips right.
    b = R.make_skill_basis(V(0, 0, 0), V(0, 2, 0.5), False)
    check('basis forward', near(b.forward, V(0, 1, 0)))
    check('basis right', near(b.right, V(-1, 0, 0)))
    bm = R.make_skill_basis(V(0, 0, 0), V(1, 0, 0), True)
    check('mirrored basis', near(R.skill_to_world(bm, V(0, 1, 0)), V(0, -1, 0)))

    # Ball regions (doc §3).
    bx = R.make_skill_basis(V(0, 0, 0), V(1, 0, 0), False)
    for region in ball_regions:
        d = R.region_direction(region, bx)
        if region == unreal.BallRegion.CENTER:
            check('center is zero', near(d, V(0, 0, 0)))
        else:
            check('unit %s' % region.name, abs(d.length() - 1.0) < 1e-4)
        check('ball mirror twice %s' % region.name, R.mirror_ball_region(R.mirror_ball_region(region)) == region)
    s = 0.70710678
    check('front right', near(R.region_direction(unreal.BallRegion.FRONT_RIGHT, bx), V(s, s, 0)))
    check('bottom back', near(R.region_direction(unreal.BallRegion.BOTTOM_BACK, bx), V(-s, 0, -s)))
    check('surface point', near(R.ball_surface_point(V(10, 0, 11), 11.0, unreal.BallRegion.TOP, bx), V(10, 0, 22)))
    check('mirror front left', R.mirror_ball_region(unreal.BallRegion.FRONT_LEFT) == unreal.BallRegion.FRONT_RIGHT)
    check('mirror top', R.mirror_ball_region(unreal.BallRegion.TOP) == unreal.BallRegion.TOP)

    # Body regions: sides swap, the midline maps to itself, feet are foot regions.
    feet = 0
    for region in body_regions:
        m = R.mirror_body_region(region)
        check('body mirror twice %s' % region.name, R.mirror_body_region(m) == region)
        sided = 'LEFT' in region.name or 'RIGHT' in region.name
        check('body mirror side %s' % region.name, (m != region) == sided)
        feet += 1 if R.is_foot_region(region) else 0
    check('foot region count', feet == 22, 'got %d' % feet)
    check('mirror heel inside', R.mirror_body_region(unreal.BodyRegion.LEFT_HEEL_INSIDE) == unreal.BodyRegion.RIGHT_HEEL_INSIDE)
    check('mirror chest left', R.mirror_body_region(unreal.BodyRegion.CHEST_LEFT) == unreal.BodyRegion.CHEST_RIGHT)

    # The five contact tests (doc §7). Body at the origin facing +X; ball region Back touches it from +X.
    r = query(V(11, 0, 0))
    check('T valid', r.validation == unreal.ContactValidation.VALID, str(r.validation))
    check('T target', near(r.target_ball_center, V(11, 0, 0)) and r.distance < 0.01)
    check('T1 too far', query(V(30, 0, 0)).validation == unreal.ContactValidation.TOO_FAR)
    check('T3 window', query(V(11, 0, 0), window=False).validation == unreal.ContactValidation.OUTSIDE_WINDOW)
    check('T4 wrong side', query(V(-11, 0, 0), ball_region=unreal.BallRegion.FRONT).validation == unreal.ContactValidation.WRONG_REGION)
    check('T2 moving away', query(V(11, 0, 0), body_vel=V(-100, 0, 0)).validation == unreal.ContactValidation.WRONG_DIRECTION)
    check('T2 ball incoming ok', query(V(11, 0, 0), ball_vel=V(-300, 0, 0)).validation == unreal.ContactValidation.VALID)
    check('T5 too slow', query(V(11, 0, 0), body_vel=V(100, 0, 0), min_speed=200.0).validation == unreal.ContactValidation.TOO_SLOW)
    rel = query(V(11, 0, 0), contact_type=unreal.ContactType.RELEASE, preferred_speed=300.0, vertical_bias=0.2, player_vel=V(0, 100, 0))
    check('release response', near(rel.response_velocity, V(300, 100, 60)), str(rel.response_velocity))
    check('touch has no response', near(query(V(11, 0, 0)).response_velocity, V(0, 0, 0)))

    # Exit velocity in a mirrored basis (doc §20).
    d = unreal.BallContactDescriptor()
    d.set_editor_property('preferred_direction', V(0, 1, 0))
    d.set_editor_property('preferred_speed', 200.0)
    d.set_editor_property('inherit_player_velocity', 0.0)
    check('mirrored exit', near(R.compute_exit_velocity(d, bm, V(0, 0, 0), V(0, 0, 0)), V(0, -200, 0)))

    # Deflection (doc §19). Body surface normal +X; the ball comes in along -X.
    out, _ = R.compute_deflection(V(-500, 0, 0), V(0, 0, 0), V(1, 0, 0), 0.5, 0.0, V(0, 0, 0), 0.0, 11.0)
    check('deflect head-on', near(out, V(250, 0, 0)), str(out))
    out, _ = R.compute_deflection(V(500, 0, 0), V(0, 0, 0), V(1, 0, 0), 0.5, 0.0, V(0, 0, 0), 0.0, 11.0)
    check('deflect moving away unchanged', near(out, V(500, 0, 0)))
    out, _ = R.compute_deflection(V(-400, 0, 0), V(100, 0, 0), V(1, 0, 0), 0.5, 0.0, V(0, 0, 0), 0.0, 11.0)
    check('deflect moving body', near(out, V(350, 0, 0)), str(out))
    out, spin = R.compute_deflection(V(-500, 200, 0), V(0, 0, 0), V(1, 0, 0), 0.5, 0.2, V(0, 0, 0), 1.0, 11.0)
    check('deflect slide damping', near(out, V(250, 160, 0)), str(out))
    check('deflect spin', near(spin, V(0, 0, 200 / 11.0 * 57.29578), 0.1), str(spin))

    # Master plan Phase 2: the §13 descriptor defaults, the §7 velocity split, the §11 sweep and the §99/§100 tracker.
    d0 = unreal.BallContactDescriptor()
    check('enum response modes', len(list(unreal.BallResponseMode)) == 7)
    check('descriptor defaults', d0.assist_strength <= 0.25 and d0.process_once and not d0.allow_recontact
          and d0.response_mode == unreal.BallResponseMode.PHYSICAL_TOUCH and d0.restitution < 0.0, str(d0.assist_strength))
    r = query(V(11, 0, 0), body_vel=V(100, 0, 0), ball_vel=V(-50, 20, 0))
    check('7 relative velocity', near(r.relative_velocity, V(150, -20, 0)) and abs(r.normal_speed - 150.0) < 0.01
          and near(r.tangent_velocity, V(0, -20, 0)), '%s %s' % (r.relative_velocity, r.normal_speed))
    dt = 1.0 / 60.0
    # A foot crossing the ball between two frames (at 30 m/s it moves 50 cm a frame): caught, touching from 8% of the path.
    gap, alpha, point = R.sweep_region_to_ball(V(0, 30, 0), V(0, 3000, 0), 5.0, V(0, 0, 0), V(0, 0, 0), 11.0, dt)
    check('11 crossing caught', gap < -15.9 and abs(alpha - 0.08) < 1e-3 and near(point, V(0, -11, 0)), '%s %s %s' % (gap, alpha, point))
    gap, _, _ = R.sweep_region_to_ball(V(0, 30, 0), V(0, 3000, 0), 5.0, V(0, 0, 0), V(0, 0, 0), 11.0, 0.0)
    check('11 point test misses it', gap > 13.9, str(gap))
    gap, alpha, _ = R.sweep_region_to_ball(V(0, 30, 0), V(0, 600, 0), 5.0, V(0, 0, 0), V(0, 0, 0), 11.0, dt)
    check('11 moving away', abs(gap - 4.0) < 1e-3 and alpha == 0.0, '%s %s' % (gap, alpha))
    gap, _, _ = R.sweep_region_to_ball(V(15.9, 30, 0), V(0, 3000, 0), 5.0, V(0, 0, 0), V(0, 0, 0), 11.0, dt)
    check('11 grazing touch', -0.11 < gap < 0.0, str(gap))
    gap, _, _ = R.sweep_region_to_ball(V(16.1, 30, 0), V(0, 3000, 0), 5.0, V(0, 0, 0), V(0, 0, 0), 11.0, dt)
    check('11 grazing miss', 0.0 < gap < 0.11, str(gap))
    # A still foot and a ball rolling into it at 12 m/s: the touch starts at 70% of the frame, the ball then at x = 4.
    gap, alpha, point = R.sweep_region_to_ball(V(20, 0, 0), V(0, 0, 0), 5.0, V(10, 0, 0), V(1200, 0, 0), 11.0, dt)
    check('11 moving ball', abs(gap + 6.0) < 1e-3 and abs(alpha - 0.7) < 1e-3 and near(point, V(15, 0, 0)), '%s %s %s' % (gap, alpha, point))

    def tracker(fn, t, *args):  # the ref parameter comes back as the return value
        out = fn(t, *args)
        return t if out is None else out

    rf, lf = unreal.BodyRegion.RIGHT_FOOT_INSIDE, unreal.BodyRegion.LEFT_FOOT_INSIDE
    t = tracker(R.track_region, unreal.ContactTracker(), rf, V(0, 0, 0), V(1, 0, 0), dt)
    check('10 first tick no velocity', t.tracking and near(t.velocity, V(0, 0, 0)) and near(t.prev_point, V(0, 0, 0)))
    t = tracker(R.track_region, t, rf, V(1, 0, 0), V(1, 0, 0), dt)
    check('10 same region velocity', near(t.velocity, V(60, 0, 0)) and near(t.prev_point, V(0, 0, 0)), str(t.velocity))
    t = tracker(R.track_region, t, lf, V(5, 0, 0), V(1, 0, 0), dt)
    check('10 new region restarts', near(t.velocity, V(0, 0, 0)) and near(t.prev_point, V(5, 0, 0)))
    t = tracker(R.track_region, tracker(R.stop_tracking, t), lf, V(6, 0, 0), V(1, 0, 0), dt)
    check('10 after stop restarts', near(t.velocity, V(0, 0, 0)))

    once = unreal.BallContactDescriptor()
    t = tracker(R.open_contact_window, unreal.ContactTracker())
    check('99 armed', R.can_process_contact(t, once))
    t = tracker(R.mark_contact_processed, tracker(R.update_contact_state, t, -1.0, dt, once))
    check('99 once per window', not R.can_process_contact(t, once))
    t = tracker(R.open_contact_window, t)  # next window, region still on the ball
    check('99 waits for separation', t.wait_separation and not R.can_process_contact(t, once))
    t = tracker(R.update_contact_state, t, 2.0, 0.03, once)
    check('99 short gap is no separation', not R.can_process_contact(t, once))
    t = tracker(R.update_contact_state, t, 2.0, 0.03, once)
    check('99 separation re-arms', R.can_process_contact(t, once) and not t.wait_separation)

    again = unreal.BallContactDescriptor()
    again.set_editor_property('allow_recontact', True)
    for desc, rearmed in ((again, True), (once, False)):
        t = tracker(R.open_contact_window, unreal.ContactTracker())
        t = tracker(R.mark_contact_processed, tracker(R.update_contact_state, t, -1.0, dt, desc))
        t = tracker(R.update_contact_state, tracker(R.update_contact_state, t, 3.0, 0.1, desc), -0.5, dt, desc)
        check('100 recontact allow=%s' % rearmed, t.recontacts == 1 and R.can_process_contact(t, desc) == rearmed, str(t.recontacts))

    hold = unreal.BallContactDescriptor()
    hold.set_editor_property('process_once', False)
    t = tracker(R.update_contact_state, tracker(R.open_contact_window, unreal.ContactTracker()), -0.2, dt, hold)
    check('100 continuous while touching', R.can_process_contact(t, hold))
    t = tracker(R.update_contact_state, t, 0.5, dt, hold)
    check('100 continuous stops off the ball', not R.can_process_contact(t, hold))

    # Master plan Phase 3: the raw physical response (§7, §91, §122), the response authority (§145, §147), the states (§4, §128).
    M, BRM = 0.43, unreal.BallResponseMode

    def respond(v, w=V(0, 0, 0), body=V(0, 0, 0), e=0.5, mu=0.5, s=1.0, mode=BRM.PHYSICAL_TOUCH):
        i = unreal.BallResponseInput()  # body on the ball's -X side, its normal +X
        for k, val in dict(mode=mode, ball_velocity=v, ball_angular_velocity=w, contact_point=V(-11, 0, 0), contact_normal=V(1, 0, 0),
                           body_velocity=body, restitution=e, friction=mu, contact_strength=s).items():
            i.set_editor_property(k, val)
        return unreal.BallPhysicsResponse.compute_response(i)

    r = respond(V(-500, 0, 0))
    check('122 head-on bounce', r.contact and near(r.velocity_after, V(250, 0, 0)) and abs(r.normal_impulse - 750 * M) < 0.01, str(r.velocity_after))
    r = respond(V(-500, 400, 0), mu=0.3)  # the shell needs 160 M of grip; mu 0.3 x the 750 M normal impulse allows 225 M
    slip = r.velocity_after + r.angular_velocity_after.cross(V(-11, 0, 0))
    k = unreal.BallResponseInput().get_editor_property('inertia_factor')  # 0.667, a thin shell (master plan U.2)
    check('122 grip stops the slip', not r.sliding and abs(slip.y) < 0.01 and abs(r.velocity_after.y - 400.0 / (1.0 + k)) < 0.01, str(r.velocity_after))
    r = respond(V(-500, 400, 0), mu=0.05)
    check('122 friction cap slides', r.sliding and abs(r.velocity_after.y - (400 - 0.05 * 750)) < 0.01, str(r.velocity_after))
    r = respond(V(0, 0, 0), body=V(1000, 300, 0))
    check('91 spin from the offset', r.contact and r.angular_velocity_after.z < -1.0
          and near(r.angular_impulse, V(-11, 0, 0).cross(r.linear_impulse), 0.001), str(r.angular_velocity_after))
    check('122 separating: no impulse', not respond(V(500, 0, 0)).contact)
    check('122 half strength, half impulse', abs(respond(V(-500, 0, 0), s=0.5).normal_impulse - 375 * M) < 0.01)
    check('127 / 128 not impulses', not respond(V(-500, 0, 0), mode=BRM.CONTROLLED_CONTACT).contact and not respond(V(-500, 0, 0), mode=BRM.CAPTURE).contact)
    import random
    rnd, worst = random.Random(7), 0.0

    def energy(v, w):
        return 0.5 * M * v.length() ** 2 + 0.5 * k * M * 121.0 * w.length() ** 2
    for _ in range(200):
        v = V(-rnd.uniform(10, 3000), rnd.uniform(-1500, 1500), rnd.uniform(-500, 500))
        w = V(rnd.uniform(-60, 60), rnd.uniform(-60, 60), rnd.uniform(-60, 60))
        r = respond(v, w, e=rnd.uniform(0, 0.3), mu=rnd.uniform(0, 1), mode=BRM.ENERGY_ABSORPTION)
        worst = max(worst, energy(r.velocity_after, r.angular_velocity_after) - energy(v, w))
    check('126 absorption never adds energy', worst <= 0.5, str(worst))

    P = unreal.ContactPriority
    order = [P.ENVIRONMENT, P.BODY, P.FOOT, P.DRIBBLE, P.RECEIVE, P.SKILL, P.CAPTURE]
    check('145 authority order', [p.value for p in order] == sorted(set(p.value for p in order)))

    def command(source, priority):
        c = unreal.BallResponseCommand()
        c.set_editor_property('source', source)
        c.set_editor_property('priority', priority)
        return c
    pick = unreal.BallInteractionComponent.pick_winning_command
    # On a tie the later command wins: it was worked out from the earlier one's result (master plan 3.5).
    check('147 the strike beats the dribble, the later of a tie wins', pick([command('Dribble', P.DRIBBLE), command('Kick', P.SKILL),
                                                                            command('Body', P.BODY), command('Skill', P.SKILL)]) == 3)
    check('147 nothing queued', pick([]) == -1)
    S, B = unreal.BallStateComponent, unreal.BallState
    check('128 caught only through Receiving', S.is_transition_allowed(B.RECEIVING, B.GOALKEEPER_CONTROLLED)
          and not S.is_transition_allowed(B.FREE, B.GOALKEEPER_CONTROLLED))
    check('4 a keeper\'s ball', not S.is_transition_allowed(B.GOALKEEPER_CONTROLLED, B.CONTROLLED)
          and not S.is_transition_allowed(B.GOALKEEPER_CONTROLLED, B.RECEIVING) and S.is_transition_allowed(B.GOALKEEPER_CONTROLLED, B.PASSING))

    # Master plan U1 / U2: the ball's air forces (UBallMovementComponent::ComputeAeroForce). U4 (knuckle) was removed.
    ball = unreal.get_default_object(unreal.BallMovementComponent)
    q = 2.28e-4  # 1/2 rho A, kg/cm

    def cd_at(speed, sp=0.0, airborne=True):  # sp: spin across the travel, r w / v
        return -ball.compute_aero_force(V(speed, 0, 0), V(0, 0, sp * speed / 11.0 * 57.29578), airborne).x / (q * speed * speed)
    check('U1 drag below the crisis (8 m/s)', abs(cd_at(800.0) - 0.4477) < 0.002, str(cd_at(800.0)))
    check('U1 drag above the crisis (25 m/s)', abs(cd_at(2500.0) - 0.187) < 0.002, str(cd_at(2500.0)))
    check('U1 the crisis between 11 and 15 m/s', cd_at(1100.0) > 0.4 and cd_at(1500.0) < 0.25)
    check('U1 spin adds drag in the air, up to Sp 0.4', abs(cd_at(2500.0, 0.2) - 0.287) < 0.002 and abs(cd_at(2500.0, 1.0) - 0.387) < 0.002)
    check('U1 no spin drag on the ground (a rolling ball has Sp 1)', abs(cd_at(2500.0, 1.0, False) - 0.187) < 0.002)

    def lift_at(sp, airborne=True):
        f = ball.compute_aero_force(V(2500, 0, 0), V(0, 0, sp * 2500.0 / 11.0 * 57.29578), airborne)
        return f.y / (q * 2500.0 ** 2), f
    cl, f = lift_at(0.1)
    check('U2 Magnus at Sp 0.1, toward spin x velocity', abs(cl - 0.32 * (1 - 2.71828 ** (-0.1 / 0.13))) < 0.002 and abs(f.z) < 1e-6, str(cl))
    check('U2 Magnus levels off', abs(lift_at(0.3)[0] - 0.288) < 0.002 and abs(lift_at(1.0)[0] - 0.32) < 0.002)
    check('U2 no Magnus on the ground', abs(lift_at(0.3, False)[1].y) < 1e-6)
    check('U2 rifle spin lifts nothing', abs(ball.compute_aero_force(V(2500, 0, 0), V(3000, 0, 0), True).y) < 1e-6)
    check('U4 removed: a spinless fast ball feels only drag', abs(ball.compute_aero_force(V(2500, 0, 0), V(0, 0, 0), True).y) < 1e-6
          and not hasattr(ball, 'compute_knuckle_force'))

    # Region table vs the enum and the mesh.
    table = unreal.load_asset(TABLE)
    mesh = unreal.load_asset(MESH)
    check('table exists', table is not None)
    if table:
        names = [str(n) for n in unreal.DataTableFunctionLibrary.get_data_table_row_names(table)]
        wanted = {r.name.replace('_', '').lower() for r in body_regions}
        check('table covers every region', {n.lower() for n in names} == wanted, str(set(n.lower() for n in names) ^ wanted))
        bones = {str(n) for n in json_bones(mesh)}
        rows = json.loads(unreal.DataTableFunctionLibrary.export_data_table_to_json_string(table))
        for row in rows:
            name = row['Name']
            if row['Socket'] != 'None':
                check('socket %s' % name, mesh.find_socket(row['Socket']) is not None)
            else:
                check('bone %s' % name, row['Bone'] in bones, row['Bone'])
                n = row['LocalNormal']
                check('normal %s' % name, abs((n['X'] ** 2 + n['Y'] ** 2 + n['Z'] ** 2) ** 0.5 - 1.0) < 1e-3)
            check('response %s' % name, 0.0 <= row['Restitution'] <= 1.0 and 0.0 <= row['TangentDamping'] <= 1.0)
    return dict(_results)


def json_bones(mesh):
    data = json.loads(unreal.MCPythonHelper.get_skeleton_bones(mesh))
    bones = data if isinstance(data, list) else data.get('bones', [])
    return [b['name'] if isinstance(b, dict) else b for b in bones]
