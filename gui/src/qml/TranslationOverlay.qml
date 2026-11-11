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
            
            // Используем ТОЧНЫЕ размеры OCR - без увеличения!
            x: modelData.boundingBox.x * scaleX
            y: modelData.boundingBox.y * scaleY
            width: modelData.boundingBox.width * scaleX
            height: modelData.boundingBox.height * scaleY
            
            // Мягкий полупрозрачный темный фон
            color: Qt.rgba(0, 0, 0, 0.75)
            
            // Тонкая мягкая рамка (синяя вместо красной)
            border.color: Qt.rgba(0.3, 0.6, 1.0, 0.6)
            border.width: 1
            radius: 4
            
            // Обрезаем текст по границам блока
            clip: true

            Text {
                anchors.fill: parent
                anchors.margins: 3
                text: modelData.translated
                color: "white"
                
                // Qt автоматически подбирает оптимальный размер шрифта
                font.pixelSize: Math.max(8, Math.min(parent.height / 1.5, 22))
                font.bold: false
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                minimumPixelSize: 7
                fontSizeMode: Text.Fit  // Qt автоматически уменьшает шрифт чтобы текст влез
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

