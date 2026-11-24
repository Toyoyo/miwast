"""Compressed STE DMA conversions of a whole song, decoded on the fly by
sndh_stream.s into a ring buffer that the DMA loops over.

  rice   lossless: order-2 predictor (2*x[n-1] - x[n-2]) + Rice codes, best k
         per block of 256 samples. Decodes to exactly the 8-bit samples.
  adpcm  4-bit IMA-style ADPCM (lossy, half the raw size).

Normally run through convert.sh, which assembles the result with build.sh:

  convert.sh rice|adpcm <input audio> <output.sndh> [--title "Title"]

Direct use:
  python convert_codec.py <input audio> rice|adpcm [--outdir DIR] [--title T] [--preview WAV]

  <input audio>  any file librosa can read (wav, flac, ogg, mp3, ...)
  --outdir       where the assembler inputs go (default: current directory):
                 packed.bin, adpcmtab.bin, codec.inc, header.bin
  --title        SNDH title (default: from the file name)
  --preview      also write the decoded 8-bit result as a WAV, to listen to
"""
import sys, os, argparse, numpy as np, librosa, soundfile as sf

ap = argparse.ArgumentParser()
ap.add_argument('input')
ap.add_argument('codec', choices=['rice', 'adpcm'])
ap.add_argument('--outdir', default='.')
ap.add_argument('--title')
ap.add_argument('--preview')
args = ap.parse_args()
codec, path = args.codec, args.input
out = lambda f: os.path.join(args.outdir, f)
# remove previous outputs first, so a failed conversion can't be built from stale data
for f in ('packed.bin', 'codec.inc', 'header.bin', 'adpcmtab.bin'):
    if os.path.exists(out(f)):
        os.remove(out(f))
if not os.path.isfile(path):
    sys.exit(f'no such file: {path}')
os.makedirs(args.outdir, exist_ok=True)
song = os.path.splitext(os.path.basename(path))[0]
title = args.title or song.replace('-', ' ').replace('_', ' ').title()
DSR, BLK = 12517, 256

src = librosa.load(path, sr=DSR, mono=True)[0]
if not np.abs(src).max():
    sys.exit(f"{path}: input is silent")
src /= np.abs(src).max() / 0.98          # peak-normalise
pcm = np.round(src * 127).astype(np.int8)
seconds = int(np.ceil(len(pcm) / DSR))          # before padding
pcm = np.concatenate([pcm, np.zeros(-len(pcm) % BLK, np.int8)])   # whole blocks
N = len(pcm)

# ---------------------------------------------------------------- rice
def rice_encode(p):
    x = p.astype(np.int32)
    pred = np.zeros(N, np.int32)
    pred[1] = 2 * x[0]
    pred[2:] = 2 * x[1:-1] - x[:-2]
    r = x - pred
    u = np.where(r >= 0, 2 * r, -2 * r - 1)
    ub = u.reshape(-1, BLK)
    cost = np.stack([((ub >> k) + 1 + k).sum(1) for k in range(8)])
    ks = np.argmin(cost, 0)                       # best k per block
    k = np.repeat(ks, BLK)
    q = u >> k
    # bit layout per block: 3-bit k, then per sample q zeros, a one, k bits of u
    nbits = q + 1 + k + np.where(np.arange(N) % BLK == 0, 3, 0)
    start = np.concatenate([[0], np.cumsum(nbits)[:-1]])
    total = int(nbits.sum())
    bits = np.zeros(total + 64, np.uint8)
    hs = start[::BLK]
    for j in range(3):
        bits[hs + j] = (ks >> (2 - j)) & 1
    s0 = start + np.where(np.arange(N) % BLK == 0, 3, 0)
    bits[s0 + q] = 1
    for j in range(7):                            # k bits, MSB first
        m = k > j
        bits[(s0 + q + 1 + j)[m]] = (u[m] >> (k[m] - 1 - j)) & 1
    data = np.packbits(bits[:total])
    data = np.concatenate([data, np.zeros(-len(data) % 4 + 4, np.uint8)])
    return data.tobytes(), total

def rice_decode(data):
    bits = np.unpackbits(np.frombuffer(data, np.uint8))
    out = np.empty(N, np.int32)
    pos, x1, x2 = 0, 0, 0
    ones = np.flatnonzero(bits)                   # positions of 1 bits
    oi = 0
    for b in range(N // BLK):
        k = int(bits[pos]) << 2 | int(bits[pos + 1]) << 1 | int(bits[pos + 2]); pos += 3
        for n in range(b * BLK, (b + 1) * BLK):
            while ones[oi] < pos: oi += 1
            q = int(ones[oi]) - pos; pos += q + 1
            v = 0
            for _ in range(k):
                v = v << 1 | int(bits[pos]); pos += 1
            u = q << k | v
            r = u >> 1 if not u & 1 else ~(u >> 1)
            x = 2 * x1 - x2 + r
            out[n] = x; x2, x1 = x1, x
    return out.astype(np.int8)

# ---------------------------------------------------------------- adpcm
STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
         50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
         253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
         1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
         3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
         11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
         32767]
