// nativeqt: a stand-in for KPipeWire's PipeWireSourceItem (org.kde.pipewire). There is no PipeWire for the
// static plasmashell (libpipewire loads its SPA plugins at run time, which a static program cannot), so the
// item never has a picture: ready stays false, and the task manager's tooltip shows the window's icon, as it
// does when a window has no stream.
import QtQuick

Item {
    property int nodeId: 0
    property int fd: 0
    property bool allowDmaBuf: true
    readonly property bool ready: false
}
