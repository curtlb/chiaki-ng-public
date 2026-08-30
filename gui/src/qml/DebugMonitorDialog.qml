import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

DialogView {
    id: dialog
    title: qsTr("Debug Monitor")
    header: qsTr("Live process log")
    buttonVisible: false

    property bool autoScroll: true
    property string processFilter: "All"
    property string levelFilter: "Info"
    property var visibleRows: []

    function refreshRows() {
        visibleRows = Chiaki.debugMonitor.filteredEntries(processFilter, levelFilter);
    }

    function levelColor(level) {
        switch ((level || "").toLowerCase()) {
        case "error": return "#ff6b6b";
        case "warning": return "#ffb347";
        case "debug":
        case "verbose": return "#9aa0a6";
        default: return "#8fd3ff";
        }
    }

    StackView.onActivated: refreshRows()

    Connections {
        target: Chiaki.debugMonitor
        function onEntriesChanged() { refreshRows(); }
        function onEntryAppended() {
            refreshRows();
            if (dialog.autoScroll && logView.count > 0)
                logView.positionViewAtEnd();
        }
    }

  ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: qsTr("Process:")
                color: "#cccccc"
            }

            ComboBox {
                id: processCombo
                Layout.preferredWidth: 200
                model: [qsTr("All")].concat(Chiaki.debugMonitor.processes)
                onActivated: {
                    processFilter = currentText;
                    refreshRows();
                }
                Component.onCompleted: currentIndex = 0
            }

            Label {
                text: qsTr("Min level:")
                color: "#cccccc"
            }

            ComboBox {
                id: levelCombo
                Layout.preferredWidth: 140
                model: ["Debug", "Info", "Warning", "Error"]
                currentIndex: 1
                onActivated: {
                    levelFilter = currentText;
                    refreshRows();
                }
            }

            CheckBox {
                text: qsTr("Auto-scroll")
                checked: autoScroll
                onToggled: autoScroll = checked
            }

            Item { Layout.fillWidth: true }

            Label {
                text: qsTr("%1 lines").arg(visibleRows.length)
                color: "#aaaaaa"
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#101418"
            border.color: "#2a3540"
            radius: 6

            ListView {
                id: logView
                anchors.fill: parent
                anchors.margins: 8
                clip: true
                spacing: 2
                model: visibleRows

                delegate: Item {
                    width: logView.width
                    height: lineText.implicitHeight + 4

                    Text {
                        id: lineText
                        width: parent.width
                        wrapMode: Text.Wrap
                        font.family: "Consolas, Courier New, monospace"
                        font.pixelSize: 13
                        color: levelColor(modelData.level)
                        text: "[" + modelData.timestamp + "] [" + modelData.process + "] [" + modelData.level + "] " + modelData.message
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Button {
                text: qsTr("Clear")
                Material.roundedScale: Material.SmallScale
                onClicked: {
                    Chiaki.debugMonitor.clear();
                    refreshRows();
                }
            }

            Button {
                text: qsTr("Copy visible")
                Material.roundedScale: Material.SmallScale
                onClicked: Chiaki.debugMonitor.copyToClipboard(
                    Chiaki.debugMonitor.copyFiltered(processFilter, levelFilter))
            }

            Button {
                text: qsTr("Refresh")
                Material.roundedScale: Material.SmallScale
                onClicked: refreshRows()
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#888888"
                text: qsTr("F12 — open/close. Processes: CloudCatalog, CloudSession, Stream, PSN, Discovery, Qt, System.")
            }

            Button {
                text: qsTr("Close")
                Material.roundedScale: Material.SmallScale
                onClicked: dialog.close()
            }
        }
    }
}
