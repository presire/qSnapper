/**
 * @file SnapshotDetailDialog.qml
 * @brief スナップショットの詳細を表示するダイアログ
 *
 * 選択スナップショットの基本情報・説明・ユーザデータを表示し
 * ファイル復元 (RestorePreviewDialog) とシステムロールバック機能を提供する
 *
 * 入力 : snapshot (表示対象)、postSnapshot (Pre/Postペア時のPost情報)、snapshotListModel (操作先モデル)
 * 出力 : なし (ロールバック結果はsnapshotListModelのシグナルで受け取る)
 * 依存 : BorderedDialog (基底)、RestorePreviewDialog (個別復元)、
 *        SnapshotListModel::rollbackSnapshot / rollbackCompleted / rollbackFailed (ロールバック)、
 *        SnapperSettingsModel::rollbackSupported (ロールバック可否判定)、SnapperService::currentConfig (判定対象の設定名)、
 *        ThemedGroupBox (情報のグループ枠)、ThemeManager (警告色とダーク判定)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root
    modal: true                                       // 背後への操作をブロックする
    // ウィンドウ幅の70%を基準に、800から1280の範囲へクランプする
    width: {
        if (!ApplicationWindow.window) return 800
        var calculated = ApplicationWindow.window.width * 0.7
        return Math.min(Math.max(calculated, 800), 1280)
    }
    // ウィンドウ高さの70%を基準に、640から1024の範囲へクランプする
    height: {
        if (!ApplicationWindow.window) return 640
        var calculated = ApplicationWindow.window.height * 0.7
        return Math.min(Math.max(calculated, 640), 1024)
    }
    title: qsTr("Snapshot Details")                   // タイトルバーの表示名

    property var snapshot: null                       // 表示するスナップショット情報
    property var postSnapshot: null                   // Pre/Postペア時のPost情報
    property var snapshotListModel: null              // スナップショットリストモデルへの参照

    // ロールバック可否の判定に使う設定情報 (SUBVOLUMEとFSTYPE)
    // 設定名からでは判定できないため、特権サービスから実際の設定を読み込んで判定する
    // SnapperSettingsModelとSnapperServiceはmain.cppで実行時に登録する型のため、qmllintは型情報を持たず警告する
    // 既存の型 (SnapshotListModelなど) と同じ理由の警告なので、この宣言と参照の箇所だけ抑止する
    // qmllint disable import missing-property unqualified
    SnapperSettingsModel {
        id: rollbackSettings
        configName: SnapperService.currentConfig
    }
    // qmllint enable import missing-property unqualified

    // ダイアログを開くたびに設定を読み込み直す
    // 設定画面での変更や設定の切り替えを反映するためで、読み込みが終わるまではrollbackSupportedがfalseのため
    // ロールバックは無効のままになる (判定できない場合に許可しない)
    // SnapperSettingsModel::reloadを呼び出し、特権サービスのGetConfigで取得する
    // qmllint disable missing-property
    onOpened: rollbackSettings.reload()
    // qmllint enable missing-property

    // 復元プレビューダイアログ
    // 個別ファイル/ディレクトリ復元用
    RestorePreviewDialog {
        id: restorePreviewDialog
        configName: SnapperService.currentConfig  // 選択中の設定 (復元元と復元先の両方の基準になる)
        // Pre/Postペアの場合はPre/Post両方の番号を設定する
        preSnapshotNumber: (postSnapshot && postSnapshot.number) ? snapshot.number : 0
        postSnapshotNumber: (postSnapshot && postSnapshot.number) ? postSnapshot.number : 0
        // 既定はPost番号 (Pre/Postペアでない場合はsnapshot番号)
        snapshotNumber: {
            if (postSnapshot && postSnapshot.number)
                return postSnapshot.number
            return (snapshot && snapshot.number) ? snapshot.number : 0
        }
    }

    // snapshotが設定されている間のみ本体を表示する
    ColumnLayout {
        anchors.fill: parent
        spacing: 15
        visible: snapshot !== null

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth

            ColumnLayout {
                width: parent.width
                spacing: 10

                // 基本情報グループ
                // スナップショット番号、タイプ、日時、ユーザ等を表示する
                ThemedGroupBox {
                    title: qsTr("Basic Information")
                    Layout.fillWidth: true

                    GridLayout {
                        anchors.fill: parent
                        columns: 2
                        columnSpacing: 15
                        rowSpacing: 10

                        Label {
                            text: qsTr("Number:")
                            font.bold: true
                        }
                        // Pre/Postペアの場合は両方の番号を併記する
                        Label {
                            textFormat: Text.PlainText  // スナップショット由来の文字列を、タグ等のリッチテキストとして解釈させない
                            text: {
                                if (!snapshot) return ""
                                if (postSnapshot && postSnapshot.number)
                                    return snapshot.number + " & " + postSnapshot.number
                                return snapshot.number
                            }
                        }

                        Label {
                            text: qsTr("Type:")
                            font.bold: true
                        }
                        // 種別名をローカライズして表示する
                        Label {
                            textFormat: Text.PlainText  // スナップショット由来の文字列を、タグ等のリッチテキストとして解釈させない
                            text: {
                                if (!snapshot) return ""
                                switch (snapshot.snapshotTypeString) {
                                case "single":
                                    return qsTr("Single")
                                case "prepost":
                                    return qsTr("Pre and Post")
                                case "pre":
                                    return qsTr("Pre")
                                case "post":
                                    return qsTr("Post")
                                default:
                                    return snapshot.snapshotTypeString
                                }
                            }
                        }

                        // Pre/Postペアでは開始日、それ以外は日時として表示する
                        Label {
                            textFormat: Text.PlainText  // 固定の文言だが、隣の値のラベルと表示形式を揃える
                            text: postSnapshot ? qsTr("Start Date:") : qsTr("Date/Time:")
                            font.bold: true
                        }
                        Label {
                            textFormat: Text.PlainText  // 数値や日時の表示でも、他のラベルと表示形式を揃える
                            text: snapshot ? Qt.formatDateTime(snapshot.timestamp, "yyyy-MM-dd HH:mm:ss") : ""
                        }

                        // 終了日 (Pre/Postペアの場合のみ表示)
                        Label {
                            text: qsTr("End Date:")
                            font.bold: true
                            visible: postSnapshot !== null && postSnapshot !== undefined
                        }
                        Label {
                            textFormat: Text.PlainText  // 数値や日時の表示でも、他のラベルと表示形式を揃える
                            text: postSnapshot ? Qt.formatDateTime(postSnapshot.timestamp, "yyyy-MM-dd HH:mm:ss") : ""
                            visible: postSnapshot !== null && postSnapshot !== undefined
                        }

                        Label {
                            text: qsTr("User:")
                            font.bold: true
                        }
                        // 作成ユーザ名 (空の場合はプレースホルダを表示する)
                        Label {
                            textFormat: Text.PlainText  // スナップショット由来の文字列を、タグ等のリッチテキストとして解釈させない
                            text: snapshot ? (snapshot.user || qsTr("Unknown")) : ""
                        }

                        // クリーンアップ設定が有る場合のみ表示する
                        Label {
                            text: qsTr("Cleanup:")
                            font.bold: true
                            visible: snapshot && snapshot.cleanupAlgoString !== ""
                        }
                        Label {
                            textFormat: Text.PlainText  // 数値や日時の表示でも、他のラベルと表示形式を揃える
                            text: snapshot ? snapshot.cleanupAlgoString : ""
                            visible: snapshot && snapshot.cleanupAlgoString !== ""
                        }

                        // 前スナップショットが有る場合のみ表示する
                        Label {
                            text: qsTr("Previous Snapshot:")
                            font.bold: true
                            visible: snapshot && snapshot.previousNumber > 0
                        }
                        Label {
                            textFormat: Text.PlainText  // 数値や日時の表示でも、他のラベルと表示形式を揃える
                            text: snapshot ? "#" + snapshot.previousNumber : ""
                            visible: snapshot && snapshot.previousNumber > 0
                        }
                    }
                }

                // 説明グループ
                ThemedGroupBox {
                    title: qsTr("Description")
                    Layout.fillWidth: true

                    // 説明文 (空の場合はプレースホルダを表示する)
                    Label {
                        anchors.fill: parent
                        textFormat: Text.PlainText  // スナップショット由来の文字列を、タグ等のリッチテキストとして解釈させない
                        text: snapshot ? (snapshot.description || qsTr("(No description)")) : ""
                        wrapMode: Text.WordWrap
                    }
                }

                // ユーザデータグループ
                // Snapperに保存されたカスタムメタデータを表示する
                ThemedGroupBox {
                    title: qsTr("User Data")
                    Layout.fillWidth: true
                    visible: snapshot && snapshot.userdata && Object.keys(snapshot.userdata).length > 0

                    GridLayout {
                        anchors.fill: parent
                        columns: 2
                        columnSpacing: 15
                        rowSpacing: 10

                        // キー列
                        Repeater {
                            model: (snapshot && snapshot.userdata) ? Object.keys(snapshot.userdata) : []

                            Label {
                                textFormat: Text.PlainText  // スナップショット由来の文字列を、タグ等のリッチテキストとして解釈させない
                                text: modelData + ":"
                                font.bold: true
                                Layout.row: index
                                Layout.column: 0
                            }
                        }

                        // 値列
                        Repeater {
                            model: (snapshot && snapshot.userdata) ? Object.keys(snapshot.userdata) : []

                            Label {
                                textFormat: Text.PlainText  // スナップショット由来の文字列を、タグ等のリッチテキストとして解釈させない
                                text: (snapshot && snapshot.userdata) ? snapshot.userdata[modelData] : ""
                                Layout.row: index
                                Layout.column: 1
                            }
                        }
                    }
                }
            }
        }

        // アクションボタン行
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            // 変更点の表示ボタン
            // RestorePreviewDialogを開いて変更ファイル一覧と個別復元UIを表示する
            Button {
                text: qsTr("Show Changes")
                icon.name: "document-revert"
                icon.color: ThemeManager.isDark ? "#FFFFFF" : palette.buttonText
                highlighted: true
                onClicked: restorePreviewDialog.open()
            }

            // システムロールバックボタン
            // ロールバックは、btrfsのルート (SUBVOLUMEが/) の設定でのみ可能 (サービス側でもSUBVOLUMEとFSTYPEで拒否する)
            // 設定名では判定せず、読み込んだ設定の内容で判定する
            // 読み込み中と読み込み失敗では無効のままにする (判定できない場合に許可しない)
            Button {
                text: qsTr("System Rollback")
                icon.name: "view-refresh"
                icon.color: ThemeManager.isDark ? "#FFFFFF" : palette.buttonText
                // 再表示時は前回の判定が残っているため、読み込み中も無効にして今回の結果だけで判定する
                // qmllint disable missing-property
                enabled: rollbackSettings.rollbackSupported && !rollbackSettings.configBusy
                // qmllint enable missing-property
                // Main.qmlがパレットの色をバインディングで指定しているため、Fusionスタイルは無効状態でも文字色を灰色に切り替えない
                // そのため、無効状態であることは不透明度を下げて示す
                opacity: enabled ? 1.0 : 0.45
                onClicked: confirmDialog.open()
            }

            Item {
                Layout.fillWidth: true
            }

            // 閉じるボタン
            Button {
                text: qsTr("Close")
                onClicked: root.close()
            }
        }
    }

    // システムロールバック確認ダイアログ
    // 破壊的操作のため、実行前に管理者権限が必要な旨と、再起動が必要な点を提示して確認する
    BorderedDialog {
        id: confirmDialog
        title: qsTr("Confirmation")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Yes | Dialog.No
        // ウィンドウ幅の40%を基準に、500から800の範囲へクランプする
        width: {
            if (!ApplicationWindow.window) return 500
            var calculated = ApplicationWindow.window.width * 0.4
            return Math.min(Math.max(calculated, 500), 800)
        }
        // ウィンドウ高さの30%を基準に、360から600の範囲へクランプする (再起動に関する警告文を含めるため下限を大きくしている)
        height: {
            if (!ApplicationWindow.window) return 360
            var calculated = ApplicationWindow.window.height * 0.3
            return Math.min(Math.max(calculated, 360), 600)
        }

        ColumnLayout {
            spacing: 10

            Label {
                textFormat: Text.PlainText  // 埋め込む値を含む文言のため、リッチテキストとして解釈させない
                text: snapshot ? qsTr("Rollback to snapshot #%1?").arg(snapshot.number) : ""
                font.bold: true
            }

            Label {
                text: qsTr("This operation will restore the system to the selected snapshot state.")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
                color: palette.text
            }

            // 管理者権限が必要である点を警告色で明示する
            Label {
                text: qsTr("Note: This operation requires administrator privileges.")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
                color: ThemeManager.warningColor
                font.italic: true
            }

            // ロールバック後は再起動が必要で、再起動までに行った変更は既定では起動されないことを警告する
            // (ロールバックは既定のサブボリュームを切り替えるだけで、再起動までの変更は現在起動中のサブボリューム側に残るため)
            Label {
                text: qsTr("A reboot is required after the rollback. Changes made after the rollback and before the reboot are not booted by default, because they remain in the currently running subvolume.")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
                color: ThemeManager.warningColor
                font.italic: true
            }
        }

        // [Yes]押下時にSnapshotListModel::rollbackSnapshotを呼び出してロールバックを実行する
        onAccepted: {
            if (snapshot && snapshotListModel) {
                // ロールバックはD-Busを非同期に呼び出すため、呼び出しはすぐに戻る
                // 認証や実行に時間がかかる間も進行中だと分かるよう、完了までの待機ダイアログを先に開く
                rollbackBusyDialog.open()
                snapshotListModel.rollbackSnapshot(snapshot.number)
            }
            root.close()
        }
    }

    // SnapshotListModelのロールバック結果シグナルを受けて通知ダイアログを開く
    Connections {
        target: snapshotListModel ?? null

        // SnapshotListModel::rollbackCompletedを受け取ったときに待機ダイアログを閉じ、成功ダイアログを開く
        function onRollbackCompleted() {
            rollbackBusyDialog.close()
            if (snapshotListModel) {
                successDialog.open()
            }
        }

        // SnapshotListModel::rollbackFailedを受け取ったときに待機ダイアログを閉じ、エラー内容を表示する
        function onRollbackFailed(error) {
            rollbackBusyDialog.close()
            if (snapshotListModel) {
                errorDialog.errorText = error
                errorDialog.open()
            }
        }
    }

    // ロールバック実行中の待機ダイアログ
    // SnapshotListModel::rollbackSnapshotは非同期で、認証や処理に時間がかかるため、完了まで進行中であることを示す
    // 二重実行を防ぐため操作では閉じられず、rollbackCompletedまたはrollbackFailedの受信時にConnectionsから閉じる
    BorderedDialog {
        id: rollbackBusyDialog  // 待機ダイアログの参照名

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
                running: rollbackBusyDialog.visible  // 表示中だけ回して、非表示時の無駄な描画を避ける
                Layout.alignment: Qt.AlignHCenter    // 水平方向の中央に配置する
            }

            // 待機中であることを利用者に伝える文言
            Label {
                text: qsTr("Rolling back the snapshot, please wait...")  // 待機中の案内文
                font.pixelSize: 14                                       // 文字の大きさ
                horizontalAlignment: Text.AlignHCenter                   // 水平方向の中央に揃える
                wrapMode: Text.WordWrap                                  // 幅に収まるよう折り返す
                Layout.fillWidth: true                                   // 折り返しの基準となる幅を確保する
            }
        }
    }

    // ロールバック成功ダイアログ
    BorderedDialog {
        id: successDialog
        title: qsTr("Success")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok
        // ウィンドウ幅の35%を基準に、450から700の範囲へクランプする
        width: {
            if (!ApplicationWindow.window) return 450
            var calculated = ApplicationWindow.window.width * 0.35
            return Math.min(Math.max(calculated, 450), 700)
        }
        // ウィンドウ高さの25%を基準に、200から500の範囲へクランプする
        height: {
            if (!ApplicationWindow.window) return 200
            var calculated = ApplicationWindow.window.height * 0.25
            return Math.min(Math.max(calculated, 200), 500)
        }

        Label {
            text: qsTr("Snapshot rollback completed.\nPlease reboot the system.")
            wrapMode: Text.WordWrap
        }
    }

    // ロールバックエラーダイアログ
    BorderedDialog {
        id: errorDialog
        title: qsTr("Error")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok
        // ウィンドウ幅の35%を基準に、450から700の範囲へクランプする
        width: {
            if (!ApplicationWindow.window) return 450
            var calculated = ApplicationWindow.window.width * 0.35
            return Math.min(Math.max(calculated, 450), 700)
        }
        // ウィンドウ高さの25%を基準に、200から500の範囲へクランプする
        height: {
            if (!ApplicationWindow.window) return 200
            var calculated = ApplicationWindow.window.height * 0.25
            return Math.min(Math.max(calculated, 200), 500)
        }

        property string errorText: ""              // 表示するエラーメッセージ

        Label {
            textFormat: Text.PlainText  // C++側のエラー文言を埋め込むため、リッチテキストとして解釈させない
            text: qsTr("Rollback failed: %1").arg(errorDialog.errorText)
            wrapMode: Text.WordWrap
        }
    }
}
