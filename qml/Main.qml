/**
 * @file Main.qml
 * @brief qSnapperのメインアプリケーションウィンドウ
 *
 * Btrfs/Snapperスナップショット管理GUIのルートウィンドウ
 * SnapshotListPageを唯一の子として全画面に配置する
 *
 * 入力 : なし
 * 出力 : なし
 * 依存 : ThemeManager (Light/Darkの判定とパレット切り替え)、SnapshotListPage (メイン画面)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

ApplicationWindow {
    id: root

    width: 1280                    // 既定のウィンドウ幅
    height: 900                    // 既定のウィンドウ高さ
    minimumWidth: 1024             // 最小ウィンドウ幅
    minimumHeight: 768             // 最小ウィンドウ高さ
    visible: true                  // 起動時にウィンドウを表示
    title: qsTr("qSnapper - Snapshot Manager") // タイトルバーに表示するアプリ名

    // ThemeManager::isDarkに応じてLight/Darkいずれかのパレットを選択する
    palette: ThemeManager.isDark ? darkPalette : lightPalette

    // ライトモード用のカラーパレット定義 (ThemeManagerがLightのときに適用)
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
        // 無効状態 (グレーアウト) の色定義
        disabled {
            buttonText: "#BDBDBD"
            windowText: "#BDBDBD"
            text: "#BDBDBD"
            button: "#EEEEEE"
            highlight: "#BDBDBD"
            highlightedText: "#FFFFFF"
        }
    }

    // ダークモード用のカラーパレット定義 (ThemeManagerがDarkのときに適用)
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
        // 無効状態 (グレーアウト) の色定義
        disabled {
            buttonText: "#6E6E6E"
            windowText: "#6E6E6E"
            text: "#6E6E6E"
            button: "#383838"
            highlight: "#6E6E6E"
            highlightedText: "#1E1E1E"
        }
    }

    // メインコンテンツ: スナップショット一覧ページをウィンドウ全面に配置する
    SnapshotListPage {
        anchors.fill: parent
    }
}
