// VK-17 gpu-matrix: a Qt Quick scene for the GPU path. 60 rotating rectangles with gradients, opacity and a text on top, kept running until the caller ends it. Every 2 s it prints "N frames in S seconds = F FPS" (frames the scene graph swapped).
import QtQuick
import QtQuick.Window

Window {
    id: win
    width: 640; height: 480
    visible: true
    color: "#1b2733"
    title: "gpu-matrix qtquick"

    property int frames: 0
    property int lastFrames: 0
    property double lastT: Date.now()
    onFrameSwapped: frames++

    Repeater {
        model: 60
        Rectangle {
            property real a: index * 6
            x: 40 + (index % 10) * 56; y: 40 + Math.floor(index / 10) * 62
            width: 44; height: 44; radius: 8
            opacity: 0.55 + 0.4 * ((index % 5) / 4)
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.hsla((index * 17 % 360) / 360, 0.7, 0.55, 1) }
                GradientStop { position: 1; color: Qt.hsla(((index * 17 + 90) % 360) / 360, 0.7, 0.35, 1) }
            }
            border.color: "white"; border.width: 1
            RotationAnimation on rotation { from: 0; to: 360; duration: 2000 + (index % 7) * 300; loops: Animation.Infinite }
        }
    }
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 420
        text: "Qt Quick on the GPU: " + win.frames + " frames"
        color: "white"; font.pixelSize: 22
    }
    Timer {
        interval: 2000; running: true; repeat: true
        onTriggered: {
            var now = Date.now(), dt = (now - win.lastT) / 1000, n = win.frames - win.lastFrames
            win.lastT = now; win.lastFrames = win.frames
            console.log(n + " frames in " + dt.toFixed(1) + " seconds = " + (n / dt).toFixed(1) + " FPS")
        }
    }
}
