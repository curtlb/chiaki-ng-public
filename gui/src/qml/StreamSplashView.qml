import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import org.streetpea.chiaking

Rectangle {
    id: splashView
    color: "black"
    
    property alias stageText: stageLabel.text
    property alias statusText: statusLabel.text
    property alias progressValue: progressBar.value
    
    ColumnLayout {
        anchors.centerIn: parent
        spacing: 30
        
        Label {
            id: stageLabel
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Подготовка подключения...")
            font.pixelSize: 28
            color: "white"
        }
        
        ProgressBar {
            id: progressBar
            Layout.preferredWidth: 400
            Layout.alignment: Qt.AlignHCenter
            value: splashView.progressValue / 100.0
            Material.accent: Material.accent
        }
        
        Label {
            id: statusLabel
            Layout.alignment: Qt.AlignHCenter
            text: ""
            font.pixelSize: 18
            color: "lightgray"
            visible: text.length > 0
        }
        
        BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 70
            Layout.preferredHeight: 70
            running: true
        }
    }
}
