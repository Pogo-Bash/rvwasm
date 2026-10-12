// nativeqt: a stand-in for KPipeWire's org.kde.pipewire.monitor MediaMonitor (the camera indicator's). There is no
// PipeWire for the static plasmashell, so no detection is available: the indicator says so in its tooltip and
// stays hidden, as on a system where PipeWire is not running.
import QtQuick

QtObject {
    property int role: 0
    readonly property bool detectionAvailable: false
    readonly property int count: 0
    readonly property int idleCount: 0
    readonly property int runningCount: 0
}
