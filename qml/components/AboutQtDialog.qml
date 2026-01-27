// Qtライセンス情報ダイアログ
// Qt Frameworkのライセンス情報を表示
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    title: qsTr("Qt License")
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
        contentHeight: aboutQtLayout.height

        ColumnLayout {
            id: aboutQtLayout
            width: parent.width
            spacing: 20

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 20
                Layout.bottomMargin: 20
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                spacing: 20

                // Qtロゴ画像
                Image {
                    id: qtLogo
                    source: "qrc:/QSnapper/icons/Qt.png"
                    fillMode: Image.PreserveAspectFit
                    Layout.alignment: Qt.AlignHCenter | Qt.AlignTop
                    Layout.preferredWidth: 200
                }

                // ライセンス情報カラム
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 20

                    // Qtの説明テキスト
                    Label {
                        text: qsTr("This software is developed with Qt 6.") + "\n\n" +
                              "Qt is a C++ toolkit for cross-platform application development." + "\n" +
                              "Qt provides single-source portability across all major desktop operating systems." + "\n" +
                              "It is also available for embedded Linux and other embedded and mobile operating systems." + "\n\n" +
                              "Qt is available under multiple licensing options designed to accommodate the needs of our various users." + "\n\n" +
                              "Qt licensed under our commercial license agreement is appropriate for development of proprietary/commercial software " +
                              "where you do not want to share any source code with third parties or otherwise cannot comply with the terms of GNU (L)GPL." + "\n\n" +
                              "Qt licensed under GNU (L)GPL is appropriate for the development of Qt applications provided you can comply with the terms and " +
                              "conditions of the respective licenses."
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    // ライセンス詳細リンク
                    Label {
                        text: qsTr("For more information, see <a href=\"https://www.qt.io/licensing/\">qt.io/licensing</a>.")
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

                    // 著作権情報
                    Label {
                        text: "Copyright (C) 2026 The Qt Company Ltd and other contributors." + "\n" +
                              "Qt and the Qt logo are trademarks of The Qt Company Ltd." + "\n" +
                              "Qt is The Qt Company Ltd product developed as an open source project."
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                    }

                    // Qt公式サイトリンク
                    Label {
                        text: qsTr("For more information, see <a href=\"https://www.qt.io/\">qt.io</a>.")
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
                }
            }
        }
    }
}
