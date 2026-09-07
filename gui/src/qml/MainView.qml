import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

Pane {
    padding: 0
    id: consolePane
    // Keep CloudPlayView alive after the first visit so an in-flight catalog fetch
    // cannot call back into a destroyed QML object when the user leaves the tab.
    property bool cloudPlayPinned: false

    CleanBlueBackground {
        anchors.fill: parent
        z: -1
    }

    StackView.onActivated: {
        forceActiveFocus(Qt.TabFocusReason);
        Chiaki.ensureFourcloudPolling();
        if(!Chiaki.autoConnect && !root.initialAsk && !Chiaki.window.directStream)
        {
            root.initialAsk = true;
            if(Chiaki.settings.remotePlayAsk)
            {
                if(!Chiaki.settings.psnRefreshToken || !Chiaki.settings.psnAuthToken || !Chiaki.settings.psnAuthTokenExpiry || !Chiaki.settings.psnAccountId)
                    root.showRemindDialog(qsTr("Remote Play via PSN"), qsTr("Would you like to connect to PSN?\nThis enables:\n- Automatic registration\n- Playing outside of your home network without port forwarding?") + "\n\n" + qsTr("(Note: If you select no now and want to do this later, go to the Config section of the settings.)"), true, () => root.showPSNTokenDialog(false));
                else
                    Chiaki.settings.remotePlayAsk = false;
            }
        }
    }
    Keys.onUpPressed: {
        if(hostsView.currentItem && hostsView.currentItem.visible)
        {
            hostsView.decrementCurrentIndex()
            while(!hostsView.currentItem.visible)
                hostsView.decrementCurrentIndex()
        }
    }
    Keys.onDownPressed: {
        if(hostsView.currentItem && hostsView.currentItem.visible)
        {
            hostsView.incrementCurrentIndex()
            while(!hostsView.currentItem.visible)
                 hostsView.incrementCurrentIndex()
        }
    }
    Keys.onMenuPressed: settingsButton.clicked()
    Keys.onReturnPressed: if (hostsView.currentItem) hostsView.currentItem.connectToHost()
    Keys.onYesPressed: if (hostsView.currentItem) hostsView.currentItem.wakeUpHost()
    Keys.onNoPressed: if (hostsView.currentItem) hostsView.currentItem.deleteHost()
    Keys.onEscapePressed: root.showConfirmDialog(qsTr("Quit"), qsTr("Are you sure you want to quit?"), () => Qt.quit())
    Keys.onPressed: (event) => {
        if (event.modifiers)
            return;
        switch (event.key) {
        case Qt.Key_PageUp:
            if (hostsView.currentItem) hostsView.currentItem.setConsolePin();
            event.accepted = true;
            break;
        case Qt.Key_PageDown:
            if (Chiaki.settings.psnAuthToken) Chiaki.refreshPsnToken();
            event.accepted = true;
            break;
        case Qt.Key_F2:
            root.showManualHostDialog();
            event.accepted = true;
            break;
        }
    }

    ToolBar {
        id: toolBar
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
        }
        height: 64
        background: Rectangle {
            color: Qt.rgba(0.05, 0.07, 0.10, 0.94)
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Qt.rgba(0.18, 0.77, 0.71, 0.18)
            }
        }

        RowLayout {
            anchors {
                fill: parent
                leftMargin: 12
                rightMargin: 12
            }
            spacing: 6

            Button {
                Layout.fillHeight: true
                Layout.preferredWidth: 72
                flat: true
                text: "×"
                font.pixelSize: 36
                focusPolicy: Qt.NoFocus
                onClicked: Qt.quit()
                Material.roundedScale: Material.SmallScale
                Material.foreground: "#e8eef4"
            }

            Button {
                Layout.fillHeight: true
                Layout.preferredWidth: 100
                flat: true
                text: qsTr("Выйти")
                focusPolicy: Qt.NoFocus
                visible: !!Chiaki.settings.jwtToken
                onClicked: Chiaki.logoutFourcloud()
                Material.roundedScale: Material.SmallScale
                Material.foreground: "#8b9aab"
            }

            Item { Layout.fillWidth: true }

            Label {
                text: "4cloud"
                font.pixelSize: 12
                font.letterSpacing: 2.5
                font.capitalization: Font.AllUppercase
                color: "#2ec4b6"
                opacity: 0.85
                Layout.rightMargin: 8
            }

            Button {
                Layout.fillHeight: true
                Layout.preferredWidth: 280
                flat: true
                text: "Автоматическая настройка"
                visible: false
                focusPolicy: Qt.NoFocus
                onClicked: autoConfigDialog.open()
                Material.roundedScale: Material.SmallScale
            }

            Button {
                Layout.fillHeight: true
                Layout.preferredWidth: 200
                flat: true
                text: "Добавить конфиг"
                icon.source: "qrc:/icons/l3.svg"
                icon.width: 20
                icon.height: 20
                display: AbstractButton.TextBesideIcon
                spacing: 10
                leftPadding: 14
                rightPadding: 14
                focusPolicy: Qt.NoFocus
                onClicked: Chiaki.settings.importSettings()
                Material.roundedScale: Material.SmallScale
                Material.foreground: "#e8eef4"
            }

            Button {
                Layout.fillHeight: true
                Layout.preferredWidth: 220
                flat: true
                text: "Обновить PSN хосты"
                icon.source: "qrc:/icons/r1.svg"
                icon.width: 20
                icon.height: 20
                display: AbstractButton.TextBesideIcon
                spacing: 10
                leftPadding: 14
                rightPadding: 14
                focusPolicy: Qt.NoFocus
                onClicked: Chiaki.refreshPsnToken();
                Material.roundedScale: Material.SmallScale
                Material.foreground: "#e8eef4"
                visible: Chiaki.settings.psnAuthToken
            }

            Button {
                Layout.fillHeight: true
                Layout.preferredWidth: 180
                flat: true
                text: "Добавить хост"
                icon.source: "qrc:/icons/r3.svg"
                icon.width: 20
                icon.height: 20
                display: AbstractButton.TextBesideIcon
                spacing: 10
                leftPadding: 14
                rightPadding: 14
                focusPolicy: Qt.NoFocus
                onClicked: root.showManualHostDialog()
                Material.roundedScale: Material.SmallScale
                Material.foreground: "#e8eef4"
            }

            Button {
                id: settingsButton
                Layout.fillHeight: true
                Layout.preferredWidth: 72
                flat: true
                icon.source: "qrc:/icons/settings-20px.svg";
                icon.width: 36
                icon.height: 36
                focusPolicy: Qt.NoFocus
                onClicked: root.showSettingsDialog()
                Material.roundedScale: Material.SmallScale
            }
        }
    }

    readonly property bool showConsoleTab: Chiaki.showConsoleCatalogTab
    readonly property bool showCloudTab: Chiaki.showCloudGamesTab
    readonly property int consoleTabIndex: showConsoleTab ? 0 : -1
    readonly property int cloudTabIndex: showCloudTab ? (showConsoleTab ? 1 : 0) : -1
    readonly property bool showMainTabs: showConsoleTab && showCloudTab

    function syncMainTabSelection() {
        if (showCloudTab)
            consolePane.cloudPlayPinned = true
        if (showMainTabs)
            return
        mainTabBar.currentIndex = 0
    }

    Connections {
        target: Chiaki
        function onAuthEntitlementsChanged() { syncMainTabSelection() }
    }

    Component.onCompleted: syncMainTabSelection()

    TabBar {
        id: mainTabBar
        visible: showMainTabs
        height: visible ? implicitHeight : 0
        anchors {
            top: toolBar.bottom
            left: parent.left
            right: parent.right
        }
        background: Rectangle {
            color: "#0a0e14"
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: "#243041"
            }
        }
        TabButton {
            text: qsTr("Консоль с дисковым каталогом")
            font.pixelSize: 14
            font.weight: Font.DemiBold
            visible: showConsoleTab
            width: visible ? implicitWidth : 0
        }
        TabButton {
            text: qsTr("Облако цифровых игр")
            font.pixelSize: 14
            font.weight: Font.DemiBold
            visible: showCloudTab
            width: visible ? implicitWidth : 0
        }
        onCurrentIndexChanged: {
            if (currentIndex === cloudTabIndex)
                consolePane.cloudPlayPinned = true
        }
    }

    ListView {
        id: hostsView
        keyNavigationWraps: true
        visible: showConsoleTab && mainTabBar.currentIndex === consoleTabIndex
        anchors {
            top: showMainTabs ? mainTabBar.bottom : toolBar.bottom
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            bottomMargin: 70
        }
        clip: true
        model: Chiaki.hosts
        spacing: 4
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        onCountChanged: {
            if(!hostsView.currentItem)
                hostsView.incrementCurrentIndex();
            if(!hostsView.currentItem)
                return;
            if(!hostsView.currentItem.visible)
            {
                for(var i = 0; i < hostsView.count; i++)
                {
                    hostsView.incrementCurrentIndex()
                    if(hostsView.currentItem.visible)
                    {
                        break;
                    }
                }
            }
        }
        delegate: ItemDelegate {
            visible: modelData.display
            id: delegate
            width: parent ? parent.width : 0
            height: modelData.display ? 160 : 0
            highlighted: ListView.isCurrentItem
            onClicked: connectToHost()
            background: Rectangle {
                color: {
                    if (delegate.highlighted)
                        return Qt.rgba(0.18, 0.77, 0.71, 0.12);
                    if (delegate.hovered)
                        return Qt.rgba(1, 1, 1, 0.04);
                    return "transparent";
                }
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: "#243041"
                    opacity: 0.7
                }
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 3
                    visible: delegate.highlighted
                    color: "#2ec4b6"
                }
            }

            function connectToHost() {
                // Всегда передаём name (для manual host с регистрацией это nickname из настроек)
                Chiaki.connectToHost(index, modelData.name || "");
            }

            function wakeUpHost() {
                if(!modelData.discovered && !modelData.duid)
                    Chiaki.wakeUpHost(index);
            }

            function deleteHost() {
                if (modelData.manual)
                    root.showConfirmDialog(qsTr("Delete Console"), qsTr("Are you sure you want to delete this console?"), () => {Chiaki.deleteHost(index)});
                        
                else if (modelData.discovered && !modelData.registered)
                    root.showConfirmDialog(qsTr("Hide Console"), qsTr("Are you sure you want to hide this console?") + "\n\n" + qsTr("Note: You can unhide from the Consoles section of the Settings under Hidden Consoles"), () => Chiaki.hideHost(modelData.mac, modelData.name));

            }

            function setConsolePin() {
                root.showConsolePinDialog(index);
            }

            RowLayout {
                anchors {
                    fill: parent
                    leftMargin: 30
                    rightMargin: 10
                    topMargin: 10
                    bottomMargin: 10
                }
                spacing: 50

                Item {
                    Layout.fillHeight: true
                    Layout.preferredWidth: 150
                    Image {
                        width: parent.height
                        height: parent.width
                        anchors.centerIn: parent
                        rotation: -90
                        fillMode: Image.PreserveAspectFit
                        source: "image://svg/console-ps" + (modelData.ps5 ? "5" : "4") + (modelData.state == "standby" ? "#light_standby" : "#light_on")
                        sourceSize: Qt.size(width, height)
                    }
                }

                ColumnLayout {
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    visible: modelData.manual
                    spacing: 2
                    // Строка 1: статус консоли с подложкой (зелёный онлайн, жёлтый спит, красный оффлайн)
                    Item {
                        implicitWidth: statusLabel.implicitWidth + 16
                        implicitHeight: statusLabel.implicitHeight + 8
                        Layout.minimumWidth: 70
                        Rectangle {
                            anchors.fill: parent
                            radius: 4
                            color: {
                                if (modelData.state === "ready") return "#4CAF50";
                                if (modelData.state === "standby") return "#FFC107";
                                if (modelData.state === "checking") return "#9E9E9E";
                                return "#F44336";
                            }
                            opacity: 0.85
                        }
                        Label {
                            id: statusLabel
                            anchors.centerIn: parent
                            text: {
                                if (modelData.state === "ready") return qsTr("Онлайн");
                                if (modelData.state === "standby") return qsTr("Спит");
                                if (modelData.state === "checking") return qsTr("Проверка…");
                                return qsTr("Оффлайн");
                            }
                            color: "#FFFFFF"
                        }
                    }
                    // Строка 2: логин PSN
                    Label {
                        text: modelData.name || ""
                    }
                    // Строка 3: дата окончания подписки
                    RowLayout {
                        spacing: 6
                        // При первом заходе после авторизации значение приходит асинхронно — показываем "Проверка…"
                        visible: !!Chiaki.settings.jwtToken
                        Image {
                            Layout.preferredWidth: 20
                            Layout.preferredHeight: 21
                            sourceSize: Qt.size(20, 21)
                            source: "qrc:/icons/clock.svg"
                            fillMode: Image.PreserveAspectFit
                        }
                        Label {
                            text: Chiaki.subscriptionTimeRemaining || qsTr("Проверка…")
                        }
                    }
                }
                Label {
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    visible: !modelData.manual
                    text: {
                        let t = "";
                        if (modelData.name)
                            t += modelData.name + "\n";
                        if (modelData.address)
                            t += qsTr("Address: %1").arg(Chiaki.settings.streamerMode ? "hidden" : modelData.address);
                        if (modelData.mac)
                            t += "\n" + qsTr("ID: %1 (%2)").arg(Chiaki.settings.streamerMode ? "hidden" : modelData.mac).arg(modelData.registered ? qsTr("registered") : qsTr("unregistered"));
                        if (modelData.duid)
                        {
                            if(modelData.discovered)
                                t += "\n" + qsTr("Automatic Registration Available");
                            else
                                t += "\n" + qsTr("Remote Connection via PSN");
                        }
                        else
                        {
                            t += "\n";
                            if(modelData.discovered)
                            {
                                if(modelData.manual)
                                    t += qsTr("discovered + manual")
                                else
                                    t += qsTr("discovered");
                            }
                            else
                                t += qsTr("manual");
                        }
                        return t;
                    }
                }

                Label {
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    visible: !modelData.manual
                    text: {
                        let t = "";
                        if(modelData.duid)
                            return t;
                        t += qsTr("State: %1").arg(modelData.state);
                        if(!modelData.discovered)
                            return t;
                        if (modelData.app)
                            t += "\n" + qsTr("App: %1").arg(modelData.app);
                        if (modelData.titleId)
                            t += "\n" + qsTr("Title ID: %1").arg(modelData.titleId);
                        return t;
                    }
                }

                Item { Layout.fillWidth: true }

                ColumnLayout {
                    Layout.fillHeight: true
                    spacing: 0

                    Button {
                        Layout.alignment: Qt.AlignCenter
                        text: modelData.manual ? "Удалить" : "Скрыть"
                        flat: true
                        padding: 20
                        leftPadding: delegate.highlighted ? 50 : undefined
                        focusPolicy: Qt.NoFocus
                        visible: modelData.manual || (modelData.discovered && !modelData.registered)
                        onClicked: delegate.deleteHost()
                        Material.roundedScale: Material.SmallScale

                        Image {
                            anchors {
                                left: parent.left
                                verticalCenter: parent.verticalCenter
                                leftMargin: 12
                            }
                            width: 28
                            height: 28
                            sourceSize: Qt.size(width, height)
                            source: root.controllerButton("box")
                            visible: delegate.highlighted
                        }
                    }

                    Button {
                        Layout.alignment: Qt.AlignCenter
                        text: "Разбудить"
                        flat: true
                        padding: 20
                        leftPadding: delegate.highlighted ? 50 : undefined
                        visible: modelData.registered && !modelData.duid && !modelData.discovered
                        focusPolicy: Qt.NoFocus
                        onClicked: delegate.wakeUpHost()
                        Material.roundedScale: Material.SmallScale

                        Image {
                            anchors {
                                left: parent.left
                                verticalCenter: parent.verticalCenter
                                leftMargin: 12
                            }
                            width: 28
                            height: 28
                            sourceSize: Qt.size(width, height)
                            source: root.controllerButton("pyramid")
                            visible: delegate.highlighted
                        }
                    }

                    Button {
                        Layout.alignment: Qt.AlignCenter
                        text: "Изменить PIN"
                        flat: true
                        padding: 20
                        leftPadding: delegate.highlighted ? 50 : undefined
                        visible: modelData.registered && !modelData.manual
                        focusPolicy: Qt.NoFocus
                        onClicked: delegate.setConsolePin()
                        Material.roundedScale: Material.SmallScale

                        Image {
                            anchors {
                                left: parent.left
                                verticalCenter: parent.verticalCenter
                                leftMargin: 12
                            }
                            width: 28
                            height: 28
                            sourceSize: Qt.size(width, height)
                            source: "qrc:/icons/l1.svg"
                            visible: delegate.highlighted
                        }
                    }
                } 
            } 
        }
    }     

    Loader {
        id: cloudPlayLoader
        anchors {
            top: showMainTabs ? mainTabBar.bottom : toolBar.bottom
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        active: consolePane.cloudPlayPinned
        visible: showCloudTab && mainTabBar.currentIndex === cloudTabIndex
        source: "CloudPlayView.qml"
        onStatusChanged: {
            if (status === Loader.Error) {
                console.error("CloudPlayView failed to load")
                Chiaki.error(qsTr("Cloud UI Error"), qsTr("Failed to load Cloud tab QML"))
            } else if (status === Loader.Ready) {
                console.log("CloudPlayView loaded successfully")
            }
        }
        onLoaded: {
            if (item) {
                item.mainTabBar = mainTabBar
                item.settingsButton = settingsButton
                item.showConfirmDialogFunc = root.showConfirmDialog
            }
        }
    }

    Label {
        visible: showCloudTab && mainTabBar.currentIndex === cloudTabIndex && cloudPlayLoader.status === Loader.Error
        anchors.centerIn: cloudPlayLoader
        width: parent.width * 0.8
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        color: "#F44336"
        font.pixelSize: 16
        text: qsTr("Не удалось загрузить вкладку «Облако цифровых игр». Проверьте, что QRCodeDialog.qml и GameShortcutDialog.qml включены в сборку.")
        z: 10
    }

    RoundButton {
        visible: showConsoleTab && mainTabBar.currentIndex === consoleTabIndex
        anchors {
            left: parent.left
            bottom: parent.bottom
            leftMargin: 20
            bottomMargin: 52
        }
        icon.source: "qrc:/icons/discover-" + (checked ? "" : "off-") + "24px.svg"
        icon.width: 40
        icon.height: 40
        padding: 14
        focusPolicy: Qt.NoFocus
        checkable: true
        checked: Chiaki.discoveryEnabled
        onToggled: Chiaki.discoveryEnabled = !Chiaki.discoveryEnabled
        Material.background: Material.accent
    }

    Label {
        id: clientUidLabel
        anchors {
            left: parent.left
            bottom: parent.bottom
            margins: 14
        }
        z: 30
        visible: !!Chiaki.settings.jwtToken
        text: Chiaki.settings.cloudBillingUserId > 0
              ? ("UID " + Chiaki.settings.cloudBillingUserId)
              : (Chiaki.settings.fourCloudEmail
                 ? qsTr("UID …")
                 : "")
        color: "#8b9aab"
        font.pixelSize: 12
        font.family: "Consolas"
    }

    Label {
        visible: showConsoleTab && mainTabBar.currentIndex === consoleTabIndex
        anchors {
            right: parent.right
            bottom: parent.bottom
            margins: 20
        }
        text: Qt.application.version
        color: "#5c6b7c"
        font.pixelSize: 12
    }

    Image {
        id: logoImage
        visible: showConsoleTab && mainTabBar.currentIndex === consoleTabIndex
        anchors.centerIn: parent
        source: "qrc:/icons/chiaking-logo-white.svg"
        sourceSize: Qt.size(Math.min(parent.width, parent.height) / 2, Math.min(parent.width, parent.height) / 2)

        PropertyAnimation {
            target: logoImage
            property: "opacity"
            from: 0.05
            to: 0.20
            duration: 1000
            easing.type: Easing.OutCubic
            running: true
        }
    }
    
    AutoConfigDialog {
        id: autoConfigDialog
    }
}
