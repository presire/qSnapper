/**
 * @file SettingsPage.qml
 * @brief snapper設定の編集とsystemdタイマの管理を行う設定画面
 *
 * Main.qmlのStackLayoutで、スナップショット一覧 (SnapshotListPage) とタブで切り替えて表示する
 * 選択中のsnapper設定 (SnapperService.currentConfig) の設定値を読み込み、変更したキーだけを保存する
 * 表示テーマの選択は画面内で保留し、[Save]ボタンの押下でThemeManagerへ反映する
 *
 * 入力 : なし (設定名はSnapperService.currentConfigを参照する)
 * 出力 : なし
 * 依存 : SnapperService (設定一覧と現在の設定名)、SnapperSettingsModel (設定値とタイマの読み書き、
 *        クォータ設定のsetupQuotaとquotaSetupSucceeded / quotaSetupFailed)、
 *        ConfigManagerModel (設定の作成と削除、configCreated / configCreationFailed /
 *        configDeleted / configDeletionFailed)、ThemeManager (成功 / エラー / 警告色と表示テーマの保存)、ThemedGroupBox (設定項目のグループ枠)、
 *        SettingsFieldRow、TimerSettingsSection、CreateConfigDialog、DeleteConfigDialog、BorderedDialog
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

Page {
    id: root

    property string resultText: ""          // 設定の読み込みと保存の直近の結果文言
    property bool resultIsError: false      // 直近の結果が失敗か

    // 表示テーマの保留中の選択 (ThemeManager.Light / Dark / Systemの値、-1は保留なし)
    // [Save]を押すまでThemeManagerへ反映せず、他の設定値と同じ保存操作で確定させる
    property int pendingThemeMode: -1

    // 保留中の選択が、現在のテーマと異なるか (現在値と同じ選択は変更とみなさない)
    readonly property bool themeModified: pendingThemeMode !== -1 && pendingThemeMode !== appearanceBox.theme.themeMode

    // ラジオボタンに表示するテーマ (保留中ならその値、なければ現在のThemeManagerの値)
    readonly property int shownThemeMode: pendingThemeMode !== -1 ? pendingThemeMode : appearanceBox.theme.themeMode

    /**
     * @brief 表示テーマの選択を保留する
     * @param mode 選択されたテーマ (ThemeManager.Light / Dark / System)
     * @note 現在のテーマと同じ値が選ばれたときは保留を解除し、変更なしの状態へ戻す
     *       ThemeManagerへはまだ反映しないため、pendingThemeModeを更新するだけで副作用は無い
     */
    function selectTheme(mode) {
        pendingThemeMode = (mode === appearanceBox.theme.themeMode) ? -1 : mode
    }

    /**
     * @brief 保留中の表示テーマをThemeManagerへ反映する
     * @note ThemeManager::setThemeModeを呼び出し、QSettingsへの保存と画面全体への反映を行う
     *       反映後は保留を解除する (保留中でなければ何もしない)
     */
    function applyPendingTheme() {
        if (!themeModified) {
            pendingThemeMode = -1
            return
        }
        appearanceBox.theme.themeMode = pendingThemeMode
        pendingThemeMode = -1
    }

    /**
     * @brief 表示中であれば設定値とタイマ状態を読み込む
     * @note 非表示の間は特権サービスへ問い合わせない (起動時に認証や常駐サービスの起動を発生させないため)
     *       未保存の編集がある場合は、タブ切り替えで編集が失われないよう設定値を読み込み直さない
     *       保留中のテーマ選択 (pendingThemeMode) は、設定値の再読み込みとは独立しているため破棄しない
     *       SnapperSettingsModel::reloadとrefreshTimersを呼び出し、完了は各シグナルで受け取る
     */
    function loadIfVisible() {
        if (!visible) {
            return
        }
        if (settingsModel.configName !== "" && !settingsModel.modified) {
            settingsModel.reload()
        }
        settingsModel.refreshTimers()
    }

    // 選択中のsnapper設定の設定値とタイマ状態を扱うViewModel
    // 設定名は一覧画面と共有するSnapperService.currentConfigに追従する
    SnapperSettingsModel {
        id: settingsModel
        configName: SnapperService.currentConfig
    }

    // 設定の作成と削除を扱うViewModel
    // 結果にかかわらず設定一覧を読み直すため、コンボボックスは実際の状態に追従する
    ConfigManagerModel {
        id: configManager
    }

    // クォータ設定の表示と操作で使うSnapperSettingsModelへの参照
    // qmllintはC++側で登録された型のメンバを解決できず、参照ごとに警告するため、var経由で参照して警告の増加を避ける
    readonly property var quotaModel: settingsModel

    // 設定の作成と削除で使うConfigManagerModelへの参照
    // qmllintはC++側で登録された型のメンバを解決できず、参照ごとに警告するため、var経由で参照して警告の増加を避ける
    readonly property var configModel: configManager

    // 設定モデルの参照
    // 新しく追加した箇所だけvar経由にして、qmllintの未解決メンバ警告の増加を抑える
    readonly property var settings: settingsModel

    // 画面が表示されたとき (タブ切り替え時) に読み込む
    onVisibleChanged: loadIfVisible()

    // 設定名が変わったときは編集内容が破棄されるため、表示中であれば新しい設定を読み込む
    Connections {
        target: settingsModel

        function onConfigNameChanged() {
            root.resultText = ""
            if (root.visible && settingsModel.configName !== "") {
                settingsModel.reload()
            }
        }

        function onSaveSucceeded() {
            root.resultIsError = false
            root.resultText = qsTr("Settings saved.")
        }

        function onSaveFailed(error) {
            root.resultIsError = true
            root.resultText = qsTr("Failed to save settings: %1").arg(error)
        }

        function onLoadFailed(error) {
            root.resultIsError = true
            root.resultText = qsTr("Failed to load settings: %1").arg(error)
        }

        // クォータの設定結果を、保存結果と同じフッターのラベルで通知する
        function onQuotaSetupSucceeded() {
            root.resultIsError = false
            root.resultText = qsTr("Quota has been set up.")
        }

        function onQuotaSetupFailed(error) {
            root.resultIsError = true
            root.resultText = qsTr("Failed to set up quota: %1").arg(error)
        }
    }

    // 設定の作成と削除の結果を、保存結果と同じフッターのラベルで通知する
    Connections {
        target: root.configModel

        function onConfigCreated(name) {
            root.resultIsError = false
            root.resultText = qsTr("Configuration %1 has been created.").arg(name)
        }

        function onConfigCreationFailed(error) {
            root.resultIsError = true
            root.resultText = qsTr("Failed to create a configuration: %1").arg(error)
        }

        function onConfigDeleted(name) {
            root.resultIsError = false
            root.resultText = qsTr("Configuration %1 has been deleted.").arg(name)
        }

        function onConfigDeletionFailed(error) {
            root.resultIsError = true
            root.resultText = qsTr("Failed to delete a configuration: %1").arg(error)
        }
    }

    // クォータ設定の確認ダイアログ
    // Btrfsのクォータは一度有効にするとqSnapperから無効にできず、一部の操作が遅くなるため、実行前に確認する
    BorderedDialog {
        id: quotaConfirmDialog
        title: qsTr("Set up quota")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, root.width - 40)

        Label {
            width: parent.width
            text: qsTr("Enabling Btrfs quota can slow down some filesystem operations, and it cannot be undone from qSnapper. Do you want to continue?")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }

        // SnapperSettingsModel::setupQuotaを呼び出し、結果はquotaSetupSucceeded / quotaSetupFailedで受け取る
        onAccepted: root.quotaModel.setupQuota()
    }

    // 設定の新規作成ダイアログ
    // fstypeとテンプレートはサーバ側の固定値なので、名前とサブボリュームだけを入力する
    CreateConfigDialog {
        id: createConfigDialog
        manager: root.configModel
    }

    // 設定の削除確認ダイアログ
    // 全スナップショットが消えるため、設定名の入力による確認を必須にする
    DeleteConfigDialog {
        id: deleteConfigDialog
        manager: root.configModel
        configName: root.settings.configName
        subvolume: root.settings.subvolume
    }

    // 設定の選択と読み込み状態を示すツールバー
    header: ToolBar {
        topPadding: 8   // FusionのToolBarは上側の余白が0で、上部のTabBarと重なって見えるため下へずらす

        RowLayout {
            anchors.fill: parent
            spacing: 8

            Label {
                text: qsTr("Config:")
                font.pixelSize: 14
                Layout.leftMargin: 10
            }

            ComboBox {
                id: configCombo
                model: SnapperService.configs
                Layout.preferredWidth: 140
                // 保存と読み込みの実行中に設定を切り替えると応答が捨てられ、結果が分からなくなるため無効にする
                enabled: !settingsModel.configBusy

                /**
                 * @brief currentConfigに対応するcurrentIndexを更新する
                 * @note 宣言的バインドにするとmodel -> currentIndex -> modelの循環でbinding loopとなるため、
                 *       Connections経由で命令的に設定する
                 */
                function syncCurrentIndex() {
                    var idx = SnapperService.configs.indexOf(SnapperService.currentConfig)
                    currentIndex = idx >= 0 ? idx : 0
                }

                Connections {
                    target: SnapperService
                    function onConfigsChanged() { configCombo.syncCurrentIndex() }
                    function onCurrentConfigChanged() { configCombo.syncCurrentIndex() }
                }

                // 設定切替時: SnapperService.currentConfig (C++) を更新する
                // 一覧画面も同じ値に追従する
                onActivated: {
                    var name = model[currentIndex]
                    if (name && name !== SnapperService.currentConfig) {
                        SnapperService.currentConfig = name
                    }
                }

                // 設定一覧は一覧画面が取得するため、ここでは選択位置の同期だけを行う
                Component.onCompleted: syncCurrentIndex()
            }

            Button {
                text: qsTr("New...")
                // 設定の切り替え中や作成中は、一覧と選択が食い違わないよう無効にする
                enabled: !root.settings.configBusy && !root.configModel.busy
                // 無効状態の見た目は、フッターのRevertボタンの注記と同じ理由で不透明度で示す
                opacity: enabled ? 1.0 : 0.45
                // CreateConfigDialogを開き、実際の作成はConfigManagerModel::createConfigが行う
                onClicked: createConfigDialog.open()
            }

            Button {
                text: qsTr("Delete...")
                // 削除対象が無い場合と実行中は無効にする
                enabled: root.settings.configName !== "" && !root.settings.configBusy && !root.configModel.busy
                // 無効状態の見た目は、フッターのRevertボタンの注記と同じ理由で不透明度で示す
                opacity: enabled ? 1.0 : 0.45
                // DeleteConfigDialogを開き、設定名の確認入力後にConfigManagerModel::deleteConfigを呼ぶ
                onClicked: deleteConfigDialog.open()
            }

            BusyIndicator {
                running: settingsModel.configBusy
                visible: running
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
            }

            // 設定の作成と削除は認証を待つ間も続くため、設定の読み込みとは別に実行中を示す
            BusyIndicator {
                running: root.configModel.busy
                visible: running
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
            }

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Reload")
                icon.name: "view-refresh"
                icon.color: ThemeManager.isDark ? "#FFFFFF" : palette.buttonText
                // 実行中は多重の問い合わせを避けるため無効にする
                enabled: !settingsModel.configBusy && settingsModel.configName !== ""
                // 無効状態の見た目は、フッターのRevertボタンの注記と同じ理由で不透明度で示す
                opacity: enabled ? 1.0 : 0.45
                Layout.rightMargin: 10
                // SnapperSettingsModel::reloadを呼び出し、特権サービスのGetConfigで最新値を取得する
                onClicked: settingsModel.reload()
            }
        }
    }

    // 保存 / 元に戻す操作と結果を示すフッター
    footer: Rectangle {
        implicitHeight: 48
        color: palette.window

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 8

            Label {
                text: root.resultText
                textFormat: Text.PlainText
                color: root.resultIsError ? ThemeManager.errorColor : ThemeManager.successColor
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Button {
                text: qsTr("Revert")
                // 設定値の編集か、保留中のテーマ選択がある場合だけ有効にする (破棄対象が無いため)
                // 保存の実行中は、送信済みの内容と画面の編集が食い違わないよう、テーマの保留があっても無効にする
                enabled: (root.settings.modified || root.themeModified) && !root.settings.configBusy
                // Fusionスタイルでは、パレットの各ロールをバインディングで指定すると無効状態 (disabled) の色が反映されず、
                // 有効時と同じ見た目になる (Main.qmlのパレットが該当する) ため、不透明度で無効状態を示す
                opacity: enabled ? 1.0 : 0.45
                // 保留中のテーマ選択を破棄し、設定値の編集がある場合は読み込み済みの値へ戻す
                onClicked: {
                    root.pendingThemeMode = -1
                    if (root.settings.modified) {
                        root.settings.revert()
                    }
                }
            }

            Button {
                text: qsTr("Save")
                highlighted: true
                // 設定値の編集があり、すべての値が妥当で、実行中でない場合に有効にする
                // 不正な値は特権サービスにも拒否されるため、送信前に止める
                // テーマの選択だけが保留中の場合も有効にする (snapper設定の有無や妥当性に依存しないため)
                // ただし不正な設定値が残る場合は、テーマだけを保存して成功と表示しないよう無効にする
                enabled: !root.settings.configBusy && (root.settings.modified || root.themeModified)
                         && (!root.settings.modified || root.settings.modifiedValid)
                // 無効状態の見た目は、Revertボタンの注記と同じ理由で不透明度で示す
                opacity: enabled ? 1.0 : 0.45
                // 保留中のテーマはThemeManagerへ反映し、QSettingsへ保存する
                // 設定値の編集はSnapperSettingsModel::saveを呼び出し、変更したキーだけを特権サービスのWriteSnapperConfigで保存する
                // 完了はsaveSucceeded / saveFailedシグナルで受け取る
                // テーマだけを保存した場合は、これらのシグナルが発行されないためここで結果を通知する
                onClicked: {
                    var settingsDirty = root.settings.modified && root.settings.modifiedValid
                    if (root.themeModified) {
                        root.applyPendingTheme()
                        if (!settingsDirty) {
                            root.resultIsError = false
                            root.resultText = qsTr("Settings saved.")
                        }
                    }
                    if (settingsDirty) {
                        root.settings.save()
                    }
                }
            }
        }
    }

    ScrollView {
        id: scrollView
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: scrollView.availableWidth
            spacing: 12

            // 設定の読み込みに失敗している間は、古い値を編集させないことを伝える
            Label {
                visible: !settingsModel.configLoaded && !settingsModel.configBusy
                text: qsTr("The settings could not be loaded. Editing is disabled.")
                textFormat: Text.PlainText
                color: ThemeManager.warningColor
                Layout.margins: 10
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }

            // 読み取り専用の設定情報 (変更するとsnapperの動作対象が変わるため、本画面では編集させない)
            ThemedGroupBox {
                title: qsTr("Config information (read only)")
                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10

                GridLayout {
                    columns: 2
                    columnSpacing: 20
                    rowSpacing: 4

                    Label { text: "SUBVOLUME" }
                    Label { text: settingsModel.subvolume; textFormat: Text.PlainText }
                    Label { text: "FSTYPE" }
                    Label { text: settingsModel.fstype; textFormat: Text.PlainText }
                    Label { text: "QGROUP"; visible: settingsModel.qgroup !== "" }
                    Label { text: settingsModel.qgroup; textFormat: Text.PlainText; visible: settingsModel.qgroup !== "" }
                }
            }

            // 番号スナップショットの自動削除
            ThemedGroupBox {
                title: qsTr("Number cleanup")
                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10

                ColumnLayout {
                    anchors.fill: parent
                    SettingsFieldRow { settings: settingsModel; settingKey: "NUMBER_CLEANUP"; label: qsTr("Enable number cleanup") }
                    SettingsFieldRow { settings: settingsModel; settingKey: "NUMBER_MIN_AGE"; label: qsTr("Minimum age") }
                    SettingsFieldRow { settings: settingsModel; settingKey: "NUMBER_LIMIT"; label: qsTr("Number limit") }
                    SettingsFieldRow { settings: settingsModel; settingKey: "NUMBER_LIMIT_IMPORTANT"; label: qsTr("Number limit (important)") }
                }
            }

            // タイムラインスナップショットの作成と自動削除
            ThemedGroupBox {
                title: qsTr("Timeline")
                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10

                ColumnLayout {
                    anchors.fill: parent
                    SettingsFieldRow { settings: settingsModel; settingKey: "TIMELINE_CREATE"; label: qsTr("Create timeline snapshots") }

                    // TIMELINE_CREATEはsnapper-timeline.timerが有効なときだけ効果があるため、タイマの設定が別であることを案内する
                    Label {
                        text: qsTr("This setting only takes effect while snapper-timeline.timer is enabled. Manage the timer in the timer section below.")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: ThemeManager.warningColor
                        Layout.fillWidth: true
                    }

                    SettingsFieldRow { settings: settingsModel; settingKey: "TIMELINE_CLEANUP"; label: qsTr("Enable timeline cleanup") }
                    SettingsFieldRow { settings: settingsModel; settingKey: "TIMELINE_MIN_AGE"; label: qsTr("Minimum age") }
                    SettingsFieldRow { settings: settingsModel; settingKey: "TIMELINE_LIMIT_HOURLY"; label: qsTr("Hourly limit") }
                    SettingsFieldRow { settings: settingsModel; settingKey: "TIMELINE_LIMIT_DAILY"; label: qsTr("Daily limit") }
                    SettingsFieldRow { settings: settingsModel; settingKey: "TIMELINE_LIMIT_WEEKLY"; label: qsTr("Weekly limit") }
                    SettingsFieldRow { settings: settingsModel; settingKey: "TIMELINE_LIMIT_MONTHLY"; label: qsTr("Monthly limit") }
                    SettingsFieldRow { settings: settingsModel; settingKey: "TIMELINE_LIMIT_QUARTERLY"; label: qsTr("Quarterly limit") }
                    SettingsFieldRow { settings: settingsModel; settingKey: "TIMELINE_LIMIT_YEARLY"; label: qsTr("Yearly limit") }
                }
            }

            // 差分のない Pre/Post ペアの自動削除
            ThemedGroupBox {
                title: qsTr("Empty pre/post pairs")
                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10

                ColumnLayout {
                    anchors.fill: parent
                    SettingsFieldRow { settings: settingsModel; settingKey: "EMPTY_PRE_POST_CLEANUP"; label: qsTr("Enable empty pre/post cleanup") }
                    SettingsFieldRow { settings: settingsModel; settingKey: "EMPTY_PRE_POST_MIN_AGE"; label: qsTr("Minimum age") }
                }
            }

            // クォータに基づくスナップショットの容量制限
            ThemedGroupBox {
                title: qsTr("Space limits")
                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10

                ColumnLayout {
                    anchors.fill: parent

                    // クォータ (qgroup) の設定状態
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            text: root.quotaModel.qgroup !== ""
                                  ? qsTr("Quota: enabled (qgroup %1)").arg(root.quotaModel.qgroup)
                                  : qsTr("Quota: not set up")
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                        }

                        Button {
                            text: qsTr("Set up quota")
                            // Btrfsで、読み込み済みかつ実行中でなく、クォータが未設定の場合だけ有効にする
                            // (QGROUPが設定済みなら再設定は不要で、Btrfs以外にはクォータが無いため)
                            enabled: root.quotaModel.configLoaded && !root.quotaModel.configBusy
                                     && root.quotaModel.fstype === "btrfs" && root.quotaModel.qgroup === ""
                            // 無効状態の見た目は、フッターのRevertボタンの注記と同じ理由で不透明度で示す
                            opacity: enabled ? 1.0 : 0.45
                            onClicked: quotaConfirmDialog.open()
                        }
                    }

                    // SPACE_LIMITとFREE_LIMITはクォータが無いと効果が無いことを案内する
                    Label {
                        text: qsTr("SPACE_LIMIT and FREE_LIMIT only take effect when the quota is set up.")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: appearanceBox.theme.warningColor
                        Layout.fillWidth: true
                    }

                    SettingsFieldRow { settings: settingsModel; settingKey: "SPACE_LIMIT"; label: qsTr("Space limit") }
                    SettingsFieldRow { settings: settingsModel; settingKey: "FREE_LIMIT"; label: qsTr("Free space limit") }
                }
            }

            // 変更前後の差分を事前計算するバックグラウンド比較
            ThemedGroupBox {
                title: qsTr("Background comparison")
                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10

                ColumnLayout {
                    anchors.fill: parent
                    SettingsFieldRow { settings: settingsModel; settingKey: "BACKGROUND_COMPARISON"; label: qsTr("Enable background comparison") }
                }
            }

            // systemdタイマ (全設定で共有され、設定値の保存とは独立して即時に反映する)
            TimerSettingsSection {
                settings: settingsModel
                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10
            }

            // 表示テーマの選択 (snapperの設定値とは保存先が異なり、ThemeManagerがQSettingsへ保存する)
            // 選択は保留として保持し、フッターの[Save]ボタンで反映と保存を行う
            ThemedGroupBox {
                id: appearanceBox
                title: qsTr("Appearance")
                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10

                // ThemeManagerシングルトンへの参照を1か所に集約する (qmllintはqmlRegisterSingletonInstanceの型を解決できず、参照ごとに警告するため)
                // qmllint disable unqualified
                readonly property var theme: ThemeManager
                // qmllint enable unqualified

                ColumnLayout {
                    anchors.fill: parent

                    // トグル時は選択を保留するだけで、ThemeManagerへの反映は[Save]ボタンで行う (設定値の保存操作と揃えるため)
                    // テーマの選択はこの画面だけで行うため、保留がなければ選択状態をThemeManager側の値から導出し、QSettingsに保存された値やSystem連動の状態を反映させる
                    RadioButton {
                        text: qsTr("Light")
                        checked: root.shownThemeMode === appearanceBox.theme.Light
                        onToggled: if (checked) root.selectTheme(appearanceBox.theme.Light)
                    }

                    RadioButton {
                        text: qsTr("Dark")
                        checked: root.shownThemeMode === appearanceBox.theme.Dark
                        onToggled: if (checked) root.selectTheme(appearanceBox.theme.Dark)
                    }

                    RadioButton {
                        text: qsTr("Follow system")
                        checked: root.shownThemeMode === appearanceBox.theme.System
                        onToggled: if (checked) root.selectTheme(appearanceBox.theme.System)
                    }

                    // 保存先がユーザごとのQSettingsであることと、[Save]ボタンの押下で反映されることを案内する
                    Label {
                        text: qsTr("The theme is saved for this user only and applies when you press Save.")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }

    // 初回表示時 (起動時に設定画面が前面の場合) の読み込み
    // 子要素とバインディングの初期化が済んでからでないとconfigNameが確定しないため、完了後に実行する
    Component.onCompleted: loadIfVisible()
}
