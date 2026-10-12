#!/usr/bin/env python3
# Where the time went in a profile of every Worker of the page (tools/desktop-drive.js --profile US with
# profstart / profstop:FILE.json), with translated code named by the guest code it is from (RV_JIT_NAMES=1:
# jPID_DEV:INODE_OFFSET, jPID_mPC). Run in the build VM (it needs riscv64-linux-gnu-nm and the Debian root).
#
#   tools/chromeprof.py [--files FILES] [--root R] [--debug DIR]... [--top N] [--pid PID] PROFILE.json
#
# FILES: lines "DEV:INODE PATH" (build/www/g-files.sh in the guest), for turning DEV:INODE into a file.
# --debug: directories of debug information (BUILDID.debug, or .build-id/xx/rest.debug under them).
#
# Prints: the samples by kind (translated code, the emulator's C, the kernel, JavaScript, the engine's own
# work), the time spent waiting left out; then by process (a Worker belongs to the process whose translated
# code it ran most); translated code by file and by function; the emulator's and the kernel's functions;
# and functions by inclusive samples (what is under the translator and the host's compiler, say).
import argparse, bisect, collections, json, os, re, struct, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('--files')
ap.add_argument('--root', default=os.path.expanduser('~/rvwasm/debian-root-kde'))
ap.add_argument('--debug', action='append', default=[])
ap.add_argument('--top', type=int, default=30)
ap.add_argument('--pid', type=int, default=None, help='only the Workers of this process')
ap.add_argument('profile')
args = ap.parse_args()
if not args.debug:
	args.debug = [os.path.expanduser('~/rvwasm/chromeperf/dbg/x/usr/lib/debug'), os.path.expanduser('~/rvwasm/gfx/debug')]

files = {}
if args.files:
	for line in open(args.files, errors='replace'):
		m = re.match(r'^([0-9a-f]+:[0-9a-f]+) (\S+)$', line.strip())
		if m:
			files[m.group(1)] = m.group(2)

def elf_info(path):
	with open(path, 'rb') as f:
		d = f.read(1 << 16)
	if d[:4] != b'\x7fELF' or d[4] != 2:
		return [], None
	phoff, = struct.unpack_from('<Q', d, 0x20)
	phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
	loads, bid = [], None
	with open(path, 'rb') as f:
		for i in range(phnum):
			f.seek(phoff + i * phentsize)
			p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack('<IIQQQQQQ', f.read(56))
			if p_type == 1:
				loads.append((p_offset, p_vaddr, p_filesz))
			elif p_type == 4:
				f.seek(p_offset)
				note = f.read(p_filesz)
				o = 0
				while o + 12 <= len(note):
					namesz, descsz, ntype = struct.unpack_from('<III', note, o)
					name = note[o + 12:o + 12 + namesz]
					desc_o = o + 12 + ((namesz + 3) & ~3)
					if ntype == 3 and name[:3] == b'GNU':
						bid = note[desc_o:desc_o + descsz].hex()
					o = desc_o + ((descsz + 3) & ~3)
	return loads, bid

symcache = {}
def symbols(path):
	if path in symcache:
		return symcache[path]
	host = args.root + path
	res = None
	if os.path.exists(host):
		loads, bid = elf_info(host)
		src = host
		for d in args.debug:
			for cand in ([os.path.join(d, bid + '.debug'), os.path.join(d, '.build-id', bid[:2], bid[2:] + '.debug')] if bid else []):
				if os.path.exists(cand):
					src = cand
					break
			if src != host:
				break
		syms = []
		for opt in (['--defined-only'], ['-D', '--defined-only']):
			out = subprocess.run(['riscv64-linux-gnu-nm', '-C', '-S', '--no-recurse-limit'] + opt + [src], capture_output=True, text=True).stdout
			for line in out.splitlines():
				parts = line.split(' ', 3)
				if len(parts) == 4 and parts[2] in 'TtWwi':
					syms.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
				elif len(parts) == 3 and parts[1] in 'TtWwi':
					syms.append((int(parts[0], 16), 0, parts[2]))
			if syms:
				break
		syms.sort()
		res = (loads, [s[0] for s in syms], syms, src != host)
	symcache[path] = res
	return res

def where(fileid, foff):
	path = files.get(fileid)
	if not path:
		return fileid, '?'
	lib = os.path.basename(path)
	info = symbols(path)
	if not info:
		return lib, '?'
	loads, addrs, syms, _ = info
	va = None
	for o, v, sz in loads:
		if o <= foff < o + sz:
			va = foff - o + v
	if va is None:
		return lib, '?'
	j = bisect.bisect_right(addrs, va) - 1
	if j < 0:
		return lib, '?+%x' % va
	a, size, name = syms[j]
	name = re.sub(r'\(.*$', '', name) if len(name) > 120 else name
	if size and va >= a + size:
		return lib, '%s+%x?' % (name[:100], va - a)
	return lib, name[:140]

