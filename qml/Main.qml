// qSnapper - メインアプリケーションウィンドウ
// Btrfs/Snapperファイルシステムのスナップショット管理ツール
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

ApplicationWindow {
    id: root
    width: 1280                    // デフォルトウィンドウ幅
    height: 900                    // デフォルトウィンドウ高さ
    minimumWidth: 1024             // 最小幅
    minimumHeight: 768             // 最小高さ
    visible: true
    title: qsTr("qSnapper - Snapshot Manager")

    // テーマに基づいてパレットを切り替え
    // ThemeManagerの設定に応じてライトモードまたはダークモードを適用
    palette: ThemeManager.isDark ? darkPalette : lightPalette

    // ライトモードカラーパレット定義
    Palette {
        id: lightPalette
        window: "#FFFFFF"
        windowText: "#000000"
        base: "#FFFFFF"
        text: "#000000"
        button: "#F5F5F5"
        buttonText: "#000000"
        highlight: "#2196F3"
        highlightedText: "#FFFFFF"
        light: "#FAFAFA"
        midlight: "#F5F5F5"
        mid: "#EEEEEE"
        dark: "#E0E0E0"
        shadow: "#BDBDBD"
        alternateBase: "#FAFAFA"
        toolTipBase: "#FFFFCC"
        toolTipText: "#000000"
        placeholderText: "#9E9E9E"
        link: "#2196F3"
        linkVisited: "#9C27B0"
    }

    // ダークモードカラーパレット定義
    Palette {
        id: darkPalette
        window: "#212121"
        windowText: "#FFFFFF"
        base: "#303030"
        text: "#FFFFFF"
        button: "#424242"
        buttonText: "#FFFFFF"
        highlight: "#42A5F5"
        highlightedText: "#000000"
        light: "#424242"
        midlight: "#383838"
        mid: "#2C2C2C"
        dark: "#1E1E1E"
        shadow: "#000000"
        alternateBase: "#2C2C2C"
        toolTipBase: "#616161"
        toolTipText: "#FFFFFF"
        placeholderText: "#757575"
        link: "#42A5F5"
        linkVisited: "#BA68C8"
    }

    // メインコンテンツ: スナップショット一覧ページ
    SnapshotListPage {
        anchors.fill: parent
    }
}
