"""Small optional local-viewer adapter. All photographic rendering is native."""
from dataclasses import dataclass
import json
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time

import numpy as np
from PIL import Image

from raw_camera_engine_check import camera_matrix, native_metadata


@dataclass(frozen=True)
class ViewRequest:
    path: str
    algorithm: str = 'rawengine.bilinear'
    exposure: float = 0.0
    saturation: float = 1.0
    vibrance: float = 0.0
    red: float = 1.0
    blue: float = 1.0
    midpoint: float = 0.5
    black: float = 0.0
    white: float = 1.0
    hue_shift: tuple = (0.0,) * 8
    saturation_delta: tuple = (0.0,) * 8
    luminance_delta: tuple = (0.0,) * 8
    before: bool = False
    detail: bool = False
    center: tuple = (0.5, 0.5)
    size: tuple = (900, 650)

    def __post_init__(self):
        # Own the band arrays even if a caller supplies mutable lists.
        for name in ('hue_shift', 'saturation_delta', 'luminance_delta'):
            object.__setattr__(self, name, tuple(getattr(self, name)))


# EXIF transform: clockwise turns, then flips in the rotated coordinates.
ORIENTATIONS = {1: (0, False, False), 2: (0, True, False),
                3: (2, False, False), 4: (0, False, True),
                5: (1, True, False), 6: (1, False, False),
                7: (1, False, True), 8: (3, False, False)}


def operation(kind, upstream, parameters, number):
    return dict(id=f'60000000-0000-0000-0000-{number:012d}', type='rawengine.' + kind,
                schema_version=1, processing_version=2, enabled=True,
                input_domain='scene_linear_prophoto_d50', output_domain='scene_linear_prophoto_d50',
                inputs=dict(image=upstream), parameters=parameters, masks={},
                blend_mode='normal', opacity=1)


def manifest(session, record, view):
    wb = record['camera_whitebalance']
    recipe = dict(red_gain=wb[0] / wb[1] * (1 if view.before else view.red), green_gain=1,
                  blue_gain=wb[2] / wb[1] * (1 if view.before else view.blue),
                  exposure_stops=0 if view.before else view.exposure,
                  camera_to_xyz_d50=camera_matrix(record), working_space='prophoto-d50',
                  output_mode='srgb-preview')
    doc = json.loads(session.export_manifest(recipe))
    calibrated = next(o['id'] for o in doc['operations'] if o['type'] == 'rawengine.camera_to_working')
    ax, ay, w, h = record['metadata']['active_area']
    crop = operation('crop', calibrated, dict(x=ax, y=ay, width=w, height=h), 1)
    turns, horizontal, vertical = ORIENTATIONS[record.get('orientation', 1)]
    oriented = operation('orientation', crop['id'], dict(quarter_turns=turns,
                         flip_horizontal=horizontal, flip_vertical=vertical), 2)
    levels = operation('levels', oriented['id'], dict(input_black=[0 if view.before else view.black] * 3,
                       input_white=[1 if view.before else view.white] * 3,
                       output_black=[0] * 3, output_white=[1] * 3), 3)
    points = [[0, 0], [0.5, 0.5 if view.before else view.midpoint], [1, 1]]
    curves = operation('curves', levels['id'], {c: points for c in ('red', 'green', 'blue')}, 4)
    saturation = operation('saturation', curves['id'], dict(amount=1 if view.before else view.saturation), 5)
    vibrance = operation('vibrance', saturation['id'], dict(amount=0 if view.before else view.vibrance), 6)
    mixer = operation('color_mixer', vibrance['id'],
        {name: [0.0] * 8 if view.before else list(getattr(view, name))
         for name in ('hue_shift', 'saturation_delta', 'luminance_delta')}, 7)
    # Rewire only the calibration's downstream output chain, leaving source edits intact.
    for op in doc['operations']:
        if op['inputs'].get('image') == calibrated:
            op['inputs']['image'] = mixer['id']
    doc['operations'].extend((crop, oriented, levels, curves, saturation, vibrance, mixer))
    return json.dumps(doc), ((h, w) if turns % 2 else (w, h))


def render_request(view, extent):
    if not view.detail:
        return dict(mip=2, quality='preview', tile_size=256), None
    width, height = extent
    rw, rh = min(width, max(1, view.size[0])), min(height, max(1, view.size[1]))
    x = max(0, min(width - rw, round(view.center[0] * width - rw / 2)))
    y = max(0, min(height - rh, round(view.center[1] * height - rh / 2)))
    return dict(mip=0, quality='final', tile_size=256, x=x, y=y,
                roi_width=rw, roi_height=rh), (x, y, rw, rh)


