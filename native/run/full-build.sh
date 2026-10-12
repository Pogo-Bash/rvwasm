#!/bin/bash
# The full build (native/run/overnight.sh with NQ_FULL=1): everything the overnight build makes, and the rest of
# what Plasma widgets and global themes import (Qt Positioning, Multimedia and Sensors, Prison, KImageFormats,
# BluezQt, KNotifyConfig, KUnitConversion, Milou, kactivitymanagerd, KScreen, kdeplasma-addons, the keyboard KCM's
# libraries), then plasma-workspace, plasma-desktop's parts and plasmashell again. Unattended, -j12 (-j8 when
# memory is short), resumable (finished modules are skipped by their markers), summary in
# ~/rvwasm/nativeqt/logs/overnight-summary.txt. Start it detached:
#   native/run/full-build.sh
# It returns at once; the build runs on (pgrep -f run/overnight.sh), "# finished" ends the summary.
if pgrep -f "run/overnight.sh" > /dev/null; then
	echo "full-build: a build runs already"
	exit 0
fi
NQ_FULL=1 setsid nohup bash "$(cd "$(dirname "$0")" && pwd)/overnight.sh" > /dev/null 2>&1 < /dev/null &
echo "full-build started"
