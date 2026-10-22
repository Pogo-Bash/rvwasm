#!/bin/bash
# The page's module cache (patch-runtime.py item 26) with libfoo.so, under node-machine, one machine per case:
#   1. five programs (and the threads, vfork children and posix_spawn children they start) load libfoo: compiled ONCE;
#   2. a thread that exists before dlopen cannot be handed the module (it is inside a system call's signal handler):
#      it compiles it itself - the one known exception, everything started after dlopen takes the page's;
#   3. libfoo compiled ahead (os.dl.preload, what a manifest + compileStreaming does in a page): no compile at all;
#   4. QtCore (qtcore-hello, libQt6Core.so as NEEDED): runs, and libQt6Core.so (7.4 MB) is compiled once for the main
#      thread and the QThread;
#   5. dynamic libraries off (?dl=0): a program that does not use them runs as before, one that does is refused.
# Needs `build.sh` output (make run.sh fetch it) and the dev server. Prints a line per check and "cache: N failed".
cd "$(dirname "$0")/../.."
R=tests/wasmdyn/run.sh
B=build/wasmdyn
fails=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1 (got $2, wanted $3)"; fails=$((fails + 1)); fi; }
stat() { python3 -c '
import json, sys
text = sys.stdin.read()
line = [l for l in text.splitlines() if l.startswith("DLSTATS ")][-1]
d = json.loads(line[8:]).get(sys.argv[1], {})
print(d.get(sys.argv[2], 0))' "$1" "$2"; }
$R 'true' 20000 > /dev/null 2>&1   # fetches out/ and builds the runtime
KEY=$(python3 tools/dl-hash.py $B/out/libfoo.so | cut -d' ' -f1)
echo "libfoo.so key $KEY"

OUT=$(HOOK=tests/wasmdyn/dl-hook.js $R 'cd /wasmdyn && ./ld-wasm ./main1 && ./ld-wasm ./main2 /wasmdyn/libfoo.so && ./ld-wasm ./main4 /wasmdyn/libfoo.so && ./ld-wasm ./main3 /wasmdyn/libfoo.so && echo SEQUENCE-DONE' 120000 2>&1)
echo "$OUT" | grep -q "SEQUENCE-DONE" && check "four programs ran" 1 1 || check "four programs ran" 0 1
check "libfoo compiled once for 4 programs, their threads and children" "$(echo "$OUT" | stat $KEY compile)" 1
echo "     libfoo: compile $(echo "$OUT" | stat $KEY compile), taken from the page's $(echo "$OUT" | stat $KEY shared) times"
[ "$(echo "$OUT" | stat $KEY shared)" -ge 4 ] && check "libfoo taken from the page's by at least 4 loads" 1 1 || check "libfoo taken from the page's by at least 4 loads" 0 1

OUT=$(HOOK=tests/wasmdyn/dl-hook.js $R 'cd /wasmdyn && ./ld-wasm ./main3 /wasmdyn/libfoo.so' 60000 2>&1)
echo "     main3 alone: compile $(echo "$OUT" | stat $KEY compile), shared $(echo "$OUT" | stat $KEY shared)"
check "main3 alone: the dlopening thread and the older thread compile (2), the newer takes the page's" "$(echo "$OUT" | stat $KEY compile)" 2

echo "[{\"key\": \"$KEY\", \"path\": \"$PWD/$B/out/libfoo.so\"}]" > $B/preload.json
OUT=$(HOOK=tests/wasmdyn/dl-hook.js PRELOAD=$B/preload.json $R 'cd /wasmdyn && ./ld-wasm ./main1 && ./ld-wasm ./main3 /wasmdyn/libfoo.so' 90000 2>&1)
check "preloaded libfoo: no Worker compiles it" "$(echo "$OUT" | stat $KEY compile)" 0
check "preloaded libfoo: compiled ahead once" "$(echo "$OUT" | stat $KEY preload)" 1
[ "$(echo "$OUT" | stat $KEY shared)" -ge 4 ] && check "preloaded libfoo taken from the page's by at least 4 loads" 1 1 || check "preloaded libfoo taken from the page's by at least 4 loads" 0 1

QKEY=$(python3 tools/dl-hash.py $B/out/libQt6Core.so.6 | cut -d' ' -f1)
OUT=$(HOOK=tests/wasmdyn/dl-hook.js $R 'cd /wasmdyn && ./ld-wasm ./qtcore-hello' 90000 2>&1)
for want in "Qt 6.8.2" "upper=HELLO, QT ON WASM! len=18 idx=7" "sorted=apple banana fig pear joined=apple+banana+fig+pear" "tick 1" \
	"ran off the main thread: yes, delivered on main: yes" "done rc=0 ticks>=1:yes threadValue=42"; do
	echo "$OUT" | grep -q -F "$want" && check "qtcore-hello prints '$want'" 1 1 || check "qtcore-hello prints '$want'" 0 1
done
check "libQt6Core.so compiled once (main thread and QThread)" "$(echo "$OUT" | stat $QKEY compile)" 1
check "libQt6Core.so taken from the page's by the QThread" "$(echo "$OUT" | stat $QKEY shared)" 1

# The page's way: tools/dl-manifest.py makes dl/libs.json and <key>.wasm files, served over HTTP; the runtime fetches the
# manifest and compiles the libraries with WebAssembly.compileStreaming (os.dl.preload_manifest), before the command.
rm -rf $B/dl && python3 tools/dl-manifest.py --out $B/dl $B/out/libQt6Core.so.6 $B/out/libfoo.so > /dev/null
PORT=$((20000 + RANDOM % 20000))
python3 -m http.server $PORT --bind 127.0.0.1 --directory $B/dl > /dev/null 2>&1 &
SERVER=$!
sleep 1
check "the manifest lists both libraries under the runtime's keys" "$(python3 -c "
import json; d = json.load(open('$B/dl/libs.json'))['libs']; print(sorted(l['key'] for l in d) == sorted(['$KEY', '$QKEY']))")" True
OUT=$(HOOK=tests/wasmdyn/dl-hook.js MANIFEST=http://127.0.0.1:$PORT/libs.json $R 'cd /wasmdyn && ./ld-wasm ./qtcore-hello' 90000 2>&1)
kill $SERVER 2> /dev/null; wait $SERVER 2> /dev/null
echo "$OUT" | grep -q "done rc=0" && check "manifest preload: qtcore-hello runs" 1 1 || check "manifest preload: qtcore-hello runs" 0 1
check "manifest preload: libQt6Core.so compiled ahead by streaming, once" "$(echo "$OUT" | stat $QKEY preload)" 1
check "manifest preload: no Worker compiles libQt6Core.so" "$(echo "$OUT" | stat $QKEY compile)" 0
check "manifest preload: the program's two loads take the page's" "$(echo "$OUT" | stat $QKEY shared)" 2
echo "     compileStreaming of libQt6Core.so: $(echo "$OUT" | stat $QKEY preload_us) us"

OUT=$(DL=0 $R 'cd /wasmdyn && ./hello-static; ./ld-wasm ./hello; echo end' 60000 2>&1)
echo "$OUT" | grep -q "hello" && check "dl off: a static program runs" 1 1 || check "dl off: a static program runs" 0 1
echo "$OUT" | grep -q "^end" && check "dl off: the shell is fine" 1 1 || check "dl off: the shell is fine" 0 1
echo "cache: $fails failed"
exit $fails
