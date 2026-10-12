# Code that a program makes and changes while it runs (tests/mm/selfmod.c),
# under rv's translator on Linux/Wasm: what it prints must be what it prints
# on a system known to be right (/rv/selfmod.expected, made when staging).
#
#   PATH=/rvbin /rvbin/sh /rv/selfmod.sh
#
# As it comes; with every translated block compared with the interpreter
# (RV_JIT_CHECK, which also ends the program if a block is run that is no
# longer the code it was translated from); with the translator eager, so
# that everything is translated the moment it has run twice; and without the
# translator. The cases with threads are in all of them, but RV_JIT_CHECK
# only compares while the program has one thread.

T=/tmp/selfmod.$$
mkdir -p $T && cd $T || exit 1
fail=0

run() {
	name="$1"
	shift
	env "$@" /rv/selfmod > out 2>&1
	env "$@" /rv/selfmod twice >> out 2>&1
	if cmp -s out /rv/selfmod.expected; then
		echo "ok    $name"
	else
		echo "FAIL  $name"
		diff /rv/selfmod.expected out | head -n 20
		fail=1
	fi
}

run "translator" RV_X=1
run "translator, checked" RV_JIT_CHECK=1
run "translator, eager" RV_JIT_HOT=3 RV_JIT_HOT_MADE=2
run "translator, eager, checked" RV_JIT_CHECK=1 RV_JIT_HOT=3 RV_JIT_HOT_MADE=2
run "translator, code the program made no sooner than a file's" RV_JIT_HOT_MADE=200
run "interpreter" RV_JIT=0
cd / && rm -rf $T
[ $fail = 0 ] && echo "selfmod: all the same" || echo "selfmod: FAILED"
