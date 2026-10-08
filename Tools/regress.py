"""Lean regression runner (master plan v2, section T).

One call runs the chosen suites back to back in one PIE session at a fixed frame rate, compares each with its baseline
(BASELINES) and writes Saved/Regress/<label>.json. The runner starts and stops PIE itself (the same request MCP's
start_pie / stop_pie make), so PIE must not be running when it starts.

Use from util.execute_python:
    import importlib.util, sys
    spec = importlib.util.spec_from_file_location('regress', r'<project>/Tools/regress.py')
    m = importlib.util.module_from_spec(spec); sys.modules['regress'] = m; spec.loader.exec_module(m)
    print(m.run('p2gate', ['parity', 'skills']))            # 'started' (fps=60 by default; 30 / 120 where T4 says so)
    ... once Saved/Regress/p2gate.json exists: print(json.dumps(sys.modules['regress'].summary('p2gate')))

Suites: selftest (no PIE, run first), parity, kicks, deflections, skills, reset. QUICK = parity + reset.
Parity and kicks must match the baseline (fixed-rate runs repeat to ~0.01 cm). Skills allow ~2.5x the run-to-run
noise of the touches measured between phase1_60 and phase1post_60 (the idle animation's phase at the request
varies), and judge the facing / mirror path checks on an absolute bound. Deflections judge only what that pose can't
change: every throw hits the body, and the applied deflection matches the formula.
"""
import importlib.util
import json
import os
import sys
import time
import unreal

TOOLS = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), 'Regress')
RATES = {30: 30.0, 60: 59.9988, 120: 120.0}  # 59.9988 = the two physics substeps baseline60 was recorded with
# Recorded 2026-10-08 after master plan U5 (knuckle removed, grass spin friction, kicks set their topspin / backspin,
# the shoot's backspin, restitution 0.66 for the FIFA rebound test) with run('ubase2NN', ..., NN). Before: ubaseNN (U1-U4),
# p3baseNN (after 3.1), baselineNN, phase1kick_NN, phase1defl_NN, phase1post_NN.
BASELINES = {'parity': 'ubase2{fps}_parity{fps}', 'kicks': 'ubase2{fps}_kicks{fps}', 'deflections': 'ubase2{fps}_deflections{fps}',
             'skills': 'ubase2{fps}_skills{fps}'}
ALL = ['selftest', 'parity', 'kicks', 'deflections', 'skills', 'reset']
QUICK = ['parity', 'reset']
TOL = dict(recorded=0.05, deflection=0.5,  # cm; cm/s of formula error
           speed=5.0, heading=2.0, time=0.02, step=3.0,  # skills, vs the baseline: cm/s, deg, s, cm
           path=8.0)  # skills, absolute: A6 / A7 paths apart (cm); a facing or mirror bug is tens of cm

_state = {}


def _world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()


def _load(name):
    """A fresh copy of a test module (picks up edits), unless the old copy is still driving a test."""
    old = sys.modules.get(name)
    if old is not None and getattr(old, '_state', {}).get('handle') is not None:
        raise RuntimeError('%s is still running' % name)
    spec = importlib.util.spec_from_file_location(name, os.path.join(TOOLS, name + '.py'))
    m = importlib.util.module_from_spec(spec)
    sys.modules[name] = m
    spec.loader.exec_module(m)
    return m


# ---------------------------------------------------------------- comparisons (plain Python, read the saved runs)

def _track(worst, kind, d, where):
    if abs(d) > abs(worst.get(kind, (0.0, ''))[0]):
        worst[kind] = (round(d, 3), where)


