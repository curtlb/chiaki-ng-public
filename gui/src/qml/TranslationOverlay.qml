import QtQuick
import QtQuick.Controls
import Qt5Compat.GraphicalEffects

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

        delegate: Item {
            id: textBlock
            
            // Масштабируем координаты из оригинального размера изображения в размер окна
            property real scaleX: root.width / root.originalImageSize.width
            property real scaleY: root.height / root.originalImageSize.height
            
            x: modelData.boundingBox.x * scaleX
            y: modelData.boundingBox.y * scaleY
            width: modelData.boundingBox.width * scaleX
            height: modelData.boundingBox.height * scaleY

            // Текстовый элемент для измерения требуемого размера
            Text {
                id: textMeasure
                text: modelData.translated
                font.pixelSize: Math.max(10, Math.min(parent.height / 2.5, 28))
                font.bold: false
                wrapMode: Text.WordWrap
                width: parent.width - 12
                visible: false
            }

            // Фон - адаптируется под размер текста
            Rectangle {
                id: background
                anchors.centerIn: parent
                width: Math.max(parent.width, Math.min(textMeasure.contentWidth + 12, parent.width * 1.5))
                height: Math.max(parent.height, Math.min(textMeasure.contentHeight + 12, parent.height * 2))
                
                // Мягкий полупрозрачный темный фон
                color: Qt.rgba(0, 0, 0, 0.75)
                
                // Тонкая мягкая рамка
                border.color: Qt.rgba(0.3, 0.6, 1.0, 0.6)  // Мягкий синий
                border.width: 1
                radius: 6
                
                // Мягкая тень для глубины
                layer.enabled: true
                layer.effect: DropShadow {
                    radius: 8
                    samples: 17
                    color: Qt.rgba(0, 0, 0, 0.5)
                    horizontalOffset: 0
                    verticalOffset: 2
                }
            }

            // Переведенный текст
            Text {
                anchors.centerIn: parent
                width: Math.min(textMeasure.contentWidth, background.width - 12)
                text: modelData.translated
                color: "white"
                font.pixelSize: Math.max(10, Math.min(parent.height / 2.5, 28))
                font.bold: false
                font.family: "Segoe UI"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                // Убираем elide - текст всегда показывается полностью
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
            text: qsTr("🌍 Translation Active\nAlt+T new | Ctrl+Y hide")
            color: Qt.rgba(0.7, 1.0, 0.7, 1.0)  // Светло-зеленый
            font.pixelSize: 11
            font.family: "Segoe UI"
            horizontalAlignment: Text.AlignHCenter
            lineHeight: 1.3
        }
    }
}

