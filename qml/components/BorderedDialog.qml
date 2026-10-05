/**
 * @file BorderedDialog.qml
 * @brief 四方に枠を付与するDialog派生コンポーネント
 *
 * 全ダイアログの視認性を確保するため、四方の境界に枠を描画する
 * アプリ内の各ダイアログは本コンポーネントを継承して使用する
 *
 * 入力 : title (空ならヘッダを非表示)、standardButtons (フッタの標準ボタン構成)
 * 出力 : なし (accepted / rejectedはDialog標準シグナル)
 * 依存 : ThemeManager::dialogBorderColor (Light/Dark連動の枠色)
 */
import QtQuick
import QtQuick.Controls
import QSnapper 1.0

Dialog {
    id: root

    // 四方の境界に視認性の高い枠を描画する
    // border.color: テーマ連動 (Light=#9E9E9E / Dark=#616161)
    // border.width: 2 (太枠で視認性を強調)
    // radius: 4 (角丸で現代的なルック)
    background: Rectangle {
        color: palette.window
        border.color: ThemeManager.dialogBorderColor
        border.width: 2
        radius: 4
    }

    // Fusion既定のヘッダ背景が上枠を覆わないよう、透明なヘッダに置き換える
    header: Label {
        text: root.title
        visible: root.title.length > 0
        elide: Text.ElideRight
        font.bold: true
        padding: 6
        background: Item { }
    }

    // 標準ボタンの機能を維持しつつ、フッタ背景による下枠の被覆を防ぐ
    footer: DialogButtonBox {
        visible: root.standardButtons !== Dialog.NoButton
        background: Item { }
    }
}
