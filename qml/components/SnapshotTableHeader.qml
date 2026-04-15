// スナップショットテーブルのヘッダ行
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    height: 36
    color: palette.mid

    // カラム幅設定 (親から注入)
    property var columnWidths: null

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 5
        anchors.rightMargin: 5
        spacing: 0

        // チェックボックス列のスペーサー
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

        // 説明列
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
        Item {
            Layout.preferredWidth: 70
        }
    }

    // 下線
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: palette.dark
    }
}