def _cmp_recorded(m, base, lbl, scenarios):
    """Every metrics() number of every scenario (positions at fixed times, apex, dribble distances) vs the baseline."""
    worst, missing = {}, []
    for sc in scenarios:
        a = m.metrics(os.path.join(m.OUT_DIR, '%s_r1_%s.json' % (base, sc['name'])))
        b = m.metrics(os.path.join(m.OUT_DIR, '%s_r1_%s.json' % (lbl, sc['name'])))
        for key in (set(a) | set(b)) - {'frame_dt_mean_ms'}:
            if key in a and key in b:
                _track(worst, 'diff', b[key] - a[key], '%s %s' % (sc['name'], key))
            else:
                missing.append('%s %s' % (sc['name'], key))
    d, where = worst.get('diff', (0.0, ''))
    return dict(passed=not missing and abs(d) <= TOL['recorded'], worst=d, at=where, missing=missing[:5])


def _cmp_deflections(m, base, lbl):
    """Judged: every throw hits the body, and every applied deflection equals ComputeDeflection with the hit region's
    row. Only reported: which region was hit and the speeds - the standing player's idle pose decides those (Phase 1's
    own two runs differed by up to 36 cm/s; the right hand can swing in front of the thigh) until the rig freezes the
    pose (plan 6.1)."""
    a, b = m.deflect_report(base), m.deflect_report(lbl)
    worst, bad, noise = {}, [], []
    for k, x in a.items():
        y = b.get(k, {})
        if not y.get('hit'):
            bad.append('%s: no contact' % k)
        if y.get('applied') and (y.get('formula_error') is None or y['formula_error'] > TOL['deflection']):
            bad.append('%s formula_error %s' % (k, y.get('formula_error')))
        if x.get('hit') != y.get('hit'):
            noise.append('%s %s -> %s' % (k, x.get('hit'), y.get('hit')))
        elif x.get('speed_out') is not None and y.get('speed_out') is not None:
            _track(worst, 'speed_out', y['speed_out'] - x['speed_out'], k)
    d, where = worst.get('speed_out', (0.0, ''))
    return dict(passed=not bad, bad=bad[:5], noise_regions=noise, noise_speed_out=d, at=where)


def _cmp_skills(m, base, lbl):
    """Exact: requested, the skill-state sequence, which contacts validated, the touch count. Within noise: each touch's
    applied speed / heading / time and the biggest ball step (A5). Absolute: the facing and mirror paths (A6 / A7)."""
    a, b = m.skill_report(base), m.skill_report(lbl)
    worst, bad = {}, []
    for sc in m.SKILL_SCENARIOS:
        n = sc['name']
        x, y = a[n], b[n]
        if x['requested'] != y['requested']:
            bad.append('%s requested %s' % (n, y['requested']))
        if [s[0] for s in x.get('states', [])] != [s[0] for s in y.get('states', [])]:
            bad.append('%s states %s' % (n, [s[0] for s in y.get('states', [])]))
        valid = [{k: v['first_valid'] is not None for k, v in r.get('A2_contacts', {}).items()} for r in (x, y)]
        if valid[0] != valid[1]:
            bad.append('%s contacts %s' % (n, valid[1]))
        tx, ty = x.get('touches', []), y.get('touches', [])
        if len(tx) != len(ty):
            bad.append('%s touches %d -> %d' % (n, len(tx), len(ty)))
        for i, (p, q) in enumerate(zip(tx, ty)):
            _track(worst, 'speed', q['applied'][0] - p['applied'][0], '%s touch %d' % (n, i))
            _track(worst, 'heading', (q['applied'][1] - p['applied'][1] + 180.0) % 360.0 - 180.0, '%s touch %d' % (n, i))
            _track(worst, 'time', q['t'] - p['t'], '%s touch %d' % (n, i))
        if x.get('A5_max_ball_step_cm') is not None and y.get('A5_max_ball_step_cm') is not None:
            _track(worst, 'step', y['A5_max_ball_step_cm'] - x['A5_max_ball_step_cm'], n)
    # A6 / A7: the skill-space ball paths across facings and feet must agree within TOL['path'] (absolute). Their change from
    # the baseline is only reported: each run's path moves up to ~3.6 cm with the idle phase (phase1_60 vs phase1post_60).
    for group in ('A6_facings', 'A7_mirror'):
        for k, w in b[group].items():
            if not w or w['max_cm'] is None:
                bad.append('%s %s: no matched path' % (group, k))
                continue
            if w['max_cm'] > TOL['path']:
                bad.append('%s %s %.2f cm apart' % (group, k, w['max_cm']))
            v = a[group].get(k)
            if v and v['max_cm'] is not None:
                _track(worst, 'path_change', w['max_cm'] - v['max_cm'], '%s %s' % (group, k))
    for kind, (d, where) in worst.items():
        if kind in TOL and abs(d) > TOL[kind]:
            bad.append('%s %+.2f at %s' % (kind, d, where))
    return dict(passed=not bad, worst={k: v[0] for k, v in worst.items()}, bad=bad[:6])


