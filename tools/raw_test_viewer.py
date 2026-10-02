"""Plain local NEF test window; optional dependencies, separate from engine install."""
import argparse
from pathlib import Path
import queue
import sys
import tkinter as tk
from tkinter import filedialog, ttk

from PIL import ImageTk

from raw_viewer_backend import ViewRequest, ViewerWorker

ROOT = Path(__file__).resolve().parents[1]
COLOR_BANDS = ('Red', 'Orange', 'Yellow', 'Green', 'Aqua', 'Blue', 'Purple', 'Magenta')


class Viewer:
    def __init__(self, root, decoder_python, native):
        self.root = root
        self.worker = ViewerWorker(decoder_python, native)
        self.path = None
        self.revision = 0
        self.debounce = None
        self.detail = False
        self.center = (0.5, 0.5)
        self.last = None
        self.photo = None
        self.image_box = None
        self.drag = None
        self.closed = False
        root.title('LibRawOps — simple RAW test')
        root.geometry('1180x820')
        root.minsize(820, 780)
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.columnconfigure(1, weight=1)
        root.rowconfigure(1, weight=1)
        toolbar = ttk.Frame(root, padding=8)
        toolbar.grid(row=0, column=0, columnspan=2, sticky='ew')
        ttk.Button(toolbar, text='Open NEF…', command=self.open_dialog).pack(side='left')
        ttk.Button(toolbar, text='Fit', command=lambda: self.set_detail(False)).pack(side='left', padx=(12, 3))
        ttk.Button(toolbar, text='100%', command=lambda: self.set_detail(True)).pack(side='left')
        self.filename = tk.StringVar(value='No file open')
        ttk.Label(toolbar, textvariable=self.filename).pack(side='left', padx=15)
        panel = ttk.Frame(root, padding=12)
        panel.grid(row=1, column=0, sticky='ns')
        self.adjustment_tabs = ttk.Notebook(panel)
        self.adjustment_tabs.pack(fill='both', expand=True)
        basic = ttk.Frame(self.adjustment_tabs, padding=(0, 6))
        self.mixer_panel = ttk.Frame(self.adjustment_tabs, padding=(0, 10))
        self.adjustment_tabs.add(basic, text='Adjustments')
        self.adjustment_tabs.add(self.mixer_panel, text='Color mixer')
        self.algorithm = tk.StringVar(value='Bilinear')
        ttk.Label(basic, text='Demosaic').pack(anchor='w')
        combo = ttk.Combobox(basic, textvariable=self.algorithm, values=('Bilinear', 'Menon'), state='readonly', width=22)
        combo.pack(fill='x', pady=(3, 10))
        combo.bind('<<ComboboxSelected>>', lambda event: self.schedule())
        self.controls = {}
        for key, label, low, high, default in (
            ('exposure', 'Exposure (stops)', -5, 5, 0),
            ('red', 'WB red × as-shot', 0.25, 4, 1),
            ('blue', 'WB blue × as-shot', 0.25, 4, 1),
            ('saturation', 'Saturation', 0, 4, 1),
            ('vibrance', 'Vibrance', -1, 1, 0),
            ('midpoint', 'Curve midpoint', 0.05, 0.95, 0.5),
            ('black', 'Levels black', -0.25, 0.25, 0),
            ('white', 'Levels white', 0.3, 2, 1)):
            variable = tk.DoubleVar(value=default)
            value = tk.StringVar(value=f'{default:.2f}')
            row = ttk.Frame(basic)
            row.pack(fill='x')
            ttk.Label(row, text=label).pack(side='left')
            ttk.Label(row, textvariable=value).pack(side='right')
            def changed(number, name=key, display=value):
                display.set(f'{float(number):.2f}')
                self.schedule()
            ttk.Scale(basic, from_=low, to=high, variable=variable, command=changed,
                      length=210).pack(fill='x', pady=(3, 9))
            self.controls[key] = (variable, value, default)
        self.mixer_settings = {name: [0.0] * 8 for name in
            ('hue_shift', 'saturation_delta', 'luminance_delta')}
        self.mixer_band = tk.StringVar(value=COLOR_BANDS[0])
        ttk.Label(self.mixer_panel, text='Color mixer (linear RGB)').pack(anchor='w')
        self.mixer_band_selector = ttk.Combobox(self.mixer_panel, textvariable=self.mixer_band,
            values=COLOR_BANDS, state='readonly', width=22)
        self.mixer_band_selector.pack(fill='x', pady=(8, 18))
        self.mixer_band_selector.bind('<<ComboboxSelected>>', self.select_mixer_band)
        self.mixer_controls = {}
        for key, label, low, high in (
            ('hue_shift', 'Hue (degrees)', -60, 60),
            ('saturation_delta', 'Chroma delta', -1, 1),
            ('luminance_delta', 'Luminance delta', -1, 1)):
            variable = tk.DoubleVar(value=0)
            value = tk.StringVar(value='0.00')
            row = ttk.Frame(self.mixer_panel)
            row.pack(fill='x')
            ttk.Label(row, text=label).pack(side='left')
            ttk.Label(row, textvariable=value).pack(side='right')
            def changed(number, name=key, display=value):
                amount = float(number)
                display.set(f'{amount:.2f}')
                self.mixer_settings[name][COLOR_BANDS.index(self.mixer_band.get())] = amount
                self.schedule()
            scale = ttk.Scale(self.mixer_panel, from_=low, to=high, variable=variable,
                command=changed, length=210)
            scale.pack(fill='x', pady=(3, 13))
            self.mixer_controls[key] = (variable, value, scale)
        ttk.Label(self.mixer_panel,
            text='Each band keeps its own values.\nLuminance fades near neutrals.',
            wraplength=215).pack(anchor='w', pady=(8, 0))
        self.before = tk.BooleanVar(value=False)
        ttk.Checkbutton(panel, text='Before (as-shot)', variable=self.before, command=self.schedule).pack(anchor='w', pady=8)
        ttk.Button(panel, text='Reset adjustments', command=self.reset).pack(fill='x', pady=5)
        self.navigation_tip = ttk.Label(panel,
            text='Double-click fit image for 100%.\nDrag the 100% view to pan.', wraplength=215)
        self.navigation_tip.pack(anchor='w', pady=(18, 0))
        self.canvas = tk.Canvas(root, background='#242424', highlightthickness=0)
        self.canvas.grid(row=1, column=1, sticky='nsew', padx=(0, 8))
        self.canvas.bind('<Configure>', self.resized)
        self.canvas.bind('<Double-Button-1>', self.pick_detail)
        self.canvas.bind('<ButtonPress-1>', self.start_drag)
        self.canvas.bind('<B1-Motion>', self.move_drag)
        self.canvas.bind('<ButtonRelease-1>', self.finish_drag)
        self.status = tk.StringVar(value='Open a NEF to begin.')
        self.status_label = ttk.Label(root, textvariable=self.status, padding=8)
        self.status_label.grid(row=2, column=0, columnspan=2, sticky='ew')
        self.canvas.create_text(450, 300, text='Open a NEF', fill='white', tags='image')
        root.after(40, self.poll)

    def open_dialog(self):
        path = filedialog.askopenfilename(parent=self.root, title='Open RAW file',
            initialdir=str(ROOT / 'tests/rawfiles'), filetypes=[('Nikon RAW', '*.nef *.NEF')])
        if path:
            self.open_file(path)

    def open_file(self, path):
        self.path = str(Path(path).resolve())
        self.filename.set(Path(path).name)
        self.center = (0.5, 0.5)
        self.detail = False
        self.last = None
        self.photo = None
        self.image_box = None
        self.drag = None
        self.canvas.delete('image')
        self.reset()

    def reset(self):
        for variable, display, default in self.controls.values():
            variable.set(default)
            display.set(f'{default:.2f}')
        for settings in self.mixer_settings.values():
            settings[:] = [0.0] * 8
        self.select_mixer_band()
        self.before.set(False)
        self.schedule()

    def select_mixer_band(self, event=None):
        index = COLOR_BANDS.index(self.mixer_band.get())
        for name, (variable, display, _) in self.mixer_controls.items():
            amount = self.mixer_settings[name][index]
            variable.set(amount)
            display.set(f'{amount:.2f}')

    def schedule(self, delay=150):
        if self.closed or self.path is None:
            return
        # Invalidate the old display immediately; an in-flight result must not
        # appear during the debounce window after the controls have changed.
        self.revision = -1
        if self.debounce is not None:
            self.root.after_cancel(self.debounce)
        self.debounce = self.root.after(delay, self.request)

    def request(self):
        self.debounce = None
        values = {k: v[0].get() for k, v in self.controls.items()}
        view = ViewRequest(self.path, algorithm='rawengine.menon_base' if self.algorithm.get() == 'Menon' else 'rawengine.bilinear',
            **values, **{name: tuple(settings) for name, settings in self.mixer_settings.items()},
            before=self.before.get(), detail=self.detail, center=self.center,
            size=(max(1, self.canvas.winfo_width()), max(1, self.canvas.winfo_height())))
        self.revision = self.worker.submit(view)
        self.status.set('Rendering…' if self.last else 'Opening and rendering…')

    def resized(self, event):
        if self.path:
            self.schedule(250)

    def set_detail(self, detail):
        self.detail = detail
        self.drag = None
        self.schedule()

    def pick_detail(self, event):
        if not self.detail and self.image_box:
            x, y, w, h = self.image_box
            if x <= event.x <= x + w and y <= event.y <= y + h:
                self.center = ((event.x - x) / w, (event.y - y) / h)
                self.set_detail(True)

    def start_drag(self, event):
        if self.detail and self.last and self.last['roi'] is not None:
            # Derive center from the displayed clipped ROI, not an off-edge
            # requested center, so dragging remains responsive at boundaries.
            x, y, w, h = self.last['roi']
            ew, eh = self.last['extent']
            self.drag = (event.x, event.y, ((x + w / 2) / ew, (y + h / 2) / eh))

    def move_drag(self, event):
        if self.drag and self.last:
            x, y, center = self.drag
            ew, eh = self.last['extent']
            self.center = (max(0, min(1, center[0] - (event.x - x) / ew)),
                           max(0, min(1, center[1] - (event.y - y) / eh)))
            self.schedule()

    def finish_drag(self, event):
        self.move_drag(event)
        self.drag = None

    def poll(self):
        if self.closed:
            return
        try:
            while True:
                revision, kind, payload = self.worker.events.get_nowait()
                if revision != self.revision:
                    continue
                if kind == 'error':
                    self.status.set(payload.replace('\n', ' ')[:700])
                    continue
                self.last = payload
                self.photo = ImageTk.PhotoImage(payload['image'], master=self.root)
                w, h = payload['image'].size
                x = (self.canvas.winfo_width() - w) // 2
                y = (self.canvas.winfo_height() - h) // 2
                self.image_box = (x, y, w, h)
                self.canvas.delete('image')
                self.canvas.create_image(x, y, image=self.photo, anchor='nw', tags='image')
                mode = '100%' if payload['view'].detail else 'Fit'
                before = ' · Before' if payload['view'].before else ''
                opening = f" · Open + render {payload['total_ms']:.0f} ms" if payload['opening'] else ''
                self.status.set(f"{mode}{before} · {payload['extent'][0]} × {payload['extent'][1]} · Render {payload['render_ms']:.0f} ms{opening}")
        except queue.Empty:
            pass
        self.root.after(40, self.poll)

    def close(self):
        self.closed = True
        if self.debounce is not None:
            self.root.after_cancel(self.debounce)
        self.worker.close()
        self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('file', nargs='?', type=Path)
    parser.add_argument('--module-dir', type=Path, default=ROOT / 'build-msvc-release')
    parser.add_argument('--decoder-python', type=Path, default=Path(sys.executable))
    args = parser.parse_args()
    sys.path.insert(0, str(args.module_dir.resolve()))
    try:
        import rawengine_native as native
    except ImportError as error:
        parser.error('Use the Python matching the built native module and --module-dir. ' + str(error))
    root = tk.Tk()
    viewer = Viewer(root, args.decoder_python, native)
    if args.file:
        viewer.open_file(args.file)
    root.mainloop()
    viewer.worker.thread.join(5)


if __name__ == '__main__':
    main()
