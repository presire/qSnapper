/**
 * @file SnapshotListPage.qml
 * @brief スナップショットの一覧表示と作成・削除・編集・比較を提供するページ
 *
 * Main.qmlから生成され、SnapshotListModelの一覧をSnapshotGroupModelでPre/Postペア単位にまとめて表示する
 * ツールバーで設定 (config) の切り替えを行い、各操作は確認ダイアログを挟んで実行する
 *
 * 入力 : なし (SnapperServiceとThemeManagerのシングルトンを直接参照)
 * 出力 : なし
 * 依存 : SnapshotListModel (一覧・作成・削除・編集、取得中を示すloading、一覧の取得失敗を知らせるlistLoadFailed、
 *        編集対象の情報を返すsnapshotInfo、ユーザデータを検証するparseUserdata、Post作成の紐付け先を求めるpendingPreNumber)、
 *        SnapshotGroupModel (Pre/Postのグループ化)、SnapshotEditDialog / CompareSnapshotsDialog (編集と比較)、
 *        SnapperService (設定一覧・現在設定)、ThemeManager (テーマ色)
 */
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

    // listModel / editDialogRef / compareDialogRefは、qmllintがC++側で登録された型のメンバを解決できず警告が増えるため、
    // var経由で参照する (listModelはsnapshotInfoの呼び出し、2つのダイアログ参照はBindingの対象に使う)
    readonly property var listModel: snapshotListModel     // SnapshotListModelへの参照
    readonly property var editDialogRef: editDialog        // 編集ダイアログへの参照
    readonly property var compareDialogRef: compareDialog  // 比較ダイアログへの参照

    // 編集ダイアログのエラー文言の色を、作成ダイアログの警告ラベルの配色 (ThemeManager.errorColor) に揃える
    // 子ダイアログはThemeManagerを直接参照しないため、親が配色を渡す
    Binding {
        target: root.editDialogRef         // 色を受け取る編集ダイアログ
        property: "errorTextColor"         // 検証エラーの文言の色
        value: userdataErrorLabel.color    // 作成ダイアログの警告ラベルと同じ色
    }

    // 比較ダイアログのエラー文言の色を、作成ダイアログの警告ラベルの配色に揃える (理由は編集ダイアログ向けのBindingと同じ)
    Binding {
        target: root.compareDialogRef      // 色を受け取る比較ダイアログ
        property: "errorTextColor"         // 取得エラーの文言の色
        value: userdataErrorLabel.color    // 作成ダイアログの警告ラベルと同じ色
    }

    /**
     * @brief グループの選択状態を切り替える
     * @param preNumber 対象グループのPreスナップショット番号
     * @param postNumber 対象グループのPostスナップショット番号 (ペアでない場合は0以下)
     * @note Pre/Postペアは両方まとめて選択・解除する
     *       selectedSnapshotsを差し替えてバインド更新を促す
     */
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

    /**
     * @brief 選択状態をクリアする
     * @note selectedSnapshotsを空配列へ差し替える
     */
    function clearSelection() {
        selectedSnapshots = []
    }

    /**
     * @brief 指定されたPre番号のグループが選択されているかを返す
     * @param preNumber 判定するPreスナップショット番号
     * @return 選択済みの場合はtrue
     */
    function isGroupSelected(preNumber) {
        return selectedSnapshots.indexOf(preNumber) >= 0
    }

    // スナップショット一覧のデータモデル
    // SnapshotListModel (C++) がD-Busサービス経由でSnapperから一覧を取得する
    SnapshotListModel {
        id: snapshotListModel

        // SnapshotListModel::snapshotCreatedシグナルを受け取り、スナップショットの作成成功を表示する
        // 作成後の一覧の再読み込みで行が作り直されても、選択中の番号の配列 (selectedSnapshots) はそのまま残る
        // Postを作成する前に選択していたPreの番号だけが残ると、PreとPostの組の行は選択済みに見える
        // しかし、[選択を削除]ボタンはPreだけを削除してしまうため、作成に成功したら選択を解除する
        onSnapshotCreated: {
            root.clearSelection()
            statusBar.showMessage(qsTr("Snapshot created successfully"), 3000)
        }

        // SnapshotListModel::listLoadFailedを受け取り、一覧が空になった理由 (認可の拒否やサービスの停止など) を表示する
        // 表示しないと、取得の失敗とスナップショットが0件の状態を利用者が区別できない
        onListLoadFailed: function(error) {
            errorDialog.text = qsTr("Failed to load the snapshot list: %1").arg(error)
            errorDialog.open()
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
        // 一部でも削除できなかった場合は成功として扱わず、残ったスナップショットを確認できるようエラーで示す
        onSnapshotsDeletionCompleted: function(successCount, failureCount) {
            deletingBusyDialog.close()
            root.clearSelection()
            if (failureCount === 0) {
                statusBar.showMessage(qsTr("Deleted %1 snapshot(s)").arg(successCount), 3000)
            } else {
                errorDialog.text = qsTr("Deleted %1 of %2 snapshots. %3 could not be deleted.")
                    .arg(successCount).arg(successCount + failureCount).arg(failureCount)
                errorDialog.open()
            }
        }

        // 複数スナップショット削除がD-Busエラーで失敗したときのシグナルハンドラ
        // この場合はSnapshotsDeletionCompletedを発行しないため、待機表示をここで閉じる
        onSnapshotsDeletionFailed: function(error) {
            deletingBusyDialog.close()
            root.clearSelection()
            errorDialog.text = qsTr("Failed to delete snapshots: %1").arg(error)
            errorDialog.open()
        }

        // SnapshotListModel::snapshotModifiedを受け取ったときのシグナルハンドラ
        // 反映はD-Busの非同期呼び出しのため、完了で待機表示を閉じて結果を知らせる
        onSnapshotModified: function(number) {
            modifyingBusyDialog.close()
            statusBar.showMessage(qsTr("Snapshot #%1 modified").arg(number), 3000)
        }

        // SnapshotListModel::snapshotModificationFailedを受け取ったときのシグナルハンドラ
        // 待機表示は失敗でも閉じないと操作できなくなるため、先に閉じてからエラーを表示する
        onSnapshotModificationFailed: function(number, error) {
            modifyingBusyDialog.close()
            errorDialog.text = qsTr("Failed to modify snapshot #%1: %2").arg(number).arg(error)
            errorDialog.open()
        }

        // 起動直後に一覧を取得して表示する
        Component.onCompleted: refresh()
    }

    // グループ化モデル (Pre/Postペアを1行にまとめる)
    // SnapshotGroupModel (C++) がソースの変更を監視し、変化のたびにグループを再構築する
    SnapshotGroupModel {
        id: snapshotGroupModel
        snapshotListModel: snapshotListModel
    }

    // テーブルカラム幅の一元管理
    QtObject {
        id: columnWidths
        property int idWidth: 80        // ID列の幅
        property int typeWidth: 140     // 種別列の幅
        property int dateWidth: 170     // 日時列の幅
        property int userdataWidth: 150 // ユーザデータ列の幅
        property int importantWidth: 40 // 重要フラグ列の幅
    }

    // ツールバーヘッダ
    // アプリケーションタイトル、設定切替、操作ボタンを配置
    header: ToolBar {
        topPadding: 8   // FusionのToolBarは上側の余白が0で、上部のTabBarと重なって見えるため下へずらす

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

                /**
                 * @brief currentConfigに対応するcurrentIndexを更新する
                 * @note model / currentConfigの変化に応じて呼ばれる
                 *       宣言的バインドにするとQMLが model -> currentIndex -> model の循環とみなして
                 *       binding loopを検出するため、Connections経由で命令的に設定する
                 */
                function syncCurrentIndex() {
                    var idx = SnapperService.configs.indexOf(SnapperService.currentConfig)
                    currentIndex = idx >= 0 ? idx : 0
                }

                // 設定の切替 (本画面と設定画面のどちらで切り替えた場合も) に追従して、選択をクリアし一覧を再取得する
                // 設定画面で切り替えた場合、本画面のComboBoxのonActivatedは呼ばれないため、ここで一括して処理する
                Connections {
                    target: SnapperService
                    function onConfigsChanged() { configCombo.syncCurrentIndex() }
                    function onCurrentConfigChanged() {
                        configCombo.syncCurrentIndex()
                        root.clearSelection()
                        snapshotListModel.refresh()
                    }
                }

                // 設定切替時: SnapperService.currentConfig (C++) を更新する
                // 選択のクリアと一覧の再取得は、設定画面からの切替にも対応するため上のConnectionsで行う
                onActivated: {
                    var name = model[currentIndex]
                    if (name && name !== SnapperService.currentConfig) {
                        SnapperService.currentConfig = name
                    }
                }
                // 初回に設定一覧を取得し、現在設定に合わせて選択位置を同期する
                Component.onCompleted: {
                    SnapperService.refreshConfigs()
                    syncCurrentIndex()
                }
            }

            Item { Layout.fillWidth: true }

            // 選択数表示ラベル
            Label {
                visible: root.selectedCount > 0
                textFormat: Text.PlainText  // 件数のみの文言だが、他のラベルと表示形式を揃える
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
                // SnapshotListModel::loadingがtrueの間 (一覧の取得中) は削除させない
                // 表示中の番号が、取得後の一覧と一致する保証がないため
                enabled: root.selectedCount > 0 && !snapshotListModel.loading
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
                // SnapshotListModel::loadingがtrueの間 (一覧の取得中) は編集させない
                // 表示中の番号が、取得後の一覧と一致する保証がないため
                enabled: root.selectedCount === 1 && !snapshotListModel.loading
                opacity: enabled ? 1.0 : 0.45
                // 選択中の1件を編集ダイアログへ渡して開く
                onClicked: {
                    var num = root.selectedSnapshots[0]

                    // 一覧の取得は非同期のため、サービスへ問い合わせず、取得済みの一覧から編集対象の情報を取り出す
                    // SnapshotListModel::snapshotInfoは、番号が一覧に無い場合は空のマップを返すため、numberの有無で判定する
                    var snap = root.listModel.snapshotInfo(num)
                    if (snap.number !== undefined) {
                        editDialog.snapshotNumber = snap.number
                        editDialog.descriptionInitial = snap.description
                        editDialog.cleanupInitial = snap.cleanup
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
                // SnapshotListModel::loadingがtrueの間 (一覧の取得中) は比較させない
                // 表示中の番号が、取得後の一覧と一致する保証がないため
                enabled: root.selectedCount === 2 && !snapshotListModel.loading
                opacity: enabled ? 1.0 : 0.45
                onClicked: {
                    compareDialog.num1 = root.selectedSnapshots[0]
                    compareDialog.num2 = root.selectedSnapshots[1]
                    compareDialog.configName = SnapperService.currentConfig
                    compareDialog.open()
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
            textFormat: Text.PlainText  // 件数のみの文言だが、他のラベルと表示形式を揃える
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

        property alias text: statusLabel.text              // ステータス文言のエイリアス

        /**
         * @brief ステータスメッセージを一定時間だけ表示する
         * @param message 表示する文言
         * @param timeout 表示を消すまでの時間 (ミリ秒)
         * @note タイマを再起動し、経過後にstatusLabelの文言を空にする
         */
        function showMessage(message, timeout) {
            statusLabel.text = message
            statusTimer.interval = timeout
            statusTimer.restart()
        }

        // ステータスメッセージ表示ラベル
        Label {
            id: statusLabel
            textFormat: Text.PlainText  // 操作結果にエラー文言などが含まれ得るため、リッチテキストとして解釈させない
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
        // ダイアログの高さは、ウィンドウの高さの半分 (400以上800以下) を基本にする
        // ただし、種類にPreを選ぶと案内のラベルが増え、本文 (ColumnLayout) が基本の高さに収まらなくなる
        // 本文より低くすると、フッターの[OK]ボタンと[キャンセル]ボタンが[ユーザデータ]の入力欄に重なるため、本文の高さより低くしない
        // 本文はanchors.topMarginで20だけ下へずらして配置するため、本文の高さに20を加える
        // ウィンドウより高くすると、ボタンが画面の外に出て押せなくなるため、ウィンドウの高さを上限にする
        height: {
            var fitted = implicitHeight + 20
            if (!ApplicationWindow.window) {
                return Math.max(fitted, 400)
            }

            var calculated = ApplicationWindow.window.height * 0.5
            var preferred = Math.min(Math.max(calculated, 400), 800)
            return Math.min(Math.max(preferred, fitted), ApplicationWindow.window.height)
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

            // スナップショットタイプ選択コンボボックス (0: Single、1: Pre、2: Post)
            // availableがfalseの項目は無効表示にする
            // Postは紐付け先のPreが決まっている場合だけ選べる (紐付け先が無いままPostを作成するとサーバが拒否するため、選択前に止める)
            ComboBox {
                id: snapshotTypeCombo
                Layout.fillWidth: true
                textRole: "text"
                model: [
                    { text: qsTr("Single Snapshot"), available: true },
                    { text: qsTr("Pre Snapshot"), available: true },
                    { text: qsTr("Post Snapshot"), available: createSnapshotDialog.pendingPre > 0 }
                ]
                currentIndex: 0

                delegate: ItemDelegate {
                    required property var modelData
                    required property int index

                    width: ListView.view.width
                    text: modelData.text
                    enabled: modelData.available
                    // Main.qmlがパレットの色をバインディングで指定しているため、Fusionスタイルは無効状態でも文字色を灰色に切り替えない
                    // そのため、無効状態であることは不透明度を下げて示す
                    opacity: enabled ? 1.0 : 0.45
                    highlighted: ListView.view.currentIndex === index
                }

                // キーボード操作で無効な項目に移った場合は、Singleへ戻して不正な組み合わせで作成させない
                onActivated: function(index) {
                    if (index === 2 && createSnapshotDialog.pendingPre <= 0) {
                        currentIndex = 0
                    }
                }
            }

            // Preスナップショットの説明ラベル
            Label {
                visible: snapshotTypeCombo.currentIndex === 1
                text: qsTr("Pre snapshot can be paired with post snapshot later")
                font.italic: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // Postを選べない理由の案内
            // 一覧でPre (Postが未作成のもの) を1件だけ選択している場合にだけPostを作成できるため、その手順を示す
            Label {
                visible: createSnapshotDialog.pendingPre <= 0
                text: qsTr("To create a post snapshot, first select a pre snapshot that has no post snapshot.")
                font.italic: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // Postの紐付け先の案内
            Label {
                visible: snapshotTypeCombo.currentIndex === 2 && createSnapshotDialog.pendingPre > 0
                textFormat: Text.PlainText  // 番号を埋め込む文言だが、他のラベルと表示形式を揃える
                text: qsTr("This snapshot will be paired with pre snapshot #%1").arg(createSnapshotDialog.pendingPre)
                font.italic: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // クリーンアップアルゴリズムと重要フラグ
            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Label {
                    text: qsTr("Cleanup:")
                }

                // 値は""、number、timelineの順 (空文字列は自動削除の対象外を表す)
                ComboBox {
                    id: cleanupCombo
                    Layout.preferredWidth: 160
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { text: qsTr("None"), value: "" },
                        { text: "number", value: "number" },
                        { text: "timeline", value: "timeline" }
                    ]
                    currentIndex: 0
                }

                // 重要なスナップショットは、番号クリーンアップで別の保持上限 (NUMBER_LIMIT_IMPORTANT) が適用される
                CheckBox {
                    id: importantCheck
                    text: qsTr("Important")
                    checked: false
                }

                Item { Layout.fillWidth: true }
            }

            // ユーザデータ入力 (key=value、1行に1ペア)
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

            // ユーザデータの形式が不正な場合の警告
            // 重複したキーを黙って上書きすると値が変わってしまうため、原因を示して修正を促す
            // 色は、編集ダイアログと比較ダイアログのエラー文言にも共有する (ルートのBindingから参照される)
            Label {
                id: userdataErrorLabel  // 警告ラベルの参照名 (色を子ダイアログへ渡すために参照される)

                visible: createSnapshotDialog.userdataCheck.ok !== true  // 検証に失敗している間だけ表示する
                text: createSnapshotDialog.userdataCheck.error           // SnapshotListModel::parseUserdataが返したエラー文言
                textFormat: Text.PlainText                               // 入力されたキー名を含み得るため、リッチテキストとして解釈させない
                color: ThemeManager.errorColor                           // エラー色 (Light/Darkに応じてThemeManagerが切り替える)
                wrapMode: Text.WordWrap                                  // 幅に収まるよう折り返す
                Layout.fillWidth: true                                   // 折り返しの基準となる幅を確保する
            }
        }

        /**
         * @brief 入力欄を初期値へ戻す
         * @note 次回の作成に前回の入力 (特にImportantとクリーンアップ) が残らないようにする
         */
        function resetFields() {
            descriptionField.text = ""
            userdataField.text = ""
            snapshotTypeCombo.currentIndex = 0
            cleanupCombo.currentIndex = 0
            importantCheck.checked = false
        }

        // Postの紐付け先となるPreスナップショット番号 (選択がPre1件で、Postが未作成の場合のみ正の値)
        // SnapshotListModel::pendingPreNumberで判定する
        // 選択の変更に加え、一覧の更新 (PostやPreの作成後) でも判定が変わるため、
        // 件数の変更通知 (countChanged) を依存に含めて再評価させる
        readonly property int pendingPre: createModel.count >= 0 ? createModel.pendingPreNumber(root.selectedSnapshots) : -1

        // 作成系メソッドの呼び出し先 (SnapshotListModel)
        // qmllintはC++側で登録された型のメンバを解決できないため、var経由で参照して警告の増加を避ける
        readonly property var createModel: snapshotListModel

        // 入力中のユーザデータの検証結果 (SnapshotListModel::parseUserdataの戻り値: ok / userdata / error)
        // 入力のたびに評価し直す
        // 値は入力どおりに保存し、キーは前後の空白だけを取り除く
        // 同じキーが複数ある場合などは警告を表示し、[OK]を無効にする
        readonly property var userdataCheck: createModel.parseUserdata(userdataField.text)

        // 形式が不正なユーザデータのまま作成できないよう、[OK]を検証結果に連動させる
        // standardButtonはフッターのDialogButtonBoxが作られた後でしか取得できないため、構築完了時に結び付ける
        Component.onCompleted: {
            createSnapshotDialog.standardButton(Dialog.Ok).enabled = Qt.binding(function() {
                return createSnapshotDialog.userdataCheck.ok === true
            })
        }

        // 表示のたびに種別をSingleへ戻す
        // 前回Postを選んだまま、選択の変更でPostが使えなくなった状態が残るのを避ける
        onAboutToShow: snapshotTypeCombo.currentIndex = 0

        // [OK]押下時: 説明文を検証し、種別に応じて SnapshotListModel::createSingleSnapshot / createPreSnapshot / createPostSnapshot を呼び出す
        onAccepted: {
            if (descriptionField.text.trim() === "") {
                errorDialog.text = qsTr("Please enter a description")
                errorDialog.open()
                return
            }

            // 検証に失敗している場合は[OK]が無効のため通常は到達しないが、念のため再検証して拒否する
            if (createSnapshotDialog.userdataCheck.ok !== true) {
                return
            }

            var ud = createSnapshotDialog.userdataCheck.userdata
            var cleanup = cleanupCombo.currentValue
            var important = importantCheck.checked
            if (snapshotTypeCombo.currentIndex === 0) {
                createSnapshotDialog.createModel.createSingleSnapshot(descriptionField.text, cleanup, important, ud)
            } else if (snapshotTypeCombo.currentIndex === 1) {
                createSnapshotDialog.createModel.createPreSnapshot(descriptionField.text, cleanup, important, ud)
            } else if (createSnapshotDialog.pendingPre > 0) {
                createSnapshotDialog.createModel.createPostSnapshot(descriptionField.text, createSnapshotDialog.pendingPre,
                                                     cleanup, important, ud)
            } else {
                // 選択が変わってPostの紐付け先が無くなっている場合は作成しない
                errorDialog.text = qsTr("Select a pre snapshot that has no post snapshot to create a post snapshot")
                errorDialog.open()
                return
            }

            createSnapshotDialog.resetFields()
        }

        // [Cancel]押下時: 入力内容を破棄して次回に残さない
        onRejected: createSnapshotDialog.resetFields()
    }

    // スナップショット編集ダイアログ
    SnapshotEditDialog {
        id: editDialog
        snapshotListModel: snapshotListModel
        anchors.centerIn: Overlay.overlay

        // SnapshotEditDialogが編集内容の反映を依頼したら、完了まで待機ダイアログを開く
        // 反映はD-Busの非同期呼び出しのため、待機表示を出さないと進行中であることが分からない
        // 待機ダイアログは、snapshotModified / snapshotModificationFailedの受信時に閉じる
        onModifySubmitted: modifyingBusyDialog.open()
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
                textFormat: Text.PlainText  // 件数のみの文言だが、他のラベルと表示形式を揃える
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

        // 削除の確認で[OK]ボタンが押されたときに、選択中のスナップショットを削除する
        // SnapshotListModel::deleteSnapshotsは、1件ならヘルパのDeleteSnapshotを呼び出す
        // DeleteSnapshotは、polkitが認可を一時保持する間は再認証を求めない
        // 複数件ならヘルパのDeleteSnapshotsを呼び出し、件数にかかわらず認証を1回だけ求める
        // 削除の結果は、どちらの場合もsnapshotsDeletionCompletedシグナルとsnapshotsDeletionFailedシグナルで受け取る
        // deleteSnapshotsはD-Busを非同期に呼び出すため、先に処理中の表示 (deletingBusyDialog) を開いてもGUIは固まらない
        onAccepted: {
            deletingBusyDialog.open()
            snapshotListModel.deleteSnapshots(root.selectedSnapshots)
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

    // スナップショット編集中の待機ダイアログ
    // SnapshotListModel::modifySnapshotは非同期で、認証や処理に時間がかかるため、完了まで進行中であることを示す
    // 二重実行を防ぐため操作では閉じられず、snapshotModifiedまたはsnapshotModificationFailedの受信時に閉じる
    BorderedDialog {
        id: modifyingBusyDialog  // 待機ダイアログの参照名

        title: qsTr("Please wait")                    // ダイアログのタイトル
        standardButtons: Dialog.NoButton              // 閉じる操作を与えないためボタンを置かない
        anchors.centerIn: Overlay.overlay             // ウィンドウ全体の中央に表示する
        // ウィンドウ幅の30%を基準に、400から600の範囲へクランプする
        width: {
            if (!ApplicationWindow.window) return 400
            var calculated = ApplicationWindow.window.width * 0.3
            return Math.min(Math.max(calculated, 400), 600)
        }
        modal: true                                   // 実行中に背後の画面を操作させない
        closePolicy: Popup.NoAutoClose                // [Esc]キーや外側のクリックで閉じさせない

        // 進行状況の表示領域
        ColumnLayout {
            width: parent.width  // ダイアログの幅いっぱいに配置する
            spacing: 15          // 要素間の間隔

            // 処理が続いていることを示す回転アニメーション
            BusyIndicator {
                running: modifyingBusyDialog.visible  // 表示中だけ回して、非表示時の無駄な描画を避ける
                Layout.alignment: Qt.AlignHCenter     // 水平方向の中央に配置する
            }

            // 待機中であることを利用者に伝える文言
            Label {
                text: qsTr("Applying changes to the snapshot, please wait...")  // 待機中の案内文
                font.pixelSize: 14                                              // 文字の大きさ
                horizontalAlignment: Text.AlignHCenter                          // 水平方向の中央に揃える
                wrapMode: Text.WordWrap                                         // 幅に収まるよう折り返す
                Layout.fillWidth: true                                          // 折り返しの基準となる幅を確保する
            }
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

        property alias text: errorLabel.text               // エラー文言のエイリアス

        Label {
            id: errorLabel
            textFormat: Text.PlainText  // C++側のエラー文言を表示するため、リッチテキストとして解釈させない
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