def _cmp_reset(m, lbl):
    r = m.reset_report(lbl)
    return dict(passed=all(v['passed'] for v in r.values()), failed=[k for k, v in r.items() if not v['passed']])


def _selftest():
    g = {}
    with open(os.path.join(TOOLS, 'framework_selftest.py'), encoding='utf-8') as f:
        exec(f.read(), g)
    r = g['run']()
    return dict(passed=not r['failed'], count=r['passed'], failed=r['failed'][:5])


# suite: (module, start(m, run label), compare(m, baseline label, run label))
SUITES = {
    'parity': ('ball_parity_test', lambda m, lbl: m.start(lbl, runs=1),
               lambda m, base, lbl: _cmp_recorded(m, base, lbl, m.SCENARIOS)),
    'kicks': ('ball_parity_test', lambda m, lbl: m.start(lbl, runs=1, scenarios=m.KICK_SCENARIOS),
              lambda m, base, lbl: _cmp_recorded(m, base, lbl, m.KICK_SCENARIOS)),
    'deflections': ('ball_parity_test', lambda m, lbl: m.start(lbl, runs=1, scenarios=m.DEFLECT_SCENARIOS), _cmp_deflections),
    'skills': ('skill_tests', lambda m, lbl: m.skill_start(lbl), _cmp_skills),
    'reset': ('skill_tests', lambda m, lbl: m.reset_start(lbl), lambda m, base, lbl: _cmp_reset(m, lbl)),
}


# ---------------------------------------------------------------- driver

