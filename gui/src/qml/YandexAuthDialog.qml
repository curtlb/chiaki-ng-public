import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import org.streetpea.chiaking

Dialog {
    id: authDialog
    title: qsTr("Авторизация")
    modal: true
    
    property bool isAuthorizing: false
    property string errorMessage: ""
    
    implicitWidth: 480
    implicitHeight: 470
    
    padding: 20
    
    contentItem: Item {
        implicitWidth: 440
        implicitHeight: 430
        
        ColumnLayout {
            anchors.fill: parent
            spacing: 8
        
            Label {
                text: qsTr("Введите логин и пароль от аккаунта 4cloud.pro")
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            
            Label {
                text: qsTr("Логин:")
                Layout.topMargin: 5
            }
            
            TextField {
                id: loginField
                Layout.fillWidth: true
                placeholderText: qsTr("Введите логин")
                enabled: !authDialog.isAuthorizing
                Keys.onReturnPressed: passwordField.forceActiveFocus()
                Keys.onEnterPressed: passwordField.forceActiveFocus()
            }
            
            Label {
                text: qsTr("Пароль:")
                Layout.topMargin: 3
            }
            
            TextField {
                id: passwordField
                Layout.fillWidth: true
                placeholderText: qsTr("Введите пароль")
                echoMode: TextInput.Password
                enabled: !authDialog.isAuthorizing
                Keys.onReturnPressed: authorizeButton.clicked()
                Keys.onEnterPressed: authorizeButton.clicked()
            }
            
            Label {
                id: errorLabel
                text: authDialog.errorMessage
                color: Material.color(Material.Red)
                visible: authDialog.errorMessage !== ""
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.topMargin: 5
                Layout.maximumHeight: 40
            }
            
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 10
            }
            
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                
                Item {
                    Layout.fillWidth: true
                }
                
                Button {
                    text: qsTr("Отмена")
                    enabled: !authDialog.isAuthorizing
                    onClicked: authDialog.reject()
                }
                
                Button {
                    id: authorizeButton
                    text: authDialog.isAuthorizing ? qsTr("Авторизация...") : qsTr("Войти")
                    enabled: !authDialog.isAuthorizing && loginField.text.length > 0 && passwordField.text.length > 0
                    highlighted: true
                    onClicked: {
                        authDialog.errorMessage = ""
                        authDialog.isAuthorizing = true
                        Chiaki.settings.authorizeYandex(loginField.text, passwordField.text)
                    }
                }
            }
        }
    }
    
    Connections {
        target: Chiaki.settings
        
        function onYandexAuthSuccess(iamToken, folderId) {
            authDialog.isAuthorizing = false
            authDialog.accept()
            loginField.text = ""
            passwordField.text = ""
        }
        
        function onYandexAuthError(errorMessage) {
            authDialog.isAuthorizing = false
            authDialog.errorMessage = errorMessage
        }
    }
}

