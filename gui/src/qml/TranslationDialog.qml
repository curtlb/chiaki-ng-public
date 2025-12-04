import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import org.streetpea.chiaking

import "controls" as C

DialogView {
    id: dialog
    
    buttonText: Chiaki.session && Chiaki.session.isTranslating ? "Перевод..." : "Перевести"
    buttonEnabled: inputField.text.length > 0 && (!Chiaki.session || !Chiaki.session.isTranslating)
    onAccepted: {
        if (inputField.text.length > 0 && Chiaki.session) {
            // Call translateText method
            Chiaki.session.translateText(inputField.text, "EN", "RU");
        }
    }
    
    onOpened: {
        inputField.text = "";
        inputField.forceActiveFocus();
    }
    
    Item {
        Layout.preferredWidth: 500
        Layout.preferredHeight: 400
        
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 15
            
            Label {
                text: "Переводчик DeepL (EN → RU)"
                font.bold: true
                font.pixelSize: 16
            }
            
            Label {
                text: "Введите текст на английском для перевода:"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 100
                
                TextArea {
                    id: inputField
                    placeholderText: "Введите текст для перевода..."
                    wrapMode: TextArea.Wrap
                    selectByMouse: true
                    enabled: !isTranslating
                }
            }
            
            RowLayout {
                Layout.fillWidth: true
                visible: Chiaki.session && Chiaki.session.isTranslating
                
                BusyIndicator {
                    running: Chiaki.session && Chiaki.session.isTranslating
                    Layout.preferredWidth: 30
                    Layout.preferredHeight: 30
                }
                
                Label {
                    text: "Перевод текста..."
                    color: Material.accent
                }
            }
            
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: Chiaki.session && Chiaki.session.translatedText !== ""
                color: Material.backgroundColor
                border.color: Material.accent
                border.width: 2
                radius: 5
                
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 10
                    
                    ColumnLayout {
                        width: parent.width
                        spacing: 10
                        
                        Label {
                            text: "Оригинал:"
                            font.bold: true
                            color: Material.accent
                        }
                        
                        Label {
                            text: Chiaki.session ? Chiaki.session.originalText : ""
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        
                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: Material.accent
                        }
                        
                        Label {
                            text: "Перевод:"
                            font.bold: true
                            color: Material.accent
                        }
                        
                        TextArea {
                            text: Chiaki.session ? Chiaki.session.translatedText : ""
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            readOnly: false
                            selectByMouse: true
                            font.pixelSize: 14
                            background: Rectangle { color: "transparent" }
                        }
                    }
                }
            }
            
            Label {
                text: "Примечание: Для автоматического распознавания (Touchpad+L3+R3) нужен OCR.space API ключ"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                font.italic: true
                font.pixelSize: 10
                color: Material.color(Material.Grey)
            }
        }
    }
}

