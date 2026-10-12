#!/bin/bash
# Committed wasm code of a process, from the outside: V8 commits wasm code space as read-write-execute pages
# (16 KiB on this Mac) and decommits it again; vmmap lists every rwx range. Prints MiB of rwx ranges smaller
# than 128 MiB (the JS code range is a single 256 MiB rwx reservation and is left out) and their count.
#   tools/codemem.sh PID
vmmap -interleaved "$1" 2>/dev/null | perl -ne '
if (m{([0-9a-f]+)-([0-9a-f]+)\s.*rwx/rwx}) { my $s = hex($2) - hex($1); if ($s < 128 * 1048576) { $t += $s; $n++ } }
END { printf "%.1f %d\n", $t / 1048576, $n }'
