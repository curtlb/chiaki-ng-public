import QtQuick
import QtQuick.Effects

// App shell backdrop: deep charcoal with a single teal wash (not purple mesh).
Rectangle {
    id: root
    color: "#07090d"

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.0; color: "#0b1017" }
            GradientStop { position: 0.45; color: "#0a0e14" }
            GradientStop { position: 1.0; color: "#06080c" }
        }
    }

    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: -width * 0.35
        anchors.topMargin: -height * 0.25
        width: parent.width * 0.55
        height: parent.height * 0.45
        radius: width
        color: Qt.rgba(0.18, 0.77, 0.71, 0.07)

        layer.enabled: true
        layer.effect: MultiEffect {
            blurEnabled: true
            blurMax: 64
            blur: 1.0
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: -width * 0.3
        anchors.bottomMargin: -height * 0.2
        width: parent.width * 0.5
        height: parent.height * 0.4
        radius: width
        color: Qt.rgba(0.35, 0.45, 0.55, 0.05)

        layer.enabled: true
        layer.effect: MultiEffect {
            blurEnabled: true
            blurMax: 48
            blur: 0.9
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: Qt.rgba(0.18, 0.77, 0.71, 0.12)
    }
}