def run(label, suites=None, fps=60):
    if _state.get('handle') is not None:
        return 'already running'
    suites = list(suites or ALL)
    if set(suites) - set(ALL) or fps not in RATES:
        return 'suites must be from %s, fps from %s' % (ALL, sorted(RATES))
    if _world() is not None:
        return 'stop PIE first (the runner starts its own session)'
    mods = {name: _load(name) for name in ('ball_parity_test', 'skill_tests')}
    path = os.path.join(OUT_DIR, label + '.json')
    if os.path.exists(path):
        os.remove(path)  # the wait watches for this file
    _state.clear()
    _state.update(label=label, fps=fps, mods=mods, results={}, t0=time.time(), result='running', index=0, phase='pie',
                  pie_suites=[s for s in suites if s != 'selftest'])
    if 'selftest' in suites:
        try:
            _state['results']['selftest'] = _selftest()
        except Exception as e:
            _state['results']['selftest'] = dict(passed=False, error=repr(e))
    if not _state['pie_suites']:
        _state['result'] = 'finished'
        _write()
        return 'finished'
    mods['skill_tests'].set_fixed_rate(True, RATES[fps])
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_request_begin_play()
    _state['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'started'


def _tick(delta):
    s = _state
    try:
        world = _world()
        if s['phase'] == 'pie':  # wait for BeginPlay: the player has loaded its body regions
            if world is None:
                if time.time() - s['t0'] > 120.0:
                    raise RuntimeError('PIE did not start')
                return
            ball, player = s['mods']['ball_parity_test']._actors(world)
            if ball is None or player is None or player.get_component_by_class(unreal.PlayerBallInteractionComponent).get_loaded_region_count() == 0:
                return
            s.update(phase='settle', until=unreal.GameplayStatics.get_time_seconds(world) + 0.5)
            return
        if world is None:
            raise RuntimeError('PIE stopped')
        if s['phase'] == 'settle':
            if unreal.GameplayStatics.get_time_seconds(world) >= s['until']:
                _submit_pair(world)
                s['phase'] = 'arbitrate'
            return
        if s['phase'] == 'arbitrate':
            s['results']['arbitration'] = _read_pair(world)
            s['phase'] = 'next'
            return
        suite = s['pie_suites'][s['index']]
        mod_name, start, compare = SUITES[suite]
        m = s['mods'][mod_name]
        lbl = '%s_%s%d' % (s['label'], suite, s['fps'])
        if s['phase'] == 'next':
            r = start(m, lbl)
            if r != 'started':
                raise RuntimeError('%s: %s' % (suite, r))
            s['phase'] = 'running'
            return
        result = m.status()['result']
        if result == 'running':
            return
        if result == 'finished':
            base = BASELINES[suite].format(fps=s['fps']) if suite in BASELINES else None
            s['results'][suite] = compare(m, base, lbl)
        else:
            s['results'][suite] = dict(passed=False, error=result)
        s['index'] += 1
        if s['index'] >= len(s['pie_suites']):
            _end('finished')
        else:
            s['phase'] = 'next'
    except Exception as e:  # never leave a broken callback, a fixed frame rate or PIE running
        import traceback
        _end('error: %r %s' % (e, traceback.format_exc()[-300:]))


def _interaction(world):
    return _state['mods']['ball_parity_test']._actors(world)[0].get_component_by_class(unreal.BallInteractionComponent)


def _submit_pair(world):
    """Plan 3.4: two commands in one frame (neither moves the ball). The next frame's log must show the higher one applied
    and the lower one dropped."""
    for name, priority in (('CheckLow', unreal.ContactPriority.BODY), ('CheckHigh', unreal.ContactPriority.SKILL)):
        c = unreal.BallResponseCommand()
        c.source = unreal.Name(name)
        c.priority = priority
        _interaction(world).submit_response(c)


def _read_pair(world):
    log = {str(r.source): r for r in _interaction(world).get_command_log() if str(r.source) in ('CheckLow', 'CheckHigh')}
    return dict(passed=len(log) == 2 and log['CheckHigh'].applied and not log['CheckLow'].applied,
                low=str(log['CheckLow'].note) if 'CheckLow' in log else None)


def _end(result):
    s = _state
    handle = s.pop('handle', None)
    if handle is not None:
        unreal.unregister_slate_post_tick_callback(handle)
    s['result'] = result
    try:
        s['mods']['skill_tests'].set_fixed_rate(False)
    except Exception:
        pass
    try:
        if _world() is not None:
            unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_request_end_play()
    except Exception:
        pass
    _write()


def _write():
    s = _state
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, s['label'] + '.json')
    with open(path + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(dict(label=s['label'], fps=s['fps'], result=s['result'], seconds=round(time.time() - s['t0'], 1),
                       suites=s['results']), f, indent=1)
    os.replace(path + '.tmp', path)  # the watcher only ever sees a complete file


def status():
    suites, i = _state.get('pie_suites') or [], _state.get('index', 0)
    return dict(result=_state.get('result'), suite=suites[i] if i < len(suites) else None, done=sorted(_state.get('results', {})))


def summary(label):
    """One line per suite: PASS / FAIL and its worst difference."""
    with open(os.path.join(OUT_DIR, label + '.json'), encoding='utf-8') as f:
        d = json.load(f)
    lines = ['%s @%d fps: %s (%.0f s)' % (label, d['fps'], d['result'], d['seconds'])]
    for suite, r in d['suites'].items():
        lines.append('%-11s %s %s' % (suite, 'PASS' if r.get('passed') else 'FAIL',
                                      json.dumps({k: v for k, v in r.items() if k != 'passed' and v not in ([], '')})))
    return lines
