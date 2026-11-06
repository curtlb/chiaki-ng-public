import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

import "controls" as C

DialogView {
    id: dialog
    title: qsTr("Sign in with PSN")
    buttonVisible: false

    function tryCapture(pageText) {
        if (Chiaki.savePsnCloudNpsso(pageText)) {
            statusLabel.text = qsTr("PSN account saved. Catalog and cloud launch will use it.");
            statusLabel.color = Material.accent;
            closeTimer.start();
            return true;
        }
        return false;
    }

    Timer {
        id: closeTimer
        interval: 900
        onTriggered: dialog.close()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            id: statusLabel
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Sign in on Sony's page. The app keeps the NPSSO cookie for this account, the same credential the cloud catalog and game launch redeem.")
        }

        Item {
            id: webHost
            Layout.fillWidth: true
            Layout.fillHeight: true
            property Item web: null
            Component.onCompleted: {
                try {
                    web = Qt.createQmlObject("
                        import QtWebEngine
                        import QtQuick
                        WebEngineView {
                            profile {
                                offTheRecord: false
                                storageName: 'psn-cloud-account'
                            }
                            onLoadingChanged: function(load) {
                                if (load.status !== WebEngineView.LoadSucceededStatus)
                                    return;
                                var u = (load.url ? load.url.toString() : '');
                                if (u.indexOf('/api/v1/ssocookie') < 0)
                                    return;
                                runJavaScript(\"document.body ? (document.body.innerText || document.body.textContent || '') : ''\", function(text) {
                                    dialog.tryCapture(text);
                                });
                            }
                        }", webHost, "psnAccountWeb");
                    Chiaki.setWebEngineHints(web.profile);
                    web.anchors.fill = webHost;
                    web.url = "https://ca.account.sony.com/api/v1/ssocookie";
                } catch (error) {
                    statusLabel.text = qsTr("The embedded browser is unavailable. Open the Sony page, copy the JSON, and paste it below.");
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            C.Button {
                text: qsTr("Open in browser")
                onClicked: Chiaki.openNpssoPage()
                Material.roundedScale: Material.SmallScale
            }
            TextField {
                id: pasteField
                Layout.fillWidth: true
                placeholderText: qsTr("Paste the ssocookie JSON")
                echoMode: TextInput.Password
            }
            C.Button {
                text: qsTr("Save")
                onClicked: {
                    if (!dialog.tryCapture(pasteField.text))
                        statusLabel.text = qsTr("That page does not contain an NPSSO cookie yet. Finish sign-in and try again.");
                }
                Material.roundedScale: Material.SmallScale
            }
        }
    }
}
