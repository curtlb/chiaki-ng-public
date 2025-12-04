import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

Rectangle {
    id: overlay
    property string translatedText: ""
    property string originalText: ""
    property bool isTranslating: false
    
    visible: translatedText !== "" || isTranslating
    color: "#CC000000"
    radius: 10
    
    anchors {
        bottom: parent.bottom
        horizontalCenter: parent.horizontalCenter
        bottomMargin: 100
    }
    
    width: Math.min(parent.width * 0.8, contentColumn.implicitWidth + 40)
    height: contentColumn.implicitHeight + 40
    
    Column {
        id: contentColumn
        anchors.centerIn: parent
        spacing: 10
        width: parent.width - 40
        
        BusyIndicator {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 40
            height: 40
            visible: isTranslating
            running: isTranslating
        }
        
        Label {
            width: parent.width
            text: "Оригинал:"
            font.bold: true
            color: "#AAAAAA"
            wrapMode: Text.WordWrap
            visible: originalText !== "" && !isTranslating
        }
        
        Label {
            width: parent.width
            text: originalText
            color: "#CCCCCC"
            wrapMode: Text.WordWrap
            visible: originalText !== "" && !isTranslating
            font.pixelSize: 14
        }
        
        Label {
            width: parent.width
            text: "Перевод:"
            font.bold: true
            color: "white"
            wrapMode: Text.WordWrap
            visible: translatedText !== "" && !isTranslating
        }
        
        Label {
            width: parent.width
            text: translatedText
            color: "white"
            wrapMode: Text.WordWrap
            visible: translatedText !== "" && !isTranslating
            font.pixelSize: 16
            font.bold: true
        }
        
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Распознавание текста..."
            color: "white"
            visible: isTranslating
        }
    }
    
    // Auto-hide after 10 seconds
    Timer {
        id: hideTimer
        interval: 10000
        running: translatedText !== "" && !isTranslating
        onTriggered: {
            translatedText = "";
            originalText = "";
        }
    }
    
    Behavior on opacity {
        NumberAnimation { duration: 300 }
    }
}

