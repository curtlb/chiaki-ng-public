import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import org.streetpea.chiaking

Dialog {
    id: authDialog
    title: qsTr("Авторизация Yandex Cloud")
    modal: true
    
    property bool isAuthorizing: false
    property string errorMessage: ""
    
    // Фиксированный размер диалога
    width: 450
    height: 350
    
    // Контент диалога с явными размерами
    contentItem: ColumnLayout {
        width: authDialog.availableWidth
        height: authDialog.availableHeight
        spacing: 15
        
        Item {
            Layout.fillWidth: true
            Layout.topMargin: 10
        }
        
        Label {
            text: qsTr("Введите логин и пароль для получения IAM токена")
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
            enabled: !authDialog.isAuthorizing
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
            Layout.preferredHeight: visible ? implicitHeight : 0
        }
        
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        
        RowLayout {
            Layout.fillWidth: true
            Layout.bottomMargin: 10
            spacing: 10
            
            Item {
                Layout.fillWidth: true
            }
            
            Button {
                text: qsTr("Отмена")
                enabled: !authDialog.isAuthorizing
                onClicked: authDialog.reject()
                Layout.preferredHeight: 40
                Layout.preferredWidth: 100
            }
            
            Button {
                id: authorizeButton
                text: authDialog.isAuthorizing ? qsTr("Авторизация...") : qsTr("Войти")
                enabled: !authDialog.isAuthorizing && loginField.text.length > 0 && passwordField.text.length > 0
                highlighted: true
                Layout.preferredHeight: 40
                Layout.preferredWidth: 120
                onClicked: {
                    authDialog.errorMessage = ""
                    authDialog.isAuthorizing = true
                    Chiaki.settings.authorizeYandex(loginField.text, passwordField.text)
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

