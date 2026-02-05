import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

DialogView {
    id: dialog
    title: qsTr("Вход в систему")
    buttonText: qsTr("Войти")
    buttonEnabled: !submitting && loginField.text.trim() && passwordField.text.trim()
    
    property bool submitting: false
    
    onAccepted: {
        if (!submitting && buttonEnabled) {
            submitting = true
            Chiaki.startAutoConfig(loginField.text.trim(), passwordField.text)
        }
    }
    
    StackView.onActivated: {
        loginField.forceActiveFocus()
    }
    
    ColumnLayout {
        anchors.fill: parent
        spacing: 20
        
        Label {
            Layout.fillWidth: true
            text: qsTr("Введите логин и пароль для входа")
            wrapMode: Text.WordWrap
        }
        
        TextField {
            id: loginField
            Layout.fillWidth: true
            placeholderText: qsTr("Логин")
            enabled: !dialog.submitting
            Keys.onReturnPressed: passwordField.forceActiveFocus()
            Keys.onEnterPressed: passwordField.forceActiveFocus()
        }
        
        TextField {
            id: passwordField
            Layout.fillWidth: true
            placeholderText: qsTr("Пароль")
            echoMode: TextField.Password
            enabled: !dialog.submitting
            Keys.onReturnPressed: if (dialog.buttonEnabled) dialog.accepted()
            Keys.onEnterPressed: if (dialog.buttonEnabled) dialog.accepted()
        }
        
        Label {
            id: statusLabel
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            color: Material.accent
        }
        
        Label {
            id: errorLabel
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            color: Material.color(Material.Red)
        }
        
        BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            visible: dialog.submitting
            running: dialog.submitting
        }
    }
    
    Connections {
        target: Chiaki
        
        function onAutoConfigStatus(message) {
            statusLabel.text = message
            errorLabel.text = ""
        }
        
        function onAutoConfigSuccess() {
            dialog.submitting = false
            root.closeDialog()
            // Main view will be shown automatically
        }
        
        function onAutoConfigError(errorMessage) {
            dialog.submitting = false
            errorLabel.text = errorMessage
            statusLabel.text = ""
        }
    }
}
