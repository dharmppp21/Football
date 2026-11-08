"""Tutorial clips in batches (BATCHES): ingest -> solve -> export and retarget -> raw bone data, using the project's queues with their globals pointed at the
test folders, so the 115 old clips and their logs stay untouched. Chained on editor ticks and kept in a module, so it goes on by itself once start(batch) has been called.
Progress: Saved/MHAQueue/dribbletest_batch<N>.json; each queue also keeps its own dribbletest_*<N>.json. Rows land in RL_Train/data/tutorial_clips_b<N>.json
as each clip finishes (batch 5: tutorial_clips.json)."""
import json, os, sys, time
import unreal
T = 'D:/Football Game/Real Project/FootballArena 5.8/Tools'
if T not in sys.path:
    sys.path.insert(0, T)
import mha_ingest_queue as iq
import mha_perf_queue as pq
import rl_export_mha as eq
import rl_export_motion as m

# Batch 5 (2026-11-07) wrote data/tutorial_clips.json and the *5.json logs; each later batch writes its own rows file and logs, merged into tutorial_clips.json afterwards.
BATCHES = {5: ['B5_Y6', 'B5_Y5', 'B6b_Y2', 'B6b_Y1', 'B6b_Y3', 'B5_Y1', 'B5_Y2', 'B5_Y3', 'B4_Y1', 'B4_Y2', 'B4_Y3', 'B4_Y5', 'B4_Y6', 'B4_Y7', 'B4_Y8', 'B4_Y9', 'B4_Y10', 'B2_Y1', 'B2_Y2'],
           6: ['B5_Y8', 'B10_Y1', 'B12_Y1', 'B18_Y1', 'B9_Y1', 'B18_Y2', 'B12_Y2', 'B12_Y3', 'B8_Y1', 'B8_Y2', 'B8_Y3', 'B8_Y4', 'B10_Y2', 'B10_Y3', 'B10_Y4'],
           7: ['B12_Y4', 'B18_Y3', 'B9_Y2', 'B9_Y3', 'B18_Y4', 'B12_Y5', 'B8_Y5', 'B8_Y6', 'B12_Y6', 'B12_Y7']}
CLIPS = []
ROOT = '/Game/Football/MHA/DribbleTest'
SAVED = os.path.join(unreal.Paths.project_saved_dir(), 'MHAQueue')
ROWS = STATUS = None
EAL = unreal.EditorAssetLibrary
iq.ASSET_DIR, iq.MEDIA_DIR = ROOT + '/CaptureData/{slate}', 'D:/Football Game/MHA_Media_Dribble/{slate}'
pq.CD_DIR, pq.PERF_DIR = ROOT + '/CaptureData', ROOT + '/Performances'
eq.PERF_DIR, eq.MH_DIR, eq.OUT_DIR = ROOT + '/Performances', ROOT + '/Exports_MH', ROOT + '/Footballer'


def _use(batch):
    global ROWS, STATUS
    CLIPS[:] = BATCHES[batch]
    ROWS = 'D:/Football Game/RL_Train/data/tutorial_clips%s.json' % ('' if batch == 5 else '_b%d' % batch)
    STATUS = os.path.join(SAVED, 'dribbletest_batch%s.json' % ('' if batch == 5 else batch))
    iq.LOG, pq.LOG, eq.LOG = (os.path.join(SAVED, 'dribbletest_%s%d.json' % (s, batch)) for s in ('ingest', 'perf', 'export'))
c = {}
_h = {}


def _save():
    os.makedirs(SAVED, exist_ok=True)
    with open(STATUS, 'w', encoding='utf-8') as f:
        json.dump({k: v for k, v in c.items() if k != 'busy'}, f, indent=1)


def _ok(results):
    return [r['slate'] for r in results if r.get('ok')]


def _start_ingest():
    videos = ['D:/Pawn Bro Record/Dribble_YouTube/%s.mp4' % s for s in CLIPS]
    assert all(os.path.exists(v) for v in videos) and os.path.exists(iq.FFMPEG)
    ok, fail = unreal.CaptureManagerIngestSuccess(), unreal.CaptureManagerIngestFailed()
    ok.bind_callable(iq._on_ok)
    fail.bind_callable(iq._on_fail)
    iq._s.clear()
    iq._s.update(queue=videos, results=[], ok=ok, fail=fail, started=time.strftime('%H:%M:%S'))
    iq._next()


def _start_perf(ok):
    cds = {a.split('.')[0].split('/')[-2]: a.split('.')[0] for a in EAL.list_assets(pq.CD_DIR, recursive=True) if '/CD_' in a}
    pq._s.clear()
    pq._s.update(queue=[(cds[s], s) for s in CLIPS if s in ok and s in cds], results=[], started=time.strftime('%H:%M:%S'))
    pq._s['handle'] = unreal.register_slate_post_tick_callback(pq._tick)


def _start_export(ok):
    eq._s.clear()
    eq._s.update(left=['%s/PF_%s' % (eq.PERF_DIR, s) for s in CLIPS if s in ok], results=[], started=time.strftime('%H:%M:%S'))
    eq._s['handle'] = unreal.register_slate_post_tick_callback(eq._tick)


def _bones(s):
    path = '%s/AS_%s' % (eq.OUT_DIR, s)
    a = unreal.load_asset(path)
    k, L = unreal.AnimationLibrary.get_num_frames(a), a.get_play_length()
    r = dict(path=path, folder='MHA_DribbleTest', name='AS_' + s, frames=k, length=L, fps=round(k / L, 3), skeleton=a.get_editor_property('skeleton').get_name())
    r['keys'] = m.export_clip(r)
    c['rows'].append(r)
    with open(ROWS, 'w', encoding='utf-8') as f:
        json.dump(c['rows'], f, indent=1)


def _step():
    st = c['stage']
    if st == 'ingest' and iq._s.get('finished'):
        c['ingest'] = iq.status()
        c['stage'] = 'solve'
        _save()
        _start_perf(_ok(iq._s['results']))
    elif st == 'solve' and pq._s.get('finished'):
        c['solve'] = pq.status()
        c['stage'] = 'export'
        _save()
        _start_export(_ok(pq._s['results']))
    elif st == 'export' and eq._s.get('finished'):
        c['export'] = eq.status()
        c['left'] = [s for s in CLIPS if s in _ok(eq._s['results'])]
        c['stage'] = 'bones'
        _save()
    elif st == 'bones':
        if c['left']:
            s = c['left'].pop(0)
            try:
                _bones(s)
            except Exception as e:
                c['errors'].append('%s: %r' % (s, e))
            _save()
        else:
            c['stage'] = 'done'
            c['finished'] = time.strftime('%H:%M:%S')
            _save()
            unreal.unregister_slate_post_tick_callback(_h.pop('tick'))


def _tick(dt):
    if c.get('busy'):  # the solve pumps editor ticks itself
        return
    c['busy'] = True
    try:
        _step()
    except Exception as e:
        c['errors'].append('stage %s: %r' % (c['stage'], e))
        c['stage'] = 'failed'
        _save()
        unreal.unregister_slate_post_tick_callback(_h.pop('tick'))
    finally:
        c['busy'] = False


def start(batch):
    if _h.get('tick') is not None:
        return 'already running: ' + c['stage']
    _use(batch)
    c.clear()
    c.update(stage='ingest', started=time.strftime('%H:%M:%S'), rows=[], left=[], errors=[], clips=len(CLIPS))
    _save()
    _start_ingest()
    _h['tick'] = unreal.register_slate_post_tick_callback(_tick)
    return 'started %d clips: %s' % (len(CLIPS), iq.status())
