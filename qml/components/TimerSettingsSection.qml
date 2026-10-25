/**
 * @file TimerSettingsSection.qml
 * @brief snapperのsystemdタイマの状態表示と有効化 / 無効化を行うセクション
 *
 * タイマはすべてのsnapper設定で共有されるため、選択中の設定には依存しない
 * 起動時の有効状態 (UnitFileState) と現在の稼働状態 (ActiveState) は別の意味なので、別々に表示する
 * SettingsPageの設定項目の下に配置する
 *
 * 入力 : settings (タイマ状態を提供するSnapperSettingsModel)
 * 出力 : なし
 * 依存 : SnapperSettingsModel (timers / refreshTimers / setTimerEnabled と各結果シグナル)、ThemedGroupBox (外枠)、ThemeManager (成功 / エラー色)
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

ThemedGroupBox {
    id: root

    required property var settings              // タイマ状態を提供するSnapperSettingsModel

    property string resultText: ""              // 直近の操作結果の文言
    property bool resultIsError: false          // 直近の操作結果が失敗か

    title: qsTr("Systemd timers")

    /**
     * @brief ユニット名を利用者向けの説明へ変換する
     * @param unit systemdのユニット名
     * @return タイマの役割の説明、未知のユニットはユニット名そのもの
     */
    function describe(unit) {
        switch (unit) {
        case "snapper-timeline.timer":
            return qsTr("Timeline snapshots (hourly)")
        case "snapper-cleanup.timer":
            return qsTr("Snapshot cleanup (daily)")
        case "snapper-boot.timer":
            return qsTr("Snapshot at boot")
        default:
            return unit
        }
    }

    /**
     * @brief UnitFileStateから有効化の操作が可能かを判定する
     * @param state systemdのUnitFileState
     * @return enabledまたはdisabledの場合はtrue
     * @note masked、static、alias、not-foundなどはsystemctl enable / disableでは状態を変えられないため操作を提供しない
     *       enabled-runtimeは、特権サービスが永続的な有効化と無効化だけを行うため、操作しても状態が一致せず失敗となるので提供しない
     */
    function isToggleable(state) {
        return state === "enabled" || state === "disabled"
    }

    /**
     * @brief UnitFileStateが起動時に有効な状態かを判定する
     * @param state systemdのUnitFileState
     * @return enabledまたはenabled-runtimeの場合はtrue
     */
    function isEnabledState(state) {
        return state === "enabled" || state === "enabled-runtime"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        // タイマは全設定で共有されることを明示する (設定ごとの切り替えと誤解されないようにするため)
        Label {
            text: qsTr("These timers are shared by all snapper configs. Changing them affects every config, not only the selected one.")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: ThemeManager.warningColor
            Layout.fillWidth: true
        }

        Repeater {
            model: root.settings.timers

            // 1つのタイマの状態と操作ボタン
            delegate: RowLayout {
                id: timerRow

                required property var modelData     // unit / unitFileState / activeState を持つマップ

                readonly property bool available: modelData.unitFileState !== "not-found"      // ユニットが存在するか
                readonly property bool toggleable: root.isToggleable(modelData.unitFileState)  // 操作を提供できる状態か
                readonly property bool enabledAtBoot: root.isEnabledState(modelData.unitFileState) // 起動時に有効か

                spacing: 12
                Layout.fillWidth: true

                ColumnLayout {
                    spacing: 2
                    Layout.preferredWidth: 280

                    Label {
                        text: root.describe(timerRow.modelData.unit)
                        textFormat: Text.PlainText
                        font.bold: true
                    }
                    Label {
                        text: timerRow.modelData.unit
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        color: palette.placeholderText
                    }
                }

                // 起動時の有効状態と現在の稼働状態を別々に示す
                ColumnLayout {
                    spacing: 2
                    Layout.fillWidth: true

                    Label {
                        text: timerRow.available
                              ? qsTr("Autostart (unit file state): %1").arg(timerRow.modelData.unitFileState)
                              : qsTr("Not available on this system")
                        textFormat: Text.PlainText
                        color: timerRow.available ? palette.text : palette.placeholderText
                    }
                    Label {
                        // 存在しないユニットの稼働状態は意味が無いため表示しない
                        visible: timerRow.available
                        text: qsTr("Current state (active state): %1").arg(timerRow.modelData.activeState)
                        textFormat: Text.PlainText
                    }
                    Label {
                        // masked、staticなどは本画面から変更できないことを説明する
                        visible: timerRow.available && !timerRow.toggleable
                        text: qsTr("This state cannot be changed here.")
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        color: palette.placeholderText
                    }
                }

                // 有効化 (有効化して起動) と無効化 (停止して無効化) の切り替え
                // 存在しないユニットと変更できない状態では表示しない
                // 処理中は多重実行と状態の取り違えを防ぐため無効にする
                Button {
                    visible: timerRow.available && timerRow.toggleable
                    enabled: !root.settings.timersBusy
                    // Main.qmlがパレットの色をバインディングで指定しているため、Fusionスタイルは無効状態でも文字色を灰色に切り替えない
                    // そのため、無効状態であることは不透明度を下げて示す
                    opacity: enabled ? 1.0 : 0.45
                    text: timerRow.enabledAtBoot ? qsTr("Disable and stop") : qsTr("Enable and start")
                    // SnapperSettingsModel::setTimerEnabledを呼び出し、特権サービスのSetTimerEnabledで切り替える
                    // 完了はtimerChangeSucceeded / timerChangeFailedシグナルで受け取る
                    onClicked: root.settings.setTimerEnabled(timerRow.modelData.unit, !timerRow.enabledAtBoot)
                }
            }
        }

        // 取得に失敗してタイマの一覧が空の場合の表示
        Label {
            visible: root.settings.timers.length === 0 && !root.settings.timersBusy
            text: qsTr("Timer states are not available.")
            textFormat: Text.PlainText
            color: palette.placeholderText
        }

        RowLayout {
            spacing: 8
            Layout.fillWidth: true

            // 操作と取得の実行中は、結果が確定するまで操作できないことを示す
            BusyIndicator {
                running: root.settings.timersBusy
                visible: running
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
            }

            Label {
                text: root.resultText
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: root.resultIsError ? ThemeManager.errorColor : ThemeManager.successColor
                Layout.fillWidth: true
            }

            Button {
                text: qsTr("Refresh")
                enabled: !root.settings.timersBusy
                // Main.qmlがパレットの色をバインディングで指定しているため、Fusionスタイルは無効状態でも文字色を灰色に切り替えない
                // そのため、無効状態であることは不透明度を下げて示す
                opacity: enabled ? 1.0 : 0.45
                onClicked: root.settings.refreshTimers()
            }
        }
    }

    // タイマ操作の結果を画面に表示する (成功と失敗の両方)
    Connections {
        target: root.settings

        function onTimerChangeSucceeded(unit, enabled) {
            root.resultIsError = false
            root.resultText = enabled
                ? qsTr("Enabled and started %1.").arg(unit)
                : qsTr("Disabled and stopped %1.").arg(unit)
        }

        function onTimerChangeFailed(unit, error) {
            root.resultIsError = true
            root.resultText = qsTr("Failed to change %1: %2").arg(unit).arg(error)
        }

        function onTimersLoadFailed(error) {
            root.resultIsError = true
            root.resultText = qsTr("Failed to get timer states: %1").arg(error)
        }
    }
}
