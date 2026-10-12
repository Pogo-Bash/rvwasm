// nativeqt: the camera indicator's Monitor.MediaRole.Camera (KPipeWire's enum)
pragma Singleton
import QtQuick

QtObject {
    enum Role {
        Camera,
        Microphone,
        Screen
    }
}
