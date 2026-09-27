"""Check a D3D12 lab stream against the pictures it was made from.

    python hevc-psnr.py <dump.hevc> <input.nv12> <codedW>x<codedH> <W>x<H>

The files come from `mw-d3d12-lab encode --dump <dump.hevc> --dump-input
<input.nv12>`: the stream, and the eight input pictures as raw NV12 at the
coded size, frame n of the stream being input n % 8. ffmpeg decodes the stream
(its conformance window crops it to W x H); numpy compares each decoded
picture's luma with its input.

Why not ffmpeg's error count: a stream can decode "without error" and be wrong
in every picture. Over a coded size that ended inside a coding tree block, the
Arc's and the AMD iGPU's 1440p streams decoded wrong from the last row of
blocks on in every picture, and ffmpeg flagged a few in a thousand
(27/09/2026). And why not ffmpeg's psnr filter: it pairs frames by timestamp,
and paired one frame in 25 with the wrong input there.

Prints, per frame, the whole picture's PSNR and its worst 64-row band's:
minimum, median, and the first and last hundred frames' means. A stream decoded
as it was coded holds the same PSNR from its first frame to its last; a wrong
one falls to noise in the band where it goes wrong (under 20 dB, listed), and
a drift shows as a fall from the first hundred to the last.

Needs ffmpeg on the PATH and numpy. Exit status: 1 if any band is under 20 dB.
"""
import subprocess
import sys

import numpy as np

BAND = 64
FLOOR_DB = 20.0


def size(text):
    w, h = text.lower().split('x')
    return int(w), int(h)


def psnr(a, b):
    mse = np.mean((a - b) ** 2)
    return 99.0 if mse == 0 else float(10 * np.log10(255 * 255 / mse))


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    dump, nv12 = sys.argv[1], sys.argv[2]
    (cw, ch), (w, h) = size(sys.argv[3]), size(sys.argv[4])
    raw = np.fromfile(nv12, dtype=np.uint8)
    picture = cw * ch * 3 // 2
    count = len(raw) // picture
    if count == 0 or len(raw) % picture:
        print(f'{nv12} is not a whole number of {cw}x{ch} NV12 pictures')
        return 2
    inputs = [raw[k * picture:k * picture + cw * ch].reshape(ch, cw)[:h, :w].astype(np.float64)
              for k in range(count)]
    decoded = subprocess.run(['ffmpeg', '-v', 'error', '-threads', '1', '-i', dump, '-f',
                              'rawvideo', '-pix_fmt', 'gray', '-'], capture_output=True)
    errors = [l for l in decoded.stderr.decode(errors='replace').splitlines() if l.strip()]
    frames = np.frombuffer(decoded.stdout, dtype=np.uint8)
    if frames.size % (w * h):
        print(f'the decoded pictures are not {w}x{h}')
        return 2
    frames = frames.reshape(-1, h, w)

    whole, worst, rows = [], [], []
    for n, f in enumerate(frames):
        d = f.astype(np.float64)
        ref = inputs[n % count]
        whole.append(psnr(d, ref))
        bands = [psnr(d[r:r + BAND], ref[r:r + BAND]) for r in range(0, h, BAND)]
        worst.append(min(bands))
        rows.append(bands.index(min(bands)) * BAND)
    whole, worst = np.array(whole), np.array(worst)
    print(f'{dump}: {len(frames)} frames, {len(errors)} ffmpeg error lines')
    for name, v in (('picture', whole), (f'worst {BAND}-row band', worst)):
        print(f'  {name}, luma PSNR: min {v.min():.1f}, median {np.median(v):.1f} dB; '
              f'first 100 frames {v[:100].mean():.1f}, last 100 {v[-100:].mean():.1f}')
    bad = [n for n in range(len(worst)) if worst[n] < FLOOR_DB]
    if bad:
        where = sorted(set(rows[n] for n in bad))
        print(f'  {len(bad)} frames with a band under {FLOOR_DB:.0f} dB: the first is frame '
              f'{bad[0]}, the bands start at rows {where[:8]}')
        return 1
    print(f'  no band under {FLOOR_DB:.0f} dB')
    return 0


if __name__ == '__main__':
    sys.exit(main())
