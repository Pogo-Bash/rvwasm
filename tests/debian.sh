# A tour of a Debian base system, for comparing emulators: run by Debian's
# bash inside the Debian root (in a chroot, with /proc mounted). The output
# depends on nothing but the root's contents.
#
#   Linux/Wasm:  cp /rv/debian.sh /debian/tmp/ && debian 'bash /tmp/debian.sh 2>&1 | md5sum'
#                (piped inside: ls writes columns to a terminal)
#   reference:   tools/debian-reference.sh    (qemu-riscv64 in a chroot of the same root)

export LC_ALL=C TZ=UTC
cd /tmp && rm -rf check && mkdir check && cd check || exit 1
printf 'pear\napple\nfig\n' > fruit

step() { echo "== $*"; }

step system
cat /etc/debian_version
uname -m
id -u
echo "bash ${BASH_VERSION%%.*}"
getent passwd root
getconf PAGE_SIZE

step shell
s=0; for i in $(seq 1 100); do s=$((s + i)); done; echo $s
a=(x y z); printf '%s-' "${a[@]}"; echo ${#a[@]}
while read -r line; do echo "<$line>"; done <<< "one
two"
(sleep 0.1; echo child) & wait; echo parent
diff <(echo a) <(echo b); echo "diff rc=$?"
f() { local n=$1; [ $n -le 1 ] && echo 1 || echo $((n * $(f $((n - 1))))); }; f 10
case fig in f*) echo matched;; esac
echo $(( 2 ** 40 )) "$(printf '%05.2f' 3.14159)"
timeout 1 sleep 5; echo "timeout rc=$?"
trap 'echo trapped' USR1; kill -USR1 $$; trap - USR1

step text tools
sort fruit | tr a-z A-Z | head -n 2
sort -r fruit
grep -c e fruit
sed -n 's/^\(p.\)\(.*\)$/\2\1/p' fruit
awk 'BEGIN { for (i = 1; i <= 10; i++) s += i * i; printf "%d %.4f\n", s, sqrt(s) }'
awk '{ print length($0), $0 }' fruit | sort -n
cut -c2-3 fruit | paste -sd, -
printf '1\n2\n3\n' | xargs -n 1 echo n
echo hello | base64 | tr A-Z a-z | rev
wc -l -c fruit
seq 1 2000 | sort -rn | tail -n 1
printf 'b\na\nb\n' | sort | uniq -c | tr -s ' '

step files
cp fruit a && cmp fruit a && mv a b && ls b && rm b
ln -sf fruit l && readlink l && rm l
f=$(mktemp -p .) && echo x > $f && cat $f && rm $f
stat -c '%s %F' fruit
dd if=/dev/zero bs=1k count=4 2> /dev/null | wc -c
tar cf t.tar fruit && tar tf t.tar && rm t.tar
gzip -c fruit | gzip -dc
gzip -c /usr/bin/perl | gzip -dc | sha256sum | cut -c1-16
md5sum fruit
sha256sum /usr/bin/bash | cut -c1-16
find /etc -maxdepth 1 -name 'deb*' | sort
ls /etc/apt
du -s /usr/share/zoneinfo > /dev/null && echo du ok
date -u -d @86400 +%Y-%m-%dT%H:%M

step perl
perl -e 'my %h = (a => 1, b => 2); print join(",", map { "$_=$h{$_}" } sort keys %h), "\n"; printf "%.3f\n", atan2(1, 1) * 4'
perl -e '$_ = "The quick brown fox"; print join("|", /(\w+) (\w+)/), "\n"; print scalar(reverse("abc")), "\n"'
perl -e 'my @p; N: for my $n (2..200) { for (@p) { next N unless $n % $_ } push @p, $n } print scalar(@p), " primes, last $p[-1]\n"'
perl -e 'open(my $f, "<", "/etc/passwd") or die; my $n = 0; $n++ while <$f>; print "passwd lines: $n\n"'
perl -MPOSIX -e 'print POSIX::floor(7.9), " ", POSIX::strftime("%Y", gmtime(0)), "\n"'

step packages
dpkg --version | head -n 1
dpkg-query -W | wc -l
dpkg -s bash | sed -n '1,3p'
dpkg -L coreutils | grep -c /usr/bin/
dpkg --print-architecture
apt-get --version | head -n 1
apt-config shell ARCH APT::Architecture
# apt maps 40 MB for its package cache: more memory than rv starts a guest with.
apt-cache policy 2>&1 | head -n 1

cd /tmp && rm -rf check
echo "== done"
