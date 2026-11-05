"""Mono-video ingest queue for MetaHuman Animator: turns every video in a folder into a Footage Capture Data asset, one at a
time (the next starts as soon as one finishes), with the Live Link Hub settings the user uses (JPG frames, "frame" prefix,
U8 BGRA), but rotation AUTO: most phone videos carry a -90 deg flag and rotation NONE left them sideways. Already-ingested videos are skipped, so start() resumes. Progress: Saved/MHAQueue/ingest_log.json.
Load as a module in the editor, then start(); status() reports progress."""
import json
import os
import re
import time
import unreal

VIDEO_DIR = 'D:/Pawn Bro Record/To Process'
ASSET_DIR = '/Game/Football/MHA/CaptureDataUpright/{slate}'
MEDIA_DIR = 'D:/Football Game/MHA_Media_Upright/{slate}'
# videos replaced by a pre-converted copy (the 4K one is scaled to 1920x1080 first)
SOURCE_OVERRIDE = {'jogsideview': 'D:/Pawn Bro Record/To Process/_scaled/jogsideview.mp4'}
FFMPEG = 'C:/Users/dharm/AppData/Local/Microsoft/WinGet/Packages/Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe/ffmpeg-9.0-full_build/bin/ffmpeg.exe'
LOG = os.path.join(unreal.Paths.project_saved_dir(), 'MHAQueue', 'ingest_log.json')
_s = {}


def _slate(path):
    return re.sub(r'[^A-Za-z0-9_]', '_', os.path.splitext(os.path.basename(path))[0])


def _done(slate):
    return unreal.EditorAssetLibrary.does_directory_exist(ASSET_DIR.format(slate=slate)) and bool(
        unreal.EditorAssetLibrary.list_assets(ASSET_DIR.format(slate=slate), recursive=True))


def _save():
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'w', encoding='utf-8') as f:
        json.dump({k: v for k, v in _s.items() if k in ('queue', 'results', 'current', 'started', 'finished')}, f, indent=1)


def _params():
    p = unreal.CaptureManagerConversionParams()
    p.set_editor_property('image_format', unreal.CaptureManagerImageFormat.JPG)
    p.set_editor_property('image_file_prefix', 'frame')
    p.set_editor_property('pixel_format', unreal.CaptureManagerPixelFormat.U8_BGRA)
    p.set_editor_property('rotation', unreal.CaptureManagerRotation.AUTO)
    return p


def _finish(ok, info):
    cur = _s.pop('current', None)
    if cur:
        _s['results'].append(dict(video=cur['video'], slate=cur['slate'], ok=ok, info=info, seconds=round(time.time() - cur['t0'], 1)))
    _save()
    # start the next one on the next editor tick, outside the ingest callback
    def tick(dt):
        unreal.unregister_slate_post_tick_callback(_s.pop('tick'))
        _next()
    _s['tick'] = unreal.register_slate_post_tick_callback(tick)


def _on_ok(ingest_id, ingest_type, capture_data):
    _finish(True, capture_data.get_path_name() if capture_data else '')


def _on_fail(ingest_id, ingest_type, message):
    _finish(False, str(message))


def _next():
    while _s['queue']:
        video = _s['queue'].pop(0)
        slate = _slate(video)
        if _done(slate):
            _s['results'].append(dict(video=video, slate=slate, ok=True, info='already ingested', seconds=0))
            continue
        S = unreal.get_default_object(unreal.CaptureManagerEditorSettings)
        S.set_editor_property('import_directory', unreal.DirectoryPath(path=ASSET_DIR.format(slate='{slate}')))
        S.set_editor_property('media_directory', unreal.DirectoryPath(path=MEDIA_DIR.format(slate='{slate}')))
        # phone .mp4s need FFmpeg: the built-in decoder rejects them ("Video file format is unsupported")
        S.set_editor_property('enable_third_party_encoder', True)
        S.set_editor_property('third_party_encoder', unreal.FilePath(file_path=FFMPEG))
        _s['current'] = dict(video=video, slate=slate, t0=time.time())
        _save()
        unreal.CaptureManagerIngestBlueprintLibrary.ingest_mono_video(SOURCE_OVERRIDE.get(slate, video), '', slate, 1, _params(), _s['ok'], _s['fail'])
        return
    _s['finished'] = time.strftime('%H:%M:%S')
    _save()


def start(limit=0):
    if _s.get('current'):
        return 'already running: ' + _s['current']['slate']
    videos = sorted(os.path.join(VIDEO_DIR, f).replace('\\', '/') for f in os.listdir(VIDEO_DIR) if f.lower().endswith(('.mp4', '.mov')))
    _s.clear()
    ok, fail = unreal.CaptureManagerIngestSuccess(), unreal.CaptureManagerIngestFailed()
    ok.bind_callable(_on_ok)
    fail.bind_callable(_on_fail)
    _s.update(queue=videos[:limit] if limit else videos, results=[], ok=ok, fail=fail, started=time.strftime('%H:%M:%S'))
    _next()
    return 'queued %d' % len(_s['queue'])


def status():
    r = _s.get('results', [])
    return dict(done=sum(x['ok'] for x in r), failed=[x['slate'] for x in r if not x['ok']], left=len(_s.get('queue', [])),
                current=_s.get('current', {}).get('slate'), finished=_s.get('finished'))