IDXADJ = [-1, -1, -1, -1, 2, 4, 6, 8]
DIFF = [[min(32767, (s >> 3) + (s if c & 4 else 0) + (s >> 1 if c & 2 else 0) + (s >> 2 if c & 1 else 0))
         for c in range(8)] for s in STEPS]
NXT = [[min(88, max(0, i + IDXADJ[c])) for c in range(8)] for i in range(89)]

def adpcm_table():
    """Per (index, code&7): diff.w, byte offset of the next index's row.w"""
    t = bytearray()
    for i in range(89):
        for c in range(8):
            t += DIFF[i][c].to_bytes(2, 'big') + (NXT[i][c] * 32).to_bytes(2, 'big')
    return bytes(t)

def adpcm_step(pred, idx, code):
    d = DIFF[idx][code & 7]
    pred = max(-32768, min(32767, pred - d if code & 8 else pred + d))
    return pred, NXT[idx][code & 7]

def adpcm_encode(x):
    tgt = np.round(x * 32767).astype(int).tolist()
    pred, idx, codes = 0, 0, []
    for t in tgt:
        best = None
        for code in range(16):                    # exhaustive: closest next value
            p, _ = adpcm_step(pred, idx, code)
            e = abs(p - t)
            if best is None or e < best[0]:
                best = (e, code)
        codes.append(best[1])
        pred, idx = adpcm_step(pred, idx, best[1])
    c = np.array(codes, np.uint8)
    return (c[0::2] << 4 | c[1::2]).tobytes()

def adpcm_decode(data):
    pred, idx, out = 0, 0, []
    for b in data:
        for code in (b >> 4, b & 15):
            pred, idx = adpcm_step(pred, idx, code)
            out.append(pred >> 8)
    return np.array(out, np.int8)

# ---------------------------------------------------------------- output
if codec == 'rice':
    data, total = rice_encode(pcm)
    dec = rice_decode(data)
    assert np.array_equal(dec, pcm), 'rice round trip failed'
    inc = f'NSAMP\tequ\t{N}\nNBLOCKS\tequ\t{N // BLK}\n'
    print(f'rice: {total / N:.2f} bit/sample, round trip exact')
elif codec == 'adpcm':
    src16 = np.concatenate([src, np.zeros(N - len(src))])
    data = adpcm_encode(src16)
    dec = adpcm_decode(data)
    ref = pcm / 128
    snr = 10 * np.log10((ref ** 2).sum() / ((ref - dec / 128) ** 2).sum())
    inc = f'NSAMP\tequ\t{N}\nNBYTES\tequ\t{len(data)}\n'
    print(f'adpcm: SNR vs 8-bit raw {snr:.1f} dB')

open(out('packed.bin'), 'wb').write(data)
open(out('adpcmtab.bin'), 'wb').write(adpcm_table())
open(out('codec.inc'), 'w').write(inc)

def sndh_header(title, comm, seconds):
    """SNDH tags up to HDNS; pads COMM so that the TIME word lands on an even
    offset (the file starts with 12 bytes of branches)."""
    asc = lambda t: t.encode('ascii', 'replace') + b'\0'
    def tags(comm):
        return (b'SNDH' + b'TITL' + asc(title) + b'COMM' + asc(comm) +
                b'RIPP' + asc('Original conversion') + b'CONV' + asc('convert_codec.py+Claude Code') +
                b'YEAR' + asc('2026') + b'##01\0' + b'TIME')
    h = tags(comm)
    if (12 + len(h)) % 2:
        h = tags(comm + ' ')
    h += int(seconds).to_bytes(2, 'big') + b'TC50\0' + b'HDNS'
    return h + b'\0' * (len(h) % 2)

open(out('header.bin'), 'wb').write(sndh_header(
    title, 'Unknown (STE DMA, ' + ('lossless)' if codec == 'rice' else '4-bit ADPCM)'), seconds))
if args.preview:
    sf.write(args.preview, dec / 128, DSR)
print(f'{song} ({title}) {codec}: {len(data) / 1024:.0f} KB (raw {N / 1024:.0f} KB, {100 * (1 - len(data) / N):.0f}% smaller)')
