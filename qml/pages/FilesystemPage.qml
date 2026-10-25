/**
 * @file FilesystemPage.qml
 * @brief Btrfsの使用量とサブボリューム一覧を表示する読み取り専用の画面
 *
 * Main.qmlのStackLayoutで3番目のタブとして表示する
 * 使用量はタブを開いたときと[Refresh]で取得し、サブボリューム一覧は認証を伴うため[Load subvolumes]を押したときだけ取得する
 * 表示専用であり、サブボリュームの作成・削除・読み取り専用の切り替えは行わない
 *
 * 入力 : なし (対象の設定名はSnapperService.currentConfigを参照する)
 * 出力 : なし
 * 依存 : SnapperService (現在の設定名)、FilesystemInfoModel (使用量とサブボリューム一覧、
 *        usageLoadFailed / subvolumesLoadFailed)、ThemedGroupBox (情報のグループ枠)、ThemeManager (警告色とエラー色)
 */
// 各Repeaterのdelegateからページ全体のid(root)を参照するため、コンポーネントのスコープを外側のファイルに束縛する
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

Page {
    id: root

    property string errorText: ""           // 直近の取得失敗の理由

    /**
     * @brief バイト数をKiB / MiB / GiBなどの読みやすい単位へ変換する
     * @param bytes 変換するバイト数
     * @return 単位付きの文字列、不正な値の場合はダッシュ
     * @note 1024未満はバイトのまま表示し、桁数に応じて小数の位置を変えて読みやすさを保つ
     */
    function formatSize(bytes) {
        if (!isFinite(bytes) || bytes < 0) {
            return "—"
        }
        var units = ["B", "KiB", "MiB", "GiB", "TiB", "PiB"]
        var value = bytes
        var index = 0
        while (value >= 1024 && index < units.length - 1) {
            value /= 1024
            index++
        }
        if (index === 0) {
            return value + " " + units[index]
        }
        var digits = value >= 100 ? 0 : (value >= 10 ? 1 : 2)
        return value.toFixed(digits) + " " + units[index]
    }

    /**
     * @brief エポック秒から変換された日時を表示用の文字列にする
     * @param created サーバが返した作成日時
     * @return 書式化した日時、無効な場合はダッシュ
     */
    function formatCreated(created) {
        if (!created || !created.isValid) {
            return "—"
        }
        return Qt.formatDateTime(created, "yyyy-MM-dd HH:mm:ss")
    }

    /**
     * @brief 取得失敗の理由を画面へ表示する
     * @param message 表示する文言
     */
    function showError(message) {
        root.errorText = message
    }

    // 使用量とサブボリューム一覧を扱うViewModel
    // 設定名は一覧画面と設定画面が共有するSnapperService.currentConfigに追従する
    FilesystemInfoModel {
        id: filesystemModel
        configName: root.service.currentConfig
    }

    // 表示で使うFilesystemInfoModelへの参照
    // qmllintはC++側で登録された型のメンバを解決できず、参照ごとに警告するため、var経由で参照して警告の増加を避ける
    readonly property var fs: filesystemModel

    // 配色の参照
    // qmllintがシングルトンを解決できないため、var経由にして警告の増加を避ける
    readonly property var theme: ThemeManager

    // 現在の設定名の参照
    // qmllintがシングルトンを解決できないため、var経由にして警告の増加を避ける
    readonly property var service: SnapperService

    // タブが表示されたときに使用量を取得する
    // 起動直後は非表示のため、特権サービスへの問い合わせを発生させない
    onVisibleChanged: {
        if (visible) {
            root.fs.refreshUsage()
        }
    }

    // 設定が切り替わったときは、表示中であれば新しい設定の使用量を取得する
    // 非表示の間はタブを開いたときのonVisibleChangedに任せる
    Connections {
        target: filesystemModel

        function onConfigNameChanged() {
            root.errorText = ""
            if (root.visible) {
                root.fs.refreshUsage()
            }
        }

        function onUsageLoadFailed(error) {
            root.showError(qsTr("Failed to load filesystem usage: %1").arg(error))
        }

        function onSubvolumesLoadFailed(error) {
            root.showError(qsTr("Failed to load subvolumes: %1").arg(error))
        }
    }

    // 設定の選択と使用量の再取得を行うツールバー
    header: ToolBar {
        topPadding: 8   // FusionのToolBarは上側の余白が0で、上部のTabBarと重なって見えるため下へずらす

        RowLayout {
            anchors.fill: parent
            spacing: 8

            // 対象の設定名を表示するラベル
            Label {
                textFormat: Text.PlainText  // 設定名は外部由来の文字列のため、リッチテキストとして解釈させない
                text: qsTr("Filesystem: %1").arg(root.service.currentConfig || qsTr("(none)"))
                font.pixelSize: 14
                font.bold: true
                Layout.leftMargin: 10
            }

            // 使用量の取得中であることを示す
            BusyIndicator {
                running: root.fs.usageBusy
                visible: running
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
            }

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Refresh")
                icon.name: "view-refresh"
                icon.color: root.theme.isDark ? "#FFFFFF" : palette.buttonText
                // 設定が無い場合と取得中は、多重の問い合わせを避けるため無効にする
                enabled: root.service.currentConfig !== "" && !root.fs.usageBusy
                // Main.qmlがパレットの色をバインディングで指定しているため、Fusionスタイルは無効状態でも文字色を灰色に切り替えない
                // そのため、無効状態であることは不透明度を下げて示す
                opacity: enabled ? 1.0 : 0.45
                Layout.rightMargin: 10
                // FilesystemInfoModel::refreshUsageを呼び出し、完了はusageLoadFailedで失敗だけを受け取る
                onClicked: root.fs.refreshUsage()
            }
        }
    }

    // 取得失敗の理由を示す領域
    // 失敗したまま古い値を実際の状態と誤解されないよう、内容の先頭に表示する
    Rectangle {
        id: errorBanner
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.errorText.length > 0 ? errorLabel.implicitHeight + 16 : 0
        visible: root.errorText.length > 0
        // 背景だけを薄くするため、色のアルファ値で透過させる
        // Rectangleのopacityは子のLabelにも掛かり、文字まで薄くなって読めなくなるため使わない
        color: Qt.rgba(root.theme.errorColor.r, root.theme.errorColor.g, root.theme.errorColor.b, 0.15)

        Label {
            id: errorLabel
            anchors.fill: parent
            anchors.margins: 8
            text: root.errorText
            textFormat: Text.PlainText
            // エラー色のままでは赤みのある背景との差が小さく読みにくいため、
            // ライトでは暗く、ダークでは明るくしてコントラスト比を4.5以上にする
            color: root.theme.isDark ? Qt.lighter(root.theme.errorColor, 1.4) : Qt.darker(root.theme.errorColor, 1.6)
            wrapMode: Text.Wrap
        }
    }

    ScrollView {
        anchors.fill: parent
        anchors.topMargin: errorBanner.height
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: 18

            // 設定が無い場合は何も取得できないため、案内だけを表示する
            Label {
                Layout.fillWidth: true
                Layout.margins: 15
                Layout.alignment: Qt.AlignTop
                visible: root.service.currentConfig === ""
                text: qsTr("No configuration is selected. Select a configuration on the Settings tab.")
                wrapMode: Text.Wrap
            }

            // 使用量の概要
            // 数値の意味を誤解しないよう、btrfs filesystem usageとの違いを同じ場所に書く
            ThemedGroupBox {
                Layout.fillWidth: true
                Layout.leftMargin: 15
                Layout.rightMargin: 15
                title: qsTr("Overview")
                visible: root.service.currentConfig !== ""

                ColumnLayout {
                    width: parent.width
                    spacing: 6

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        text: qsTr("These values come from the Btrfs space information and can differ from the output of btrfs filesystem usage.")
                    }

                    // デバイス全体の容量と、チャンクへ予約済みの容量を並べる
                    GridLayout {
                        columns: 2
                        columnSpacing: 20
                        rowSpacing: 4

                        Label { text: qsTr("Size:") }
                        // デバイスの合計サイズを表示するラベル
                        Label {
                            text: root.formatSize(root.fs.deviceTotal)  // デバイスの合計サイズ
                            textFormat: Text.PlainText                  // 外部由来の値を、リッチテキストとして解釈させない
                        }

                        Label { text: qsTr("Allocated:") }
                        // 割り当て済みのサイズを表示するラベル
                        Label {
                            text: root.formatSize(root.fs.deviceAllocated)  // 割り当て済みのサイズ
                            textFormat: Text.PlainText                      // 外部由来の値を、リッチテキストとして解釈させない
                        }

                        Label { text: qsTr("Unallocated:") }
                        // 未割り当てのサイズを表示するラベル (合計から割り当て済みを引いて求める)
                        Label {
                            text: root.formatSize(root.fs.deviceTotal - root.fs.deviceAllocated)  // 未割り当てのサイズ
                            textFormat: Text.PlainText                                            // 外部由来の値を、リッチテキストとして解釈させない
                        }

                        Label { text: qsTr("Node size:") }
                        // ノードサイズを表示するラベル
                        Label {
                            text: root.formatSize(root.fs.nodesize)  // ノードサイズ
                            textFormat: Text.PlainText               // 外部由来の値を、リッチテキストとして解釈させない
                        }

                        Label { text: qsTr("Sector size:") }
                        // セクタサイズを表示するラベル
                        Label {
                            text: root.formatSize(root.fs.sectorsize)  // セクタサイズ
                            textFormat: Text.PlainText                 // 外部由来の値を、リッチテキストとして解釈させない
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        text: qsTr("Size is the total size of all devices. Allocated is the space reserved for chunks on the devices. Unallocated = Size - Allocated.")
                    }

                    // 使用量をまだ取得できていない場合の案内
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        visible: !root.fs.usageLoaded && !root.fs.usageBusy
                        text: qsTr("No usage data has been loaded yet.")
                    }
                }
            }

            // 種別ごとのチャンクの割り当て容量と使用容量
            ThemedGroupBox {
                Layout.fillWidth: true
                Layout.leftMargin: 15
                Layout.rightMargin: 15
                title: qsTr("Space by chunk type")
                visible: root.service.currentConfig !== ""

                ColumnLayout {
                    width: parent.width
                    spacing: 4

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        text: qsTr("Total is the space allocated to chunks of this type. Used is the space actually used inside those chunks. The global reserve is part of metadata.")
                    }

                    // 表の見出し
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 6

                        Label { text: qsTr("Type"); Layout.preferredWidth: 160; font.bold: true }
                        Label { text: qsTr("Profile"); Layout.preferredWidth: 100; font.bold: true }
                        Label { text: qsTr("Total"); Layout.preferredWidth: 110; font.bold: true; horizontalAlignment: Text.AlignRight }
                        Label { text: qsTr("Used"); Layout.preferredWidth: 110; font.bold: true; horizontalAlignment: Text.AlignRight }
                        Item { Layout.fillWidth: true }
                    }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: palette.mid }

                    // サーバが返したspace行をそのまま1行ずつ表示する
                    Repeater {
                        model: root.fs.usage

                        RowLayout {
                            id: spaceRow
                            required property var modelData     // space行の種類・プロファイル・割当量・使用量を持つマップ

                            Layout.fillWidth: true

                            // 割り当ての種別名を表示するラベル
                            Label {
                                Layout.preferredWidth: 160          // 列の幅
                                text: spaceRow.modelData.typeLabel  // 割り当ての種別名
                                textFormat: Text.PlainText          // 外部由来の値を、リッチテキストとして解釈させない
                                elide: Text.ElideRight              // 幅を超える部分を省略表示する
                            }

                            // RAIDなどのプロファイル名を表示するラベル
                            Label {
                                Layout.preferredWidth: 100             // 列の幅
                                text: spaceRow.modelData.profileLabel  // RAIDなどのプロファイル名
                                textFormat: Text.PlainText             // 外部由来の値を、リッチテキストとして解釈させない
                                elide: Text.ElideRight                 // 幅を超える部分を省略表示する
                            }

                            // 割り当て済みの合計サイズを表示するラベル
                            Label {
                                Layout.preferredWidth: 110                       // 列の幅
                                text: root.formatSize(spaceRow.modelData.total)  // 割り当て済みの合計サイズ
                                textFormat: Text.PlainText                       // 外部由来の値を、リッチテキストとして解釈させない
                                horizontalAlignment: Text.AlignRight             // 数値を右揃えにする
                            }

                            // 使用中のサイズを表示するラベル
                            Label {
                                Layout.preferredWidth: 110                      // 列の幅
                                text: root.formatSize(spaceRow.modelData.used)  // 使用中のサイズ
                                textFormat: Text.PlainText                      // 外部由来の値を、リッチテキストとして解釈させない
                                horizontalAlignment: Text.AlignRight            // 数値を右揃えにする
                            }

                            Item { Layout.fillWidth: true }
                        }
                    }
                }
            }

            // デバイスごとの容量と割り当て済み容量
            ThemedGroupBox {
                Layout.fillWidth: true
                Layout.leftMargin: 15
                Layout.rightMargin: 15
                title: qsTr("Devices")
                visible: root.service.currentConfig !== "" && root.fs.devices.length > 0

                ColumnLayout {
                    width: parent.width
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true

                        Label { text: qsTr("Device ID"); Layout.preferredWidth: 100; font.bold: true }
                        Label { text: qsTr("Size"); Layout.preferredWidth: 120; font.bold: true; horizontalAlignment: Text.AlignRight }
                        Label { text: qsTr("Allocated"); Layout.preferredWidth: 120; font.bold: true; horizontalAlignment: Text.AlignRight }
                        Item { Layout.fillWidth: true }
                    }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: palette.mid }

                    // デバイスごとの行
                    Repeater {
                        model: root.fs.devices

                        RowLayout {
                            id: deviceRow
                            required property var modelData     // デバイスID・サイズ・割当量を持つマップ

                            Layout.fillWidth: true

                            // デバイスIDを表示するラベル
                            Label {
                                Layout.preferredWidth: 100       // 列の幅
                                text: deviceRow.modelData.devid  // デバイスID
                                textFormat: Text.PlainText       // 外部由来の値を、リッチテキストとして解釈させない
                            }

                            // デバイスのサイズを表示するラベル
                            Label {
                                Layout.preferredWidth: 120                       // 列の幅
                                text: root.formatSize(deviceRow.modelData.size)  // デバイスのサイズ
                                textFormat: Text.PlainText                       // 外部由来の値を、リッチテキストとして解釈させない
                                horizontalAlignment: Text.AlignRight             // 数値を右揃えにする
                            }

                            // デバイスの割り当て済みサイズを表示するラベル
                            Label {
                                Layout.preferredWidth: 120                            // 列の幅
                                text: root.formatSize(deviceRow.modelData.allocated)  // デバイスの割り当て済みサイズ
                                textFormat: Text.PlainText                            // 外部由来の値を、リッチテキストとして解釈させない
                                horizontalAlignment: Text.AlignRight                  // 数値を右揃えにする
                            }

                            Item { Layout.fillWidth: true }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        text: qsTr("Allocated is the space reserved for chunks on this device.")
                    }
                }
            }

            // サブボリューム一覧
            // 取得には管理者認証が必要なため、自動では読み込まず、ボタンで明示的に要求させる
            ThemedGroupBox {
                Layout.fillWidth: true
                Layout.leftMargin: 15
                Layout.rightMargin: 15
                Layout.bottomMargin: 15
                title: qsTr("Subvolumes")
                visible: root.service.currentConfig !== ""

                ColumnLayout {
                    width: parent.width
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        text: qsTr("Loading the subvolume list requires administrator authentication. This page only displays the list.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Button {
                            text: qsTr("Load subvolumes")
                            // 取得中は多重の認証要求を避けるため無効にする
                            enabled: !root.fs.subvolumesBusy
                            // Main.qmlがパレットの色をバインディングで指定しているため、Fusionスタイルは無効状態でも文字色を灰色に切り替えない
                            // そのため、無効状態であることは不透明度を下げて示す
                            opacity: enabled ? 1.0 : 0.45
                            // FilesystemInfoModel::refreshSubvolumesを呼び出し、ListSubvolumesを非同期に実行する
                            onClicked: root.fs.refreshSubvolumes()
                        }

                        BusyIndicator {
                            running: root.fs.subvolumesBusy
                            visible: running
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                        }

                        Item { Layout.fillWidth: true }

                        CheckBox {
                            text: qsTr("Hide Snapper snapshots")
                            checked: root.fs.hideSnapperSnapshots
                            // 表示の絞り込みだけを行い、サーバから取得済みの一覧は保持する
                            onToggled: root.fs.hideSnapperSnapshots = checked
                        }
                    }

                    // 一覧が上限件数で打ち切られた場合は、見えていない項目があることを示す
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        visible: root.fs.subvolumesTruncated
                        color: root.theme.warningColor
                        text: qsTr("The list was truncated to the maximum number of subvolumes. Some entries are not shown.")
                    }

                    // 表の見出し
                    RowLayout {
                        Layout.fillWidth: true

                        Label { text: qsTr("ID"); Layout.preferredWidth: 70; font.bold: true }
                        Label { text: qsTr("Parent"); Layout.preferredWidth: 80; font.bold: true }
                        Label { text: qsTr("Created"); Layout.preferredWidth: 160; font.bold: true }
                        Label { text: qsTr("Read-only"); Layout.preferredWidth: 90; font.bold: true }
                        Label { text: qsTr("Path"); Layout.fillWidth: true; font.bold: true }
                    }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: palette.mid }

                    // サブボリュームごとの行
                    // パスが長い場合は行を崩さず省略表示し、全体はツールチップで確認できるようにする
                    Repeater {
                        model: root.fs.subvolumes

                        RowLayout {
                            id: subvolumeRow
                            required property var modelData     // ID・親ID・作成日時・read-only・パスを持つマップ

                            Layout.fillWidth: true

                            // サブボリュームIDを表示するラベル
                            Label {
                                Layout.preferredWidth: 70        // 列の幅
                                text: subvolumeRow.modelData.id  // サブボリュームID
                                textFormat: Text.PlainText       // 外部由来の値を、リッチテキストとして解釈させない
                            }

                            // 親サブボリュームのIDを表示するラベル
                            Label {
                                Layout.preferredWidth: 80              // 列の幅
                                text: subvolumeRow.modelData.parentId  // 親サブボリュームのID
                                textFormat: Text.PlainText             // 外部由来の値を、リッチテキストとして解釈させない
                            }

                            // 作成日時を表示するラベル
                            Label {
                                Layout.preferredWidth: 160                                // 列の幅
                                text: root.formatCreated(subvolumeRow.modelData.created)  // 作成日時
                                textFormat: Text.PlainText                                // 外部由来の値を、リッチテキストとして解釈させない
                            }

                            // 読み取り専用かどうかを表示するラベル
                            Label {
                                Layout.preferredWidth: 90                                         // 列の幅
                                text: subvolumeRow.modelData.readOnly ? qsTr("Yes") : qsTr("No")  // 読み取り専用かどうか
                                textFormat: Text.PlainText                                        // 外部由来の値を、リッチテキストとして解釈させない
                            }

                            // サブボリュームのパスを表示するラベル
                            // 長いパスは中央を省略し、全体はツールチップで確認できるようにする
                            Label {
                                id: pathLabel
                                textFormat: Text.PlainText  // パスは外部由来の文字列のため、リッチテキストとして解釈させない
                                text: subvolumeRow.modelData.path
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle

                                // ツールチップの表示条件にするため、ポインタが載っているかを検知する
                                HoverHandler { id: hoverArea }

                                // パスは外部由来の文字列のため、ツールチップの本文もリッチテキストとして解釈させない
                                // 添付プロパティのToolTip.textは本文の書式を指定できないため、contentItemを明示したToolTipを使う
                                ToolTip {
                                    // 省略表示されている場合だけ、全体を確認できるように表示する
                                    visible: hoverArea.hovered && pathLabel.implicitWidth > pathLabel.width
                                    delay: 500

                                    // 本文をプレーンテキストで描画するための表示部品
                                    contentItem: Text {
                                        text: subvolumeRow.modelData.path
                                        textFormat: Text.PlainText
                                        font: pathLabel.font
                                        color: palette.toolTipText
                                    }
                                }
                            }
                        }
                    }

                    // 一覧をまだ読み込んでいない場合の案内
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        visible: root.fs.subvolumes.length === 0 && !root.fs.subvolumesBusy
                        text: qsTr("No subvolumes have been loaded yet.")
                    }
                }
            }
        }
    }
}
