/**
 * @file SnapshotTableRow.qml
 * @brief スナップショットテーブルの行デリゲート
 *
 * SnapshotGroupModelの1グループをテーブル行として表示する
 * 選択チェックボックス、種別、重要マーカー、開始/終了日時、説明、ユーザデータ、詳細ボタンを持つ
 *
 * 入力 : SnapshotGroupModelのロール (displayId / groupType / groupTypeString / startTime / endTime /
 *        description / user / userdata / preNumber / postNumber / cleanupAlgoString / isImportant)、
 *        listModel (SnapshotListModelへの参照)、detailDialog (詳細ダイアログ)、columnWidths (列幅)
 * 出力 : selectionToggled (チェックボックス操作時に発行する)
 * 依存 : SnapshotGroupModel (ロール)、ThemeManager (種別色と重要色)、SnapshotDetailDialog (遷移先)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

ItemDelegate {
    id: root

    // SnapshotGroupModelから自動的に提供されるロール (required property)
    required property string displayId          // 表示用ID (単発は番号、Pre/Postは対応する番号)
    required property int groupType             // グループ種別 (数値)
    required property string groupTypeString    // グループ種別文字列 (single/prepost/pre/post)
    required property date startTime            // 開始日時
    required property date endTime              // 終了日時
    required property string description        // 説明
    required property string user               // 作成ユーザ
    required property var userdata              // ユーザデータマップ
    required property int preNumber             // Pre番号 (無い場合は0)
    required property int postNumber            // Post番号 (無い場合は0)
    required property string cleanupAlgoString  // クリーンアップアルゴリズム文字列
    required property bool isImportant          // 重要フラグ

    // ThemeManagerシングルトンへの参照を1か所に集約する (qmllintはqmlRegisterSingletonInstanceの型を解決できず、参照ごとに警告するため)
    readonly property var theme: ThemeManager

    property var listModel: null                // SnapshotListModelへの参照 (現状は未使用)
    property var detailDialog: null             // 詳細ダイアログへの参照
    property var columnWidths: null             // 親が管理する列幅オブジェクト
    property bool selected: false               // 選択状態 (親が保持する選択集合を反映する)
    signal selectionToggled()                   // 選択切り替え要求 (親が選択集合を更新する)

    height: 40                                  // 行の高さ

    contentItem: RowLayout {
        anchors.leftMargin: 5
        anchors.rightMargin: 5
        spacing: 0

        // 選択チェックボックス
        // 選択状態は親が所有するため、クリックはselectionToggledで親へ委譲する
        CheckBox {
            checked: root.selected
            onToggled: root.selectionToggled()
            Layout.preferredWidth: 40
            Layout.alignment: Qt.AlignVCenter
        }

        // ID列
        Label {
            textFormat: Text.PlainText  // スナップショット由来の文字列を、タグ等のリッチテキストとして解釈させない
            text: root.displayId
            font.pixelSize: 15
            Layout.preferredWidth: columnWidths ? columnWidths.idWidth : 80
            Layout.alignment: Qt.AlignVCenter
            elide: Text.ElideRight
            leftPadding: 5
        }

        // 種類列 (カラーインジケーター付き)
        RowLayout {
            spacing: 6
            Layout.preferredWidth: columnWidths ? columnWidths.typeWidth : 140
            Layout.maximumWidth: Layout.preferredWidth   // 親の伸縮に追従せず固定幅を維持する
            Layout.alignment: Qt.AlignVCenter

            // スナップショットタイプを示すカラーバー
            // groupTypeStringに応じてThemeManagerの種別色を切り替える
            Rectangle {
                width: 4
                height: 22
                radius: 2
                color: {
                    switch (root.groupTypeString) {
                    case "single":
                        return root.theme.snapshotTypeSingle
                    case "prepost":
                        return root.theme.snapshotTypePost
                    case "pre":
                        return root.theme.snapshotTypePre
                    case "post":
                        return root.theme.snapshotTypePost
                    default:
                        return root.theme.snapshotTypeDefault
                    }
                }
            }

            // 種別名をローカライズして表示する
            Label {
                textFormat: Text.PlainText  // スナップショット由来の文字列を、タグ等のリッチテキストとして解釈させない
                text: {
                    switch (root.groupTypeString) {
                    case "single":
                        return qsTr("Single")
                    case "prepost":
                        return qsTr("Pre and Post")
                    case "pre":
                        return qsTr("Pre")
                    case "post":
                        return qsTr("Post")
                    default:
                        return root.groupTypeString
                    }
                }
                font.pixelSize: 15
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
        }

        // 重要フラグ列
        // 重要なスナップショットだけにマーカーを描き、それ以外は空欄にして一覧での見分けを付けやすくする
        Item {
            Layout.preferredWidth: root.columnWidths ? root.columnWidths.importantWidth : 40
            Layout.maximumWidth: Layout.preferredWidth   // 親の伸縮に追従せず固定幅を維持する
            Layout.fillHeight: true

            // 重要色の円と感嘆符 (色だけに頼らず形でも伝える)
            Rectangle {
                visible: root.isImportant
                anchors.centerIn: parent
                width: 20
                height: 20
                radius: 10
                color: root.theme.importantColor

                Label {
                    anchors.centerIn: parent
                    text: "!"
                    font.bold: true
                    font.pixelSize: 14
                    // 重要色 (黄系) の上でLight/Darkとも読めるよう、暗い文字色に固定する
                    color: "#212121"
                }
            }

            HoverHandler {
                id: importantHover
                enabled: root.isImportant
            }

            ToolTip.visible: root.isImportant && importantHover.hovered
            ToolTip.text: qsTr("Important snapshot")
        }

        // 開始日列
        Label {
            textFormat: Text.PlainText  // スナップショット由来の文字列を、タグ等のリッチテキストとして解釈させない
            text: Qt.formatDateTime(root.startTime, "yyyy-MM-dd HH:mm:ss")
            font.pixelSize: 15
            Layout.preferredWidth: columnWidths ? columnWidths.dateWidth : 170
            Layout.alignment: Qt.AlignVCenter
            elide: Text.ElideRight
            leftPadding: 5
        }

        // 終了日列 (Post番号を持つグループのみ時刻を表示する)
        Label {
            textFormat: Text.PlainText  // スナップショット由来の文字列を、タグ等のリッチテキストとして解釈させない
            text: root.postNumber > 0 ? Qt.formatDateTime(root.endTime, "yyyy-MM-dd HH:mm:ss") : ""
            font.pixelSize: 15
            Layout.preferredWidth: columnWidths ? columnWidths.dateWidth : 170
            Layout.alignment: Qt.AlignVCenter
            elide: Text.ElideRight
            leftPadding: 5
        }

        // 説明列 (空の場合はプレースホルダを表示する)
        Label {
            textFormat: Text.PlainText  // スナップショット由来の文字列を、タグ等のリッチテキストとして解釈させない
            text: root.description || qsTr("(No description)")
            font.pixelSize: 15
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            elide: Text.ElideRight
            leftPadding: 5
        }

        // ユーザデータ列 (key=valueをカンマ区切りで1行に連結する)
        Label {
            textFormat: Text.PlainText  // スナップショット由来の文字列を、タグ等のリッチテキストとして解釈させない
            text: {
                if (!root.userdata || Object.keys(root.userdata).length === 0)
                    return ""
                var parts = []
                var keys = Object.keys(root.userdata)
                for (var i = 0; i < keys.length; i++) {
                    parts.push(keys[i] + "=" + root.userdata[keys[i]])
                }
                return parts.join(", ")
            }
            font.pixelSize: 14
            Layout.preferredWidth: columnWidths ? columnWidths.userdataWidth : 150
            Layout.alignment: Qt.AlignVCenter
            elide: Text.ElideRight
            leftPadding: 5
        }

        // 詳細ボタン
        Button {
            text: qsTr("Details")
            flat: true
            font.pixelSize: 14
            Layout.preferredWidth: 70
            Layout.alignment: Qt.AlignVCenter
            // 詳細ダイアログへ表示用のスナップショット情報を設定して開く
            // Pre/Postペアの場合はPost情報 (postSnapshot) も併せて渡す
            onClicked: {
                if (root.detailDialog) {
                    root.detailDialog.snapshot = {
                        number: root.preNumber,
                        snapshotType: root.groupType === 1 ? 1 : (root.groupTypeString === "single" ? 0 : 1),
                        snapshotTypeString: root.groupTypeString,
                        previousNumber: 0,
                        timestamp: root.startTime,
                        user: root.user,
                        cleanupAlgo: 0,
                        cleanupAlgoString: root.cleanupAlgoString,
                        description: root.description,
                        userdata: root.userdata
                    }
                    if (root.postNumber > 0) {
                        root.detailDialog.postSnapshot = {
                            number: root.postNumber,
                            timestamp: root.endTime
                        }
                    } else {
                        root.detailDialog.postSnapshot = null
                    }
                    root.detailDialog.open()
                }
            }
        }
    }

    // 行背景 (選択中は強調色、ホバー中は淡色、それ以外は透明)
    background: Rectangle {
        color: {
            if (root.selected) {
                return Qt.rgba(palette.highlight.r, palette.highlight.g, palette.highlight.b, 0.25)
            } else if (root.hovered) {
                return palette.midlight
            } else {
                return "transparent"
            }
        }

        // 行区切り線
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: palette.mid
        }
    }
}