class LoadedRaw:
    def __init__(self, path, decoder_python, native, stop_event=None):
        self.path = path
        self.native = native
        self.sessions = {}
        with tempfile.TemporaryDirectory(prefix='librawops-viewer-') as directory:
            output = Path(directory)
            with subprocess.Popen([str(decoder_python), str(Path(__file__).with_name('raw_viewer_decode.py')),
                                   path, str(output)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                  creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)) as process:
                deadline = time.monotonic() + 90
                while True:
                    try:
                        stdout, stderr = process.communicate(timeout=0.1)
                        break
                    except subprocess.TimeoutExpired:
                        if (stop_event is not None and stop_event.is_set()) or time.monotonic() >= deadline:
                            process.kill()
                            process.communicate()
                            raise RuntimeError('File opening cancelled or timed out.')
                if process.returncode:
                    detail = (stderr or stdout).strip().splitlines()
                    raise ValueError('Could not open RAW file: ' + (detail[-1] if detail else 'decoder failed'))
            self.record = json.loads((output / 'metadata.json').read_text(encoding='utf-8'))
            meta = self.record['metadata']
            self.codes = np.fromfile(output / 'bayer.bin', dtype='<u2').reshape(meta['height'], meta['width'])
            camera_matrix(self.record)  # Fail before presenting unsupported color data.

    def session(self, algorithm):
        if algorithm not in self.sessions:
            meta = self.record['metadata']
            self.sessions[algorithm] = self.native.RawSession(self.codes, meta['width'], meta['height'],
                native_metadata(meta), cache_bytes=64 * 1024 * 1024, workers=1, max_pending=2,
                demosaic=dict(algorithm=algorithm, processing_version=1))
        return self.sessions[algorithm]

    def close(self):
        for session in self.sessions.values():
            session.close()
        self.sessions.clear()


class ViewerWorker:
    """One pending view, stale-result rejection and tile-boundary cancellation."""
    def __init__(self, decoder_python, native, loader=LoadedRaw):
        self.decoder_python, self.native, self.loader = decoder_python, native, loader
        self.events = queue.Queue()
        self.condition = threading.Condition()
        self.revision = 0
        self.pending = None
        self.job = None
        self.stopped = False
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.run, name='RAW viewer', daemon=True)
        self.thread.start()

    def submit(self, view):
        with self.condition:
            if self.stopped:
                raise RuntimeError('Viewer is closed.')
            self.revision += 1
            self.pending = (self.revision, view)
            if self.job is not None:
                self.job.cancel()
            self.condition.notify()
            return self.revision

    def current(self, revision):
        with self.condition:
            return not self.stopped and self.revision == revision

    def close(self):
        with self.condition:
            self.stopped = True
            self.stop_event.set()
            self.pending = None
            if self.job is not None:
                self.job.cancel()
            self.condition.notify()

    def run(self):
        loaded = None
        try:
            while True:
                with self.condition:
                    self.condition.wait_for(lambda: self.stopped or self.pending is not None)
                    if self.stopped:
                        return
                    revision, view = self.pending
                    self.pending = None
                started = time.perf_counter()
                try:
                    opening = loaded is None or loaded.path != view.path
                    if opening:
                        replacement = self.loader(view.path, self.decoder_python, self.native, self.stop_event)
                        if loaded is not None:
                            loaded.close()
                        loaded = replacement
                    session = loaded.session(view.algorithm)
                    if not self.current(revision):
                        continue
                    text, extent = manifest(session, loaded.record, view)
                    request, roi = render_request(view, extent)
                    render_started = time.perf_counter()
                    with self.condition:
                        if self.stopped or self.revision != revision:
                            continue
                        self.job = session.submit_manifest_latest('viewer', text, request)
                        job = self.job
                    result = job.result()
                    render_ms = (time.perf_counter() - render_started) * 1000
                    if not self.current(revision):
                        continue
                    w, h, data = result
                    pixels = np.frombuffer(data, dtype=np.float32).reshape(h, w, 3)
                    if not np.isfinite(pixels).all():
                        raise ValueError('Engine returned nonfinite display pixels.')
                    image = Image.fromarray(np.rint(np.clip(pixels, 0, 1) * 255).astype(np.uint8))
                    if not view.detail:
                        image.thumbnail(view.size, Image.Resampling.LANCZOS)
                    self.events.put((revision, 'image', dict(image=image, extent=extent, roi=roi,
                        render_ms=render_ms, total_ms=(time.perf_counter() - started) * 1000,
                        opening=opening, view=view, cache=session.cache_stats(), manifest=text, result=result)))
                except Exception as error:
                    if self.current(revision):
                        self.events.put((revision, 'error', str(error)))
                finally:
                    with self.condition:
                        self.job = None
        finally:
            if loaded is not None:
                loaded.close()
