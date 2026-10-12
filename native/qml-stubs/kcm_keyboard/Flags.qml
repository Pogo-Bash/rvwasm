// nativeqt: a stand-in for plasma-desktop's org.kde.plasma.private.kcm_keyboard Flags (the keyboard layout
// widget's flag icons): the KCM needs X11's xkb libraries and is not built. No flag icon: the widget shows the
// layout's country code as text, as it does for a layout without a flag.
pragma Singleton
import QtQuick

QtObject {
    function getIcon(shortName) {
        return "";
    }
}
