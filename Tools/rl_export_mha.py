"""Export every processed MetaHuman Performance (mha_perf_queue) as an Animation Sequence on the performer (MetaHuman body)
skeleton, whole clip, body only, at the footage's own frame rate, then retarget it onto the Footballer with
RTG_MetaHuman_To_Footballer. Direct "Existing Skeleton" export can't be used: the exporter writes timecode attributes on a
bone named "root" (hard-coded) and the Footballer's root is Hips, so from Python the export aborts with only the ref pose. One export per editor tick; existing outputs are skipped, so start() resumes.
Progress: Saved/MHAQueue/rl_export_log.json. Load as a module in the editor, then start(); status() reports progress."""
import json
import os
import time
import unreal

PERF_DIR = '/Game/Football/MHA/Performances'
OUT_DIR = '/Game/RL_Data/MHA_Videos'
MH_DIR = '/Game/Football/MHA/Exports_MH'
TARGET_MESH = '/Game/Football/Models/Footballer/Footballer_latest'
RETARGETER = '/Game/Football/Models/Footballer/RTG_MetaHuman_To_Footballer'
LOG = os.path.join(unreal.Paths.project_saved_dir(), 'MHAQueue', 'rl_export_log.json')
EAL = unreal.EditorAssetLibrary
_s = {}


def _save():
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'w', encoding='utf-8') as f:
        json.dump({k: _s.get(k) for k in ('left', 'results', 'started', 'finished')}, f, indent=1)


def export_one(perf_path):
    perf = unreal.load_asset(perf_path)
    slate = perf.get_name()[3:]  # PF_<slate>
    name = 'AS_' + slate
    if EAL.does_asset_exist('%s/%s' % (OUT_DIR, name)):
        return dict(slate=slate, ok=True, info='exists')
    S = unreal.MetaHumanPerformanceExportUtils.get_export_animation_sequence_settings(perf)
    S.set_editor_property('show_export_dialog', False)
    S.set_editor_property('auto_save_anim_sequence', True)
    S.set_editor_property('export_range', unreal.PerformanceExportRange.WHOLE_SEQUENCE)
    S.set_editor_property('export_face', False)
    S.set_editor_property('export_body', True)
    S.set_editor_property('export_skeleton', unreal.PerformanceExportSkeleton.PERFORMER_SKELETON)
    S.set_editor_property('package_path', '%s/%s' % (MH_DIR, slate))
    S.set_editor_property('asset_name', name)
    src = '%s/%s/%s' % (MH_DIR, slate, name)
    if not EAL.does_asset_exist(src):
        unreal.MetaHumanPerformanceExportUtils.export_animation_sequence(perf, S)
    mesh = unreal.load_asset('%s/%s/SK_%s' % (MH_DIR, slate, slate))  # the performer-proportioned mesh the export writes
    unreal.IKRetargetBatchOperation.duplicate_and_retarget([EAL.find_asset_data(src)], mesh, unreal.load_asset(TARGET_MESH),
                                                           unreal.load_asset(RETARGETER), '', '', '', '', OUT_DIR, False, False, False)
    dst = '%s/%s' % (OUT_DIR, name)
    a = unreal.load_asset(dst)
    EAL.save_asset(dst, only_if_is_dirty=False)
    return dict(slate=slate, ok=True, frames=a.get_editor_property('number_of_sampled_frames'), seconds=round(a.get_play_length(), 2))


def _tick(dt):
    if _s.get('busy'):
        return
    _s['busy'] = True
    try:
        if _s['left']:
            p = _s['left'].pop(0)
            try:
                _s['results'].append(export_one(p))
            except Exception as e:
                _s['results'].append(dict(slate=p, ok=False, info=repr(e)))
        else:
            unreal.unregister_slate_post_tick_callback(_s.pop('handle'))
            _s['finished'] = time.strftime('%H:%M:%S')
        _save()
    finally:
        _s['busy'] = False


def start(limit=0):
    if _s.get('handle') is not None:
        return 'already running'
    perfs = sorted(a.split('.')[0] for a in EAL.list_assets(PERF_DIR, recursive=False) if '/PF_' in a)
    _s.clear()
    _s.update(left=perfs[:limit] if limit else perfs, results=[], started=time.strftime('%H:%M:%S'))
    _s['handle'] = unreal.register_slate_post_tick_callback(_tick)
    return 'queued %d' % len(_s['left'])


def status():
    r = _s.get('results', [])
    return dict(done=sum(1 for x in r if x['ok']), failed=[x for x in r if not x['ok']], left=len(_s.get('left', [])), finished=_s.get('finished'))
