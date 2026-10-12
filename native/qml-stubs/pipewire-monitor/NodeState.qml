// nativeqt: Monitor.NodeState.Running and the others (KPipeWire's enum, PipeWire's node states)
pragma Singleton
import QtQuick

QtObject {
    enum State {
        Error = -1,
        Creating = 0,
        Suspended = 1,
        Idle = 2,
        Running = 3
    }
}
