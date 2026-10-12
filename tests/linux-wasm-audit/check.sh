#!/bin/bash
# The reproducers as regression tests: each case runs on a machine of its own under Node (run.sh, one at a time)
# and passes when its output matches what Linux would print. Build them first (build.sh with every NAME used
# below; cas_p with CFLAGS="-DMUSL_ATOMIC -I<musl source>/arch/wasm"); NM and EXTRA are passed on to run.sh.
#   tests/linux-wasm-audit/check.sh [CASE...]      (no CASE: all of them)
# Prints "ok CASE" or "FAIL CASE" with the output; the exit status is the number of failures.
here=$(cd "$(dirname "$0")" && pwd)

# name @@ command @@ what the output must contain (extended regular expressions, one per line) @@ timeout in ms
# [@@ more for the kernel's command line]
cases=(
	# 1: a trap with a handler runs the handler; without one (or blocked) the program dies by the signal.
	'fault @@ /audit/fault; echo rc=$? @@ ^handler ran$
^rc=7$ @@ 40000'
	'fault-blocked @@ /audit/fault x; echo rc=$? @@ ^rc=132$ @@ 40000'
	# 11: the signal says what the trap was; a handler that returns cannot resume the trap.
	'trapsig @@ for k in oob div0 unreachable; do /audit/trapsig $k; echo "$k rc=$?"; /audit/trapsig $k nohandler; echo "$k nohandler rc=$?"; done; /audit/trapsig oob return; echo "return rc=$?" @@ ^oob: handler got signal 11$
^oob nohandler rc=139$
^div0: handler got signal 8$
^div0 nohandler rc=136$
^unreachable: handler got signal 4$
^unreachable nohandler rc=132$
^return rc=139$ @@ 90000'
	# 2: SIGKILL and fatal signals reach a task that makes no system calls; exit_group ends a computing thread.
	'spin-kill @@ /audit/spin & sleep 1; kill -9 $!; sleep 1; kill -0 $! 2>/dev/null && echo STILL ALIVE || echo killed; wait $!; echo status=$? @@ ^killed$
^status=137$ @@ 40000'
	'spin-term @@ /audit/spin & sleep 1; kill $!; wait $!; echo status=$? @@ ^status=143$ @@ 40000'
	'spinexit @@ /audit/spinexit; echo exited rc=$? @@ ^main returns$
^exited rc=0$ @@ 40000'
	'spinexec @@ /audit/spinexit exec; echo rc=$? @@ ^exec ran$
^rc=0$ @@ 40000'
	# 3: SA_RESTART: the handler runs when the signal comes, the read is restarted after it.
	'restart @@ /audit/restart @@ ^SA_RESTART: read returned 1 at .*\(ok\)$ @@ 40000'
	'restart-eintr @@ /audit/restart x @@ ^no SA_RESTART: read returned -1 at .*\(ok\)$ @@ 40000'
	# 4: a signal before a new task first runs is delivered there.
	# (In a pool of one CPU, which the parent and its children share: the children never run before the signal.)
	'earlysig @@ /audit/earlysig 20 pool @@ ^children: 20 ran the handler, 0 did not, 0 were killed$ @@ 60000 @@ wasm_pool=1'
	# 5: pthread_cancel of a thread in read() and in nanosleep().
	'cancel-read @@ /audit/cancel @@ ^joined: PTHREAD_CANCELED \(ok\)$ @@ 40000'
	'cancel-sleep @@ /audit/cancel sleep @@ ^joined: PTHREAD_CANCELED \(ok\)$ @@ 40000'
	# 6: fork() fails, so that a program can fall back to posix_spawn().
	'fork @@ /audit/fork @@ ^fork failed: .* \(ok\)$ @@ 40000'
	# 7: musl's own a_cas_p (cas_p built with -DMUSL_ATOMIC).
	'cas_p @@ /audit/cas_p @@ ^cas\(0 -> 0x100001000\): returned 0, slot now 0x100001000 \(ok\)$
^cas\(0x80001000 -> 0x1000\): returned 0x80001000 \(ok\) @@ 40000'
	# 8: musl's ucontext_t is the kernel's.
	'ucontext @@ /audit/ucontext @@ ^uc_stack.ss_flags 2 \(want 2\), ss_size 0 \(want 0\)
^SIGUSR2 added to uc_sigmask is blocked after the handler \(want blocked\)$ @@ 40000'
	# 12: AT_EGID and AT_RANDOM.
	'auxv @@ /audit/auxv @@ AT_EGID 5 \(ok\); AT_RANDOM 0x[0-9a-f]+ \(ok\)$ @@ 40000'
	# 13: (passes before and after: 4 KiB pages.)
	'mmapoff @@ /audit/mmapoff @@ \(ok\)$ @@ 40000'
	# 14: int main(void).
	'mainvoid @@ /audit/mainvoid; echo rc=$? @@ ^main\(void\) ran$
^rc=0$ @@ 40000'
)

fails=0
for c in "${cases[@]}"; do
	name=${c%% @@ *}; rest=${c#* @@ }
	cmd=${rest%% @@ *}; rest=${rest#* @@ }
	append=
	case "$rest" in *" @@ "*" @@ "*) append=${rest##* @@ }; rest=${rest% @@ *};; esac
	want=${rest% @@ *}; timeout=${rest##* @@ }
	if [ $# -gt 0 ]; then
		sel=0
		for a in "$@"; do [ "$a" = "$name" ] && sel=1; done
		[ $sel = 1 ] || continue
	fi
	out=$(APPEND="$append" "$here/run.sh" "$cmd" "$timeout" 2>&1)
	ok=1
	while IFS= read -r line; do
		echo "$out" | grep -E -q -- "$line" || ok=0
	done <<< "$want"
	if [ $ok = 1 ]; then
		echo "ok $name"
	else
		echo "FAIL $name"
		echo "$out" | grep -v -E "^booted in|^\[log\]|^    at |^RuntimeError|^TypeError" | tail -8 | sed 's/^/    /'
		fails=$((fails + 1))
	fi
done
exit $fails
