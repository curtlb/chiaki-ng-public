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
    
    // Фиксированный размер
    implicitWidth: 550
    implicitHeight: 550
    
    padding: 20
    
    contentItem: Item {
        implicitWidth: 510
        implicitHeight: 510
        
        ColumnLayout {
            anchors.fill: parent
            spacing: 10
        
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
        
            Label {
                text: qsTr("Статус:")
                Layout.topMargin: 10
                font.bold: true
            }
            
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 180
                Layout.maximumHeight: 200
                border.color: "#555"
                border.width: 1
                color: "#2a2a2a"
                radius: 4
                
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 5
                    clip: true
                    
                    TextArea {
                        id: statusArea
                        readOnly: true
                        text: autoConfigDialog.statusMessage
                        wrapMode: Text.Wrap
                        background: null
                    }
                }
            }
            
            Label {
                id: errorLabel
                text: autoConfigDialog.errorMessage
                color: Material.color(Material.Red)
                visible: autoConfigDialog.errorMessage !== ""
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.maximumHeight: 40
            }
            
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 10
                spacing: 10
                
                Item {
                    Layout.fillWidth: true
                }
                
                Button {
                    text: qsTr("Закрыть")
                    enabled: !autoConfigDialog.isConfiguring
                    onClicked: autoConfigDialog.reject()
                }
                
                Button {
                    id: startButton
                    text: autoConfigDialog.isConfiguring ? qsTr("Настройка...") : qsTr("Начать")
                    enabled: !autoConfigDialog.isConfiguring && loginField.text.length > 0 && passwordField.text.length > 0
                    highlighted: true
                    onClicked: {
                    console.log("Start button clicked!")
                    autoConfigDialog.errorMessage = ""
                    autoConfigDialog.statusMessage = "Начало автоматической настройки...\n"
                    autoConfigDialog.isConfiguring = true
                    console.log("Calling Chiaki.startAutoConfig...")
                    Chiaki.startAutoConfig(loginField.text, passwordField.text)
                    console.log("Chiaki.startAutoConfig called")
                    }
                }
            }
        }
    }
    
    Connections {
        target: Chiaki
        
        function onAutoConfigStatus(message) {
            console.log("AutoConfig status:", message)
            autoConfigDialog.statusMessage += message + "\n"
        }
        
        function onAutoConfigSuccess() {
            console.log("AutoConfig SUCCESS!")
            autoConfigDialog.isConfiguring = false
            autoConfigDialog.statusMessage += "\n✓ Автоматическая настройка успешно завершена!\n"
            autoConfigDialog.statusMessage += "Разбудите консоль перед подключением (если кнопка доступна)\n"
            autoConfigDialog.statusMessage += "и подключайтесь нажатием на иконку с консолью."
        }
        
        function onAutoConfigError(errorMessage) {
            console.log("AutoConfig ERROR:", errorMessage)
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

