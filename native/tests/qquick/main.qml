import QtQuick
Rectangle {
	property bool animate: false
	width: 420; height: 320
	color: "#3a5a40"
	Repeater {
		model: 12
		Rectangle {
			required property int index
			x: parent.width / 2 - width / 2; y: parent.height / 2 - height / 2
			width: 40 + index * 22; height: width
			radius: width / 5
			color: "transparent"
			border.width: 3
			border.color: Qt.hsva(index / 12, 0.6, 0.95, 1)
			RotationAnimation on rotation {
				from: 0; to: 360; duration: 4000 + index * 500
				loops: Animation.Infinite
				running: animate
			}
		}
	}
	Text {
		x: 10; y: 8
		color: "white"
		font.pixelSize: 16
		text: "native Qt Quick " + Qt.application.version + " on linux-wasm"
	}
}
