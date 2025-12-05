import QtQuick
import QtQuick.Controls

import org.streetpea.chiaking

// Компонент для отображения переведенного текста поверх видео
Item {
    id: root
    anchors.fill: parent
    
    // Привязываемся к C++ объекту TextOverlay через Chiaki.window.textOverlay
    visible: Chiaki.window.textOverlay ? Chiaki.window.textOverlay.active : false
    
    property var textBlocks: Chiaki.window.textOverlay ? Chiaki.window.textOverlay.textBlocks : []
    property size originalImageSize: Chiaki.window.textOverlay ? Qt.size(Chiaki.window.textOverlay.imageWidth, Chiaki.window.textOverlay.imageHeight) : Qt.size(1920, 1080)

    // Repeater для отображения всех текстовых блоков
    Repeater {
        model: root.textBlocks

        delegate: Rectangle {
            id: textBlock
            
            // Масштабируем координаты из оригинального размера изображения в размер окна
            property real scaleX: root.width / root.originalImageSize.width
            property real scaleY: root.height / root.originalImageSize.height
            
            x: modelData.boundingBox.x * scaleX
            y: modelData.boundingBox.y * scaleY
            width: modelData.boundingBox.width * scaleX
            height: modelData.boundingBox.height * scaleY

            // Полупрозрачный черный фон
            color: Qt.rgba(0, 0, 0, 0.7)
            border.color: "red"
            border.width: 2
            radius: 4

            Text {
                anchors.fill: parent
                anchors.margins: 5
                text: modelData.translated
                color: "white"
                font.pixelSize: Math.max(12, Math.min(parent.height / 3, 32))
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                elide: Text.ElideRight
            }
        }
    }

    // Индикатор в углу экрана, показывающий что оверлей активен
    Rectangle {
        visible: root.visible && root.textBlocks.length > 0
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 10
        width: 200
        height: 40
        color: Qt.rgba(0, 0, 0, 0.8)
        radius: 5
        border.color: "lime"
        border.width: 2

        Text {
            anchors.centerIn: parent
            text: qsTr("Translation Active\nAlt+T to toggle")
            color: "lime"
            font.pixelSize: 12
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
        }
    }
}

