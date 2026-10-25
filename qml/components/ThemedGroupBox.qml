/**
 * @file ThemedGroupBox.qml
 * @brief Light/Darkのどちらでも枠と内側が見分けられるGroupBox派生コンポーネント
 *
 * Fusionスタイル既定のGroupBoxは、枠をウィンドウ背景より暗い色、内側をほぼ透明な黒で描画する
 * ダークモードではウィンドウ背景自体が暗いため、枠も内側も背景に埋もれてグループの範囲が分からなくなる
 * 本コンポーネントは背景だけをThemeManagerの色で描画し直し、タイトルの位置と余白はFusion既定のままにする
 * ライトモードの色はFusion既定の描画結果と同じ値にしているため、見た目は変わらない
 *
 * 入力 : title、およびGroupBoxの標準プロパティ
 * 出力 : なし
 * 依存 : ThemeManager (groupBoxBackground / groupBoxBorder)
 */
import QtQuick
import QtQuick.Controls
import QSnapper 1.0

GroupBox {
    id: root

    // ThemeManagerシングルトンへの参照を1か所に集約する (qmllintはqmlRegisterSingletonInstanceの型を解決できず、参照ごとに警告するため)
    // qmllint disable unqualified
    readonly property var themeManager: ThemeManager
    // qmllint enable unqualified

    // 枠と内側の背景
    // 位置と大きさはFusionのGroupBoxと同じ (タイトルの下から始め、タイトルの領域は塗らない)
    background: Rectangle {
        y: root.topPadding - root.bottomPadding
        width: parent.width
        height: parent.height - root.topPadding + root.bottomPadding

        radius: 2
        color: root.themeManager.groupBoxBackground
        border.color: root.themeManager.groupBoxBorder
    }
}
