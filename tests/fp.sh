# Floating point under rv's translator on Linux/Wasm: the fused multiply-add
# instructions on millions of operands (tests/mm/fma.c), which the translator
# does with the host's own instruction where the host has one, and every
# instruction of F and D in every rounding mode on the special values
# (tests/fp.c, as /rv/fp-gc). What they print must be what a system known to
# be right prints (/rv/fma.expected and /rv/fp-gc.expected, made when
# staging): with the translator as it comes, without the host's fused
# multiply-add (RV_JIT_OFF=64), with every block compared with the
# interpreter, eagerly (so that the blocks of fp-gc, which run only a few
# times each, are translated at all), and without the translator.
#
#   PATH=/rvbin /rvbin/sh /rv/fp.sh
#
# (No $(...) here: the machine's own shell has none.)

T=/tmp/fp.$$
mkdir -p $T && cd $T || exit 1
fail=0
run() {
	name="$1"
	prog="$2"
	shift 2
	env "$@" /rv/$prog > out 2>&1
	if cmp -s out /rv/$prog.expected; then
		echo "ok    $prog, $name"
	else
		echo "FAIL  $prog, $name"
		head -n 3 out
		fail=1
	fi
}
for prog in fma fp-gc; do
	run "translator" $prog RV_X=1
	run "translator, the emulator's own fused multiply-add" $prog RV_JIT_OFF=64
	run "translator, checked" $prog RV_JIT_CHECK=1
	run "translator, eager, checked" $prog RV_JIT_CHECK=1 RV_JIT_HOT=2
	run "translator, eager" $prog RV_JIT_HOT=2
	run "interpreter" $prog RV_JIT=0
done
cd / && rm -rf $T
[ $fail = 0 ] && echo "fp: all the same" || echo "fp: FAILED"
