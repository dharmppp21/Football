"""MetaHuman Performance queue: one Performance per ingested Capture Data (mha_ingest_queue), mono footage, face tracking
off, body tracking on, every other setting Epic's default, the whole clip (no trimming). One clip at a time: the next
starts on the tick after the previous finishes. Already-processed clips are skipped, so start() resumes.
Progress: Saved/MHAQueue/perf_log.json. Load as a module in the editor, then start(); status() reports progress."""
import json
import os
import time
import unreal

CD_DIR = '/Game/Football/MHA/CaptureDataUpright'
PERF_DIR = '/Game/Football/MHA/Performances'
LOG = os.path.join(unreal.Paths.project_saved_dir(), 'MHAQueue', 'perf_log.json')
EAL = unreal.EditorAssetLibrary
_s = {}


def _save():
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'w', encoding='utf-8') as f:
        json.dump(dict({k: _s.get(k) for k in ('queue', 'results', 'started', 'finished')}, current=(_s.get('current') or {}).get('slate')), f, indent=1)


def _perf_path(slate):
    return '%s/PF_%s' % (PERF_DIR, slate)


def _make(cd_path, slate):
    cd = unreal.load_asset(cd_path)
    perf = unreal.load_asset(_perf_path(slate)) if EAL.does_asset_exist(_perf_path(slate)) else \
        unreal.AssetToolsHelpers.get_asset_tools().create_asset('PF_' + slate, PERF_DIR, unreal.MetaHumanPerformance, unreal.MetaHumanPerformanceFactoryNew())
    perf.set_editor_property('input_type', unreal.DataInputType.MONO_FOOTAGE)
    perf.set_editor_property('footage_capture_data', cd)
    perf.set_editor_property('face_tracking', False)
    perf.set_editor_property('body_tracking', True)  # routes through the SetBodyTracking BlueprintSetter
    perf.set_blocking_processing(False)
    return perf


def _tick(dt):
    # start_pipeline pumps editor ticks itself; without this guard the queue re-entered and tried every other clip while
    # the first was starting (all refused as Disabled: MHA processes one performance at a time)
    if _s.get('busy'):
        return
    _s['busy'] = True
    try:
        _step()
    finally:
        _s['busy'] = False


def _step():
    cur = _s.get('current')
    if cur and cur['perf'].is_processing():
        return
    if cur:
        perf = _s.pop('current')['perf']
        ok = perf.can_export_animation()
        EAL.save_asset(perf.get_path_name().split('.')[0], only_if_is_dirty=False)
        _s['results'].append(dict(slate=cur['slate'], ok=ok, seconds=round(time.time() - cur['t0'], 1)))
        _save()
    while _s['queue']:
        cd_path, slate = _s['queue'].pop(0)
        if EAL.does_asset_exist(_perf_path(slate)) and unreal.load_asset(_perf_path(slate)).can_export_animation():
            _s['results'].append(dict(slate=slate, ok=True, seconds=0, info='already processed'))
            continue
        try:
            perf = _make(cd_path, slate)
            err = perf.start_pipeline(True)
        except Exception as e:
            _s['results'].append(dict(slate=slate, ok=False, info=repr(e)))
            continue
        if err == unreal.StartPipelineErrorType.DISABLED and _s.setdefault('retries', {}).get(slate, 0) < 60:
            _s['retries'][slate] = _s['retries'].get(slate, 0) + 1  # another performance still busy: retry next tick
            _s['queue'].insert(0, (cd_path, slate))
            return
        if err != unreal.StartPipelineErrorType.NONE:
            _s['results'].append(dict(slate=slate, ok=False, info=str(err)))
            continue
        _s['current'] = dict(slate=slate, perf=perf, t0=time.time())
        _save()
        return
    unreal.unregister_slate_post_tick_callback(_s.pop('handle'))
    _s['finished'] = time.strftime('%H:%M:%S')
    _save()


def start(limit=0):
    if _s.get('handle') is not None:
        return 'already running'
    cds = sorted(a.split('.')[0] for a in EAL.list_assets(CD_DIR, recursive=True) if '/CD_' in a)
    queue = [(p, p.split('/')[-2]) for p in cds]
    _s.clear()
    _s.update(queue=queue[:limit] if limit else queue, results=[], started=time.strftime('%H:%M:%S'))
    _s['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'queued %d' % len(_s['queue'])


def status():
    r = _s.get('results', [])
    return dict(done=sum(1 for x in r if x['ok']), failed=[x['slate'] for x in r if not x['ok']], left=len(_s.get('queue', [])),
                current=(_s.get('current') or {}).get('slate'), finished=_s.get('finished'))


def stop():
    cur = _s.get('current')
    if cur:
        cur['perf'].cancel_pipeline()
    if _s.get('handle') is not None:
        unreal.unregister_slate_post_tick_callback(_s.pop('handle'))
    return 'stopped'
