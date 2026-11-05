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

    # Master plan Phase 4: bounded assistance (UBallAssist, spec §14, §70, §105) and the player attributes (§62).
    import math
    A = unreal.BallAssist

    def assist(raw, want, speed=0.0, s=0.15, ang=10.0, dv=100.0, arcade=False):
        i = unreal.BallAssistInput()
        for k, val in dict(raw_velocity=raw, intended_direction=want, intended_speed=speed, strength=s, max_angle_degrees=ang,
                           max_speed_change=dv, arcade=arcade).items():
            i.set_editor_property(k, val)
        return A.bounded_blend(i), i

    def angle(a, b):
        la, lb = a.length(), b.length()
        return 0.0 if la < 1e-6 or lb < 1e-6 else math.degrees(math.acos(max(-1.0, min(1.0, a.dot(b) / (la * lb)))))
    r, _ = assist(V(1000, 0, 0), V(0, 1, 0), 1500.0, s=0.0)
    check('§14 strength 0 = the raw result', near(r.velocity, V(1000, 0, 0), 1e-3) and r.velocity_change.length() < 1e-3)
    r, _ = assist(V(1000, 0, 0), V(0, 1, 0), s=1.0, ang=90.0)
    check('§14 capped at 0.40 without the arcade flag', abs(r.strength_used - 0.4) < 1e-6 and r.strength_capped)
    r, _ = assist(V(1000, 0, 0), V(0, 1, 0), s=1.0, ang=90.0, arcade=True)
    check('§14 an arcade action may go to 1.0', abs(r.strength_used - 1.0) < 1e-6 and angle(r.velocity, V(0, 1, 0)) < 0.01)
    rnd, bad = random.Random(11), []
    for n in range(300):
        raw = V(rnd.uniform(-3000, 3000), rnd.uniform(-3000, 3000), rnd.uniform(-800, 800))
        want = V(rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(-0.3, 0.3))
        speed, ang, dv = rnd.choice((0.0, rnd.uniform(0, 4000))), rnd.uniform(0, 45), rnd.uniform(0, 400)
        prev = None
        for s in (0.0, 0.1, 0.25, 0.4):
            r, _ = assist(raw, want, speed, s, ang, dv)
            err0 = angle(raw, want)
            if r.angle_correction_degrees > ang + 1e-3 or r.angle_correction_degrees > err0 * s + 1e-3 \
                    or abs(r.velocity.length() - raw.length()) > dv + 1e-2 or angle(r.velocity, raw) > ang + 1e-2:
                bad.append(('bounds', n, s))
            if prev is not None and angle(r.velocity, want) > angle(prev.velocity, want) + 1e-3:
                bad.append(('monotonic', n, s))
            prev = r
    check('§14 / §70 bounds never exceeded, more strength never moves away from the intent (300 random)', not bad, str(bad[:3]))
    _, base = assist(V(1000, 0, 0), V(0, 1, 0), s=0.2, ang=10.0, dv=100.0)
    check('§105 no curve = x1', abs(A.scale_for_skill(base, 99.0, None).strength - 0.2) < 1e-6)
    curve = unreal.load_asset('/Game/Football/Data/C_PlayerSkillToAssist')  # 0 -> 0.5, 100 -> 1.5, linear
    check('§105 C_PlayerSkillToAssist exists', curve is not None)
    if curve:
        sc = A.scale_for_skill(base, 75.0, curve)
        check('§105 / §62 skill scales strength and bounds only', abs(sc.strength - 0.25) < 1e-4 and abs(sc.max_angle_degrees - 12.5) < 1e-3
              and abs(sc.max_speed_change - 125.0) < 1e-3 and near(sc.raw_velocity, base.raw_velocity, 1e-6), str((sc.strength, sc.max_angle_degrees)))
        hi = A.scale_for_skill(assist(V(1000, 0, 0), V(0, 1, 0), s=0.35)[1], 100.0, curve)
        check('§14 the 0.40 cap holds after a skill boost', abs(A.bounded_blend(hi).strength_used - 0.4) < 1e-6)
    da = unreal.load_asset('/Game/Football/Data/DA_PlayerAttributes_Default')
    check('§62 DA_PlayerAttributes_Default exists and uses C_PlayerSkillToAssist', da is not None
          and da.get_editor_property('skill_to_assist_scale') == curve)
    if da:
        attrs = da.get_editor_property('attributes')
        names = ['ball_control', 'dribbling', 'agility', 'balance', 'acceleration', 'sprint_speed', 'passing', 'long_passing', 'crossing',
                 'shot_power', 'finishing', 'curve', 'volleys', 'heading', 'reactions', 'composure', 'weak_foot', 'skill_moves',
                 'goalkeeper_handling', 'goalkeeper_diving', 'goalkeeper_reflexes', 'goalkeeper_kicking']
        check('§62 the 22 attributes, 0-100', all(0.0 <= attrs.get_editor_property(n) <= 100.0 for n in names))
        c = da.get_editor_property('skill_to_assist_scale')
        if c:
            check('§105 PlayerSkill -> assist rises (0.5 at 0, 1 at 50, 1.5 at 100)', abs(c.get_float_value(0.0) - 0.5) < 1e-3
                  and abs(c.get_float_value(50.0) - 1.0) < 1e-3 and abs(c.get_float_value(100.0) - 1.5) < 1e-3)

    # ---- master plan Phase 5: the action layer (spec §63-65, §72-73, §119-121) ----
    AC, EV = unreal.FootballActionComponent, unreal.ActionValidation
    act = unreal.new_object(unreal.FootballSkillData)  # transient: StartDistance 150, Priority 40, InputBufferTime 0.12
    act.set_editor_property('contact_descriptors', [unreal.BallContactDescriptor()])
    ctx = unreal.ActionContext()

    def vin(**kw):
        v = unreal.ActionValidationInput()
        v.set_editor_property('ball_valid', True)
        for k, val in kw.items():
            v.set_editor_property(k, val)
        return v

    def val(**kw):
        return AC.validate_action(act, ctx, vin(**kw))
    check('§3 skill defaults: priority 40, buffer 0.12 s, start 150 cm', act.priority == 40 and abs(act.input_buffer_time - 0.12) < 1e-6
          and abs(act.start_distance - 150.0) < 1e-6)
    check('§119 valid', val() == EV.VALID)
    check('§119 state', val(player_can_act=False) == EV.INVALID_STATE)
    check('§119 / §64 busy at equal priority', val(active_priority=40) == EV.BUSY)
    check('§64 a higher priority takes over', val(active_priority=30) == EV.VALID and val(active_priority=30, active_protected=True) == EV.BUSY)
    check('recovery holds the same priority, not a higher one', val(recovering_priority=40) == EV.BUSY and val(recovering_priority=30) == EV.VALID)
    check('§119 cooldown', val(cooldown_left=0.5) == EV.COOLDOWN)
    check('§119 ball', val(ball_valid=False) == EV.NO_BALL and val(ball_state_allows=False) == EV.BALL_STATE)
    check('§119 ownership', val(ball_owned_by_other=True) == EV.NOT_OWNER)
    check('§119 distance', val(ball_distance=151.0) == EV.TOO_FAR and val(ball_distance=150.0) == EV.VALID)
    check('§119 direction', val(has_direction=False) == EV.WRONG_DIRECTION)
    check('§73 reachability', val(reachable=False) == EV.UNREACHABLE)
    check('§119 collision space', val(space_free=False) == EV.NO_SPACE)
    act.set_editor_property('max_speed', 500.0)
    slow_ok, fast = val(player_speed=400.0), val(player_speed=600.0)
    act.set_editor_property('max_speed', 0.0)
    act.set_editor_property('min_speed', 100.0)
    too_slow = val(player_speed=50.0)
    act.set_editor_property('min_speed', 0.0)
    check('§119 speed range', slow_ok == EV.VALID and fast == EV.WRONG_SPEED and too_slow == EV.WRONG_SPEED and val(player_speed=5000.0) == EV.VALID)
    act.set_editor_property('allowed_foot', unreal.FootSelection.RIGHT)
    ctx.set_editor_property('mirrored', True)
    left_on_right = val()
    ctx.set_editor_property('mirrored', False)
    check('§119 foot', left_on_right == EV.WRONG_FOOT and val() == EV.VALID)
    act.set_editor_property('allowed_foot', unreal.FootSelection.BOTH)
    act.set_editor_property('contact_descriptors', [])
    check('§119 no contacts, no ball needed', val(ball_valid=False, ball_distance=900.0) == EV.VALID)
    check('§63 buffer: transient reasons wait, only for the buffer time', AC.should_buffer(EV.BUSY, 0.05, 0.12) and AC.should_buffer(EV.UNREACHABLE, 0.0, 0.12)
          and not AC.should_buffer(EV.BUSY, 0.13, 0.12) and not AC.should_buffer(EV.WRONG_FOOT, 0.0, 0.12)
          and not AC.should_buffer(EV.UNKNOWN_ACTION, 0.0, 0.12) and not AC.should_buffer(EV.CANNOT_PLAY, 0.0, 0.12))
    check('§121 fallback only when the ball is out of reach', AC.wants_fallback(EV.TOO_FAR) and AC.wants_fallback(EV.UNREACHABLE)
          and not AC.wants_fallback(EV.BUSY) and not AC.wants_fallback(EV.WRONG_FOOT))
    prof = unreal.ActionInterruptionProfile()
    reasons = list(unreal.ActionInterruptReason)
    check('§65 every reason by default', len(reasons) == 7 and all(AC.allows_interrupt(prof, r) for r in reasons))
    prof.set_editor_property('allowed_reasons', 1 << unreal.ActionInterruptReason.BALL_LOST.value)
    check('§65 the mask decides', AC.allows_interrupt(prof, unreal.ActionInterruptReason.BALL_LOST)
          and not AC.allows_interrupt(prof, unreal.ActionInterruptReason.INTERRUPTED_BY_TACKLE))
    check('§64 supersedes', AC.supersedes(50, 40, False) and not AC.supersedes(40, 40, False) and not AC.supersedes(50, 40, True))

    def align(ball, offset=V(20, 10, 11), t=0.25, tol=4.0, rot=True, maxpos=30.0, maxrot=20.0, speed=250.0):
        q = unreal.AlignmentQuery()
        q.set_editor_property('predicted_ball', ball)
        q.set_editor_property('predicted_root', V(0, 0, 0))
        q.set_editor_property('pivot', V(0, 0, 0))
        q.set_editor_property('basis', R.make_skill_basis(V(0, 0, 0), V(1, 0, 0), False))
        q.set_editor_property('expected_ball_offset', offset)
        q.set_editor_property('tolerance', tol)
        q.set_editor_property('time_available', t)
        q.set_editor_property('allow_rotation', rot)
        p = unreal.ActionAlignmentProfile()
        for k, v in (('align_player', True), ('max_position_correction', maxpos), ('max_rotation_correction', maxrot), ('max_correction_speed', speed)):
            p.set_editor_property(k, v)
        q.set_editor_property('profile', p)
        return AC.calculate_alignment_target(q)
    a = align(V(20, 10, 11))
    check('§120 ball on the spot: no correction', a.reachable and a.position_correction.length() < 1e-3 and abs(a.rotation_correction) < 1e-3
          and a.residual < 1e-3 and str(a.warp_target_name) == 'Contact0')
    a = align(V(20, 25, 11), rot=False)
    check('§120 sideways move', a.reachable and near(a.position_correction, V(0, 15, 0)) and near(a.location, V(0, 15, 0)), str(a.position_correction))
    a = align(V(20, 60, 11), rot=False)
    check('§120 clamped by distance -> unreachable', not a.reachable and abs(a.position_correction.length() - 30.0) < 1e-3 and abs(a.residual - 20.0) < 1e-3
          and abs(a.required_correction - 50.0) < 1e-3, '%s %s' % (a.position_correction, a.residual))
    a = align(V(20, 40, 11), rot=False, t=0.05)
    check('§120 clamped by speed (do not over-warp)', abs(a.position_correction.length() - 12.5) < 1e-3 and not a.reachable)
    c15, s15 = 0.9659258, 0.2588190
    a = align(V(20 * c15 - 10 * s15, 20 * s15 + 10 * c15, 11))
    check('§120 the turn comes first', a.reachable and abs(a.rotation_correction - 15.0) < 1e-2 and a.position_correction.length() < 1e-2
          and abs(a.rotation.yaw - 15.0) < 1e-2, '%s %s' % (a.rotation_correction, a.position_correction))
    c40, s40 = 0.7660444, 0.6427876
    a = align(V(20 * c40 - 10 * s40, 20 * s40 + 10 * c40, 11))
    check('§120 the turn is clamped, the move does the rest', abs(a.rotation_correction - 20.0) < 1e-2 and a.position_correction.length() > 1.0 and a.reachable)
    a = align(V(20, 10, 40))
    check('§120 height is not warped', not a.reachable and abs(a.residual - 29.0) < 1e-3)
    BM = unreal.BallMovementComponent

    def predict(p0, v0, air, t, decel=81.0, k=1.425e-4):
        out = BM.predict_free_motion(p0, v0, air, 11.0, -980.0, decel, k, t)
        return out if isinstance(out, tuple) else (out, None)

    def euler(v0, decel, k, t, dt=1e-4):
        x, v, s = 0.0, v0, 0.0
        while s < t - 1e-12 and v > 0.0:
            h = min(dt, t - s)
            acc = decel + k * v * v
            if v <= acc * h:
                x += v * v / (2.0 * acc)
                v = 0.0
                break
            x, v, s = x + v * h - 0.5 * acc * h * h, v - acc * h, s + h
        return x, v
    worst = 0.0
    for v0, t in ((340.0, 0.5), (340.0, 3.0), (340.0, 12.0), (900.0, 1.0), (100.0, 2.0), (430.0, 0.2)):
        (p, vel), (x, v) = predict(V(0, 0, 11), V(v0, 0, 0), False, t), euler(v0, 81.0, 1.425e-4, t)
        worst = max(worst, abs(p.x - x), abs(vel.x - v))
    check('§72 rolling closed form = integrated model (cm, cm/s)', worst < 0.05, str(worst))
    p, _ = predict(V(0, 0, 11), V(340, 0, 0), False, 30.0)
    check('§72 FIFA roll test: 3.4 m/s stops after 6.50 m (PIE measured 6.57)', abs(p.x - 650.0) < 3.0, str(p.x))
    p, vel = predict(V(0, 0, 11), V(300, 400, 0), False, 0.3)
    check('§72 rolls along its direction', abs(p.y / p.x - 4.0 / 3.0) < 1e-4 and abs(vel.y / vel.x - 4.0 / 3.0) < 1e-4)
    p, vel = predict(V(0, 0, 111), V(300, 0, 0), True, 0.3)
    check('§72 airborne: ballistic arc', near(p, V(90.0, 0.0, 111.0 - 0.5 * 980.0 * 0.09), 1e-2) and abs(vel.z + 294.0) < 1e-2, str(p))
    land = (2.0 * 100.0 / 980.0) ** 0.5
    p, _ = predict(V(0, 0, 111), V(300, 0, 0), True, land + 0.5)
    roll, _ = euler(300.0, 81.0, 1.425e-4, 0.5)
    check('§72 airborne: lands, then rolls', abs(p.x - (300.0 * land + roll)) < 0.05 and abs(p.z - 11.0) < 1e-3, '%s vs %s' % (p.x, 300.0 * land + roll))
    p, vel = predict(V(5, 5, 11), V(0, 0, 0), False, 1.0)
    check('§72 a still ball stays', near(p, V(5, 5, 11)) and vel.length() < 1e-6)
    el = unreal.load_asset('/Game/Football/Data/Skills/DA_Skill_Elastico')
    check('§3 DA_Skill_Elastico tags + fallback + priority', el is not None and 'Action.Football.Skill.Elastico' in el.get_editor_property('action_tag').export_text()
          and 'Action.Football.Skill' in el.get_editor_property('action_category').export_text()
          and 'Action.Football.Control.Dribble' in el.get_editor_property('fallback_action_tag').export_text() and el.get_editor_property('priority') == 40)
    if el:
        offsets = [d.get_editor_property('expected_ball_offset') for d in el.get_editor_property('contact_descriptors')]
        check('§71 every Elastico contact has its expected ball offset', len(offsets) == 2 and all(o.length() > 1.0 for o in offsets), str(offsets))

    # ---- master plan Phase 6: the converted Elastico (physical touches, entry momentum, rolling after a touch) ----
    RV = unreal.BallPhysicsResponse.rolling_velocity
    check('§6.4 a rolling ball keeps rolling', near(RV(V(250, 40, 0), V(-40 / 11.0, 250 / 11.0, 3.0), 11.0, 0.667), V(250, 40, 0)))
    check('§6.4 a ball sliding without spin keeps 1/(1+k)', near(RV(V(250, 0, -30), V(0, 0, 0), 11.0, 0.667), V(250 / 1.667, 0, 0)))
    check('§6.4 topspin on a still ball rolls it forward k/(1+k)', near(RV(V(0, 0, 0), V(0, 100 / 11.0, 0), 11.0, 0.667), V(100 * 0.667 / 1.667, 0, 0)))
    attrs = unreal.FootballAttributes()
    attrs.set_editor_property('skill_moves', 70.0)
    check('§62 GetAttribute by name (-1 if none)', abs(A.get_attribute(attrs, 'SkillMoves') - 70.0) < 1e-6 and A.get_attribute(attrs, 'NoSuchThing') < 0.0)
    worst = 0.0
    for dist, t in ((60.0, 0.25), (150.0, 0.6), (400.0, 1.5)):
        s = BM.speed_to_roll(dist, t, 81.0, 1.425e-4)
        p, _ = predict(V(0, 0, 11), V(s, 0, 0), False, t)
        worst = max(worst, abs(p.x - dist))
    check('§6.4 SpeedToRoll inverts the roll', worst < 0.05, str(worst))
    s = BM.speed_to_roll(5.0, 1.0, 81.0, 1.425e-4)
    check('§6.4 SpeedToRoll: a short roll stops there', abs(predict(V(0, 0, 11), V(s, 0, 0), False, 5.0)[0].x - 5.0) < 0.05, str(s))
    prof = unreal.ActionAlignmentProfile()
    for k, v in (('align_player', True), ('max_position_correction', 30.0), ('max_rotation_correction', 20.0), ('max_correction_speed', 250.0)):
        prof.set_editor_property(k, v)
    q = unreal.AlignmentQuery()
    for k, v in (('predicted_ball', V(80, 10, 11)), ('entry_offset', V(60, 0, 0)), ('basis', R.make_skill_basis(V(0, 0, 0), V(1, 0, 0), False)),
                 ('expected_ball_offset', V(20, 10, 11)), ('tolerance', 4.0), ('time_available', 0.25), ('allow_rotation', True), ('profile', prof)):
        q.set_editor_property(k, v)
    a = AC.calculate_alignment_target(q)
    check('§120 the entry momentum is not swung by the turn', a.reachable and abs(a.rotation_correction) < 1e-3 and a.position_correction.length() < 1e-3,
          '%s %s' % (a.rotation_correction, a.position_correction))
    tick = unreal.FootballSkillComponent.get_default_object().get_editor_property('primary_component_tick').get_editor_property('tick_group')
    check('§6.1 the skill ticks after physics (one clock with the ball)', tick == unreal.TickingGroup.TG_POST_PHYSICS, str(tick))
    if el:
        ds = el.get_editor_property('contact_descriptors')
        al = el.get_editor_property('alignment_profile')
        check('§6.3 Elastico: physical touch + redirect, bounded assist, aligned on the run', len(ds) == 2
              and [d.get_editor_property('response_mode') for d in ds] == [BRM.PHYSICAL_TOUCH, BRM.PHYSICAL_REDIRECT]
              and all(d.get_editor_property('assist_strength') <= 0.4 for d in ds) and al.align_player and al.carry_entry_velocity
              and el.get_editor_property('min_speed') > 0.0)

    # ---- 2026-10-10: the touch dribble (spec §18-22, §67-70, §78; FIFA's dribble touches, master plan 7.3/7.4 brought forward) ----
    PB, DR = unreal.PlayerBallInteractionComponent, unreal.DribbleTouchReason
    cdo = PB.get_default_object()
    walk, sprint, agile = (cdo.get_editor_property(n) for n in ('walk_control', 'sprint_control', 'agile_control'))

    def touch(ball, ball_vel=V(0, 0, 0), player_vel=V(0, 0, 0), desired=V(0, 0, 0), first=False, since=1.0e6, due=0.0,
              last=DR.NONE, grounded=True, profile=None, pref=None, feet=(0.0, 0.0), sync=None):
        q = unreal.DribbleTouchQuery()
        for k, val in dict(player_location=V(0, 0, 88), player_velocity=player_vel, player_forward=V(1, 0, 0), desired_velocity=desired,
                           ball_location=ball, ball_velocity=ball_vel, ball_grounded=grounded, profile=profile or walk, time_since_touch=since,
                           time_to_planned_touch=due, last_reason=last, first_touch=first, left_foot_side=feet[0], right_foot_side=feet[1]).items():
            q.set_editor_property(k, val)
        if pref is not None:
            q.set_editor_property('preferred_foot', pref)
        if sync is not None:  # in step with the feet: dict(l=window now, r=..., age_l, age_r, stride)
            for k, val in dict(sync_to_feet=True, left_window=sync.get('l', False), right_window=sync.get('r', False),
                               left_window_age=sync.get('age_l', 0.3), right_window_age=sync.get('age_r', 0.0),
                               stride_period=sync.get('stride', 0.6), max_window_wait=0.25, turn_window_wait=sync.get('turn_wait', 0.0)).items():
                q.set_editor_property(k, val)
        return PB.plan_dribble_touch(q)
    # (Python names bInReach 'reach': the plugin drops an 'In' prefix.)
    check('touch dribble is the default', cdo.get_editor_property('touch_dribble'))
    check('§68 / §69 / §20 profiles: agile closest and quickest, sprint farthest and slowest, reach at the feet only',
          agile.touch_distance < walk.touch_distance < sprint.touch_distance and agile.control_distance < walk.control_distance < sprint.control_distance
          and agile.touch_interval < walk.touch_interval < sprint.touch_interval and max(p.touch_reach for p in (walk, sprint, agile)) <= 35.0)
    r = touch(V(100, 0, 11), V(430, 0, 0), V(430, 0, 0), V(430, 0, 0))
    check('§70 a ball out of the foot\'s reach is never touched (no pull)', not r.reach and r.reason == DR.NONE and abs(r.distance - 60.0) < 1e-3, str(r.distance))
    r = touch(V(40, 0, 11), V(0, 300, 0), first=True, grounded=False)
    check('§70 nor is a ball in the air', not r.reach and r.reason == DR.NONE)
    r = touch(V(55, 0, 11), V(430, 0, 0), V(430, 0, 0), V(430, 0, 0), since=0.05)
    check('no two touches closer than MinTouchInterval', r.reach and r.reason == DR.NONE)
    r = touch(V(55, 0, 11), V(430, 0, 0), V(430, 0, 0), V(430, 0, 0), since=0.3, due=-0.1)
    check('§18 between planned touches nothing moves the ball', r.reach and r.reason == DR.NONE)
    r = touch(V(55, 0, 11), V(430, 0, 0), V(430, 0, 0), V(430, 0, 0), since=0.4, due=0.0)
    end, _ = predict(V(55, 0, 11), r.velocity, False, 0.4)
    check('§78 a planned touch sends the ball ControlDistance ahead of where the player will be at the next touch',
          r.reason == DR.PLANNED and abs(r.velocity.y) < 1e-3 and abs(end.x - (430.0 * 0.4 + 55.0)) < 0.5, '%s %s' % (r.velocity, end.x))
    r = touch(V(55, 0, 11), V(430, 0, 0), V(430, 0, 0), V(0, 430, 0), since=0.2, due=-0.2)
    turned = math.degrees(math.atan2(r.velocity.y, r.velocity.x))
    check('§21 a sharp turn: a turn touch, capped, so the ball curves round over more than one touch', r.reason == DR.TURN and r.capped
          and abs(r.correction - walk.max_correction_speed) < 1e-2 and 25.0 < turned < 85.0, '%s %.1f' % (r.velocity, turned))
    r = touch(V(55, 0, 11), V(430, 0, 0), V(430, 0, 0), V(0, 0, 0), since=0.2, due=-0.2)
    rest, _ = predict(V(55, 0, 11), r.velocity, False, 10.0)
    stop = 430.0 ** 2 / (2.0 * 2200.0)
    check('§22 stick let go: one stop touch, the ball comes to rest TouchDistance in front of where the player stops',
          r.reason == DR.STOP and abs(rest.x - (stop + walk.touch_distance)) < 0.5, '%s %.1f' % (r.velocity, rest.x))
    r = touch(V(60, 0, 11), V(60, 0, 0), desired=V(0, 0, 0), since=0.2, last=DR.STOP)
    check('§22 ... and only one', r.reason == DR.NONE)
    r = touch(V(40, 0, 11), desired=V(430, 0, 0))
    check('setting off: a still ball at the feet is pushed ahead', r.reason == DR.START and r.velocity.x > 100.0 and abs(r.velocity.y) < 1e-3, str(r.velocity))
    r = touch(V(20, 0, 11), V(200, 0, 0), V(430, 0, 0), V(430, 0, 0), since=0.2, due=-0.2)
    check('the player running over the ball touches it on', r.reason == DR.OVERRUN and r.velocity.x > 200.0, str(r.velocity))
    r = touch(V(40, 0, 11), V(-600, 0, 0), first=True)
    check('§22 / §127 a first touch on a ball coming at the player takes the pace off it, bounded', r.reason == DR.FIRST_TOUCH and r.capped
          and abs(r.velocity.x - (-600.0 + walk.max_correction_speed)) < 1e-2, str(r.velocity))
    r = touch(V(10, 50, 11), V(300, 0, 0), V(300, 0, 0), V(0, 430, 0), since=0.2, due=-0.2)
    loose = touch(V(10, 50, 11), first=True)
    check('§21 a held ball beside the player can still be turned; a loose one there is not taken', r.reach and r.reason == DR.TURN
          and not loose.reach and loose.reason == DR.NONE)
    r = touch(V(-40, 0, 11), V(0, 0, 0), V(430, 0, 0), V(430, 0, 0), since=0.2, due=0.0)
    heel = touch(V(-20, 0, 11), V(400, 0, 0), V(430, 0, 0), V(430, 0, 0), since=0.2, due=0.0)
    check('a ball left behind the player is out of reach; one at the heels is not', not r.reach and r.reason == DR.NONE
          and heel.reach and heel.reason != DR.NONE)
    # Step 2 (2026-10-20): the foot on the ball's side touches it, the preferred one with the ball in the middle; and the ball
    # is sent on in front of that foot.
    FS = unreal.FootSelection
    left, right = (touch(V(55, y, 11), V(430, 0, 0), V(430, 0, 0), V(430, 0, 0), since=0.4, due=0.0) for y in (-20, 20))
    mid_r = touch(V(55, 0, 11), V(430, 0, 0), V(430, 0, 0), V(430, 0, 0), since=0.4, due=0.0)
    mid_l = touch(V(55, 0, 11), V(430, 0, 0), V(430, 0, 0), V(430, 0, 0), since=0.4, due=0.0, pref=FS.LEFT)
    check('the foot on the ball\'s side touches it; in the middle the preferred foot', left.foot == FS.LEFT and right.foot == FS.RIGHT
          and mid_r.foot == FS.RIGHT and mid_l.foot == FS.LEFT, '%s %s %s %s' % (left.foot, right.foot, mid_r.foot, mid_l.foot))
    side = touch(V(55, 12, 11), V(430, 0, 0), V(430, 0, 0), V(430, 0, 0), since=0.4, due=0.0, feet=(-12.0, 12.0))
    end, _ = predict(V(55, 12, 11), side.velocity, False, 0.4)
    check('the ball is sent on in front of the touching foot', side.foot == FS.RIGHT and side.reason == DR.PLANNED and abs(end.y - 12.0) < 0.5,
          '%s %s' % (side.velocity, end.y))
    # 2026-10-24: touches in step with the feet - a planned touch at the touching foot's window (late swing, the run loops'
    # Dribble Foot Window notify), once a stride, sending the ball to where that foot will be a stride later; no window, no
    # planned touch until MaxWindowWait late; any touch plans to the touching foot's next usable window.
    run = dict(ball=V(55, 0, 11), ball_vel=V(430, 0, 0), player_vel=V(430, 0, 0), desired=V(430, 0, 0))
    wait = touch(since=0.5, due=0.1, sync=dict(), **run)
    at_win = touch(since=0.6, due=0.0, sync=dict(r=True), **run)
    other = touch(since=0.6, due=0.0, sync=dict(l=True, age_l=0.0, age_r=0.3), **run)
    twice = touch(since=0.2, due=-0.4, sync=dict(r=True), **run)
    late = touch(since=1.2, due=0.6, sync=dict(age_r=0.25), **run)
    waiting = touch(since=0.85, due=0.3, sync=dict(age_r=0.25), **run)
    end, _ = predict(V(55, 0, 11), at_win.velocity, False, 0.6)
    check('in step with the feet: the planned touch waits for the touching foot\'s window (not the other foot\'s, not twice a stride) ...',
          wait.reason == DR.NONE and other.reason == DR.NONE and twice.reason == DR.NONE and at_win.reason == DR.PLANNED,
          '%s %s %s %s' % (wait.reason, other.reason, twice.reason, at_win.reason))
    check('... and sends the ball ControlDistance ahead of where the player will be at that foot\'s next window (a stride on)',
          abs(at_win.interval - 0.6) < 1e-4 and abs(end.x - (430.0 * 0.6 + 55.0)) < 0.5, '%s %s' % (at_win.interval, end.x))
    check('... and never waits for it longer than a stride: a stride late the planned touch is made without the window, before that not',
          late.reason == DR.PLANNED and waiting.reason == DR.NONE, '%s %s' % (late.reason, waiting.reason))
    turn = touch(V(55, 0, 11), V(430, 0, 0), V(430, 0, 0), V(0, 430, 0), since=0.2, due=-0.2, sync=dict(age_r=0.45))
    turn2 = touch(V(55, 0, 11), V(430, 0, 0), V(430, 0, 0), V(0, 430, 0), since=0.2, due=-0.2, sync=dict(age_r=0.1))
    check('... a touch between windows (a turn) plans to the foot\'s next usable window: 0.15 s away is too soon (half a stride), so 0.75 s; 0.5 s away is kept',
          turn.reason == DR.TURN and abs(turn.interval - 0.75) < 1e-4 and abs(turn2.interval - 0.5) < 1e-4, '%s %s' % (turn.interval, turn2.interval))
    tq = dict(ball=V(55, 0, 11), ball_vel=V(430, 0, 0), player_vel=V(430, 0, 0), desired=V(0, 430, 0), since=0.2, due=-0.2, feet=(-12.0, 12.0))
    soon = touch(sync=dict(age_r=0.55, age_l=0.25, turn_wait=0.1), **tq)       # the right foot comes through in 0.05 s
    now = touch(sync=dict(age_r=0.55, age_l=0.25, turn_wait=0.0), **tq)
    by_left = touch(sync=dict(l=True, age_l=0.0, age_r=0.3, turn_wait=0.1), **tq)
    none_near = touch(sync=dict(age_r=0.2, age_l=0.45, turn_wait=0.1), **tq)  # the next windows 0.4 / 0.15 s away
    check('in step with the feet a turn touch waits for a foot coming through within TurnWindowWait (0 = at once), is made by '
          'the foot whose window it is (in a turn the ball changes sides), and is made at once with no foot that close',
          soon.reason == DR.NONE and now.reason == DR.TURN and by_left.reason == DR.TURN and by_left.foot == FS.LEFT
          and none_near.reason == DR.TURN, '%s %s %s %s %s' % (soon.reason, now.reason, by_left.reason, by_left.foot, none_near.reason))
    # 2026-10-20 turn fix: a nearly still player reaches a ball close behind him (a drag-back), a moving one doesn't.
    slow = touch(V(-40, 0, 11), V(0, 0, 0), V(0, 0, 0), V(-430, 0, 0), since=0.2)
    check('a still player reaches a ball at his heels 40 cm behind and sets off with it', slow.reach and slow.reason == DR.START, str(slow.reason))
    # Step 4 (2026-10-20): which foot kicks, and what the weak foot costs.
    ck = lambda ball, aim, weak=50.0: PB.choose_kick_foot(V(0, 0, 88), V(1, 0, 0), ball, aim, FS.RIGHT, weak)
    a, b, c, d = ck(V(40, 0, 11), V(1, 0, 0)), ck(V(40, -20, 11), V(1, 0, 0)), ck(V(40, 0, 11), V(1, 1.7, 0)), ck(V(40, -20, 11), V(1, 0, 0), 0.0)
    check('kick foot: the strong foot straight on; the left foot for a ball 20 cm to the left or a kick 60 deg out to the right; a 0 weak foot keeps the strong one',
          a.foot == FS.RIGHT and not a.weak_foot and b.foot == FS.LEFT and c.foot == FS.LEFT and d.foot == FS.RIGHT, '%s %s %s %s' % (a.foot, b.foot, c.foot, d.foot))
    check('the weak foot (WeakFoot 50) loses 7.5% speed and aims up to 4 deg off; the strong foot loses nothing',
          b.weak_foot and abs(b.speed_scale - 0.925) < 1e-3 and abs(b.error_deg - 4.0) < 1e-3 and a.speed_scale == 1.0 and a.error_deg == 0.0,
          '%s %s' % (b.speed_scale, b.error_deg))
    r = touch(V(50, 0, 11), V(430, 0, 0), V(430, 0, 0), V(150, 0, 0), since=0.2, due=-0.25)
    check('the player slows: the ball would run out of reach before the next touch, so it is corrected now', r.reason == DR.CORRECT
          and r.velocity.x < 430.0, str(r.velocity))
    r = touch(V(64, 0, 11), V(420, 0, 0), V(430, 0, 0), V(250, 0, 0), since=0.25, due=-0.2, profile=agile)
    check('RB pressed with the ball at walking distance: still within the legs\' reach (the same in every profile), corrected closer',
          r.reach and r.reason == DR.CORRECT and r.velocity.x < 420.0, str(r.velocity))
    rnd, bad = random.Random(17), []
    for n in range(400):
        prof = rnd.choice((walk, sprint, agile))
        ang = rnd.uniform(0, 2 * math.pi)
        rad = rnd.uniform(0, prof.touch_reach)
        ball = V(prof.touch_distance + rad * math.cos(ang), rad * math.sin(ang), 11)
        bv, pv = V(rnd.uniform(-900, 900), rnd.uniform(-900, 900), 0), V(rnd.uniform(-660, 660), rnd.uniform(-660, 660), 0)
        dv = rnd.choice((V(0, 0, 0), V(rnd.uniform(-660, 660), rnd.uniform(-660, 660), 0)))
        r = touch(ball, bv, pv, dv, first=rnd.random() < 0.3, since=rnd.uniform(0, 1), due=rnd.uniform(-0.5, 0.5), profile=prof)
        if r.reason != DR.NONE and (V(r.velocity.x - bv.x, r.velocity.y - bv.y, 0).length() > prof.max_correction_speed + 1e-2
                                    or abs(r.velocity.z - bv.z) > 1e-6):
            bad.append(n)
        far = touch(V(prof.touch_distance + prof.touch_reach + 1.0 + rnd.uniform(0, 300), 0, 11), bv, pv, dv, first=True, profile=prof)
        if far.reason != DR.NONE:
            bad.append(('far', n))
    check('§70 every touch is bounded and keeps the vertical velocity; nothing out of reach is ever touched (400 random)', not bad, str(bad[:4]))
    D = PB.predict_player_displacement
    check('player prediction: steady, setting off, stopping', near(D(V(430, 0, 0), V(430, 0, 0), 1800.0, 2200.0, 0.4), V(172, 0, 0), 1e-3)
          and abs(D(V(0, 0, 0), V(430, 0, 0), 1800.0, 2200.0, 0.4).x - (430.0 * 0.5 * 430.0 / 1800.0 + 430.0 * (0.4 - 430.0 / 1800.0))) < 1e-3
          and abs(D(V(430, 0, 0), V(0, 0, 0), 1800.0, 2200.0, 5.0).x - stop) < 1e-3)
    mid = PB.blend_control_profiles(walk, sprint, 0.5)
    check('profile blend', abs(mid.control_distance - 0.5 * (walk.control_distance + sprint.control_distance)) < 1e-3
          and abs(mid.touch_interval - 0.5 * (walk.touch_interval + sprint.touch_interval)) < 1e-4)

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
