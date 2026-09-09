import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Effects

import org.streetpea.chiaking

import "controls" as C

Pane {
    id: root
    padding: 0
    
    property var mainTabBar: null
    property var settingsButton: null
    property var showConfirmDialogFunc: null
    
    // Expose child components for navigation
    readonly property Item catalogButtonItem: searchContainer
    readonly property Item searchContainerItem: searchContainer
    readonly property Item refreshButtonItem: refreshButton
    
    property var allGames: [] // unused; kept for compatibility
    property int catalogTotalCount: 0
    property int filteredGameCount: 0
    property var filteredGames: []
    property var recentGames: []
    property var currentPageGames: []
    property bool isLoading: false
    property bool isSearching: false
    property string searchQuery: ""
    property string authErrorMessage: ""
    property string fallbackRegion: ""
    property bool catalogNativeMode: true
    property var activeTagFilters: [] // empty = show all; values: owned, streamable, purchaseable
    property bool showFavoritesOnly: false
    property int sortState: 0 // 0=Playable First, 1=A-Z, 2=Z-A
    property var favoriteProductIds: []
    property var qrCodeDialogRef: null

    readonly property var tagFilterCategories: ["owned", "streamable", "purchaseable"]
    readonly property var tagFilterLabels: [qsTr("Owned"), qsTr("Streamable"), qsTr("Store")]
    readonly property int maxGridGames: 600
    property bool gridTruncated: false
    property var catalogFetchToken: null
    
    // Clean blue background
    CleanBlueBackground {
        anchors.fill: parent
        z: -2
    }
    
    function controllerButton(name) {
        let type = "deck";
        for (let i = 0; i < Chiaki.controllers.length; ++i) {
            if (Chiaki.controllers[i].playStation) {
                type = "ps";
                break;
            }
        }
        return `image://svg/button-${type}#${name}`;
    }
    
    Component.onCompleted: {
        fallbackRegion = Chiaki.settings.cloudResolvedStoreCountry || "";
        catalogNativeMode = Chiaki.settings.cloudCatalogNativeMode;
        sortState = Chiaki.settings.cloudSortState || 0;
        let savedTagFilters = Chiaki.settings.cloudTagFilters;
        if (savedTagFilters) {
            try {
                let parsed = JSON.parse(savedTagFilters);
                if (Array.isArray(parsed))
                    activeTagFilters = parsed;
            } catch (e) {
                console.warn("Failed to parse cloud tag filters:", e);
            }
        }
        let savedFavorites = Chiaki.settings.cloudFavorites;
        if (savedFavorites) {
            try {
                favoriteProductIds = JSON.parse(savedFavorites);
            } catch (e) {
                console.error("Failed to parse saved favorites:", e);
                favoriteProductIds = [];
            }
        }
        Qt.callLater(() => loadUnifiedCatalog());
        initialFocusTimer.restart();
        if (Chiaki.ensureFourcloudPolling)
            Chiaki.ensureFourcloudPolling();
    }

    Connections {
        target: Chiaki.settings
        function onFourCloudEmailChanged() {
            // After account switch / logout / login, refresh cloud auth banner.
            Qt.callLater(() => loadUnifiedCatalog());
        }
        function onCloudBillingUserIdChanged() {
            if (visible)
                applySearchFilter();
        }
    }
    
    onVisibleChanged: {
        if (visible) {
            if (catalogTotalCount === 0)
                loadUnifiedCatalog();
            else
                applySearchFilter();
            initialFocusTimer.restart();
        }
    }

    Timer {
        id: filterDebounceTimer
        interval: 400
        repeat: false
        onTriggered: {
            // Yield so the search spinner paints before a heavy catalog scan.
            Qt.callLater(() => {
                const q = (searchQuery || "").trim();
                if (q.length > 0 && isCloudBillingServerConfigured()) {
                    // First search loads PS Now via assigned-account NPSSO (may take a while).
                    Chiaki.cloudCatalog.ensurePsNowSearchCatalog(function(ok, message) {
                        // Avoid noisy toast when in-memory/disk cache is already usable.
                        if (!ok && message && message !== "not_billing"
                                && !Chiaki.cloudCatalog.psNowSearchCatalogReady())
                            showErrorToast(qsTr("Поиск"), message || qsTr("Не удалось загрузить каталог PS Now"));
                        applySearchFilter();
                        isSearching = false;
                    });
                } else {
                    applySearchFilter();
                    isSearching = false;
                }
            });
        }
    }

    // Account/profile switch, NPSSO change, or cloud-language change wipes the catalog cache in the
    // backend; reload here so the visible grid never keeps showing the previous account's games.
    Connections {
        target: Chiaki.cloudCatalog
        function onCacheInvalidated() {
            loadUnifiedCatalog();
        }
    }

    // Pins default focus to the first game card (or the filter toggle if games
    // haven't loaded yet) after startup focus churn settles, so the search field
    // never holds focus by default. Runs late enough to override the window's
    // initial active-focus assignment.
    Timer {
        id: initialFocusTimer
        interval: 150
        repeat: false
        onTriggered: {
            if (gamesGrid.count > 0) {
                gamesGrid.currentIndex = 0;
                gamesGrid.forceActiveFocus();
            } else {
                filterToggle.forceActiveFocus();
            }
        }
    }
    
    // Handle Escape/B button for quit confirmation dialog
    Keys.onEscapePressed: {
        if (showConfirmDialogFunc) {
            showConfirmDialogFunc(qsTr("Quit"), qsTr("Are you sure you want to quit?"), () => Qt.quit());
        }
    }
    
    // Handle RB/LB navigation for section switching
    Keys.onPressed: (event) => {
        if (event.modifiers)
            return;
        
        // Handle B button (Back key) for quit confirmation dialog
        if (event.key === Qt.Key_Back) {
            if (showConfirmDialogFunc) {
                showConfirmDialogFunc(qsTr("Quit"), qsTr("Are you sure you want to quit?"), () => Qt.quit());
            }
            event.accepted = true;
            return;
        }
        
    }

    function tagFilterSummary() {
        if (!activeTagFilters || activeTagFilters.length === 0)
            return qsTr("All games");
        let labels = [];
        for (let i = 0; i < tagFilterCategories.length; i++) {
            if (activeTagFilters.indexOf(tagFilterCategories[i]) !== -1)
                labels.push(tagFilterLabels[i]);
        }
        return labels.length > 0 ? labels.join(" · ") : qsTr("All games");
    }

    function isTagFilterActive(tag) {
        return !activeTagFilters || activeTagFilters.length === 0
               || activeTagFilters.indexOf(tag) !== -1;
    }

    function setTagFilters(tags) {
        activeTagFilters = tags;
        Chiaki.settings.cloudTagFilters = JSON.stringify(tags);
        applySearchFilter();
    }

    function toggleTagFilter(tag) {
        // Empty active set means "all selected", so start from every category and remove from there.
        let current = (!activeTagFilters || activeTagFilters.length === 0)
                      ? tagFilterCategories.slice()
                      : activeTagFilters.slice();
        let idx = current.indexOf(tag);
        if (idx !== -1)
            current.splice(idx, 1);
        else
            current.push(tag);
        if (current.length === 0 || current.length === tagFilterCategories.length)
            setTagFilters([]);
        else
            setTagFilters(current);
    }

    function isCloudBillingServerConfigured() {
        if (!Chiaki || !Chiaki.settings)
            return false;
        return Chiaki.settings.cloudBillingEnabled
            && (Chiaki.settings.cloudBillingHost || "").length > 0;
    }

    function isCloudBillingActive() {
        if (!isCloudBillingServerConfigured())
            return false;
        return (Chiaki.settings.fourCloudEmail || "").length > 0;
    }

    function launchCloudGameFromCard(modelData, streamingId, platform, serviceType) {
        let gameName = "";
        if (modelData) {
            gameName = modelData.name || "";
            if (!gameName && modelData.game_meta && modelData.game_meta.name)
                gameName = modelData.game_meta.name;
        }

        function launchCloudStream(accountId) {
            let mainComp = root;
            while (mainComp && !mainComp.showStreamView) {
                mainComp = mainComp.parent;
            }
            if (mainComp && mainComp.showStreamView) {
                mainComp.showStreamView();
            }
            Chiaki.cloudStreaming.startCompleteCloudSession(
                serviceType,
                streamingId,
                gameName,
                platform || "",
                accountId || 0,
                function(success, message, serverIp) {
                    if (!success) {
                        let isOAuthError = message && (message.includes("OAuth") || message.includes("authorization"));
                        Chiaki.error(qsTr("Ошибка облачного стрима"), message, isOAuthError ? 10000 : 3000);
                    } else {
                        applySearchFilter();
                    }
                }
            );
        }

        function confirmAndLaunch(accountId) {
            Chiaki.cloudStreaming.fetchBillingQuote(
                serviceType,
                streamingId,
                gameName,
                accountId || 0,
                function(ok, message, hourlyPrice, resumeSession) {
                    if (!ok) {
                        Chiaki.error(qsTr("Оплата"), message || qsTr("Не удалось получить информацию об оплате"), 8000);
                        return;
                    }
                    let title = resumeSession ? qsTr("Продолжить игру") : qsTr("Оплата за час игры");
                    let priceLine = (!resumeSession && hourlyPrice > 0)
                        ? qsTr("\n\nСумма: %1 ₽ за 1 час.").arg(Math.round(hourlyPrice))
                        : "";
                    let actionLine = resumeSession
                        ? qsTr("\n\nНажмите «Да» — стрим продолжится без списания.")
                        : qsTr("\n\nНажмите «Да» — произойдёт списание с привязанной карты и запуск стрима.");
                    let confirmText = (message || qsTr("Списать оплату за 1 час игры?")) + priceLine + actionLine;
                    if (showConfirmDialogFunc) {
                        showConfirmDialogFunc(title, confirmText, () => launchCloudStream(accountId));
                    } else {
                        launchCloudStream(accountId);
                    }
                }
            );
        }

        function parseAccountChoices(choicesJson) {
            try {
                let arr = (typeof choicesJson === "string") ? JSON.parse(choicesJson || "[]") : (choicesJson || []);
                return Array.isArray(arr) ? arr : [];
            } catch (e) {
                return [];
            }
        }

        let useBilling = isCloudBillingActive();
        if (isCloudBillingServerConfigured() && !useBilling) {
            Chiaki.error(
                qsTr("4cloud.pro"),
                qsTr("Войдите в аккаунт 4cloud.pro в приложении, затем повторите запуск."),
                10000
            );
            return;
        }
        if (!useBilling) {
            launchCloudStream(0);
            return;
        }
        Chiaki.cloudStreaming.fetchBillingQuote(
            serviceType,
            streamingId,
            gameName,
            function(ok, message, hourlyPrice, resumeSession, choicesJson) {
                if (!ok) {
                    Chiaki.error(qsTr("Оплата"), message || qsTr("Не удалось получить информацию об оплате"), 8000);
                    return;
                }
                let choices = parseAccountChoices(choicesJson);
                if (choices.length > 1) {
                    accountChoiceDialog.promptText = message
                        || qsTr("Игра куплена на нескольких аккаунтах. Выберите аккаунт:");
                    accountChoiceDialog.accounts = choices;
                    accountChoiceDialog.onPicked = function(accountId) {
                        confirmAndLaunch(accountId);
                    };
                    accountChoiceDialog.open();
                    return;
                }
                let accountId = (choices.length === 1) ? (choices[0].account_id || 0) : 0;
                let title = resumeSession ? qsTr("Продолжить игру") : qsTr("Оплата за час игры");
                let priceLine = (!resumeSession && hourlyPrice > 0)
                    ? qsTr("\n\nСумма: %1 ₽ за 1 час.").arg(Math.round(hourlyPrice))
                    : "";
                let actionLine = resumeSession
                    ? qsTr("\n\nНажмите «Да» — стрим продолжится без списания.")
                    : qsTr("\n\nНажмите «Да» — произойдёт списание с привязанной карты и запуск стрима.");
                let confirmText = (message || qsTr("Списать оплату за 1 час игры?")) + priceLine + actionLine;
                if (showConfirmDialogFunc) {
                    showConfirmDialogFunc(title, confirmText, () => launchCloudStream(accountId));
                } else {
                    launchCloudStream(accountId);
                }
            }
        );
    }

    function applySearchFilter() {
        try {
            let hadFocus = searchField && searchField.activeFocus;
            recentGames = Chiaki.cloudCatalog.recentDisplayGames(
                isCloudBillingServerConfigured(),
                16
            ) || [];
            let result = Chiaki.cloudCatalog.filterDisplayCatalog(
                searchQuery || "",
                activeTagFilters || [],
                showFavoritesOnly ? favoriteProductIds : [],
                sortState,
                isCloudBillingServerConfigured(),
                maxGridGames
            );
            filteredGameCount = result.totalFiltered || 0;
            if (result.totalGames !== undefined)
                catalogTotalCount = result.totalGames;
            currentPageGames = result.games || [];
            gridTruncated = !!result.truncated;

            if (hadFocus) {
                Qt.callLater(() => {
                    if (searchField)
                        searchField.forceActiveFocus();
                });
            }
        } catch (e) {
            console.error("applySearchFilter failed:", e);
            filteredGameCount = 0;
            currentPageGames = [];
            gridTruncated = false;
            showErrorToast(qsTr("Catalog Error"), qsTr("Failed to display catalog: %1").arg(e.toString()));
        }
    }

    function loadUnifiedCatalog() {
        console.log("[CloudPlayView] loadUnifiedCatalog()");
        let billingServer = isCloudBillingServerConfigured();
        let billingActive = isCloudBillingActive();
        if (billingServer && !billingActive) {
            authErrorMessage = qsTr("Войдите в 4cloud.pro — каталог с сервера доступен, но запуск игр требует аккаунт 4cloud.");
        } else if (!billingServer) {
            authErrorMessage = qsTr("Не настроен сервер биллинга — облачные игры недоступны.");
        } else {
            authErrorMessage = "";
        }

        let fetchToken = {};
        catalogFetchToken = fetchToken;

        if (gamesGrid.activeFocus)
            filterToggle.forceActiveFocus();

        catalogTotalCount = 0;
        filteredGameCount = 0;
        currentPageGames = [];
        isLoading = true;

        Chiaki.cloudCatalog.fetchUnifiedCatalog(function(success, message, jsonData) {
            if (catalogFetchToken !== fetchToken)
                return;
            isLoading = false;
            if (!success || !jsonData) {
                catalogTotalCount = 0;
                filteredGameCount = 0;
                currentPageGames = [];
                showErrorToast(qsTr("API Error"), message || qsTr("Failed to fetch game catalog"));
                return;
            }
            try {
                let data = (typeof jsonData === "string") ? JSON.parse(jsonData) : jsonData;
                if (data && data.totalGames !== undefined) {
                    catalogTotalCount = data.totalGames;
                    fallbackRegion = data.fallbackRegion || "";
                    catalogNativeMode = data.nativeMode !== false;
                    if (Chiaki.settings) {
                        Chiaki.settings.cloudResolvedStoreCountry = fallbackRegion;
                        Chiaki.settings.cloudCatalogNativeMode = catalogNativeMode;
                    }
                    if (data.warning && !billingActive)
                        authErrorMessage = data.warning;
                    else if (billingActive || billingServer)
                        authErrorMessage = "";
                    if (message && message !== "Success" && message !== "Cached")
                        showErrorToast(qsTr("Partial Catalog"), message);
                    applySearchFilter();
                    // Warm PS Now search catalog once per session (owned grid stays from billing DB).
                    if (billingServer) {
                        Chiaki.cloudCatalog.ensurePsNowSearchCatalog(function(ok, message) {
                            if (!ok && message && message !== "not_billing")
                                console.warn("[CloudPlayView] PS Now search prewarm:", message);
                        });
                    }
                    Qt.callLater(() => {
                        if (gamesGrid.count > 0
                                && !searchField.activeFocus
                                && !tagFilterPopup.opened) {
                            gamesGrid.currentIndex = 0;
                            gamesGrid.forceActiveFocus();
                        }
                    });
                } else {
                    showErrorToast(qsTr("Error"), qsTr("No games found in catalog"));
                }
            } catch (e) {
                console.error("Failed to parse unified catalog:", e);
                showErrorToast(qsTr("Parse Error"), qsTr("Failed to parse catalog data: %1").arg(e.toString()));
            }
        });
    }

    function toggleFavorite(productId) {
        if (!productId) return;
        
        let index = favoriteProductIds.indexOf(productId);
        let newFavorites = favoriteProductIds.slice(); // Create a new array
        
        if (index !== -1) {
            // Remove from favorites
            newFavorites.splice(index, 1);
        } else {
            // Add to favorites
            newFavorites.push(productId);
        }
        
        // Assign the new array to trigger property change notification
        favoriteProductIds = newFavorites;
        
        // Save to settings
        Chiaki.settings.cloudFavorites = JSON.stringify(favoriteProductIds);
        
        // Re-apply filter to update view
        applySearchFilter();
    }
    
    function showShortcutToast(title, message) {
        shortcutToastTitle.text = title;
        shortcutToastMessage.text = message;
        shortcutToast.color = "#2ec4b6";
        shortcutToastTimer.restart();
    }
    
    function showErrorToast(title, message) {
        errorToastTitle.text = title;
        errorToastMessage.text = message;
        errorToast.color = "#F44336";
        errorToastTimer.restart();
    }
    
    // Watch for search query changes (debounced — filter runs in C++)
    onSearchQueryChanged: {
        const q = (searchQuery || "").trim();
        if (q.length > 0) {
            isSearching = true;
            filterDebounceTimer.restart();
        } else {
            // Clearing search: restore owned-only grid immediately.
            filterDebounceTimer.stop();
            isSearching = false;
            applySearchFilter();
        }
    }
    
    // Single unified header - production quality design
    Rectangle {
        id: toolBar
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
        }
        height: 52
        
        color: Qt.rgba(7/255, 9/255, 13/255, 0.95)
        
        // Subtle bottom border
        Rectangle {
            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
            }
            height: 1
            color: Qt.rgba(46/255, 196/255, 182/255, 0.2)
        }
        
        RowLayout {
            anchors {
                fill: parent
                leftMargin: 25
                rightMargin: 25
                topMargin: 6
                bottomMargin: 6
            }
            spacing: 8

            // Acquisition-tag filter hidden for rental clients (owned-first catalog).
            Item {
                id: filterToggle
                visible: false
                width: 0
                height: 0
                Layout.preferredWidth: 0
                Layout.preferredHeight: 0
            }

            // Flexible gap pushes search + the right-side controls to the right edge.
            Item { Layout.fillWidth: true }

            // Search bar - icon that expands leftward when focused (right side, left of favorites)
            Rectangle {
                id: searchContainer
                Layout.preferredHeight: 36
                Layout.preferredWidth: searchContainer.activeFocus || searchField.activeFocus || searchField.text.length > 0 ? 360 : 36
                radius: 18
                color: searchContainer.activeFocus || searchField.activeFocus ? Qt.rgba(255, 255, 255, 0.15) : Qt.rgba(255, 255, 255, 0.1)
                border.color: searchContainer.activeFocus || searchField.activeFocus ? "#2ec4b6" : Qt.rgba(255, 255, 255, 0.2)
                border.width: searchContainer.activeFocus || searchField.activeFocus ? 2 : 1
                focusPolicy: Qt.StrongFocus
                // Keep search OUT of the automatic focus chain so it never grabs default
                // focus on launch. It's still reachable by click and arrow/controller nav.
                activeFocusOnTab: false
                
                Behavior on Layout.preferredWidth {
                    NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
                }
                Behavior on color {
                    ColorAnimation { duration: 200 }
                }
                Behavior on border.color {
                    ColorAnimation { duration: 200 }
                }
                
                onActiveFocusChanged: {
                    if (activeFocus) {
                        Qt.callLater(() => {
                            searchField.forceActiveFocus();
                        });
                    }
                }
                
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Space || event.key === Qt.Key_Enter) {
                        searchField.forceActiveFocus();
                        event.accepted = true;
                    }
                }
                
                Keys.onLeftPressed: {
                    // Filter toggle sits to the left of search now
                    filterToggle.forceActiveFocus();
                    event.accepted = true;
                }
                
                Keys.onRightPressed: {
                    favoritesToggle.forceActiveFocus();
                    event.accepted = true;
                }
                
                KeyNavigation.up: mainTabBar ? mainTabBar.itemAt(1) : null
                
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        searchField.forceActiveFocus();
                    }
                }
                
                RowLayout {
                    anchors {
                        fill: parent
                        leftMargin: searchField.activeFocus || searchField.text.length > 0 ? 16 : 0
                        rightMargin: searchField.activeFocus || searchField.text.length > 0 ? 16 : 0
                    }
                    spacing: 12
                    
                    // Search icon - visible when collapsed (custom magnifying glass icon)
                    Item {
                        Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
                        Layout.preferredWidth: 20
                        Layout.preferredHeight: 20
                        visible: !searchContainer.activeFocus && !searchField.activeFocus && searchField.text.length === 0
                        
                        Canvas {
                            anchors.fill: parent
                            onPaint: {
                                var ctx = getContext("2d");
                                ctx.strokeStyle = searchField.activeFocus ? "#2ec4b6" : Qt.rgba(255, 255, 255, 0.7);
                                ctx.lineWidth = 2;
                                ctx.lineCap = "round";
                                
                                // Draw magnifying glass circle
                                ctx.beginPath();
                                ctx.arc(8, 8, 5, 0, 2 * Math.PI);
                                ctx.stroke();
                                
                                // Draw handle
                                ctx.beginPath();
                                ctx.moveTo(12, 12);
                                ctx.lineTo(16, 16);
                                ctx.stroke();
                            }
                        }
                    }
                    
                    TextField {
                        id: searchField
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        visible: searchField.activeFocus || searchField.text.length > 0
                        opacity: visible ? 1 : 0
                        placeholderText: qsTr("Поиск по каталогу…")
                        font.pixelSize: 14
                        color: "white"
                        selectByMouse: true
                        focusPolicy: Qt.StrongFocus
                        // Not auto-focusable on launch; only via click / explicit navigation.
                        activeFocusOnTab: false
                        verticalAlignment: TextInput.AlignVCenter
                        topPadding: 0
                        bottomPadding: 0
                        background: Rectangle {
                            color: "transparent"
                        }
                        
                        Behavior on opacity {
                            NumberAnimation { duration: 200 }
                        }
                        
                        KeyNavigation.right: favoritesToggle
                        KeyNavigation.left: filterToggle
                        KeyNavigation.down: gamesGrid.count > 0 ? gamesGrid : null
                        
                        KeyNavigation.up: mainTabBar ? mainTabBar.itemAt(1) : null
                        
                        Keys.onLeftPressed: (event) => {
                            filterToggle.forceActiveFocus();
                            event.accepted = true;
                        }
                        
                        Keys.onReturnPressed: {
                            // When Enter is pressed, move focus to first game
                            if (gamesGrid.count > 0) {
                                gamesGrid.currentIndex = 0;
                                gamesGrid.forceActiveFocus();
                                event.accepted = true;
                            }
                        }
                        
                        onTextChanged: {
                            searchQuery = text;
                        }
                        
                        Keys.onEscapePressed: {
                            text = "";
                            searchQuery = "";
                            focus = false;
                        }
                    }
                    
                    Button {
                        visible: searchField.text.length > 0
                        opacity: visible ? 1 : 0
                        text: "×"
                        font.pixelSize: 18
                        font.weight: Font.Bold
                        Layout.preferredWidth: 26
                        Layout.preferredHeight: 26
                        flat: true
                        focusPolicy: Qt.NoFocus
                        onClicked: {
                            searchField.text = "";
                            searchQuery = "";
                            searchField.forceActiveFocus();
                        }
                        
                        Behavior on opacity {
                            NumberAnimation { duration: 200 }
                        }
                        
                        background: Rectangle {
                            radius: 13
                            color: parent.hovered ? Qt.rgba(255, 255, 255, 0.2) : "transparent"
                        }
                    }
                }
            }
            
            // Right side controls
            RowLayout {
                spacing: 0
                
                // Filter dialog: mirrors the proven ConfirmDialog pattern (overlay-parented,
                // root-centered, content-sized) so it centers correctly and captures input.
                Dialog {
                    id: tagFilterPopup
                    parent: Overlay.overlay
                    x: Math.round((root.width - width) / 2)
                    y: Math.round((root.height - height) / 2)
                    width: 320
                    modal: true
                    focus: true
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                    title: qsTr("Filter games")
                    Material.roundedScale: Material.MediumScale

                    Component.onCompleted: {
                        header.horizontalAlignment = Text.AlignHCenter;
                        // Qt 6.6: workaround dialog header background flashing transparent on close.
                        header.background = null;
                    }

                    background: Rectangle {
                        color: Qt.rgba(7/255, 9/255, 13/255, 0.98)
                        radius: 12
                        border.color: "#2ec4b6"
                        border.width: 2
                    }

                    // Sync checkbox visuals to current state on open, then capture focus so the
                    // grid behind never receives our Enter / confirm key.
                    onOpened: {
                        ownedCheck.checked = isTagFilterActive(tagFilterCategories[0]);
                        streamableCheck.checked = isTagFilterActive(tagFilterCategories[1]);
                        storeCheck.checked = isTagFilterActive(tagFilterCategories[2]);
                        ownedCheck.forceActiveFocus(Qt.TabFocusReason);
                    }
                    onClosed: filterToggle.forceActiveFocus()

                    ColumnLayout {
                        spacing: 10

                        CheckBox {
                            id: ownedCheck
                            text: tagFilterLabels[0]
                            Layout.fillWidth: true
                            focusPolicy: Qt.StrongFocus
                            onClicked: toggleTagFilter(tagFilterCategories[0])
                            KeyNavigation.down: streamableCheck
                            Keys.onReturnPressed: { toggle(); toggleTagFilter(tagFilterCategories[0]); event.accepted = true; }
                        }
                        CheckBox {
                            id: streamableCheck
                            text: tagFilterLabels[1]
                            Layout.fillWidth: true
                            focusPolicy: Qt.StrongFocus
                            onClicked: toggleTagFilter(tagFilterCategories[1])
                            KeyNavigation.up: ownedCheck
                            KeyNavigation.down: storeCheck
                            Keys.onReturnPressed: { toggle(); toggleTagFilter(tagFilterCategories[1]); event.accepted = true; }
                        }
                        CheckBox {
                            id: storeCheck
                            text: tagFilterLabels[2]
                            Layout.fillWidth: true
                            focusPolicy: Qt.StrongFocus
                            onClicked: toggleTagFilter(tagFilterCategories[2])
                            KeyNavigation.up: streamableCheck
                            KeyNavigation.down: showAllButton
                            Keys.onReturnPressed: { toggle(); toggleTagFilter(tagFilterCategories[2]); event.accepted = true; }
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignCenter
                            Layout.topMargin: 6
                            spacing: 12
                            Button {
                                id: showAllButton
                                text: qsTr("Show all")
                                focusPolicy: Qt.StrongFocus
                                Material.roundedScale: Material.SmallScale
                                onClicked: {
                                    setTagFilters([]);
                                    ownedCheck.checked = true;
                                    streamableCheck.checked = true;
                                    storeCheck.checked = true;
                                    tagFilterPopup.close();
                                }
                                KeyNavigation.up: storeCheck
                                KeyNavigation.right: closeButton
                                Keys.onReturnPressed: { clicked(); event.accepted = true; }
                            }
                            Button {
                                id: closeButton
                                text: qsTr("Close")
                                focusPolicy: Qt.StrongFocus
                                Material.roundedScale: Material.SmallScale
                                onClicked: tagFilterPopup.close()
                                KeyNavigation.up: storeCheck
                                KeyNavigation.left: showAllButton
                                Keys.onReturnPressed: { clicked(); event.accepted = true; }
                            }
                        }
                    }
                }

                // Favorites filter toggle
                Item {
                    id: favoritesToggle
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 36
                    Layout.rightMargin: 8

                    Rectangle {
                        anchors.fill: parent
                        color: favoritesToggle.activeFocus ? Qt.rgba(46/255, 196/255, 182/255, 0.15) : "transparent"
                        border.color: favoritesToggle.activeFocus ? "#2ec4b6" : "transparent"
                        border.width: favoritesToggle.activeFocus ? 1 : 0
                        radius: 4
                    }

                    // Star glyph: filled gold when favorites-only is active, outline otherwise
                    Canvas {
                        id: favoritesStar
                        anchors.centerIn: parent
                        width: 20; height: 20
                        property bool active: showFavoritesOnly
                        onActiveChanged: requestPaint()
                        onPaint: {
                            var ctx = getContext("2d");
                            ctx.reset();
                            var cx = 10, cy = 10.5, spikes = 5, outer = 8.5, inner = 3.6;
                            var rot = -Math.PI / 2;
                            var step = Math.PI / spikes;
                            ctx.beginPath();
                            ctx.moveTo(cx + Math.cos(rot) * outer, cy + Math.sin(rot) * outer);
                            for (var i = 0; i < spikes; i++) {
                                rot += step;
                                ctx.lineTo(cx + Math.cos(rot) * inner, cy + Math.sin(rot) * inner);
                                rot += step;
                                ctx.lineTo(cx + Math.cos(rot) * outer, cy + Math.sin(rot) * outer);
                            }
                            ctx.closePath();
                            if (active) {
                                ctx.fillStyle = "#FFD700";
                                ctx.fill();
                            } else {
                                ctx.strokeStyle = Qt.rgba(255, 255, 255, 0.7);
                                ctx.lineWidth = 1.6;
                                ctx.lineJoin = "round";
                                ctx.stroke();
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { showFavoritesOnly = !showFavoritesOnly; applySearchFilter(); }
                    }

                    focusPolicy: Qt.StrongFocus
                    KeyNavigation.left: searchContainer
                    KeyNavigation.right: refreshButton
                    KeyNavigation.down: gamesGrid.count > 0 ? gamesGrid : null
                    KeyNavigation.up: mainTabBar ? mainTabBar.itemAt(1) : null
                    Keys.onReturnPressed: { showFavoritesOnly = !showFavoritesOnly; applySearchFilter(); event.accepted = true; }
                }

                // Refresh button (icon-only, matches Android/iOS) — plain Item so the
                // bundled Material SVG renders at full size, uniform with the star/sort glyphs.
                Item {
                    id: refreshButton
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 36
                    Layout.rightMargin: 8
                    enabled: !isLoading

                    function activate() {
                        if (!enabled)
                            return;
                        // invalidateCache() emits cacheInvalidated, which triggers the reload above.
                        Chiaki.cloudCatalog.invalidateCache();
                    }

                    Rectangle {
                        anchors.fill: parent
                        color: refreshButton.activeFocus ? Qt.rgba(46/255, 196/255, 182/255, 0.15) : "transparent"
                        border.color: refreshButton.activeFocus ? "#2ec4b6" : "transparent"
                        border.width: refreshButton.activeFocus ? 1 : 0
                        radius: 4
                    }

                    Image {
                        anchors.centerIn: parent
                        source: "qrc:/icons/refresh-24px.svg"
                        sourceSize: Qt.size(48, 48)
                        width: 24
                        height: 24
                        smooth: true
                        opacity: refreshButton.enabled ? 1.0 : 0.4
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: refreshButton.activate()
                    }

                    focusPolicy: Qt.StrongFocus
                    KeyNavigation.left: favoritesToggle
                    KeyNavigation.right: sortToggle
                    KeyNavigation.down: gamesGrid.count > 0 ? gamesGrid : null
                    KeyNavigation.up: mainTabBar ? mainTabBar.itemAt(1) : null
                    Keys.onReturnPressed: { activate(); event.accepted = true; }
                }

                // Sort toggle (far right, just before the game count)
                Item {
                    id: sortToggle
                    Layout.preferredWidth: sortToggleRow.implicitWidth + 16
                    Layout.preferredHeight: 36
                    Layout.rightMargin: 8

                    Rectangle {
                        anchors.fill: parent
                        color: sortToggle.activeFocus ? Qt.rgba(46/255, 196/255, 182/255, 0.15) : "transparent"
                        border.color: sortToggle.activeFocus ? "#2ec4b6" : "transparent"
                        border.width: sortToggle.activeFocus ? 1 : 0
                        radius: 4
                    }

                    Row {
                        id: sortToggleRow
                        anchors.centerIn: parent
                        spacing: 5

                        // Up/down arrows glyph (matches iOS arrow.up.arrow.down)
                        Canvas {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 18; height: 18
                            onPaint: {
                                var ctx = getContext("2d");
                                ctx.reset();
                                ctx.strokeStyle = "#2ec4b6";
                                ctx.lineWidth = 1.8; ctx.lineCap = "round"; ctx.lineJoin = "round";
                                ctx.beginPath();
                                ctx.moveTo(5, 15); ctx.lineTo(5, 3);
                                ctx.moveTo(2, 6); ctx.lineTo(5, 3); ctx.lineTo(8, 6);
                                ctx.stroke();
                                ctx.beginPath();
                                ctx.moveTo(13, 3); ctx.lineTo(13, 15);
                                ctx.moveTo(10, 12); ctx.lineTo(13, 15); ctx.lineTo(16, 12);
                                ctx.stroke();
                            }
                        }

                        Text {
                            id: sortToggleText
                            anchors.verticalCenter: parent.verticalCenter
                            text: sortState === 1 ? qsTr("А → Я") : (sortState === 2 ? qsTr("Я → А") : qsTr("С обложкой"))
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            color: "#2ec4b6"
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            sortState = (sortState + 1) % 3;
                            Chiaki.settings.cloudSortState = sortState;
                            applySearchFilter();
                        }
                    }

                    focusPolicy: Qt.StrongFocus
                    KeyNavigation.left: refreshButton
                    KeyNavigation.right: filterToggle
                    KeyNavigation.down: gamesGrid.count > 0 ? gamesGrid : null
                    KeyNavigation.up: mainTabBar ? mainTabBar.itemAt(1) : null
                    Keys.onReturnPressed: {
                        sortState = (sortState + 1) % 3;
                        Chiaki.settings.cloudSortState = sortState;
                        applySearchFilter();
                        event.accepted = true;
                    }
                }
                
                // Game count label
                Label {
                    text: {
                        if (isSearching)
                            return qsTr("Поиск…");
                        if (searchQuery && searchQuery.trim() !== "") {
                            return filteredGameCount > 0
                                ? qsTr("%1 из %2").arg(filteredGameCount).arg(catalogTotalCount)
                                : qsTr("Ничего не найдено");
                        } else if (gridTruncated) {
                            return qsTr("%1 из %2").arg(currentPageGames.length).arg(filteredGameCount);
                        } else {
                            return filteredGameCount > 0
                                ? qsTr("%1 купленных").arg(filteredGameCount)
                                : qsTr("Нет купленных игр");
                        }
                    }
                    font.pixelSize: 12
                    opacity: 0.75
                    color: "white"
                    Layout.preferredWidth: 110
                    Layout.leftMargin: -6
                    horizontalAlignment: Text.AlignRight
                }
            }
        }
    }
    
    ColumnLayout {
        anchors.top: toolBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.topMargin: 15
        spacing: 0
        
        // Region fallback banner hidden for 4cloud rental clients (not actionable).
        Rectangle {
            id: fallbackBanner
            Layout.fillWidth: true
            Layout.preferredHeight: 0
            visible: false
            color: Qt.rgba(255/255, 193/255, 7/255, 0.2)
            border.color: "#FFC107"
            border.width: 2
            clip: true

            Label {
                anchors {
                    fill: parent
                    leftMargin: 20
                    rightMargin: 20
                    topMargin: 8
                    bottomMargin: 8
                }
                text: qsTr("PlayStation cloud isn't offered natively in your region — showing the %1 catalog. Some titles may not stream.").arg(fallbackRegion)
                wrapMode: Text.Wrap
                color: "#FFFFFF"
                font.pixelSize: 13
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        // Persistent authentication error banner
        Rectangle {
            id: authErrorBanner
            Layout.fillWidth: true
            Layout.preferredHeight: authErrorMessage.length > 0 ? 80 : 0
            visible: authErrorMessage.length > 0
            color: Qt.rgba(244/255, 67/255, 54/255, 0.15) // Red background with transparency
            border.color: "#F44336"
            border.width: 2
            clip: true
            
            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: 300; easing.type: Easing.OutCubic }
            }
            Behavior on opacity {
                NumberAnimation { duration: 200 }
            }
            
            RowLayout {
                anchors {
                    fill: parent
                    leftMargin: 25
                    rightMargin: 25
                    topMargin: 12
                    bottomMargin: 12
                }
                spacing: 16
                
                Item {
                    Layout.fillWidth: true
                }
                
                // Warning icon
                Text {
                    text: "⚠"
                    font.pixelSize: 32
                    color: "#F44336"
                    Layout.alignment: Qt.AlignVCenter
                }
                
                // Error message
                Label {
                    text: authErrorMessage
                    wrapMode: Text.Wrap
                    color: "#FFFFFF"
                    font.pixelSize: 14
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    Layout.alignment: Qt.AlignVCenter
                }
                
                Item {
                    Layout.fillWidth: true
                }
            }
        }
        
        // Loading indicator
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: isLoading
            
            BusyIndicator {
                anchors.centerIn: parent
                running: isLoading
            }
        }

        // Catalog area: owned grid by default; search overlay while scanning full catalog
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !isLoading

            // Search-in-progress overlay (full catalog scan can take a moment)
            Item {
                anchors.fill: parent
                visible: isSearching
                z: 2

                Rectangle {
                    anchors.fill: parent
                    color: Qt.rgba(7/255, 9/255, 13/255, 0.72)
                }

                Column {
                    anchors.centerIn: parent
                    spacing: 16

                    BusyIndicator {
                        anchors.horizontalCenter: parent.horizontalCenter
                        running: isSearching
                    }

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Идёт поиск по каталогу…")
                        color: "white"
                        font.pixelSize: 16
                        font.bold: true
                    }

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Это может занять несколько секунд")
                        color: Qt.rgba(1, 1, 1, 0.7)
                        font.pixelSize: 13
                    }
                }
            }
        
            // Games grid (single Flickable — mouse wheel scrolls the whole catalog)
            Flickable {
                id: catalogFlickable
                anchors.fill: parent
                visible: !isSearching
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                contentWidth: width
                contentHeight: catalogColumn.implicitHeight
                focus: false

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }

                WheelHandler {
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: (event) => {
                        if (event.angleDelta.y === 0)
                            return;
                        const step = event.angleDelta.y * 0.85;
                        catalogFlickable.flick(0, -step);
                        event.accepted = true;
                    }
                }

                Column {
                    id: catalogColumn
                    width: catalogFlickable.width
                    spacing: 12

                    // Recently played on this profile
                    Column {
                        width: parent.width
                        visible: recentGames.length > 0 && !isLoading
                        spacing: 8

                        Label {
                            anchors.left: parent.left
                            anchors.leftMargin: 20
                            text: qsTr("Недавние")
                            font.pixelSize: 16
                            font.bold: true
                            color: "white"
                        }

                        ListView {
                            id: recentList
                            width: parent.width
                            height: 270
                            orientation: ListView.Horizontal
                            spacing: 12
                            leftMargin: 20
                            rightMargin: 20
                            clip: true
                            model: recentGames
                            delegate: CloudGameCard {
                                required property int index
                                required property var modelData
                                width: 180
                                height: 260
                                gameData: modelData
                                qrCodeDialog: root.qrCodeDialogRef
                                onStreamGame: (streamingId, platform, serviceType) => {
                                    root.launchCloudGameFromCard(modelData, streamingId, platform, serviceType);
                                }
                                onToggleFavorite: (productId) => root.toggleFavorite(productId)
                            }
                        }
                    }

                    Label {
                        anchors.left: parent.left
                        anchors.leftMargin: 20
                        visible: recentGames.length > 0 && currentPageGames.length > 0 && !isLoading
                        text: qsTr("Каталог")
                        font.pixelSize: 16
                        font.bold: true
                        color: "white"
                    }

                    GridView {
                        id: gamesGrid

                        property int gridAvailWidth: catalogColumn.width - 40
                        property int _layoutVersion: 0
                        
                        width: {
                            let modelCount = count;
                            let version = _layoutVersion;
                            let availableWidth = gridAvailWidth;
                            let cols = Math.floor(availableWidth / cellWidth);
                            if (cols === 0) cols = 1;
                            return Math.min(cols * cellWidth, availableWidth);
                        }
                        height: {
                            let cols = Math.max(1, Math.floor(gridAvailWidth / cellWidth));
                            let rows = Math.ceil(Math.max(count, 1) / cols);
                            return rows * cellHeight + 20;
                        }
                        // Left-align with recent row (centering looked crooked under «Недавние»)
                        x: 20
                    
                    Connections {
                        target: catalogColumn
                        function onWidthChanged() {
                            Qt.callLater(() => { gamesGrid._layoutVersion++; });
                        }
                    }
                    cellWidth: 200
                    cellHeight: 280
                    focus: true
                    clip: false
                    interactive: false
                    
                    KeyNavigation.up: searchContainer
                    
                    model: currentPageGames
                    highlightFollowsCurrentItem: true
                    keyNavigationEnabled: true
                    keyNavigationWraps: false
                    
                    highlight: Rectangle {
                        color: "transparent"
                        border.color: Material.accent
                        border.width: 3
                        radius: 8
                        z: 10
                    }
                    
                    delegate: CloudGameCard {
                        required property int index
                        required property var modelData
                        width: gamesGrid.cellWidth - 20
                        height: gamesGrid.cellHeight - 20
                        gameData: modelData
                        focus: false  // GridView handles focus, not individual cards
                        activeFocusOnTab: false
                        qrCodeDialog: root.qrCodeDialogRef
                        
                        // Bind isFavorite to favoriteProductIds array changes
                        Binding on isFavorite {
                            value: {
                                if (!modelData) return false;
                                let productId = modelData.productId || modelData.product_id || modelData.id;
                                // Force re-evaluation by referencing the array
                                let favs = root.favoriteProductIds;
                                return favs.indexOf(productId) !== -1;
                            }
                        }
                        
                        onToggleFavorite: (productId) => {
                            root.toggleFavorite(productId);
                        }

                        onStreamGame: (streamingId, platform, serviceType) => {
                            root.launchCloudGameFromCard(modelData, streamingId, platform, serviceType);
                        }
                        
                        onCreateShortcut: (productId, entitlementId, platform, serviceType, gameName) => {
                            console.log("Create shortcut for cloud game:", gameName, "productId:", productId, "entitlementId:", entitlementId, platform, serviceType);
                            
                            // Determine the command and identifier to use
                            let command;
                            let gameIdentifier = entitlementId; // Use entitlement ID for launch command
                            
                            if (serviceType === "psnow") {
                                command = "cloudGameCatalog";
                                // For PSNOW, entitlementId is the same as productId
                                gameIdentifier = entitlementId;
                            } else if (serviceType === "pscloud") {
                                command = "cloudGameLibrary";
                                // For PSCloud, use entitlement ID for the launch command
                                gameIdentifier = entitlementId;
                            } else {
                                showErrorToast(qsTr("Error"), qsTr("Unknown service type: %1").arg(serviceType));
                                return;
                            }
                            
                            // Show the dialog - it will fetch game details itself using productId
                            // gameIdentifier (entitlementId) is used for the launch command
                            cloudShortcutDialog.showCloudDialog(gameName, gameIdentifier, serviceType, command, productId);
                        }
                    }
                    
                    Keys.onPressed: (event) => {
                        if (event.modifiers)
                            return;
                        
                        let cols = Math.floor(gamesGrid.gridAvailWidth / cellWidth);
                        if (cols === 0) cols = 1;
                        
                        if (event.key === Qt.Key_Left) {
                            if (currentIndex % cols !== 0) {
                                currentIndex = Math.max(0, currentIndex - 1);
                            }
                            event.accepted = true;
                            return;
                        }
                        
                        if (event.key === Qt.Key_Right) {
                            let totalItems = model.length;
                            let colInRow = currentIndex % cols;
                            let isLastItem = currentIndex === totalItems - 1;
                            let isRightmostInRow = colInRow === cols - 1;
                            
                            if (!isLastItem && !isRightmostInRow) {
                                currentIndex = Math.min(totalItems - 1, currentIndex + 1);
                            }
                            event.accepted = true;
                            return;
                        }
                        
                        if (event.key === Qt.Key_Up) {
                            // Move up one row
                            let currentRow = Math.floor(currentIndex / cols);
                            if (currentRow > 0) {
                                let colInRow = currentIndex % cols;
                                let prevRowStartIndex = (currentRow - 1) * cols;
                                let targetIndex = prevRowStartIndex + colInRow;
                                currentIndex = Math.max(0, targetIndex);
                                positionViewAtIndex(currentIndex, GridView.Contain);
                                event.accepted = true;
                                return;
                            }
                            filterToggle.forceActiveFocus();
                            event.accepted = true;
                            return;
                        }
                        
                        if (event.key === Qt.Key_Down) {
                            let totalItems = model.length;
                            let currentRow = Math.floor(currentIndex / cols);
                            let nextRowStartIndex = (currentRow + 1) * cols;
                            let nextRowEndIndex = Math.min(nextRowStartIndex + cols - 1, totalItems - 1);
                            
                            if (nextRowStartIndex < totalItems) {
                                let colInRow = currentIndex % cols;
                                let targetIndex = nextRowStartIndex + colInRow;
                                
                                if (targetIndex <= nextRowEndIndex) {
                                    currentIndex = targetIndex;
                                } else {
                                    currentIndex = nextRowEndIndex;
                                }
                                positionViewAtIndex(currentIndex, GridView.Contain);
                            }
                            event.accepted = true;
                            return;
                        }
                        
                        // Square/X button - Create shortcut (same gate as the card's button/X handler:
                        // Steam installed and game not in the non-owned "Add Game" state)
                        if (event.key === Qt.Key_X || event.key === Qt.Key_Backslash || event.key === Qt.Key_No) {
                            if (currentItem && currentItem.createShortcut && currentItem.showCloudSteamShortcut) {
                                // Use getProductIdForApi() to get the correct product ID for API calls
                                let productId = currentItem.getProductIdForApi ? currentItem.getProductIdForApi() : currentItem.getProductId();
                                // Use getStreamingIdentifier() to get the entitlement ID for launch command
                                let entitlementId = currentItem.getStreamingIdentifier ? currentItem.getStreamingIdentifier() : currentItem.getProductId();
                                let platform = currentItem.getPlatform();
                                let serviceType = currentItem.getServiceType();
                                let gameName = currentItem.getGameName();
                                if (productId !== "") {
                                    currentItem.createShortcut(productId, entitlementId, platform, serviceType, gameName);
                                    event.accepted = true;
                                }
                            }
                            return;
                        }
                        
                        switch (event.key) {
                        case Qt.Key_PageDown:
                            let visibleRows = Math.floor(catalogFlickable.height / cellHeight);
                            let jumpIndex = Math.min(currentIndex + (visibleRows * cols), model.length - 1);
                            currentIndex = jumpIndex;
                            positionViewAtIndex(currentIndex, GridView.Contain);
                            event.accepted = true;
                            break;
                        case Qt.Key_PageUp:
                            let visibleRowsUp = Math.floor(catalogFlickable.height / cellHeight);
                            let jumpIndexUp = Math.max(currentIndex - (visibleRowsUp * cols), 0);
                            currentIndex = jumpIndexUp;
                            positionViewAtIndex(currentIndex, GridView.Contain);
                            event.accepted = true;
                            break;
                        }
                    }
                    
                    Component.onCompleted: {
                        if (model && model.length > 0) {
                            currentIndex = 0;
                        }
                    }
                    
                    onModelChanged: {
                        // Force layout recalculation after model changes
                        Qt.callLater(() => {
                            _layoutVersion++;
                        });
                        if (model && model.length > 0) {
                            if (currentIndex < 0) {
                                currentIndex = 0;
                            }
                            // Ensure focus when model changes, but never steal it from the
                            // search field or an open modal (e.g. the filter dialog), otherwise
                            // a live re-filter yanks focus back to the grid mid-interaction.
                            Qt.callLater(() => {
                                if (count > 0 && !searchField.activeFocus && !tagFilterPopup.opened) {
                                    currentIndex = 0;
                                    forceActiveFocus();
                                }
                            });
                        }
                    }
                    
                    onCountChanged: {
                        // Force layout recalculation after count changes (including when going to 0)
                        Qt.callLater(() => {
                            _layoutVersion++;
                        });
                        if (count > 0) {
                            if (currentIndex < 0) {
                                currentIndex = 0;
                            }
                            // Only auto-focus if neither the search field nor the filter dialog
                            // is active; a live re-filter must not pull focus off an open modal.
                            Qt.callLater(() => {
                                if (count > 0 && !searchField.activeFocus && !tagFilterPopup.opened) {
                                    currentIndex = 0;
                                    forceActiveFocus();
                                }
                            });
                        }
                    }
                    
                    // Ensure focus is maintained
                    onActiveFocusChanged: {
                        if (activeFocus && count > 0 && currentIndex < 0) {
                            currentIndex = 0;
                        }
                    }
                }
            }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            visible: gridTruncated && !isLoading && !isSearching
            opacity: 0.75
            font.pixelSize: 12
            color: "#FFC107"
            wrapMode: Text.Wrap
            text: qsTr("Showing the first %1 games. Use search or filters to narrow the list.").arg(maxGridGames)
        }
        
    }
    
    // QR Code Dialog
    QRCodeDialog {
        id: qrCodeDialog
        
        Component.onCompleted: {
            root.qrCodeDialogRef = qrCodeDialog;
        }
    }
    
    // Cloud Shortcut Dialog (reusing GameShortcutDialog)
    GameShortcutDialog {
        id: cloudShortcutDialog
        anchors.centerIn: parent
        
        onShowToast: (message, color) => {
            shortcutToastTitle.text = qsTr("Notice")
            shortcutToastMessage.text = message
            shortcutToast.color = color
            shortcutToastTimer.restart()
        }
        
        onAllDialogsClosed: {
            // Restore focus to games grid after all dialogs close
            Qt.callLater(() => {
                if (gamesGrid.count > 0) {
                    gamesGrid.forceActiveFocus(Qt.TabFocusReason)
                }
            })
        }
        
        onClosed: {
            // Restore focus to games grid after dialog closes
            Qt.callLater(() => {
                if (gamesGrid.count > 0) {
                    gamesGrid.forceActiveFocus(Qt.TabFocusReason)
                }
            })
        }
    }
    
    // Toast notification for shortcut creation
    Rectangle {
        id: shortcutToast
        anchors {
            bottom: parent.bottom
            horizontalCenter: parent.horizontalCenter
            bottomMargin: 80
        }
        color: Material.accent
        width: Math.max(shortcutToastTitle.implicitWidth, shortcutToastMessage.implicitWidth) + 40
        height: shortcutToastColumn.implicitHeight + 20
        radius: 8
        opacity: shortcutToastTimer.running ? 0.8 : 0.0
        z: 1000
        
        Behavior on opacity { NumberAnimation { duration: 300 } }
        Behavior on color { ColorAnimation { duration: 300 } }
        
        ColumnLayout {
            id: shortcutToastColumn
            anchors.centerIn: parent
            spacing: 5
            
            Label {
                id: shortcutToastTitle
                Layout.alignment: Qt.AlignCenter
                font.bold: true
                font.pixelSize: 16
                color: "white"
            }
            
            Label {
                id: shortcutToastMessage
                Layout.alignment: Qt.AlignCenter
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 14
                color: "white"
            }
        }
        
        Timer {
            id: shortcutToastTimer
            interval: 3000
        }
    }
    
    // Error toast notification
    Rectangle {
        id: errorToast
        anchors {
            bottom: parent.bottom
            horizontalCenter: parent.horizontalCenter
            bottomMargin: 80
        }
        color: "#F44336"
        width: Math.max(errorToastTitle.implicitWidth, errorToastMessage.implicitWidth) + 40
        height: errorToastColumn.implicitHeight + 20
        radius: 8
        opacity: errorToastTimer.running ? 0.9 : 0.0
        z: 1001
        
        Behavior on opacity { NumberAnimation { duration: 300 } }
        Behavior on color { ColorAnimation { duration: 300 } }
        
        ColumnLayout {
            id: errorToastColumn
            anchors.centerIn: parent
            spacing: 5
            
            Label {
                id: errorToastTitle
                Layout.alignment: Qt.AlignCenter
                font.bold: true
                font.pixelSize: 16
                color: "white"
            }
            
            Label {
                id: errorToastMessage
                Layout.alignment: Qt.AlignCenter
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 14
                color: "white"
                wrapMode: Text.Wrap
                width: Math.min(implicitWidth, parent.parent.width - 40)
            }
        }
        
        Timer {
            id: errorToastTimer
            interval: 5000
        }
    }

    Dialog {
        id: accountChoiceDialog
        property string promptText: ""
        property var accounts: []
        property var onPicked: null
        // Same proven pattern as tagFilterPopup / ConfirmDialog
        parent: Overlay.overlay
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        width: 440
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        title: qsTr("Выбор аккаунта")
        Material.roundedScale: Material.MediumScale

        Component.onCompleted: {
            header.horizontalAlignment = Text.AlignHCenter;
            header.background = null;
        }

        background: Rectangle {
            color: Qt.rgba(7/255, 9/255, 13/255, 0.98)
            radius: 12
            border.color: "#2ec4b6"
            border.width: 2
        }

        onClosed: {
            onPicked = null;
            if (gamesGrid)
                gamesGrid.forceActiveFocus(Qt.TabFocusReason);
        }

        ColumnLayout {
            spacing: 12

            Label {
                Layout.preferredWidth: 400
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: "#e8eef2"
                text: accountChoiceDialog.promptText
            }

            Repeater {
                model: accountChoiceDialog.accounts
                delegate: Button {
                    Layout.preferredWidth: 400
                    Layout.fillWidth: true
                    flat: true
                    Material.background: modelData.played_before ? "#1a3d38" : Material.accent
                    Material.roundedScale: Material.SmallScale
                    contentItem: Column {
                        spacing: 4
                        width: parent.width
                        Label {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            color: "white"
                            text: {
                                let label = modelData.label || qsTr("Аккаунт #%1").arg(modelData.account_id);
                                return modelData.has_ps_plus ? (label + " · PS Plus") : label;
                            }
                        }
                        Label {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            visible: !!modelData.played_before
                            font.pixelSize: 12
                            color: "#9fd9d2"
                            text: qsTr("играли ранее")
                        }
                    }
                    onClicked: {
                        let cb = accountChoiceDialog.onPicked;
                        let aid = modelData.account_id || 0;
                        accountChoiceDialog.close();
                        if (cb)
                            cb(aid);
                    }
                }
            }

            Button {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Отмена")
                flat: true
                onClicked: accountChoiceDialog.reject()
            }
        }
    }
}
