import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import org.streetpea.chiaking

Dialog {
    id: autoConfigDialog
    title: qsTr("Автоматическая настройка")
    modal: true
    
    property bool isConfiguring: false
    property string statusMessage: ""
    property string errorMessage: ""
    
    width: 500
    height: 400
    
    contentItem: ColumnLayout {
        width: autoConfigDialog.availableWidth
        height: autoConfigDialog.availableHeight
        spacing: 15
        
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 10
        }
        
        Label {
            text: qsTr("Введите логин и пароль для автоматической настройки")
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 10
        }
        
        Label {
            text: qsTr("Логин:")
        }
        
        TextField {
            id: loginField
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            placeholderText: qsTr("Введите логин")
            enabled: !autoConfigDialog.isConfiguring
            Keys.onReturnPressed: passwordField.forceActiveFocus()
            Keys.onEnterPressed: passwordField.forceActiveFocus()
        }
        
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 5
        }
        
        Label {
            text: qsTr("Пароль:")
        }
        
        TextField {
            id: passwordField
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            placeholderText: qsTr("Введите пароль")
            echoMode: TextInput.Password
            enabled: !autoConfigDialog.isConfiguring
            Keys.onReturnPressed: startButton.clicked()
            Keys.onEnterPressed: startButton.clicked()
        }
        
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: 150
            
            TextArea {
                id: statusArea
                readOnly: true
                text: autoConfigDialog.statusMessage
                wrapMode: Text.Wrap
            }
        }
        
        Label {
            id: errorLabel
            text: autoConfigDialog.errorMessage
            color: Material.color(Material.Red)
            visible: autoConfigDialog.errorMessage !== ""
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 10
        }
        
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            
            Item {
                Layout.fillWidth: true
            }
            
            Button {
                text: qsTr("Закрыть")
                enabled: !autoConfigDialog.isConfiguring
                onClicked: autoConfigDialog.reject()
                Layout.preferredHeight: 40
                Layout.preferredWidth: 100
            }
            
            Button {
                id: startButton
                text: autoConfigDialog.isConfiguring ? qsTr("Настройка...") : qsTr("Начать")
                enabled: !autoConfigDialog.isConfiguring && loginField.text.length > 0 && passwordField.text.length > 0
                highlighted: true
                Layout.preferredHeight: 40
                Layout.preferredWidth: 120
                onClicked: {
                    autoConfigDialog.errorMessage = ""
                    autoConfigDialog.statusMessage = "Начало автоматической настройки...\n"
                    autoConfigDialog.isConfiguring = true
                    Chiaki.backend.startAutoConfig(loginField.text, passwordField.text)
                }
            }
        }
    }
    
    Connections {
        target: Chiaki.backend
        
        function onAutoConfigStatus(message) {
            autoConfigDialog.statusMessage += message + "\n"
        }
        
        function onAutoConfigSuccess() {
            autoConfigDialog.isConfiguring = false
            autoConfigDialog.statusMessage += "\n✓ Автоматическая настройка успешно завершена!\n"
            autoConfigDialog.statusMessage += "Разбудите консоль перед подключением (если кнопка доступна)\n"
            autoConfigDialog.statusMessage += "и подключайтесь нажатием на иконку с консолью."
        }
        
        function onAutoConfigError(errorMessage) {
            autoConfigDialog.isConfiguring = false
            autoConfigDialog.errorMessage = errorMessage
            autoConfigDialog.statusMessage += "\n✗ Ошибка: " + errorMessage + "\n"
        }
    }
    
    onRejected: {
        loginField.text = ""
        passwordField.text = ""
        autoConfigDialog.statusMessage = ""
        autoConfigDialog.errorMessage = ""
    }
}

