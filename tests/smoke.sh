# Shell smoke test for riscv64 BusyBox: process creation, pipes, signals and
# file handling, as a script whose output is the same on any correct system.
#
# Run it with PATH pointing only at riscv64 BusyBox applets, so that every
# command the shell starts is a riscv64 program:
#
#   PATH=/rvbin /rvbin/sh smoke.sh 2>&1 | md5sum                      (Linux/Wasm)
#   env -i PATH=$applets [RV_FORK=snapshot] rv busybox sh smoke.sh 2>&1 | md5sum   (natively)
#
# where $applets is a directory of links to the riscv64 BusyBox. The hash must
# be the same in all three cases.
#
# No dates, process ids, user names or absolute paths appear in the output.

set -u
dir=smoke.$$
mkdir "$dir" && cd "$dir" || exit 1

echo "== pipelines"
seq 1 200 | sort -rn | head -n 3 | tr '\n' ' '; echo
seq 1 50 | awk '{ s += $1 * $1 } END { print s }'
printf 'pear\napple\nfig\napple\n' | sort | uniq -c | sed 's/^ *//'
echo "status: $(true | false; echo $?) $(false | true; echo $?)"

echo "== command substitution and loops"
total=0
for n in $(seq 1 10); do total=$((total + n)); done
echo "total=$total words=$(echo a b c d | wc -w)"
i=0; while [ $i -lt 3 ]; do i=$((i + 1)); echo -n "$i:$(expr $i \* $i) "; done; echo

echo "== subshells and exit status"
(exit 7); echo "subshell=$?"
(cd / && pwd)
sh -c 'exit 42'; echo "child=$?"
nosuchcommand 2>/dev/null; echo "missing=$?"

echo "== here-document and files"
cat > data.txt <<EOF
name,qty
bolt,12
nut,7
washer,30
EOF
awk -F, 'NR > 1 { n += $2 } END { print "sum=" n }' data.txt
grep -c , data.txt
cp data.txt copy.txt && cmp data.txt copy.txt && echo "copy identical"
mkdir -p a/b/c && touch a/b/c/leaf && find a | sort | tr '\n' ' '; echo
ln -s data.txt link && readlink link
wc -c < data.txt

echo "== archives"
tar cf t.tar data.txt a && tar tf t.tar | sort | tr '\n' ' '; echo
gzip -c data.txt | gunzip -c | md5sum | cut -c1-32
seq 1 3000 | gzip | gunzip | wc -l

echo "== background jobs and wait"
(sleep 0.2; echo late > late.txt) &
(echo early > early.txt) &
wait
cat early.txt late.txt | tr '\n' ' '; echo
sleep 0.1 & pid=$!
wait $pid; echo "waited=$?"

echo "== signals"
trap 'echo "caught TERM"' TERM
kill -TERM $$
trap - TERM
trap 'echo "caught USR1"' USR1
(kill -USR1 $$) ; sleep 0.1
echo "after traps"
sh -c 'kill -KILL $$'; echo "killed=$?"
timeout 1 sh -c 'while :; do :; done'; echo "timeout=$?"
sh -c 'trap "echo child exiting; exit 5" INT; kill -INT $$; echo not reached'; echo "trap exit=$?"

echo "== xargs and find -exec"
printf 'one\ntwo\nthree\n' | xargs -n 1 echo item:
find a -type f -exec basename {} \;

cd .. && rm -r "$dir" && echo "== done"
