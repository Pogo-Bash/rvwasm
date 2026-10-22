#!/bin/bash
# What the store of translated code (rv/jitcache.c) buys: programs started
# cold and warm. Run inside Debian on the machine, from the page with a disk
# (needs gcc, g++, python3):
#
#   tools/with-browser.sh WHO node tools/bench-browser.js sh \
#     'cp /rv/jitcache-warm.sh /debian/root/; debian "bash /root/jitcache-warm.sh first"' 3000000 --disk daily \
#     --then 'debian "bash /root/jitcache-warm.sh again"'
#
# "first" (a machine whose store is empty: a new browser profile, or
# ?fresh-jitcache):
#   off1  every program once with the store off (RV_JIT_CACHE=0): cold;
#   off2  again, store off: what the engine keeps of modules it compiled for
#         the same bytes before (translations have no addresses of a
#         process's in them, so a second process makes the same bytes);
#   fill  with the store, which is empty: the cost of keeping;
#   hit   with the store, which now has the regions: warm.
# "again" (the page loaded again, the store as the page kept it):
#   kept  with the store as it was kept: warm, but every module compiled
#         anew by this new engine;
#   cold  store off: cold, for comparison in the same page load.
# Before either, the programs' files are read once, so the disk is not what
# is measured. Prints "WARM round program ms" lines and a summary.

cd /root || exit 1
printf '#include <iostream>\n#include <map>\n#include <string>\nint main() { std::map<std::string, int> m{{"c++", 20}}; std::cout << "c++ " << m.size() << "\\n"; }\n' > jw.cc
printf '#include <stdio.h>\nint main(void) { printf("c\\n"); return 0; }\n' > jw.c

p_python() { python3 -c 'import json, hashlib, sqlite3, re; print(len(json.dumps(list(range(20000)))))'; }
p_gcc() { gcc -O2 jw.c -o jw-c; }
p_gxx() { g++ -O2 -std=c++20 jw.cc -o jw-cxx; }
p_small() { for i in 1 2 3 4 5 6 7 8 9 10; do ls /usr/bin > /dev/null; grep -c root /etc/passwd > /dev/null; sed -n 1p /etc/passwd > /dev/null; done; }
PROGRAMS="python gcc gxx small"

timed() {
	local s e
	s=$(date +%s%N)
	"$@" > /dev/null 2>&1
	e=$(date +%s%N)
	echo $(( (e - s) / 1000000 ))
}

round() {	# name, environment
	local name=$1 p ms
	shift
	for p in $PROGRAMS; do
		ms=$(env "$@" bash -c "$(declare -f p_$p timed); timed p_$p")
		echo "WARM $name $p $ms"
	done
}

# The files, read once (and the C++ headers).
tar cf /dev/null /usr/bin/python3* /usr/lib/python3* /usr/bin/gcc* /usr/bin/g++* /usr/bin/riscv64-linux-gnu-* \
	/usr/libexec/gcc /usr/lib/gcc /usr/include /usr/lib/riscv64-linux-gnu 2> /dev/null

case "$1" in
first)
	round off1 RV_JIT_CACHE=0
	round off2 RV_JIT_CACHE=0
	round fill RV_JIT_NOTHING=1
	round hit RV_JIT_NOTHING=1
	;;
again)
	round kept RV_JIT_NOTHING=1
	round cold RV_JIT_CACHE=0
	;;
image)	# (for tools/jitcache-image.sh: every program twice with the store, the times do not matter)
	round image1 RV_JIT_NOTHING=1
	round image2 RV_JIT_NOTHING=1
	;;
esac
