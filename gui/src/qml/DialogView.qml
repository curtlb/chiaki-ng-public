import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

Item {
    id: dialog
    property alias header: headerLabel.text
    property alias title: titleLabel.text
    property alias buttonText: okButton.text
    property alias buttonEnabled: okButton.enabled
    property alias buttonVisible: okButton.visible
    property Item restoreFocusItem
    default property Item mainItem: null

    signal accepted()
    signal rejected()

    function close() {
        // Try to find StackView through StackView.view attached property
        var stackView = null;
        var item = dialog;
        while (item && !stackView) {
            // Check if this item is in a StackView
            var attached = item.StackView;
            if (attached && attached.view) {
                stackView = attached.view;
                break;
            }
            item = item.parent;
        }
        
        if (stackView && stackView.depth > 1) {
            stackView.pop();
        } else {
            // Fallback: try to find root through Window
            var rootItem = Window.window ? Window.window.contentItem : null;
            if (rootItem) {
                // Find stack in root
                var stack = rootItem.children ? rootItem.children.find(function(child) {
                    return child && child.hasOwnProperty("pop");
                }) : null;
                if (stack && stack.depth > 1) {
                    stack.pop();
                }
            }
        }
    }

    Keys.onEscapePressed: close()

    Keys.onMenuPressed: {
        if (okButton.enabled)
            okButton.clicked()
    }

    StackView.onDeactivating: {
        restoreFocusItem = Window.window.activeFocusItem;
    }

    StackView.onActivated: {
        if (!restoreFocusItem) {
            let item = mainItem.nextItemInFocusChain();
            if (item)
                item.forceActiveFocus(Qt.TabFocusReason);
        } else {
            restoreFocusItem.forceActiveFocus(Qt.TabFocusReason);
            restoreFocusItem = null;
        }
    }

    onMainItemChanged: {
        if (mainItem) {
            mainItem.parent = contentItem;
            mainItem.anchors.fill = contentItem;
        }
    }

    ToolBar {
        id: toolBar
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
        }
        height: 80

        RowLayout {
            anchors {
                fill: parent
                leftMargin: 10
                rightMargin: 10
            }

            Button {
                Layout.fillHeight: true
                Layout.preferredWidth: 100
                flat: true
                text: "❮"
                focusPolicy: Qt.NoFocus
                Material.roundedScale: Material.SmallScale
                onClicked: {
                    dialog.rejected();
                    dialog.close();
                }
            }

            Item { Layout.fillWidth: true }

            Button {
                id: okButton
                Layout.fillHeight: true
                flat: true
                padding: 30
                font.pixelSize: 25
                focusPolicy: Qt.NoFocus
                Material.roundedScale: Material.SmallScale
                onClicked: dialog.accepted()
                icon.source: "qrc:/icons/options.svg";
                icon.width: 50
                icon.height: 50
            }
        }

        Label {
            id: titleLabel
            anchors.centerIn: parent
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            font.bold: true
            font.pixelSize: 26
        }

        Label {
            id: headerLabel
            anchors {
                top: parent.top
                left: titleLabel.right
                right: parent.right
                verticalCenter: parent.verticalCenter
            }
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            font.bold: true
            font.pixelSize: 14
        }
    }

    Item {
        id: contentItem
        anchors {
            top: toolBar.bottom
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
    }
}
