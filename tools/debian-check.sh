#!/bin/bash
# Run programs from a Debian riscv64 root filesystem under qemu-riscv64 and
# under rv, and compare output and exit status. For finding out what real
# distribution binaries need that rv does not have yet. Run in the build VM.
#
#   sudo debootstrap --arch=riscv64 --variant=minbase --foreign trixie ~/rvwasm/debian-rv
#   sudo chown -R $USER ~/rvwasm/debian-rv
#   tools/debian-check.sh RV [ROOT]
#
# Both emulators see the host's file system, with the Debian root named by
# $R: programs are run by their full path, PATH is $R/usr/bin, so what a
# shell starts is Debian's too, and tests name their files under $R. (An
# absolute path such as /etc/passwd would mean Debian's file to rv -L and the
# host's to qemu-riscv64, which uses its -L for the dynamic loader only.)
# Programs a guest starts run under the same emulator: rv starts itself for a
# riscv64 program, and the host's binfmt_misc starts qemu-riscv64.
#
# rv runs with -v, so the report includes the system calls it does not implement.

set -u

RV="$(realpath "$1")"
export R="$(realpath "${2:-${RVWASM_WORK:-$HOME/rvwasm}/debian-rv}")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" "$R/tmp/check"' EXIT

pass=0
fail=0
: > "$TMP/missing"
# (The harness's own tools by their full paths: PATH is the guest's.)
ENV=(/usr/bin/env -i "PATH=$R/usr/bin" "R=$R" HOME=/nonexistent LC_ALL=C TZ=UTC)
QEMU="$(command -v qemu-riscv64)"

# check NAME PROGRAM ARGS...: PROGRAM is the name of a program in $R/usr/bin.
check() {
	local name="$1" prog="$R/usr/bin/$2"
	shift 2
	local expected actual
	expected="$(cd "$R/tmp/check" && "${ENV[@]}" "QEMU_LD_PREFIX=$R" \
		/usr/bin/timeout 60 "$QEMU" "$prog" "$@" 2>&1 < /dev/null; echo "[exit $?]")"
	actual="$(cd "$R/tmp/check" && "${ENV[@]}" "RV_SYSROOT=$R" \
		/usr/bin/timeout 60 "$RV" -v "$prog" "$@" 2> "$TMP/err" < /dev/null; echo "[exit $?]")"
	grep -h "unimplemented syscall" "$TMP/err" | sed 's/ at pc.*//' >> "$TMP/missing"
	# rv's own remarks aside, standard error counts as output (order between the two streams does not).
	actual="$(grep -v "^rv: " "$TMP/err"; echo "$actual")"
	if [ "$(echo "$expected" | sort)" == "$(echo "$actual" | sort)" ]; then
		pass=$((pass + 1))
		echo "ok    $name"
	else
		fail=$((fail + 1))
		echo "FAIL  $name"
		diff <(echo "$expected") <(echo "$actual") | head -n 8 | sed 's/^/        /'
		grep "^rv: " "$TMP/err" | grep -v "instructions\|jit:" | sed 's/ at pc.*//' | sort | uniq -c | head -n 5 | sed 's/^/        /'
	fi
}

mkdir -p "$R/tmp/check"
printf 'pear\napple\nfig\n' > "$R/tmp/check/fruit"

check "bash: echo"		bash -c 'echo hello from bash ${BASH_VERSION%%.*}'
check "bash: arithmetic, loop"	bash -c 's=0; for i in $(seq 1 100); do s=$((s + i)); done; echo $s'
check "bash: pipeline"		bash -c 'sort fruit | tr a-z A-Z | head -n 2'
check "bash: subshell, wait"	bash -c '(sleep 0.1; echo child) & wait; echo parent'
check "bash: here-string, read"	bash -c 'while read -r a; do echo "<$a>"; done <<< "one
two"'
check "bash: process substitution"	bash -c 'diff <(echo a) <(echo b); echo rc=$?'
check "bash: arrays, printf"	bash -c 'a=(x y z); printf "%s-" "${a[@]}"; echo ${#a[@]}'
check "ls -la"			bash -c 'ls -la fruit | cut -d" " -f1,5'
check "ls of a directory"	ls "$R/etc/apt"
check "cat"			cat "$R/etc/debian_version"
check "sort"			sort -r fruit
check "grep -c"			grep -c e fruit
check "sed"			sed -n 's/^\(p.\)\(.*\)$/\2\1/p' fruit
check "find"			bash -c 'cd $R/etc && find . -maxdepth 1 -name "deb*" | sort'
check "tar round trip"		bash -c 'tar cf t.tar fruit && tar tf t.tar && rm t.tar'
check "gzip round trip"		bash -c 'gzip -c fruit | gzip -dc'
check "date"			date -u -d @0 +%Y-%m-%d
check "id"			id -u
check "uname -m"		uname -m
check "stat"			stat -c '%s %F' fruit
check "wc"			wc -l -c fruit
check "md5sum"			md5sum fruit
check "sha256sum"		sha256sum fruit
check "dpkg --version"		dpkg --version
check "dpkg-deb --help"		bash -c 'dpkg-deb --help | head -n 2'
check "apt-get --version"	apt-get --version
check "apt-config"		apt-config shell ARCH APT::Architecture
check "perl"			perl -e 'my %h = (a => 1, b => 2); print join(",", map { "$_=$h{$_}" } sort keys %h), "\n"; printf "%.3f\n", atan2(1, 1) * 4'
check "perl: regex, sprintf"	perl -e '$_ = "The quick brown fox"; print join("|", /(\w+) (\w+)/), "\n"; print scalar(reverse("abc")), "\n"'
check "awk"			awk 'BEGIN { for (i = 1; i <= 10; i++) s += i * i; printf "%d %.4f\n", s, sqrt(s) }'
check "locale"			locale
check "env, printenv"		env A=1 printenv A
check "mktemp, rm"		bash -c 'f=$(mktemp -p .) && echo x > $f && cat $f && rm $f'
check "ln, readlink"		bash -c 'ln -sf fruit l && readlink l && rm l'
check "dd"			bash -c 'dd if=/dev/zero bs=1k count=4 2>/dev/null | wc -c'
check "getconf"			getconf PAGE_SIZE
check "cp, cmp, mv"		bash -c 'cp fruit a && cmp fruit a && mv a b && ls b && rm b'
check "sleep, timeout"		bash -c 'timeout 1 sleep 5; echo rc=$?'
check "xargs"			bash -c 'printf "1\n2\n3\n" | xargs -n 1 echo n'
check "base64, tr, rev"		bash -c 'echo hello | base64 | tr A-Z a-z | rev'

echo
echo "debian: $pass passed, $fail failed"
if [ -s "$TMP/missing" ]; then
	echo "system calls rv does not implement (times seen):"
	sort "$TMP/missing" | uniq -c | sort -rn | sed 's/^/  /'
fi
[ "$fail" -eq 0 ]
