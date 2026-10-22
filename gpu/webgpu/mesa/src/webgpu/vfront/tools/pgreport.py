#!/usr/bin/env python3
"""pgreport.py BASE NEW LISTFILE: compare two piglit result directories (BASE: zink on lavapipe, NEW: zink on wgvk)
and write the tests that pass in BASE and do not in NEW to LISTFILE (status, group, name, message), classified by
the wgvk gap behind them (rules below, from the failing tests' output and the server's wgvk log lines, checked by
running one test of each group alone: scripts/pg-reps.sh). Prints the groups with their counts."""
import bz2
import collections
import glob
import json
import re
import sys


def load(d):
    f = glob.glob(d + '/results.json*')[0]
    with (bz2.open(f) if f.endswith('.bz2') else open(f)) as fh:
        return json.load(fh)['tests']


def message(v):
    t = (v.get('err', '') + '\n' + v.get('out', '')).strip().splitlines()
    t = [l.strip() for l in t if l.strip() and not l.startswith('PIGLIT:') and 'artificially' not in l
         and 'Warning' not in l and not l.startswith('Test failure') and not l.startswith('Observed')]
    return (t[0] if t else '')[:100]


# (group, test-name regex, message regex): the first rule that matches wins
RULES = [
    ('server lost (a test before it killed or restarted the server)', None, r'lost connection to rendering server'),
    ('transform feedback not implemented (wgvk fakes the extension: Begin/End/BindBuffers skipped)',
     r'ext_transform_feedback|gl-3\.2-adj-prims|primitive-id-restart|tri-strip-ordering|geometry-shaders|xfb|tf-',
     None),
    ('tessellation advertised but not implemented in wgvk (vfront drops the shaders and the patch draws)',
     r'vertex-program-two-side|tessellation|tess', None),
    ('multisampled array textures and sample counts WebGPU lacks (one layer; 4 samples only)',
     r'arb_texture_multisample|arb_sample_shading|alpha-to-coverage|msaa', None),
    ('integer textures and vertex attributes: Sint texture bound as UnfilterableFloat, Sint attribute vs Float shader input',
     r'texelfetch|texturesize|isampler|usampler|vs-attrib', None),
    ('gl_ClipDistance read by the fragment shader (no clip-distance input slot in WGSL)',
     r'clip-distance|clip_distance|clipdistance', None),
    ('shadow sampling with bias or gradients (WGSL has textureSampleCompare and ...Level(0) only)',
     r'shadow|samplers', None),
    ('inter-stage variable limits (varying packing exceeds WebGPU\'s 16 locations / 60 components)',
     r'varying-packing|varying', None),
    ('1D textures are 2D in wgvk: mip level selection and LOD bias differ', r'tex-miplevel-selection', None),
    ('depth/stencil copies and blits (WebGPU copies whole subresources; separate stencil references)',
     r'stencil|depth|blit|copypixels|drawpixels|copyteximage|getteximage|sync', None),
    ('formats and limits wgvk reports differently (luminance/alpha-luminance and snorm teximage, LOD bias range, '
     'texture buffer and uniform block sizes, GS output components)',
     r'teximage-colors|texture-buffer|maxuniformblocksize|minmax|built-in constants|generatemipmap', None),
    ('layered rendering (clears of array and 3D layers)', r'layered-rendering', None),
    ('logic ops (not in WebGPU; faked)', r'xor|logicop', None),
    ('compute: storage image bound with another format than the shader declares', r'arb_compute_shader', None),
    ('geometry shader parts not compiled (see the wgvk log: clip distance, primitive restart, line loops)',
     r'glsl-1\.50@execution@geometry|gs-', None),
]


def classify(name, msg):
    for g, nre, mre in RULES:
        if (nre is None or re.search(nre, name)) and (mre is None or re.search(mre, msg)):
            return g
    return 'other (not yet attributed)'


base, new = load(sys.argv[1]), load(sys.argv[2])
rows = []
for k in sorted(new):
    b, n = base.get(k, {}).get('result'), new[k]['result']
    if b == 'pass' and n != 'pass':
        m = message(new[k])
        rows.append((classify(k, m) if n != 'skip' else 'skipped where lavapipe runs it (sample counts, image types WebGPU lacks)', n, k, m))
cnt = collections.Counter(r[0] for r in rows)
with open(sys.argv[3], 'w') as f:
    f.write('# pass on zink+lavapipe, not on zink+wgvk: group<TAB>status<TAB>test<TAB>first output line\n')
    for r in sorted(rows):
        f.write('\t'.join(r) + '\n')
tot = 0
for g, n in cnt.most_common():
    print('%5d  %s' % (n, g))
    tot += n
print('%5d  total pass -> not pass' % tot)
