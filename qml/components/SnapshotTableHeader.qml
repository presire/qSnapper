/**
 * @file SnapshotTableHeader.qml
 * @brief スナップショットテーブルのヘッダ行
 *
 * SnapshotListPageのテーブル上部に表示する列見出し
 * 親から渡されたcolumnWidthsで各列幅を決め、行デリゲート (SnapshotTableRow) と列位置を揃える
 *
 * 入力 : columnWidths (親が管理する列幅オブジェクト、null時は既定幅を使用)
 * 出力 : なし
 * 依存 : なし (QSnapperのC++型は未使用)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    height: 36                                     // ヘッダ行の高さ
    color: palette.mid                             // テーマ連動の背景色

    // 列幅設定 (親から注入、未設定時は各列の既定値を使用する)
    property var columnWidths: null

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 5
        anchors.rightMargin: 5
        spacing: 0

        // チェックボックス列のスペーサー
        // SnapshotTableRowのCheckBox列と同じ幅を確保して列を揃える
        Item {
            Layout.preferredWidth: 40
        }

        // ID列
        Label {
            text: qsTr("ID")
            font.bold: true
            font.pixelSize: 15
            Layout.preferredWidth: columnWidths ? columnWidths.idWidth : 80
            horizontalAlignment: Text.AlignLeft
            leftPadding: 5
        }

        // 種類列
        Label {
            text: qsTr("Type")
            font.bold: true
            font.pixelSize: 15
            Layout.preferredWidth: columnWidths ? columnWidths.typeWidth : 140
            horizontalAlignment: Text.AlignLeft
            leftPadding: 5
        }

        // 開始日列
        Label {
            text: qsTr("Start Date")
            font.bold: true
            font.pixelSize: 15
            Layout.preferredWidth: columnWidths ? columnWidths.dateWidth : 170
            horizontalAlignment: Text.AlignLeft
            leftPadding: 5
        }

        // 終了日列
        Label {
            text: qsTr("End Date")
            font.bold: true
            font.pixelSize: 15
            Layout.preferredWidth: columnWidths ? columnWidths.dateWidth : 170
            horizontalAlignment: Text.AlignLeft
            leftPadding: 5
        }

        // 説明列 (残り幅を占有する)
        Label {
            text: qsTr("Description")
            font.bold: true
            font.pixelSize: 15
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            leftPadding: 5
        }

        // ユーザデータ列
        Label {
            text: qsTr("User Data")
            font.bold: true
            font.pixelSize: 15
            Layout.preferredWidth: columnWidths ? columnWidths.userdataWidth : 150
            horizontalAlignment: Text.AlignLeft
            leftPadding: 5
        }

        // 操作列のスペーサー
        // SnapshotTableRowの詳細ボタン列と同じ幅を確保して列を揃える
        Item {
            Layout.preferredWidth: 70
        }
    }

    // ヘッダ下端の区切り線
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: palette.dark
    }
}
