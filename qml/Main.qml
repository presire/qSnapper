/**
 * @file Main.qml
 * @brief qSnapperのメインアプリケーションウィンドウ
 *
 * Btrfs/Snapperスナップショット管理GUIのルートウィンドウ
 * 上部のTabBarで、スナップショット一覧 (SnapshotListPage)、設定画面 (SettingsPage)、
 * ファイルシステム情報 (FilesystemPage) を切り替える
 *
 * 入力 : なし
 * 出力 : なし
 * 依存 : ThemeManager (Light/Dark/Systemに応じたパレットの各色)、SnapshotListPage (スナップショット一覧)、
 *        SettingsPage (設定画面)、FilesystemPage (使用量とサブボリューム一覧)
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

    // ThemeManagerシングルトンへの参照 (qmllintはqmlRegisterSingletonInstanceの型を解決できず、参照ごとに警告するため、1か所に集約する)
    readonly property var theme: ThemeManager

    // 全ての色をThemeManagerから取得する (Light/Darkの値はC++側に集約し、QML側に二重定義を持たない)
    // ThemeManager::themeChangedでパレットの各ロールが再評価され、再起動なしにLight/Dark/Systemが切り替わる
    palette {
        window: theme.paletteWindow
        windowText: theme.paletteWindowText
        base: theme.paletteBase
        text: theme.paletteText
        button: theme.paletteButton
        buttonText: theme.paletteButtonText
        highlight: theme.paletteHighlight
        highlightedText: theme.paletteHighlightedText
        light: theme.paletteLight
        midlight: theme.paletteMidlight
        mid: theme.paletteMid
        dark: theme.paletteDark
        shadow: theme.paletteShadow
        alternateBase: theme.paletteAlternateBase
        toolTipBase: theme.paletteToolTipBase
        toolTipText: theme.paletteToolTipText
        placeholderText: theme.palettePlaceholderText
        link: theme.paletteLink
        linkVisited: theme.paletteLinkVisited

        // 無効状態 (グレーアウト) の色
        disabled {
            buttonText: theme.paletteDisabledButtonText
            windowText: theme.paletteDisabledWindowText
            text: theme.paletteDisabledText
            button: theme.paletteDisabledButton
            highlight: theme.paletteDisabledHighlight
            highlightedText: theme.paletteDisabledHighlightedText
        }
    }

    // 画面の切り替えタブ (スナップショット一覧 / 設定 / ファイルシステム)
    header: TabBar {
        id: tabBar

        TabButton { text: qsTr("Snapshots") }
        TabButton { text: qsTr("Settings") }
        TabButton { text: qsTr("Filesystem") }
    }

    // メインコンテンツ: タブの選択に応じて1つのページだけを表示する
    // 非選択のページはvisibleがfalseになる (設定画面とファイルシステム画面はこれを契機に特権サービスから読み込む)
    StackLayout {
        anchors.fill: parent
        currentIndex: tabBar.currentIndex

        SnapshotListPage { }

        SettingsPage { }

        FilesystemPage { }
    }
}
