// スナップショット詳細ダイアログ
// 選択されたスナップショットの詳細情報を表示し、
// ファイル復元とシステムロールバック機能を提供
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

Dialog {
    id: root
    modal: true
    width: {
        if (!ApplicationWindow.window) return 800
        var calculated = ApplicationWindow.window.width * 0.7
        return Math.min(Math.max(calculated, 800), 1280)
    }
    height: {
        if (!ApplicationWindow.window) return 640
        var calculated = ApplicationWindow.window.height * 0.7
        return Math.min(Math.max(calculated, 640), 1024)
    }
    title: qsTr("Snapshot Details")

    property var snapshot: null                      // 表示するスナップショット情報
    property var snapshotListModel: null             // スナップショットリストモデルへの参照

    // 復元プレビューダイアログ
    // 個別ファイル/ディレクトリ復元用
    RestorePreviewDialog {
        id: restorePreviewDialog
        configName: "root"  // 通常はrootファイルシステムの設定を使用
        snapshotNumber: (snapshot && snapshot.number) ? snapshot.number : 0
    }

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
                // スナップショット番号、タイプ、日時、ユーザー等を表示
                GroupBox {
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
                        Label {
                            text: snapshot ? snapshot.number : ""
                        }

                        Label {
                            text: qsTr("Type:")
                            font.bold: true
                        }
                        Label {
                            text: {
                                if (!snapshot) return ""
                                switch (snapshot.snapshotTypeString) {
                                case "single":
                                    return qsTr("Single")
                                case "pre":
                                    return qsTr("Pre")
                                case "post":
                                    return qsTr("Post")
                                default:
                                    return snapshot.snapshotTypeString
                                }
                            }
                        }

                        Label {
                            text: qsTr("Date/Time:")
                            font.bold: true
                        }
                        Label {
                            text: snapshot ? Qt.formatDateTime(snapshot.timestamp, "yyyy-MM-dd HH:mm:ss") : ""
                        }

                        Label {
                            text: qsTr("User:")
                            font.bold: true
                        }
                        Label {
                            text: snapshot ? (snapshot.user || qsTr("Unknown")) : ""
                        }

                        Label {
                            text: qsTr("Cleanup:")
                            font.bold: true
                            visible: snapshot && snapshot.cleanupAlgoString !== ""
                        }
                        Label {
                            text: snapshot ? snapshot.cleanupAlgoString : ""
                            visible: snapshot && snapshot.cleanupAlgoString !== ""
                        }

                        Label {
                            text: qsTr("Previous Snapshot:")
                            font.bold: true
                            visible: snapshot && snapshot.previousNumber > 0
                        }
                        Label {
                            text: snapshot ? "#" + snapshot.previousNumber : ""
                            visible: snapshot && snapshot.previousNumber > 0
                        }
                    }
                }

                // 説明グループ
                GroupBox {
                    title: qsTr("Description")
                    Layout.fillWidth: true

                    Label {
                        anchors.fill: parent
                        text: snapshot ? (snapshot.description || qsTr("(No description)")) : ""
                        wrapMode: Text.WordWrap
                    }
                }

                // ユーザーデータグループ
                // Snapperに保存されたカスタムメタデータを表示
                GroupBox {
                    title: qsTr("User Data")
                    Layout.fillWidth: true
                    visible: snapshot && snapshot.userdata && Object.keys(snapshot.userdata).length > 0

                    GridLayout {
                        anchors.fill: parent
                        columns: 2
                        columnSpacing: 15
                        rowSpacing: 10

                        Repeater {
                            model: (snapshot && snapshot.userdata) ? Object.keys(snapshot.userdata) : []

                            Label {
                                text: modelData + ":"
                                font.bold: true
                                Layout.row: index
                                Layout.column: 0
                            }
                        }

                        Repeater {
                            model: (snapshot && snapshot.userdata) ? Object.keys(snapshot.userdata) : []

                            Label {
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

            // ファイル復元ボタン
            Button {
                text: qsTr("Restore Files")
                icon.name: "document-revert"
                highlighted: true
                onClicked: restorePreviewDialog.open()
            }

            // システムロールバックボタン
            Button {
                text: qsTr("System Rollback")
                icon.name: "view-refresh"
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
    Dialog {
        id: confirmDialog
        title: qsTr("Confirmation")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Yes | Dialog.No
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
            spacing: 10

            Label {
                text: snapshot ? qsTr("Rollback to snapshot #%1?").arg(snapshot.number) : ""
                font.bold: true
            }

            Label {
                text: qsTr("This operation will restore the system to the selected snapshot state.")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
                color: palette.text
            }

            Label {
                text: qsTr("Note: This operation requires administrator privileges.")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 400
                color: ThemeManager.warningColor
                font.italic: true
            }
        }

        // ロールバック実行
        onAccepted: {
            if (snapshot && snapshotListModel) {
                snapshotListModel.rollbackSnapshot(snapshot.number)
            }
            root.close()
        }
    }

    // ロールバック結果通知接続
    Connections {
        target: snapshotListModel ?? null

        // ロールバック成功時
        function onRollbackCompleted() {
            if (snapshotListModel) {
                successDialog.open()
            }
        }

        // ロールバック失敗時
        function onRollbackFailed(error) {
            if (snapshotListModel) {
                errorDialog.errorText = error
                errorDialog.open()
            }
        }
    }

    // ロールバック成功ダイアログ
    Dialog {
        id: successDialog
        title: qsTr("Success")
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

        Label {
            text: qsTr("Snapshot rollback completed.\nPlease reboot the system.")
            wrapMode: Text.WordWrap
        }
    }

    // ロールバックエラーダイアログ
    Dialog {
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

        property string errorText: ""

        Label {
            text: qsTr("Rollback failed: %1").arg(errorDialog.errorText)
            wrapMode: Text.WordWrap
        }
    }
}
