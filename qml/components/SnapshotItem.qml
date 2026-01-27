// スナップショットリストアイテムコンポーネント
// 個別スナップショットの情報を表示するデリゲート
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

ItemDelegate {
    id: root

    // ListViewから自動的に提供されるプロパティ
    // FsSnapshotクラスのロールに対応
    required property int number                        // スナップショット番号
    required property int snapshotType                  // スナップショットタイプ (列挙型)
    required property int previousNumber                // 前のスナップショット番号 (Postの場合)
    required property date timestamp                    // 作成日時
    required property string user                       // 作成ユーザー
    required property int cleanupAlgo                   // クリーンアップアルゴリズム
    required property string description                // 説明
    required property string snapshotTypeString         // スナップショットタイプ文字列
    required property string cleanupAlgoString          // クリーンアップアルゴリズム文字列
    required property var userdata                      // ユーザーデータマップ

    property var listModel: null                        // リストモデルへの参照
    property var detailDialog: null                     // 詳細ダイアログへの参照
    property bool isImportant: userdata ? (userdata.important === "yes") : false  // 重要フラグ
    property bool selected: false                       // 選択状態
    signal selectionToggled()                           // 選択切り替えシグナル

    // コンテンツレイアウト
    contentItem: RowLayout {
        spacing: 15

        // 選択チェックボックス
        CheckBox {
            checked: root.selected
            onToggled: root.selectionToggled()
        }

        // スナップショットタイプを示す円形バッジ
        Rectangle {
            width: 50
            height: 50
            radius: 25
            color: {
                switch (snapshotTypeString) {
                case "single":
                    return ThemeManager.snapshotTypeSingle
                case "pre":
                    return ThemeManager.snapshotTypePre
                case "post":
                    return ThemeManager.snapshotTypePost
                default:
                    return ThemeManager.snapshotTypeDefault
                }
            }

            // スナップショット番号表示
            Label {
                anchors.centerIn: parent
                text: number
                font.bold: true
                font.pixelSize: root.selected ? 19 : 18
                color: "white"
            }
        }

        // スナップショット情報カラム
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 5

            // 説明と重要バッジ行
            RowLayout {
                spacing: 5
                Layout.fillWidth: true

                // 説明文表示
                Label {
                    text: description || qsTr("(No description)")
                    font.bold: true
                    font.pixelSize: root.selected ? 15 : 14
                    color: root.selected ? palette.windowText : palette.text
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                // 重要バッジ
                Rectangle {
                    visible: isImportant
                    width: 60
                    height: 20
                    radius: 3
                    color: ThemeManager.importantColor

                    Label {
                        anchors.centerIn: parent
                        text: qsTr("Important")
                        font.pixelSize: 10
                        font.bold: true
                        color: "#000000"
                    }
                }
            }

            // メタ情報行 (タイプ、ユーザー、前スナップショット)
            RowLayout {
                spacing: 10

                Label {
                    text: qsTr("Type:")
                    font.pixelSize: 12
                    color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.7)
                }

                Label {
                    text: {
                        switch (snapshotTypeString) {
                        case "single":
                            return qsTr("Single")
                        case "pre":
                            return qsTr("Pre")
                        case "post":
                            return qsTr("Post")
                        default:
                            return snapshotTypeString
                        }
                    }
                    font.pixelSize: 12
                    color: palette.text
                }

                Rectangle {
                    width: 1
                    height: 15
                    color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.4)
                }

                Label {
                    text: qsTr("User:")
                    font.pixelSize: 12
                    color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.7)
                }

                Label {
                    text: user || qsTr("Unknown")
                    font.pixelSize: 12
                    color: palette.text
                }

                Rectangle {
                    width: 1
                    height: 15
                    color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.4)
                    visible: previousNumber > 0
                }

                Label {
                    visible: previousNumber > 0
                    text: qsTr("Prev: #%1").arg(previousNumber)
                    font.pixelSize: 12
                    color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.7)
                }
            }

            // タイムスタンプ表示
            Label {
                text: {
                    var dateStr = Qt.formatDateTime(timestamp, "yyyy-MM-dd HH:mm:ss")
                    switch (snapshotTypeString) {
                    case "single":
                        return qsTr("Date: %1").arg(dateStr)
                    case "pre":
                        return qsTr("Start: %1").arg(dateStr)
                    case "post":
                        return qsTr("End: %1").arg(dateStr)
                    default:
                        return dateStr
                    }
                }
                font.pixelSize: 11
                color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.7)
            }

            // クリーンアップアルゴリズム表示
            Label {
                visible: cleanupAlgoString !== ""
                text: qsTr("Cleanup: %1").arg(cleanupAlgoString)
                font.pixelSize: 11
                color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.7)
            }

            // ユーザーデータタグ表示
            Flow {
                visible: userdata && Object.keys(userdata).length > 0
                Layout.fillWidth: true
                spacing: 5

                Repeater {
                    model: userdata ? Object.keys(userdata) : []

                    Rectangle {
                        width: userdataLabel.width + 10
                        height: 18
                        radius: 2
                        color: palette.alternateBase
                        border.color: palette.mid
                        border.width: 1

                        Label {
                            id: userdataLabel
                            anchors.centerIn: parent
                            text: modelData + "=" + userdata[modelData]
                            font.pixelSize: 10
                            color: palette.text
                        }
                    }
                }
            }
        }

        // 詳細表示ボタン
        Button {
            text: qsTr("Details")
            flat: true
            onClicked: {
                if (root.detailDialog) {
                    root.detailDialog.snapshot = Qt.binding(function() {
                        return {
                            number: root.number,
                            snapshotType: root.snapshotType,
                            snapshotTypeString: root.snapshotTypeString,
                            previousNumber: root.previousNumber,
                            timestamp: root.timestamp,
                            user: root.user,
                            cleanupAlgo: root.cleanupAlgo,
                            cleanupAlgoString: root.cleanupAlgoString,
                            description: root.description,
                            userdata: root.userdata
                        }
                    })
                    root.detailDialog.open()
                }
            }
        }
    }

    // アイテム背景 (選択状態、ホバー状態に応じて色変更)
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
        border.color: root.selected ? palette.highlight : "transparent"
        border.width: root.selected ? 2 : 0
        radius: 5
    }
}
