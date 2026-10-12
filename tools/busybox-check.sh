#!/bin/bash
# Run a battery of riscv64 BusyBox commands under qemu-riscv64 and under rv,
# and compare standard output, standard error and exit status.
#
# Usage: busybox-check.sh RV BUSYBOX [NAME=VALUE...]
#
# Extra NAME=VALUE arguments are added to the environment of every command,
# e.g. RV_FORK=snapshot to exercise fork-by-snapshot on a host that can fork.
#
# Each command runs in a fresh copy of a small fixture directory, once per
# emulator, with the same minimal environment. In a command, BB stands for
# "the emulator running BusyBox", so pipelines connect emulated applets.

set -u

RV="$(realpath "$1")"
BUSYBOX="$(realpath "$2")"
QEMU="${QEMU:-qemu-riscv64}"
shift 2
extra_env=("$@")

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/fixture/dir/sub"
(
	cd "$work/fixture"
	printf 'pear\napple\nfig\napple\ncherry\n' > fruit.txt
	seq 1 2000 > numbers.txt
	printf 'name,qty\nbolt,12\nnut,7\nwasher,30\n' > parts.csv
	head -c 300000 /dev/zero | tr '\0' 'x' > big.txt
	echo nested > dir/sub/deep.txt
	echo top > dir/file.txt
	ln -s fruit.txt link.txt
	chmod 640 parts.csv
	# Fixed timestamps, so that listings do not depend on when the test ran.
	find . -exec touch -h -d '2024-01-02 03:04:05 UTC' {} +
)

tests=(
	'BB echo hello world'
	'BB printf "%5d|%-6s|%x\n" 42 ab 255'
	'BB uname -s -m'
	'BB pwd | BB sed "s|.*/||"'
	'BB true; echo $?'
	'BB false; echo $?'
	'BB sh -c "exit 7"; echo $?'
	'BB ls'
	'BB ls -la'
	'BB ls -lR dir'
	'BB cat fruit.txt'
	'BB cat nonexistent'
	'BB wc numbers.txt fruit.txt'
	'BB head -n 3 numbers.txt; BB tail -n 2 numbers.txt'
	'BB sort fruit.txt | BB uniq -c'
	'BB sort -rn numbers.txt | BB head -n 4'
	'BB cut -d, -f2 parts.csv | BB tr "\n" " "'
	'BB grep -n -c apple fruit.txt; BB grep -r nested .'
	'BB sed -n "2,3p;s/apple/APPLE/p" fruit.txt'
	'BB awk -F, "NR>1 { n += \$2 } END { printf \"%d %.3f\n\", n, n / 7 }" parts.csv'
	'BB find . -type f | BB sort'
	'BB find . -name "*.txt" -size +100k'
	'BB md5sum fruit.txt big.txt'
	'BB sha256sum numbers.txt'
	'BB cksum big.txt'
	'BB seq 1 10 | BB paste -s -d+ | BB bc'
	'BB expr 6 "*" 7; BB expr length abcdef'
	'echo "2 10 ^ p 3 k 22 7 / p" | BB dc'
	'BB basename /a/b/c.txt .txt; BB dirname /a/b/c.txt'
	'BB date -u -d @86400 +%Y-%m-%dT%H:%M:%S'
	'BB id -u; BB id -g; BB id -G'
	'BB env | BB grep -v "^_=" | BB sort'
	'BB test -f fruit.txt && echo file; BB test -d dir && echo dir; BB test -L link.txt && echo link'
	'BB stat -c "%n %s %a %F" fruit.txt parts.csv dir link.txt'
	'BB readlink link.txt; BB realpath dir/sub/../file.txt | BB sed "s|.*/dir|dir|"'
	'BB du -s dir | BB cut -f2; BB du -ab big.txt'
	'BB od -A x -t x1 fruit.txt | BB head -n 2'
	'BB hexdump -C parts.csv | BB tail -n 2'
	'BB mkdir -p new/a/b && BB touch new/a/b/f && BB find new | BB sort'
	'BB cp -r dir copy && BB mv copy moved && BB rm -r dir && BB ls'
	'BB ln -s numbers.txt l2 && BB ln fruit.txt hard && BB ls -1 && BB stat -c %h fruit.txt'
	'BB chmod 600 fruit.txt && BB stat -c %a fruit.txt'
	'BB touch -d "2020-05-06 07:08:09" fruit.txt && BB stat -c %Y fruit.txt'
	'BB truncate -s 10 big.txt && BB wc -c big.txt'
	'BB tar cf t.tar dir && BB tar tf t.tar | BB sort && BB mkdir x && BB tar xf t.tar -C x && BB cat x/dir/sub/deep.txt'
	'BB gzip -c numbers.txt | BB gunzip -c | BB md5sum'
	'BB gzip big.txt && BB ls && BB zcat big.txt.gz | BB wc -c'
	'BB dd if=numbers.txt bs=100 skip=2 count=1 2>/dev/null'
	'BB tee out.txt < fruit.txt | BB wc -l; BB cat out.txt | BB wc -l'
	'BB xxd -l 16 fruit.txt'
	'BB sleep 0.05; echo slept'
	'BB sh -c "seq 1 20 | grep 7 | wc -l; ls | head -n 2; echo \$(basename /x/y) \`expr 2 + 3\`"'
	'BB sh -c "sleep 0.05 & wait; echo bg done; false || echo rc=\$?; (exit 3); echo sub=\$?"'
	'BB sh -c "cat fruit.txt | while read w; do echo -n \$w,; done; echo; nosuchcmd; echo rc=\$?"'
	'BB xargs -n 2 echo pair: < fruit.txt'
	'BB find . -name "*.csv" -exec BUSYBOX wc -l {} ";"'
	'BB sh -c "for i in 1 2 3; do x=\$((x + i * i)); done; echo \$x; case abc in a*) echo match;; esac"'
	'BB sh -c "cd dir && pwd | sed s,.*/,, ; echo \${PATH:-unset}; f() { echo fn:\$1; }; f arg"'
	'BB sh -c "read a b < parts.csv; echo [\$a] [\$b]"'
	'BB sh -c "exec BUSYBOX echo exec works"'
	'BB env FOO=bar BUSYBOX sh -c "echo \$FOO"'
	'BB df . | BB wc -l'
)

run() { # emulator, command template
	local dir="$work/run"
	rm -rf "$dir"
	cp -a "$work/fixture" "$dir"
	local cmd="${2//BUSYBOX/$BUSYBOX}"
	cmd="${cmd//BB/$1 $BUSYBOX}"
	(
		cd "$dir" || exit 99
		# A hang is a failure, not a reason to stall the whole run.
		env -i PATH=/usr/bin:/bin HOME=/nonexistent TZ=UTC "${extra_env[@]}" timeout -k 1 20 bash -c "$cmd" 2>&1
		echo "[exit $?]"
	)
}

pass=0
fail=0
for t in "${tests[@]}"; do
	expected="$(run "$QEMU" "$t")"
	actual="$(run "$RV" "$t")"
	if [ "$expected" == "$actual" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL: $t"
		diff <(echo "$expected") <(echo "$actual") | head -8 | sed 's/^/    /'
	fi
done

echo "busybox: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
