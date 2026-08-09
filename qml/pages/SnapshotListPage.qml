// スナップショット一覧ページ
// スナップショットの表示、作成、削除、復元機能を提供
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

Page {
    id: root
    title: qsTr("Snapshot List")

    // 選択状態管理プロパティ
    property var selectedSnapshots: ([])                       // 選択されたスナップショット番号の配列
    property int selectedCount: selectedSnapshots.length       // 選択された項目数
    property alias snapshotDetailDialog: detailDialog          // 詳細ダイアログへのエイリアス

    // ヘルパー関数: グループの選択状態を切り替え (Pre/Postペアは両方選択)
    function toggleGroupSelection(preNumber, postNumber) {
        var preIdx = selectedSnapshots.indexOf(preNumber)
        if (preIdx >= 0) {
            // 選択解除
            selectedSnapshots.splice(preIdx, 1)
            if (postNumber > 0) {
                var postIdx = selectedSnapshots.indexOf(postNumber)
                if (postIdx >= 0) selectedSnapshots.splice(postIdx, 1)
            }
        } else {
            // 選択
            selectedSnapshots.push(preNumber)
            if (postNumber > 0) selectedSnapshots.push(postNumber)
        }
        selectedSnapshots = selectedSnapshots  // バインディング更新をトリガー
    }

    // 選択状態をクリア
    function clearSelection() {
        selectedSnapshots = []
    }

    // 指定されたpreNumberのグループが選択されているかチェック
    function isGroupSelected(preNumber) {
        return selectedSnapshots.indexOf(preNumber) >= 0
    }

    // スナップショット一覧のデータモデル
    // D-Busサービス経由でSnapperからデータを取得
    SnapshotListModel {
        id: snapshotListModel

        // スナップショット作成成功時のシグナルハンドラ
        onSnapshotCreated: {
            statusBar.showMessage(qsTr("Snapshot created successfully"), 3000)
        }

        // スナップショット作成失敗時のシグナルハンドラ
        onSnapshotCreationFailed: function(error) {
            errorDialog.text = error
            errorDialog.open()
        }

        // スナップショット削除成功時のシグナルハンドラ
        onSnapshotDeleted: function(number) {
            statusBar.showMessage(qsTr("Snapshot #%1 deleted").arg(number), 3000)
        }

        // スナップショット削除失敗時のシグナルハンドラ
        onSnapshotDeletionFailed: function(number, error) {
            errorDialog.text = qsTr("Failed to delete snapshot #%1: %2").arg(number).arg(error)
            errorDialog.open()
        }

        // 複数スナップショット削除完了時のシグナルハンドラ
        onSnapshotsDeletionCompleted: function(successCount, failureCount) {
            deletingBusyDialog.close()
            root.clearSelection()
            if (failureCount === 0) {
                statusBar.showMessage(qsTr("Deleted %1 snapshot(s)").arg(successCount), 3000)
            } else {
                statusBar.showMessage(qsTr("Deletion completed: %1 succeeded, %2 failed").arg(successCount).arg(failureCount), 5000)
            }
        }

        Component.onCompleted: refresh()
    }

    // グループ化モデル (Pre/Postペアを1行にまとめる)
    SnapshotGroupModel {
        id: snapshotGroupModel
        snapshotListModel: snapshotListModel
    }

    // テーブルカラム幅の一元管理
    QtObject {
        id: columnWidths
        property int idWidth: 80
        property int typeWidth: 140
        property int dateWidth: 170
        property int userdataWidth: 150
    }

    // ツールバーヘッダ
    // アプリケーションタイトル、操作ボタン、テーマ切り替えを配置
    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            spacing: 5

            // アプリケーション名表示
            Label {
                text: qsTr("qSnapper")
                font.bold: true
                font.pixelSize: 18
            }

            // Snapper config 切替 ComboBox
            Label {
                text: qsTr("Config:")
                font.pixelSize: 14
                Layout.leftMargin: 10
            }
            ComboBox {
                id: configCombo
                model: SnapperService.configs
                Layout.preferredWidth: 140

                // model / currentConfig の変化に応じて currentIndex を更新する。
                // 宣言的バインドにすると QML が model→currentIndex→model の
                // 循環とみなして binding loop を検出するため、Connections 経由で
                // 命令的に設定する。
                function syncCurrentIndex() {
                    var idx = SnapperService.configs.indexOf(SnapperService.currentConfig)
                    currentIndex = idx >= 0 ? idx : 0
                }

                Connections {
                    target: SnapperService
                    function onConfigsChanged() { configCombo.syncCurrentIndex() }
                    function onCurrentConfigChanged() { configCombo.syncCurrentIndex() }
                }

                onActivated: {
                    var name = model[currentIndex]
                    if (name && name !== SnapperService.currentConfig) {
                        SnapperService.currentConfig = name
                        root.clearSelection()
                        snapshotListModel.refresh()
                    }
                }
                Component.onCompleted: {
                    SnapperService.refreshConfigs()
                    syncCurrentIndex()
                }
            }

            Item { Layout.fillWidth: true }

            // 選択数表示ラベル
            Label {
                visible: root.selectedCount > 0
                text: qsTr("%1 selected").arg(root.selectedCount)
                font.pixelSize: 14
                color: palette.highlight
            }

            // 更新ボタン
            Button {
                text: qsTr("Refresh")
                icon.name: "view-refresh"
                icon.color: palette.buttonText
                onClicked: {
                    root.clearSelection()
                    snapshotListModel.refresh()
                }
            }

            // 選択削除ボタン
            Button {
                text: qsTr("Delete Selected")
                icon.name: "edit-delete"
                icon.color: enabled ? "#ef5350" : palette.buttonText
                enabled: root.selectedCount > 0
                opacity: enabled ? 1.0 : 0.45
                onClicked: deleteConfirmDialog.open()
            }

            // スナップショット作成ボタン
            Button {
                text: qsTr("Create Snapshot")
                icon.name: "list-add"
                icon.color: enabled ? "#66bb6a" : palette.buttonText
                onClicked: createSnapshotDialog.open()
            }

            // 編集ボタン (Modify)
            Button {
                text: qsTr("Modify")
                icon.name: "document-edit"
                icon.color: palette.buttonText
                enabled: root.selectedCount === 1
                opacity: enabled ? 1.0 : 0.45
                onClicked: {
                    var num = root.selectedSnapshots[0]
                    var snap = SnapperService.find(num)
                    if (snap) {
                        editDialog.snapshotNumber = snap.number
                        editDialog.descriptionInitial = snap.description
                        editDialog.cleanupInitial = snap.cleanupAlgoString()
                        editDialog.userdataInitial = snap.userdata
                        editDialog.open()
                    }
                }
            }

            // 2スナップショット比較ボタン
            Button {
                text: qsTr("Compare")
                icon.name: "view-visible"
                icon.color: palette.buttonText
                enabled: root.selectedCount === 2
                opacity: enabled ? 1.0 : 0.45
                onClicked: {
                    compareDialog.num1 = root.selectedSnapshots[0]
                    compareDialog.num2 = root.selectedSnapshots[1]
                    compareDialog.configName = SnapperService.currentConfig
                    compareDialog.open()
                }
            }

            // セパレータ
            Rectangle {
                width: 1
                height: 30
                color: palette.mid
                Layout.leftMargin: 5
                Layout.rightMargin: 5
            }

            // ダークモード切り替えラベル
            Label {
                text: qsTr("Dark Mode")
                font.pixelSize: 14
            }

            // テーマ切り替えスイッチ
            Switch {
                id: themeSwitch
                checked: ThemeManager.isDark
                onToggled: {
                    ThemeManager.themeMode = checked ? ThemeManager.Dark : ThemeManager.Light
                }
            }
        }
    }

    // メインコンテンツレイアウト
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        // スナップショット総数表示
        Label {
            text: qsTr("Total Snapshots: %1").arg(snapshotListModel.count)
            font.bold: true
        }

        // テーブルヘッダ
        SnapshotTableHeader {
            Layout.fillWidth: true
            columnWidths: columnWidths
        }

        // スナップショット一覧リストビュー (テーブル形式)
        ListView {
            id: listView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            model: snapshotGroupModel

            // 各スナップショットグループのテーブル行デリゲート
            delegate: SnapshotTableRow {
                width: listView.width
                listModel: snapshotListModel
                detailDialog: root.snapshotDetailDialog
                columnWidths: columnWidths
                selected: root.isGroupSelected(preNumber)
                onSelectionToggled: root.toggleGroupSelection(preNumber, postNumber)
            }

            ScrollBar.vertical: ScrollBar {}

            // スナップショットが存在しない場合の表示
            Label {
                anchors.centerIn: parent
                visible: snapshotGroupModel.count === 0
                text: qsTr("No snapshots available")
                font.pixelSize: 16
                color: palette.placeholderText
            }
        }
    }

    // フッターステータスバー
    // ステータスメッセージと「About」ボタンを配置
    footer: Rectangle {
        id: statusBar
        height: 30
        color: palette.window

        property alias text: statusLabel.text

        // 一時的なステータスメッセージを表示
        function showMessage(message, timeout) {
            statusLabel.text = message
            statusTimer.interval = timeout
            statusTimer.restart()
        }

        // ステータスメッセージ表示ラベル
        Label {
            id: statusLabel
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: 10
        }

        // qSnapperについてボタン
        Button {
            id: aboutqSnapperButton
            text: qsTr("About qSnapper")
            flat: true
            anchors.right: aboutQtButton.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: 10
            onClicked: aboutqSnapperDialog.open()
        }

        // Qtライセンスボタン
        Button {
            id: aboutQtButton
            text: qsTr("Qt License")
            flat: true
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: 10
            onClicked: aboutQtDialog.open()
        }

        // ステータスメッセージ自動クリアタイマ
        Timer {
            id: statusTimer
            interval: 3000
            onTriggered: statusLabel.text = ""
        }
    }

    // スナップショット作成ダイアログ
    BorderedDialog {
        id: createSnapshotDialog
        title: qsTr("Create Snapshot")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: {
            if (!ApplicationWindow.window) return 600
            var calculated = ApplicationWindow.window.width * 0.5
            return Math.min(Math.max(calculated, 600), 1200)
        }
        height: {
            if (!ApplicationWindow.window) return 400
            var calculated = ApplicationWindow.window.height * 0.5
            return Math.min(Math.max(calculated, 400), 800)
        }

        ColumnLayout {
            width: parent.width
            spacing: 20
            anchors.top: parent.top
            anchors.topMargin: 20

            // 説明ラベル
            Label {
                text: qsTr("Description:")
            }

            // 説明入力フィールド
            TextField {
                id: descriptionField
                Layout.fillWidth: true
                placeholderText: qsTr("Enter snapshot description")
            }

            // スナップショットタイプ選択コンボボックス
            ComboBox {
                id: snapshotTypeCombo
                Layout.fillWidth: true
                model: [qsTr("Single Snapshot"), qsTr("Pre Snapshot")]
                currentIndex: 0
            }

            // Preスナップショットの説明ラベル
            Label {
                visible: snapshotTypeCombo.currentIndex === 1
                text: qsTr("Pre snapshot can be paired with post snapshot later")
                font.italic: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // ユーザデータ入力 (key=value, 1 行 1 ペア, カンマ区切りも可)
            Label {
                text: qsTr("User data (key=value per line):")
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 80
                color: palette.base
                border.color: userdataField.activeFocus
                    ? palette.highlight
                    : Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.35)
                border.width: userdataField.activeFocus ? 2 : 1
                radius: 3

                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true

                    TextArea {
                        id: userdataField
                        placeholderText: qsTr("e.g.\nimportant=yes\nreason=manual-test")
                        wrapMode: TextArea.NoWrap
                        color: palette.text
                        placeholderTextColor: palette.placeholderText
                        background: null
                    }
                }
            }
        }

        // userdata テキストを { k: v } に変換するヘルパー
        function parseUserdata(text) {
            var result = ({})
            var lines = text.split(/[\n,]/)
            for (var i = 0; i < lines.length; i++) {
                var line = lines[i].trim()
                if (line.length === 0) continue
                var eq = line.indexOf("=")
                if (eq <= 0) continue
                var key = line.substring(0, eq).trim()
                var val = line.substring(eq + 1).trim()
                if (key.length > 0) result[key] = val
            }
            return result
        }

        // OKボタン押下時の処理
        onAccepted: {
            if (descriptionField.text.trim() === "") {
                errorDialog.text = qsTr("Please enter a description")
                errorDialog.open()
                return
            }

            var ud = createSnapshotDialog.parseUserdata(userdataField.text)
            if (snapshotTypeCombo.currentIndex === 0) {
                snapshotListModel.createSingleSnapshot(descriptionField.text, ud)
            } else {
                snapshotListModel.createPreSnapshot(descriptionField.text, ud)
            }

            descriptionField.text = ""
            userdataField.text = ""
        }

        // キャンセルボタン押下時の処理
        onRejected: {
            descriptionField.text = ""
            userdataField.text = ""
        }
    }

    // スナップショット編集ダイアログ
    SnapshotEditDialog {
        id: editDialog
        snapshotListModel: snapshotListModel
        anchors.centerIn: Overlay.overlay
    }

    // 2スナップショット比較ダイアログ
    CompareSnapshotsDialog {
        id: compareDialog
        anchors.centerIn: Overlay.overlay
    }

    // 削除確認ダイアログ
    BorderedDialog {
        id: deleteConfirmDialog
        title: qsTr("Confirmation")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: {
            if (!ApplicationWindow.window) return 500
            var calculated = ApplicationWindow.window.width * 0.4
            return Math.min(Math.max(calculated, 500), 800)
        }
        height: {
            if (!ApplicationWindow.window) return 250
            var calculated = ApplicationWindow.window.height * 0.3
            return Math.min(Math.max(calculated, 250), 600)
        }

        ColumnLayout {
            width: parent.width
            spacing: 15

            Label {
                text: root.selectedCount === 1
                    ? qsTr("Delete selected snapshot?")
                    : qsTr("Delete %1 snapshots?").arg(root.selectedCount)
                font.bold: true
                font.pixelSize: 14
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("This operation cannot be undone.")
                color: ThemeManager.errorColor
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("Note: This operation requires administrator privileges.")
                font.italic: true
                font.pixelSize: 12
                color: palette.placeholderText
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        // 削除実行
        // Polkit 認証は D-Bus サービス側 DeleteSnapshot 呼び出し時に
        // 都度実行される (P0-2 で事前認証 API を撤廃)。
        // 連続削除のプロンプト抑止は polkit の auth_admin_keep に委ねる。
        onAccepted: {
            deletingBusyDialog.open()
        }
    }

    // 削除処理中のウェイト表示ダイアログ
    BorderedDialog {
        id: deletingBusyDialog
        title: qsTr("Please wait")
        anchors.centerIn: Overlay.overlay
        modal: true
        closePolicy: Popup.NoAutoClose
        standardButtons: Dialog.NoButton
        width: {
            if (!ApplicationWindow.window) return 400
            var calculated = ApplicationWindow.window.width * 0.3
            return Math.min(Math.max(calculated, 400), 600)
        }

        // ダイアログが完全に表示された後にタイマを起動
        onOpened: {
            deletionDelayTimer.start()
        }

        ColumnLayout {
            width: parent.width
            spacing: 15

            BusyIndicator {
                running: deletingBusyDialog.visible
                Layout.alignment: Qt.AlignHCenter
            }

            Label {
                text: qsTr("Deleting snapshots, please wait...")
                font.pixelSize: 14
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
    }

    // ダイアログ描画完了後に削除処理を開始するためのタイマ
    Timer {
        id: deletionDelayTimer
        interval: 200
        repeat: false
        onTriggered: {
            snapshotListModel.deleteSnapshots(root.selectedSnapshots)
        }
    }

    // エラーダイアログ
    BorderedDialog {
        id: errorDialog
        title: qsTr("Error")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok
        width: {
            if (!ApplicationWindow.window) return 450
            var calculated = ApplicationWindow.window.width * 0.35
            return Math.min(Math.max(calculated, 450), 700)
        }
        height: {
            if (!ApplicationWindow.window) return 200
            var calculated = ApplicationWindow.window.height * 0.25
            return Math.min(Math.max(calculated, 200), 500)
        }

        property alias text: errorLabel.text

        Label {
            id: errorLabel
            width: parent.width
            wrapMode: Text.WordWrap
        }
    }

    // qSnapperについてダイアログ
    AboutqSnapperDialog {
        id: aboutqSnapperDialog
    }

    // Qtライセンスダイアログ
    AboutQtDialog {
        id: aboutQtDialog
    }

    // スナップショット詳細ダイアログ
    SnapshotDetailDialog {
        id: detailDialog
        snapshotListModel: snapshotListModel
        anchors.centerIn: Overlay.overlay
    }
}
