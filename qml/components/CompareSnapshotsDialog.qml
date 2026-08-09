// 任意の 2 スナップショット間のファイル差分を表示するダイアログ
// YaST snapper の create_comparison + get_files 相当
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root

    property string configName: "root"
    property int num1: 0
    property int num2: 0

    title: qsTr("Compare Snapshot #%1 ↔ #%2").arg(num1).arg(num2)
    modal: true
    standardButtons: Dialog.Close
    width: {
        if (!ApplicationWindow.window) return 900
        var calculated = ApplicationWindow.window.width * 0.75
        return Math.min(Math.max(calculated, 900), 1600)
    }
    height: {
        if (!ApplicationWindow.window) return 600
        var calculated = ApplicationWindow.window.height * 0.75
        return Math.min(Math.max(calculated, 600), 1000)
    }

    // 差分モデル
    FileChangeModel {
        id: compareModel
        configName: root.configName
    }

    onOpened: {
        if (num1 > 0 && num2 > 0) {
            compareModel.loadChangesBetween(num1, num2, true)   // flat=true: ListView 表示
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            text: compareModel.loading
                  ? qsTr("Loading comparison...")
                  : qsTr("Files that differ between snapshot #%1 and #%2").arg(num1).arg(num2)
            font.bold: true
        }

        // 差分ファイル一覧 (ツリー表示はフラットに省略)
        ListView {
            id: fileList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: compareModel
            delegate: ItemDelegate {
                width: fileList.width
                height: 28
                contentItem: RowLayout {
                    spacing: 10
                    Label {
                        text: model.statusFlags || ""
                        font.family: "monospace"
                        Layout.preferredWidth: 60
                    }
                    Label {
                        text: model.filePath || ""
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                    }
                }
            }
            ScrollBar.vertical: ScrollBar {}

            Label {
                anchors.centerIn: parent
                visible: !compareModel.loading && !compareModel.hasChanges
                text: qsTr("No differences found")
                color: palette.placeholderText
            }
        }
    }
}
