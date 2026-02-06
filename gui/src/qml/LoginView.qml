import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

Pane {
    id: root
    padding: 40
    Material.theme: Material.Dark
    Material.accent: "#00a7ff"

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(400, parent.width - 80)
        spacing: 30

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "Авторизация"
            font.pixelSize: 32
            font.bold: true
            color: Material.foreground
        }

        TextField {
            id: emailField
            Layout.fillWidth: true
            placeholderText: "Email"
            text: ""
            focus: true
            Material.accent: Material.accent
            Keys.onReturnPressed: passwordField.focus = true
            Keys.onEnterPressed: passwordField.focus = true
        }

        TextField {
            id: passwordField
            Layout.fillWidth: true
            placeholderText: "Пароль"
            echoMode: TextField.Password
            Material.accent: Material.accent
            Keys.onReturnPressed: loginButton.clicked()
            Keys.onEnterPressed: loginButton.clicked()
        }

        Button {
            id: loginButton
            Layout.fillWidth: true
            Layout.preferredHeight: 50
            text: "Войти"
            enabled: emailField.text.length > 0 && passwordField.text.length > 0 && !authenticating
            Material.accent: Material.accent
            onClicked: {
                authenticating = true
                errorText.text = ""
                Chiaki.authenticate(emailField.text, passwordField.text)
            }
        }

        Label {
            id: errorText
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            visible: text.length > 0
            text: ""
            color: Material.color(Material.Red)
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
        }

        BusyIndicator {
            id: busyIndicator
            Layout.alignment: Qt.AlignHCenter
            visible: authenticating
            running: authenticating
        }
    }

    property bool authenticating: false

    Connections {
        target: Chiaki

        function onAuthenticationSuccess() {
            authenticating = false
            // The signal will be handled by Main.qml to show main view
        }

        function onAuthenticationError(errorMessage) {
            authenticating = false
            errorText.text = errorMessage
        }

        function onSubscriptionExpired(message) {
            authenticating = false
            errorText.text = message
        }
    }
}
