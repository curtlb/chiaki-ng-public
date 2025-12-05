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
            width: modelData.boundingBox.width * scaleX * 1.4  // +40% ширины для размещения текста
            height: modelData.boundingBox.height * scaleY * 2.0  // +100% высоты для размещения текста

            // Мягкий полупрозрачный темный фон
            color: Qt.rgba(0, 0, 0, 0.75)
            
            // Тонкая мягкая рамка (синяя вместо красной)
            border.color: Qt.rgba(0.3, 0.6, 1.0, 0.6)
            border.width: 1
            radius: 6
            
            // Обрезаем текст по границам блока чтобы не было наложений
            clip: true

            Text {
                anchors.fill: parent
                anchors.margins: 6
                text: modelData.translated
                color: "white"
                font.pixelSize: Math.max(10, Math.min(parent.height / 3, 26))
                font.bold: false
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                // НЕ используем elide - текст полностью виден
            }
        }
    }

    // Индикатор в углу экрана, показывающий что оверлей активен
    Rectangle {
        visible: root.visible && root.textBlocks.length > 0
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 10
        width: 220
        height: 50
        color: Qt.rgba(0, 0, 0, 0.85)
        radius: 8
        border.color: Qt.rgba(0.2, 0.8, 0.2, 0.7)  // Мягкий зеленый
        border.width: 1

        Text {
            anchors.centerIn: parent
            text: qsTr("🌍 Translation Active\nAlt+T new | Alt+Y hide")
            color: Qt.rgba(0.7, 1.0, 0.7, 1.0)  // Светло-зеленый
            font.pixelSize: 11
            font.family: "Segoe UI"
            horizontalAlignment: Text.AlignHCenter
            lineHeight: 1.3
        }
    }
}

