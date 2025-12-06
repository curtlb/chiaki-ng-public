Dialog {
    id: autoConfigDialog
    title: qsTr("Автоматическая настройка")
    modal: true
    
    property bool isConfiguring: false
    property bool isConfigured: false // Новое свойство для отслеживания успешной настройки
    property string currentTaskMessage: ""
    property string errorMessage: ""
    
    implicitWidth: 530
    implicitHeight: 540
    
    padding: 20
    
    contentItem: Item {
        implicitWidth: 490
        implicitHeight: 500
        
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
                enabled: !autoConfigDialog.isConfiguring && !autoConfigDialog.isConfigured // Обновлено
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
                enabled: !autoConfigDialog.isConfiguring && !autoConfigDialog.isConfigured // Обновлено
                Keys.onReturnPressed: startButton.clicked()
                Keys.onEnterPressed: startButton.clicked()
            }
            
            Label {
                id: currentTaskLabel
                text: autoConfigDialog.currentTaskMessage
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.topMargin: 15
                Layout.minimumHeight: 60
                font.pixelSize: 13
                color: autoConfigDialog.currentTaskMessage.startsWith("✓") ? Material.color(Material.Green) : Material.foreground
            }
            
            Label {
                id: errorLabel
                text: autoConfigDialog.errorMessage
                color: Material.color(Material.Red)
                visible: autoConfigDialog.errorMessage !== ""
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.maximumHeight: 60
            }
            
            Item {
                Layout.fillHeight: true
                Layout.minimumHeight: 10
            }
            
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
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
                    text: {
                        if (autoConfigDialog.isConfigured) return qsTr("Настроено")
                        else if (autoConfigDialog.isConfiguring) return qsTr("Настройка...")
                        else return qsTr("Начать")
                    }
                    enabled: !autoConfigDialog.isConfiguring && !autoConfigDialog.isConfigured && loginField.text.length > 0 && passwordField.text.length > 0 // Обновлено
                    highlighted: true && !autoConfigDialog.isConfigured // Обновлено
                    onClicked: {
                        autoConfigDialog.errorMessage = ""
                        autoConfigDialog.currentTaskMessage = "Начало автоматической настройки..."
                        autoConfigDialog.isConfiguring = true
                        Chiaki.startAutoConfig(loginField.text, passwordField.text)
                    }
                }
            }
        }
    }
    
    Connections {
        target: Chiaki
        
        function onAutoConfigStatus(message) {
            autoConfigDialog.currentTaskMessage = message
        }
        
        function onAutoConfigSuccess() {
            autoConfigDialog.isConfiguring = false
            autoConfigDialog.isConfigured = true // Устанавливаем флаг успешной настройки
            autoConfigDialog.currentTaskMessage = "✓ Автоматическая настройка успешно завершена!\nРазбудите консоль перед подключением (если кнопка доступна) и подключайтесь нажатием на иконку с консолью."
        }
        
        function onAutoConfigError(errorMessage) {
            autoConfigDialog.isConfiguring = false
            autoConfigDialog.errorMessage = errorMessage
        }
    }
    
    onRejected: {
        loginField.text = ""
        passwordField.text = ""
        autoConfigDialog.currentTaskMessage = ""
        autoConfigDialog.errorMessage = ""
        autoConfigDialog.isConfigured = false // Сбрасываем флаг при закрытии
    }
}
