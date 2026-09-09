import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

Pane {
    id: root
    padding: 0
    Material.theme: Material.Dark
    Material.accent: "#2ec4b6"

    // Saved JWT: Main.qml shows LoginView while checkJwtToken() runs.
    readonly property bool hasSavedToken: !!(Chiaki.settings.jwtToken && Chiaki.settings.jwtToken.length > 0)
    property bool authenticating: false
    property bool tokenChecking: false

    CleanBlueBackground {
        anchors.fill: parent
        z: -1
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(440, parent.width - 48)
        height: loginColumn.implicitHeight + 56
        radius: 14
        color: Qt.rgba(0.08, 0.10, 0.14, 0.92)
        border.width: 1
        border.color: Qt.rgba(0.18, 0.77, 0.71, 0.28)

        ColumnLayout {
            id: loginColumn
            anchors {
                left: parent.left
                right: parent.right
                verticalCenter: parent.verticalCenter
                leftMargin: 28
                rightMargin: 28
            }
            spacing: 18

            Label {
                Layout.alignment: Qt.AlignHCenter
                Layout.bottomMargin: 8
                text: "4cloud"
                font.pixelSize: 13
                font.letterSpacing: 3
                font.capitalization: Font.AllUppercase
                color: "#2ec4b6"
                opacity: 0.9
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: tokenChecking ? qsTr("Вход…") : qsTr("Авторизация")
                font.pixelSize: 28
                font.weight: Font.DemiBold
                color: "#e8eef4"
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                Layout.bottomMargin: 8
                visible: tokenChecking
                text: qsTr("Проверяем сохранённый вход — подождите")
                font.pixelSize: 13
                color: "#8b9aab"
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                Layout.fillWidth: true
            }

            TextField {
                id: emailField
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                placeholderText: "Email"
                text: ""
                focus: !tokenChecking
                enabled: !authenticating
                opacity: enabled ? 1.0 : 0.45
                leftPadding: 14
                rightPadding: 14
                Material.accent: Material.accent
                Keys.onReturnPressed: passwordField.focus = true
                Keys.onEnterPressed: passwordField.focus = true
                background: Rectangle {
                    radius: 8
                    color: "#141a22"
                    border.width: 1
                    border.color: emailField.activeFocus ? "#2ec4b6" : "#243041"
                }
            }

            TextField {
                id: passwordField
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                placeholderText: "Пароль"
                echoMode: TextField.Password
                enabled: !authenticating
                opacity: enabled ? 1.0 : 0.45
                leftPadding: 14
                rightPadding: 14
                Material.accent: Material.accent
                Keys.onReturnPressed: loginButton.clicked()
                Keys.onEnterPressed: loginButton.clicked()
                background: Rectangle {
                    radius: 8
                    color: "#141a22"
                    border.width: 1
                    border.color: passwordField.activeFocus ? "#2ec4b6" : "#243041"
                }
            }

            Button {
                id: loginButton
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                Layout.topMargin: 6
                text: tokenChecking ? qsTr("Проверка входа…") : qsTr("Войти")
                font.pixelSize: 15
                font.weight: Font.DemiBold
                enabled: !authenticating && emailField.text.length > 0 && passwordField.text.length > 0
                Material.background: enabled ? "#2ec4b6" : "#1a222d"
                Material.foreground: enabled ? "#071210" : "#5c6b7c"
                Material.roundedScale: Material.SmallScale
                onClicked: {
                    authenticating = true
                    tokenChecking = false
                    errorText.text = ""
                    Chiaki.authenticate(emailField.text, passwordField.text)
                }
            }

            Label {
                id: errorText
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                visible: text.length > 0
                text: ""
                color: "#e85d5d"
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 13
            }

            BusyIndicator {
                id: busyIndicator
                Layout.alignment: Qt.AlignHCenter
                visible: authenticating
                running: authenticating
                Material.accent: "#2ec4b6"
            }
        }
    }

    Component.onCompleted: {
        if (hasSavedToken) {
            tokenChecking = true
            authenticating = true
            errorText.text = ""
        }
    }

    Connections {
        target: Chiaki

        function onAuthenticationSuccess() {
            authenticating = false
            tokenChecking = false
        }

        function onAuthenticationError(errorMessage) {
            authenticating = false
            tokenChecking = false
            errorText.text = errorMessage
        }

        function onJwtTokenValid() {
            authenticating = false
            tokenChecking = false
        }

        function onJwtTokenExpired() {
            authenticating = false
            tokenChecking = false
            if (!errorText.text)
                errorText.text = qsTr("Сессия истекла — войдите снова")
        }

        function onSubscriptionExpired(message) {
            authenticating = false
            tokenChecking = false
            errorText.text = message || qsTr("Нет активной подписки")
        }
    }
}
