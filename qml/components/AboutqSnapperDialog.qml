/**
 * @file AboutqSnapperDialog.qml
 * @brief qSnapperのアプリケーション情報を表示するダイアログ
 *
 * バージョン、アプリ説明、著作権、ライセンス、GitHub / Issuesへのリンクを表示する
 *
 * 入力 : なし
 * 出力 : なし
 * 依存 : BorderedDialog (基底)、SnapperService::openExternalUrl (リンクを外部ブラウザで開く)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

BorderedDialog {
    id: root
    title: qsTr("About qSnapper")                  // タイトルバーの表示名
    modal: true                                    // 背後への操作をブロックする
    // ウィンドウ幅の70%を基準に、800から1000の範囲へクランプする
    width: {
        if (!ApplicationWindow.window) return 800
        var calculated = ApplicationWindow.window.width * 0.7
        return Math.min(Math.max(calculated, 800), 1000)
    }
    // ウィンドウ高さの70%を基準に、600から800の範囲へクランプする
    height: {
        if (!ApplicationWindow.window) return 600
        var calculated = ApplicationWindow.window.height * 0.7
        return Math.min(Math.max(calculated, 600), 800)
    }
    anchors.centerIn: Overlay.overlay              // 常にオーバーレイ中央へ配置する
    standardButtons: Dialog.Close                  // 閉じるボタンのみ表示する

    // 内容が収まらない場合にスクロール可能にする
    ScrollView {
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        contentHeight: aboutqSnapperLayout.height

        ColumnLayout {
            id: aboutqSnapperLayout
            width: parent.width
            spacing: 20

            // ロゴとアプリ情報を左右に並べる
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
                    source: "qrc:/QSnapper/icons/qSnapper@256.png"
                    fillMode: Image.PreserveAspectFit
                    Layout.alignment: Qt.AlignHCenter | Qt.AlignTop
                    Layout.preferredWidth: 200
                }

                // アプリケーション情報カラム
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 20

                    // アプリケーション名とバージョン (Qt.application.versionを併記する)
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
                        text: qsTr("This program is licensed under the GNU General Public License v2.0 or later.")
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                    }

                    // GPLライセンスリンク
                    Label {
                        text: qsTr("For more information, see <a href=\"https://www.gnu.org/licenses/old-licenses/gpl-2.0.html\">gnu.org/licenses/gpl-2.0</a>.")
                        textFormat: Text.RichText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true

                        // リンククリック時に外部ブラウザでURLを開く
                        onLinkActivated: function(link) {
                            SnapperService.openExternalUrl(link)
                        }

                        // リンク上ではポインタカーソルを表示する
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

                        // リンククリック時に外部ブラウザでURLを開く
                        onLinkActivated: function(link) {
                            SnapperService.openExternalUrl(link)
                        }

                        // リンク上ではポインタカーソルを表示する
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

                        // リンククリック時に外部ブラウザでURLを開く
                        onLinkActivated: function(link) {
                            SnapperService.openExternalUrl(link)
                        }

                        // リンク上ではポインタカーソルを表示する
                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }
                    }
                }
            }
        }
    }
}