# What a Worker does while it waits (for a lock, for its turn on a CPU, idle): not time anybody spent.
WAITING = re.compile(r'^(lock_wait|wasm_serialize_tasks|arch_cpu_idle|default_idle_call|cpu_idle_poll|do_idle|'
		     r'__wasm_futex_wait|wasm_task_wait|wait_for_turn|switch_wait|serve|onmessage disk-worker|host_callbacks\.<computed>|wasm_serialize_tasks|\(idle\)|\(program\)|Atomics\.wait|atomic_wait)( |$)')
JNAME = re.compile(r'^j(\d+)_(?:([0-9a-f]+:[0-9a-f]+)_([0-9a-f]+)|m([0-9a-f]+))$')

def kind(fn, url):
	if JNAME.match(fn) or (url == 'wasm:' and fn in ('r', 'reserve')):
		return 'translated'
	if url.startswith('wasm:rv'):
		return 'rv'
	if 'vmlinux' in url:
		return 'kernel'
	if url.startswith('wasm:'):
		return 'wasm:' + url[5:]
	if fn == '(garbage collector)':
		return 'gc'
	if fn in ('(program)', '(idle)', '(root)'):
		return fn
	return 'js'

prof = json.load(open(args.profile))
workers = prof['workers']
interval = prof.get('interval_us', 1000) / 1e6

def split(key):
	fn, _, url = key.rpartition(' ')
	return fn, url

# Each Worker's process: the one whose translated code it ran most.
for w in workers:
	c = collections.Counter()
	for key, n in w['self'].items():
		m = JNAME.match(split(key)[0])
		if m:
			c[int(m.group(1))] += n
	w['pid'] = c.most_common(1)[0][0] if c else None
if args.pid is not None:
	workers = [w for w in workers if w['pid'] == args.pid]

kinds = collections.Counter()
by_proc = collections.defaultdict(collections.Counter)
tfile = collections.Counter()
tfn = collections.Counter()
rvfn = collections.Counter()
kfn = collections.Counter()
other = collections.Counter()
waiting = collections.Counter()
incl = collections.Counter()
busy = 0
for w in workers:
	for key, n in w['self'].items():
		fn, url = split(key)
		if WAITING.match(key):
			waiting[fn] += n
			continue
		k = kind(fn, url)
		if k in ('(program)', '(idle)', '(root)'):
			waiting[fn] += n
			continue
		busy += n
		kinds[k] += n
		by_proc[w['pid']][k] += n
		if k == 'translated':
			m = JNAME.match(fn)
			if m and m.group(2):
				lib, sym = where(m.group(2), int(m.group(3), 16))
			elif m:
				lib, sym = '[made by the program]', '?'
			else:
				lib, sym = '[' + fn + ']', ''
			tfile[lib] += n
			tfn[(lib, sym)] += n
		elif k == 'rv':
			rvfn[fn] += n
		elif k == 'kernel':
			kfn[fn] += n
		else:
			other[(k, fn)] += n
	for key, n in w['incl'].items():
		fn, url = split(key)
		k = kind(fn, url)
		if k in ('rv', 'kernel', 'js', 'gc') and not WAITING.match(key):
			incl[(k, fn)] += n

def pct(n, of=None):
	return '%5.1f%%' % (100.0 * n / (of or busy or 1))

print('%d Workers, %d busy samples (%.1f s of CPU at %d us), %d waiting' % (len(workers), busy, busy * interval,
      prof.get('interval_us', 1000), sum(waiting.values())))
for k, n in kinds.most_common():
	print('  %s  %s' % (pct(n), k))
print('by process (Workers counted by the translated code they ran; None: no translated code):')
for pid, c in sorted(by_proc.items(), key=lambda x: -sum(x[1].values()))[:12]:
	t = sum(c.values())
	print('  %s  pid %s (%.1f s): %s' % (pct(t), pid, t * interval, ', '.join('%s %s' % (k, pct(n, t).strip()) for k, n in c.most_common(5))))
print('translated code by file:')
for lib, n in tfile.most_common(15):
	print('  %s  %s' % (pct(n), lib))
print('translated code by function (region start):')
for (lib, sym), n in tfn.most_common(args.top):
	print('  %s  %s  %s' % (pct(n), lib, sym))
print("the emulator's C, own samples:")
for fn, n in rvfn.most_common(args.top):
	print('  %s  %s' % (pct(n), fn))
print('the kernel, own samples:')
for fn, n in kfn.most_common(args.top // 2):
	print('  %s  %s' % (pct(n), fn))
print('other (JavaScript, the engine):')
for (k, fn), n in other.most_common(args.top // 2):
	print('  %s  %s %s' % (pct(n), k, fn))
print('inclusive (the function anywhere in the stack):')
for (k, fn), n in incl.most_common(args.top):
	print('  %s  %s %s' % (pct(n), k, fn))
print('waiting (left out):')
for fn, n in waiting.most_common(8):
	print('  %8d  %s' % (n, fn))
