#!/bin/sh
# Inside the chroot, as a client of `desktop wayland`: start Konsole the way the launcher does (the Exec of org.kde.konsole.desktop) and
# say what it printed and how it ended. tests/wasmdyn/check-default.sh SCENARIO=konsole [PROG=kate|dolphin].
P=${PROG:-konsole}
echo "KCHK-ENV: PATH=$PATH"
echo "KCHK-EXEC: $(grep -m1 '^Exec=' /opt/nq-shared/share/applications/org.kde.$P.desktop 2> /dev/null)"
ls -la /usr/bin/perl /opt/nq-shared/perl 2>&1 | head -4 | sed 's/^/KCHK-PERL: /'
/opt/nq-shared/bin/nqs-run $P > /tmp/k.out 2>&1 &
K=$!
sleep ${KCHK_WAIT:-90}
if kill -0 $K 2> /dev/null; then echo "KCHK-ALIVE: $P still running after ${KCHK_WAIT:-90} s"; else wait $K; echo "KCHK-DIED: $P exit status $?"; fi
ps | grep -E "ld-wasm|$P|bash" | grep -v grep | head -6 | sed 's/^/KCHK-PS: /'
echo KCHK-OUT-BEGIN; tail -40 /tmp/k.out; echo KCHK-OUT-END
