import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

Rectangle {
    id: card
    
    property var gameData
    // HoverHandler (unlike MouseArea hover) stays hovered while the pointer is over
    // child mouse areas, so the revealed action row doesn't flicker in a hover loop
    property bool isHovered: cardHoverHandler.hovered
    property bool isCurrentItem: GridView.isCurrentItem || false
    property bool hasFocus: isCurrentItem && GridView.view.activeFocus
    property bool isPsnow: isPsnowGame()
    property string cachedImageUrl: ""
    property int imageFallbackIndex: 0
    property var qrCodeDialog: null // Reference to QR code dialog
    // With 4cloud hourly billing, PS5 cloud titles marked "purchaseable" in the user's
    // catalog are played via a rented PS account NPSSO after payment — not the user's library.
    function isCloudBillingServerConfigured() {
        if (!Chiaki || !Chiaki.settings)
            return false;
        return Chiaki.settings.cloudBillingEnabled
            && (Chiaki.settings.cloudBillingHost || "").length > 0;
    }
    function isCloudBillingRentalActive() {
        return isCloudBillingServerConfigured();
    }
    readonly property bool needsAddToLibrary: gameData && gameData.category === "purchaseable"
        && !isCloudBillingRentalActive()
    property bool isFavorite: false // Whether this game is favorited
    
    // Steam library shortcut: shown when a Steam install is detected on this device (steam-shortcut build only)
    readonly property bool showCloudSteamShortcut: Chiaki.cloudSteamShortcutEnabled
        && !needsAddToLibrary
    
    signal streamGame(string productId, string platform, string serviceType)
    signal createShortcut(string productId, string entitlementId, string platform, string serviceType, string gameName)
    signal toggleFavorite(string productId)
    
    // Generate controller button icon path
    function getControllerIcon(buttonName) {
        let type = "deck";
        for (let i = 0; i < Chiaki.controllers.length; ++i) {
            if (Chiaki.controllers[i].playStation) {
                type = "ps";
                break;
            }
        }
        return `image://svg/button-${type}#${buttonName}`;
    }
    
    // The unified catalog (libchiaki) precomputes serviceType for every row; this is a
    // read-only convenience, NOT a re-derivation.
    function isPsnowGame() {
        return !!(gameData && gameData.serviceType === "psnow");
    }

    // Extract game information
    function getGameName() {
        if (!gameData) return qsTr("Unknown Game");
        if (gameData.name) return gameData.name;
        if (gameData.game_meta && gameData.game_meta.name) return gameData.game_meta.name;
        return qsTr("Unknown Game");
    }
    
    // Get product ID (general purpose - may return entitlement ID for PSCloud if product_id not available)
    function getProductId() {
        if (!gameData) return "";
        // Prioritize product_id/productId over id
        if (gameData.product_id) return gameData.product_id; // Owned games (PSCloud library)
        if (gameData.productId) return gameData.productId; // Game catalog
        if (gameData.id) return gameData.id; // Fallback: PSNOW or if product_id/productId not available
        return "";
    }
    
    // productId for the per-game details API (fetchGameDetails). The unified contract
    // exposes the canonical catalog productId; no platform guessing needed.
    function getProductIdForApi() {
        if (!gameData) return "";
        return gameData.productId || gameData.product_id || gameData.id || "";
    }

    // Exact id handed to the streaming session. Precomputed by libchiaki
    // (chiaki/cloudcatalog.h "streamIdentifier"); read it verbatim.
    function getStreamingIdentifier() {
        if (!gameData) return "";
        return gameData.streamIdentifier || getProductId();
    }

    // Platform badge, precomputed (ps3/ps4/ps5).
    function getPlatform() {
        return (gameData && gameData.platform) ? gameData.platform : "ps4";
    }

    // serviceType selects the catalog/shortcut routing (psnow vs pscloud); precomputed.
    function getServiceType() {
        return (gameData && gameData.serviceType) ? gameData.serviceType : "pscloud";
    }

    // The endpoint the stream action targets (may differ from catalog serviceType for
    // some owned cross-buy rows). Precomputed by libchiaki.
    function getStreamServiceType() {
        if (gameData && gameData.streamServiceType) return gameData.streamServiceType;
        return getServiceType();
    }
    
    function titleSkuFromPid(pid) {
        if (!pid) return "";
        let raw = pid;
        let dash = pid.indexOf("-");
        if (dash >= 0 && dash + 1 < pid.length) {
            let rest = pid.substring(dash + 1);
            let next = rest.indexOf("-");
            raw = next > 0 ? rest.substring(0, next) : rest;
        }
        let up = raw.toUpperCase();
        let prefixes = ["CUSA", "PPSA", "NPEA", "NPEB", "NPUB", "NPUA", "NPUG"];
        for (let i = 0; i < prefixes.length; i++) {
            let pref = prefixes[i];
            if (!up.startsWith(pref))
                continue;
            let rest = up.substring(pref.length);
            if (rest.indexOf("_") >= 0)
                return up;
            if (/^[0-9]+$/.test(rest))
                return pref + rest + "_00";
            return up;
        }
        return raw;
    }

    function chihiroCandidates(pid) {
        let out = [];
        if (!pid) return out;
        let prefix = pid.substring(0, 2).toUpperCase();
        let country = (prefix === "UP" || prefix === "HP" || prefix === "HN") ? "US" : "GB";
        let sku = titleSkuFromPid(pid);
        if (!sku) return out;
        // titlecontainer works; full-id /container/ 404s
        out.push(`https://store.playstation.com/store/api/chihiro/00_09_000/titlecontainer/${country}/en/999/${sku}/image?w=440&h=440`);
        if (country !== "US")
            out.push(`https://store.playstation.com/store/api/chihiro/00_09_000/titlecontainer/US/en/999/${sku}/image?w=440&h=440`);
        if (country !== "GB")
            out.push(`https://store.playstation.com/store/api/chihiro/00_09_000/titlecontainer/GB/en/999/${sku}/image?w=440&h=440`);
        return out;
    }

    function getImageUrl() {
        if (!gameData) return "";
        
        if (gameData.extracted_images) {
            if (gameData.extracted_images.cover) return gameData.extracted_images.cover;
            if (gameData.extracted_images.landscape) return gameData.extracted_images.landscape;
        }
        
        let url = gameData.imageUrl || "";
        // Drop broken chihiro /container/{full-id}/image URLs (404)
        if (url.indexOf("/chihiro/") >= 0 && url.indexOf("/container/") >= 0 && url.indexOf("/titlecontainer/") < 0)
            url = "";
        if (url)
            return url;
        if (gameData.images && Array.isArray(gameData.images) && gameData.images.length > 0) {
            for (let i = 0; i < gameData.images.length; i++) {
                let img = gameData.images[i];
                if (img && img.url && img.type === 10) return img.url;
            }
            for (let i = 0; i < gameData.images.length; i++) {
                let img = gameData.images[i];
                if (img && img.url && (img.type === 12 || img.type === 13)) return img.url;
            }
            for (let i = 0; i < gameData.images.length; i++) {
                let img = gameData.images[i];
                if (img && img.url) return img.url;
            }
        }
        let pid = getProductIdForApi() || getProductId();
        let candidates = chihiroCandidates(pid);
        return candidates.length > 0 ? candidates[0] : "";
    }

    function nextImageFallback() {
        let pid = getProductIdForApi() || getProductId();
        let candidates = [];
        let primary = getImageUrl();
        if (primary)
            candidates.push(primary);
        chihiroCandidates(pid).forEach((u) => {
            if (candidates.indexOf(u) < 0)
                candidates.push(u);
        });
        imageFallbackIndex++;
        if (imageFallbackIndex < candidates.length) {
            cachedImageUrl = candidates[imageFallbackIndex];
            return true;
        }
        return false;
    }
    
    onGameDataChanged: {
        imageFallbackIndex = 0;
        cachedImageUrl = getImageUrl();
    }
    
    Component.onCompleted: {
        imageFallbackIndex = 0;
        cachedImageUrl = getImageUrl();
    }
    
    color: isHovered || isCurrentItem ? "#1a222d" : "#121820"
    radius: 12
    border.width: (isHovered || isCurrentItem) ? 1 : 0
    border.color: isHovered || isCurrentItem ? Qt.rgba(0.18, 0.77, 0.71, 0.45) : "transparent"
    
    Behavior on color { ColorAnimation { duration: 160 } }
    Behavior on border.color { ColorAnimation { duration: 160 } }
    
    HoverHandler {
        id: cardHoverHandler
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        onClicked: parent.GridView.view.currentIndex = index
    }
    
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4
        
        // Game Image with overlays
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 140
            color: "transparent"
            radius: 4
            clip: true
            
            Image {
                id: gameImage
                anchors.fill: parent
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: true
                smooth: true
                
                source: cachedImageUrl || (gameData ? getImageUrl() : "")
                
                onStatusChanged: {
                    if (status === Image.Error)
                        nextImageFallback();
                }
                
                BusyIndicator {
                    anchors.centerIn: parent
                    running: gameImage.status === Image.Loading
                    visible: running
                }
                
                Label {
                    anchors.centerIn: parent
                    text: getGameName().substring(0, 2)
                    font.pixelSize: 48
                    font.bold: true
                    opacity: 0.3
                    visible: gameImage.status !== Image.Ready && gameImage.status !== Image.Loading
                }
            }
            
            // Favorite star button - Top Left (no background)
            Item {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.topMargin: 0
                anchors.leftMargin: 8
                width: 30
                height: 30
                
                Label {
                    id: favoriteStarLabel
                    anchors.centerIn: parent
                    text: card.isFavorite ? "★" : "☆"
                    font.pixelSize: 24
                    color: card.isFavorite ? "#FFD700" : "#FFFFFF"
                    style: Text.Outline
                    styleColor: "black"
                }
                
                MouseArea {
                    id: favoriteMouseArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        let productId = getProductId();
                        if (productId) {
                            toggleFavorite(productId);
                        }
                        mouse.accepted = true;
                    }
                }
            }
            
            // Category badge - Top Right (owned / streamable / purchaseable)
            Rectangle {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 8
                anchors.rightMargin: 8
                width: categoryLabel.implicitWidth + 12
                height: 22
                radius: 4
                visible: gameData && gameData.category
                color: {
                    if (!gameData) return "#e0a84a";
                    if (gameData.category === "owned") return "#1a8f84";
                    if (gameData.category === "streamable") return "#2a5f8f";
                    return "#a8782e";
                }

                Label {
                    id: categoryLabel
                    anchors.centerIn: parent
                    text: {
                        if (!gameData || !gameData.category) return "";
                        if (gameData.category === "owned") return qsTr("КУПЛЕНО");
                        if (gameData.category === "streamable") return qsTr("ПОТОК");
                        if (gameData.category === "purchaseable" && isCloudBillingRentalActive())
                            return qsTr("ИГРАТЬ");
                        return qsTr("ДОБАВИТЬ");
                    }
                    font.pixelSize: 10
                    font.weight: Font.Bold
                    color: "white"
                }
            }
            
            // Bottom overlay: title always visible; action row revealed on hover/focus
            // (progressive disclosure — controller hints live in the footer legend, not here)
            Rectangle {
                id: titleOverlay
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                readonly property bool showActions: card.isHovered || card.isCurrentItem
                readonly property real titleAreaHeight: Math.max(titleRow.implicitHeight + 12, 36)
                height: titleAreaHeight + (showActions ? actionsRow.height + 8 : 0)
                color: Qt.rgba(0, 0, 0, 0.6)

                Behavior on height { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

                RowLayout {
                    id: titleRow
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: titleOverlay.titleAreaHeight - 12
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    anchors.topMargin: 6
                    spacing: 6
                    
                    Label {
                        id: titleLabel
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        Layout.minimumWidth: 0
                        text: getGameName()
                        font.pixelSize: 14
                        font.bold: true
                        color: "white"
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.NoWrap
                    }
                    
                    Rectangle {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: platformLabel.implicitWidth + 12
                        Layout.preferredHeight: 24
                        radius: 4
                        color: Qt.rgba(0, 0, 0, 0.7)
                        visible: getPlatform() !== ""
                        
                        Label {
                            id: platformLabel
                            anchors.centerIn: parent
                            text: {
                                let platform = getPlatform();
                                if (platform === "ps3") return "3";
                                if (platform === "ps4") return "4";
                                if (platform === "ps5") return "5";
                                return "";
                            }
                            font.pixelSize: 14
                            font.weight: Font.Bold
                            color: "#FFD700"
                        }
                    }
                }

                // Action row — only on the hovered/focused card (text carries the meaning;
                // controller glyphs stay in the footer legend)
                RowLayout {
                    id: actionsRow
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    anchors.bottomMargin: 6
                    height: 28
                    spacing: 6
                    opacity: titleOverlay.showActions ? 1 : 0
                    visible: opacity > 0
                    Behavior on opacity { NumberAnimation { duration: 150 } }

                    // Add to Steam — secondary, Square/X glyph; the footer legend carries the label
                    // (shown when Steam is installed; hidden for non-owned in "All" filter)
                    Rectangle {
                        Layout.preferredWidth: 34
                        Layout.fillHeight: true
                        visible: showCloudSteamShortcut
                        radius: 6
                        color: shortcutMouseArea.containsMouse ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(1, 1, 1, 0.06)
                        border.width: 1
                        border.color: shortcutMouseArea.containsMouse ? Qt.rgba(1, 1, 1, 0.45) : Qt.rgba(1, 1, 1, 0.22)

                        Behavior on color { ColorAnimation { duration: 150 } }
                        Behavior on border.color { ColorAnimation { duration: 150 } }

                        MouseArea {
                            id: shortcutMouseArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor

                            onClicked: {
                                let productIdForApi = getProductIdForApi();
                                let entitlementId = getStreamingIdentifier(); // For PSCloud: entitlement ID, for PSNOW: product ID
                                let platform = getPlatform();
                                let serviceType = getServiceType();
                                let gameName = getGameName();

                                if (productIdForApi !== "") {
                                    // Open dialog - it will fetch game details itself using productIdForApi
                                    // entitlementId is used for the launch command
                                    console.log("[CloudGameCard] Opening shortcut dialog, productId for API:", productIdForApi, "entitlementId:", entitlementId, "isPsnow:", isPsnow);
                                    createShortcut(productIdForApi, entitlementId, platform, serviceType, gameName);
                                } else {
                                    console.warn("[CloudGameCard] Cannot create shortcut - missing product ID for API");
                                }
                            }
                        }

                        Image {
                            anchors.centerIn: parent
                            width: 16
                            height: 16
                            sourceSize: Qt.size(32, 32)
                            source: getControllerIcon("box")
                            opacity: 0.9
                            smooth: true
                            antialiasing: true
                        }
                    }

                    // Play — primary CTA (muted accent fill + ring; stronger on hover)
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 6
                        readonly property color streamFillIdle: Qt.alpha(Material.accent, 0.25)
                        readonly property color streamFillHover: Qt.alpha(Material.accent, 0.45)
                        readonly property color streamBorderIdle: Qt.alpha(Material.accent, 0.55)
                        readonly property color streamBorderHover: Qt.alpha(Material.accent, 0.95)
                        color: streamMouseArea.containsMouse ? streamFillHover : streamFillIdle
                        border.width: 1
                        border.color: streamMouseArea.containsMouse ? streamBorderHover : streamBorderIdle

                        Behavior on color { ColorAnimation { duration: 150 } }
                        Behavior on border.color { ColorAnimation { duration: 150 } }

                        MouseArea {
                            id: streamMouseArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor

                            onClicked: {
                                console.log("[CloudGameCard] Button clicked - isPsnow:", isPsnow, "gameData:", gameData, "isOwned:", gameData ? gameData.isOwned : "N/A");
                                console.log("[CloudGameCard] qrCodeDialog:", qrCodeDialog);

                                // Check if this is a non-owned game in "All" filter mode
                                if (needsAddToLibrary) {
                                    console.log("[CloudGameCard] Condition met for QR code - showing dialog");
                                    // Show QR code dialog with conceptUrl
                                    let conceptUrl = gameData.conceptUrl || gameData.concept_url;
                                    console.log("[CloudGameCard] conceptUrl:", conceptUrl);
                                    console.log("[CloudGameCard] qrCodeDialog type:", typeof qrCodeDialog, "qrCodeDialog value:", qrCodeDialog);

                                    if (conceptUrl) {
                                        console.log("[CloudGameCard] conceptUrl found:", conceptUrl);
                                        if (qrCodeDialog) {
                                            console.log("[CloudGameCard] Calling qrCodeDialog.showDialog()");
                                            qrCodeDialog.showDialog(conceptUrl);
                                            console.log("[CloudGameCard] showDialog() called");
                                        } else {
                                            console.error("[CloudGameCard] ERROR: qrCodeDialog is null/undefined!");
                                        }
                                    } else {
                                        console.error("[CloudGameCard] ERROR: conceptUrl is missing!");
                                    }
                                } else {
                                    console.log("[CloudGameCard] Normal stream behavior");
                                    // Normal stream behavior - use getStreamingIdentifier for correct ID
                                    let streamingId = getStreamingIdentifier();
                                    let platform = getPlatform();
                                    let serviceType = getServiceType();
                                    if (streamingId !== "") {
                                        streamGame(streamingId, platform, getStreamServiceType());
                                    }
                                }
                            }
                        }

                        Row {
                            anchors.centerIn: parent
                            spacing: 7

                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                text: needsAddToLibrary ? qsTr("Добавить в библиотеку") : qsTr("Играть")
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                color: streamMouseArea.containsMouse ? "#FFFFFF" : "#DCECF3"
                                Behavior on color { ColorAnimation { duration: 150 } }
                            }
                        }
                    }
                }
            }
        }
    }
    
    Keys.onPressed: (event) => {
        // Cross/A button (Enter/Space) - Stream game or show QR code
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Space || event.key === Qt.Key_Enter) {
            // Check if this is a non-owned game in "All" filter mode
            if (needsAddToLibrary) {
                // Show QR code dialog with conceptUrl
                let conceptUrl = gameData.conceptUrl || gameData.concept_url;
                if (conceptUrl && qrCodeDialog) {
                    qrCodeDialog.showDialog(conceptUrl);
                    event.accepted = true;
                }
            } else {
                // Normal stream behavior - use getStreamingIdentifier for correct ID
                let streamingId = getStreamingIdentifier();
                let platform = getPlatform();
                let serviceType = getServiceType();
                if (streamingId !== "") {
                    streamGame(streamingId, platform, getStreamServiceType());
                    event.accepted = true;
                }
            }
        }
        // Square/X button (X key) - Create shortcut
        else if (event.key === Qt.Key_X && showCloudSteamShortcut) {
            let productIdForApi = getProductIdForApi();
            let entitlementId = getStreamingIdentifier(); // For PSCloud: entitlement ID, for PSNOW: product ID
            let platform = getPlatform();
            let serviceType = getServiceType();
            let gameName = getGameName();
            
            if (productIdForApi !== "") {
                // Open dialog - it will fetch game details itself using productIdForApi
                // entitlementId is used for the launch command
                console.log("[CloudGameCard] Opening shortcut dialog (keyboard), productId for API:", productIdForApi, "entitlementId:", entitlementId, "isPsnow:", isPsnow);
                createShortcut(productIdForApi, entitlementId, platform, serviceType, gameName);
                event.accepted = true;
            } else {
                console.warn("[CloudGameCard] Cannot create shortcut (keyboard) - missing product ID for API");
            }
        }
    }
}

