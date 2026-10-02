"""Private file-unpack bridge for the local viewer; run in the rawpy runtime."""
import argparse
import json
from pathlib import Path

from raw_camera_extract import sensor_metadata


def decode(path, output):
    import numpy as np
    import rawpy
    from PIL import Image

    if path.suffix.lower() != '.nef':
        raise ValueError('This test viewer currently supports Nikon NEF files.')
    with Image.open(path) as image:
        exif = image.getexif()
        orientation = int(exif.get(274, 1))
    if orientation not in range(1, 9):
        raise ValueError('Unsupported file orientation.')
    with rawpy.imread(str(path)) as raw:
        metadata, _, _ = sensor_metadata(raw)
        wb = [float(v) for v in raw.camera_whitebalance[:3]]
        if len(wb) != 3 or not all(np.isfinite(v) and v > 0 for v in wb):
            raise ValueError('File has no usable as-shot RGB white balance.')
        np.ascontiguousarray(raw.raw_image, dtype='<u2').tofile(output / 'bayer.bin')
        record = dict(metadata=metadata, camera_whitebalance=wb, orientation=orientation,
                      decoder_rgb_xyz_matrix_uninterpreted=raw.rgb_xyz_matrix.tolist())
    (output / 'metadata.json').write_text(json.dumps(record, allow_nan=False), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('file', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    decode(args.file, args.output)
