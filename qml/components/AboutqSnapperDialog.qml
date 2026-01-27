// qSnapperについてダイアログ
// アプリケーション情報、バージョン、ライセンス、リンク情報を表示
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    title: qsTr("About qSnapper")
    modal: true
    width: {
        if (!ApplicationWindow.window) return 800
        var calculated = ApplicationWindow.window.width * 0.7
        return Math.min(Math.max(calculated, 800), 1000)
    }
    height: {
        if (!ApplicationWindow.window) return 600
        var calculated = ApplicationWindow.window.height * 0.7
        return Math.min(Math.max(calculated, 600), 800)
    }
    anchors.centerIn: Overlay.overlay
    standardButtons: Dialog.Close

    ScrollView {
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        contentHeight: aboutqSnapperLayout.height

        ColumnLayout {
            id: aboutqSnapperLayout
            width: parent.width
            spacing: 20

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 20
                Layout.bottomMargin: 20
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                spacing: 20

                // qSnapperロゴ画像
                Image {
                    id: qSnapperLogo
                    source: "qrc:/QSnapper/icons/qSnapper.png"
                    fillMode: Image.PreserveAspectFit
                    Layout.alignment: Qt.AlignHCenter | Qt.AlignTop
                    Layout.preferredWidth: 200
                }

                // アプリケーション情報カラム
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 20

                    // アプリケーション名とバージョン
                    Label {
                        text: "qSnapper " + Qt.application.version
                        font.bold: true
                        font.pixelSize: 20
                        Layout.fillWidth: true
                    }

                    // アプリケーション説明
                    Label {
                        text: qsTr("A Qt application for managing Btrfs/Snapper filesystem snapshots on Linux.")
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    // 著作権情報
                    Label {
                        text: "Copyright (C) 2026 Presire"
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                    }

                    // ライセンス情報
                    Label {
                        text: qsTr("This program is licensed under the GNU General Public License v3.0 or later.")
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                    }

                    // GPLライセンスリンク
                    Label {
                        text: qsTr("For more information, see <a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">gnu.org/licenses/gpl-3.0</a>.")
                        textFormat: Text.RichText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true

                        onLinkActivated: function(link) {
                            Qt.openUrlExternally(link)
                        }

                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }
                    }

                    // GitHubリポジトリリンク
                    Label {
                        text: qsTr("GitHub: <a href=\"https://github.com/presire/qSnapper\">github.com/presire/qSnapper</a>")
                        textFormat: Text.RichText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        Layout.topMargin: 10

                        onLinkActivated: function(link) {
                            Qt.openUrlExternally(link)
                        }

                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }
                    }

                    // Issuesページリンク
                    Label {
                        text: qsTr("Issues: <a href=\"https://github.com/presire/qSnapper/issues\">github.com/presire/qSnapper/issues</a>")
                        textFormat: Text.RichText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true

                        onLinkActivated: function(link) {
                            Qt.openUrlExternally(link)
                        }

                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }
                    }
                }
            }
        }
    }
}
