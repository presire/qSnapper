<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="ja_JP">
<context>
    <name>AboutQtDialog</name>
    <message>
        <location filename="../qml/components/AboutQtDialog.qml" line="18"/>
        <source>Qt License</source>
        <translation>Qt ライセンス</translation>
    </message>
    <message>
        <location filename="../qml/components/AboutQtDialog.qml" line="72"/>
        <source>This software is developed with Qt 6.</source>
        <translation>このソフトウェアはQt 6で開発されています。</translation>
    </message>
    <message>
        <location filename="../qml/components/AboutQtDialog.qml" line="87"/>
        <source>For more information, see &lt;a href=&quot;https://www.qt.io/licensing/&quot;&gt;qt.io/licensing&lt;/a&gt;.</source>
        <translation>詳細については&lt;a href=&quot;https://www.qt.io/licensing/&quot;&gt;qt.io/licensing&lt;/a&gt;をご覧ください。</translation>
    </message>
    <message>
        <location filename="../qml/components/AboutQtDialog.qml" line="116"/>
        <source>For more information, see &lt;a href=&quot;https://www.qt.io/&quot;&gt;qt.io&lt;/a&gt;.</source>
        <translation>詳細については&lt;a href=&quot;https://www.qt.io/&quot;&gt;qt.io&lt;/a&gt;をご覧ください。</translation>
    </message>
</context>
<context>
    <name>AboutqSnapperDialog</name>
    <message>
        <location filename="../qml/components/AboutqSnapperDialog.qml" line="18"/>
        <source>About qSnapper</source>
        <translation>qSnapper について</translation>
    </message>
    <message>
        <location filename="../qml/components/AboutqSnapperDialog.qml" line="80"/>
        <source>A Qt application for managing Btrfs/Snapper filesystem snapshots on Linux.</source>
        <translation>Linux上でBtrfs/Snapperファイルシステムのスナップショットを管理するためのQtアプリケーション。</translation>
    </message>
    <message>
        <location filename="../qml/components/AboutqSnapperDialog.qml" line="94"/>
        <source>This program is licensed under the GNU General Public License v2.0 or later.</source>
        <translation>このプログラムは、GNU General Public License v2.0以降の下でライセンスされています。</translation>
    </message>
    <message>
        <location filename="../qml/components/AboutqSnapperDialog.qml" line="102"/>
        <source>For more information, see &lt;a href=&quot;https://www.gnu.org/licenses/old-licenses/gpl-2.0.html&quot;&gt;gnu.org/licenses/gpl-2.0&lt;/a&gt;.</source>
        <translation>詳細については&lt;a href=&quot;https://www.gnu.org/licenses/old-licenses/gpl-2.0.html&quot;&gt;gnu.org/licenses/gpl-2.0&lt;/a&gt;をご覧ください。</translation>
    </message>
    <message>
        <location filename="../qml/components/AboutqSnapperDialog.qml" line="120"/>
        <source>GitHub: &lt;a href=&quot;https://github.com/presire/qSnapper&quot;&gt;github.com/presire/qSnapper&lt;/a&gt;</source>
        <translation>GitHub: &lt;a href=&quot;https://github.com/presire/qSnapper&quot;&gt;github.com/presire/qSnapper&lt;/a&gt;</translation>
    </message>
    <message>
        <location filename="../qml/components/AboutqSnapperDialog.qml" line="139"/>
        <source>Issues: &lt;a href=&quot;https://github.com/presire/qSnapper/issues&quot;&gt;github.com/presire/qSnapper/issues&lt;/a&gt;</source>
        <translation>Issues: &lt;a href=&quot;https://github.com/presire/qSnapper/issues&quot;&gt;github.com/presire/qSnapper/issues&lt;/a&gt;</translation>
    </message>
</context>
<context>
    <name>CompareSnapshotsDialog</name>
    <message>
        <location filename="../qml/components/CompareSnapshotsDialog.qml" line="24"/>
        <source>Compare Snapshot #%1 ↔ #%2</source>
        <translation>スナップショット #%1 ↔ #%2 の比較</translation>
    </message>
    <message>
        <location filename="../qml/components/CompareSnapshotsDialog.qml" line="61"/>
        <source>Loading comparison...</source>
        <translation>比較情報を読み込み中...</translation>
    </message>
    <message>
        <location filename="../qml/components/CompareSnapshotsDialog.qml" line="62"/>
        <source>Files that differ between snapshot #%1 and #%2</source>
        <translation>スナップショット #%1 と #%2 で異なるファイル</translation>
    </message>
    <message>
        <location filename="../qml/components/CompareSnapshotsDialog.qml" line="99"/>
        <source>No differences found</source>
        <translation>差分はありません</translation>
    </message>
</context>
<context>
    <name>ConfigManagerModel</name>
    <message>
        <location filename="../src/configmanagermodel.cpp" line="102"/>
        <source>The configuration name or subvolume is not valid.</source>
        <translation>設定名またはサブボリュームが不正です</translation>
    </message>
    <message>
        <location filename="../src/configmanagermodel.cpp" line="133"/>
        <source>The service did not create the configuration.</source>
        <translation>サービスが設定を作成しませんでした</translation>
    </message>
    <message>
        <location filename="../src/configmanagermodel.cpp" line="158"/>
        <source>The configuration name is not valid.</source>
        <translation>設定名が不正です</translation>
    </message>
    <message>
        <location filename="../src/configmanagermodel.cpp" line="189"/>
        <source>The service did not delete the configuration.</source>
        <translation>サービスが設定を削除しませんでした</translation>
    </message>
</context>
<context>
    <name>CreateConfigDialog</name>
    <message>
        <location filename="../qml/components/CreateConfigDialog.qml" line="31"/>
        <source>Create Configuration</source>
        <translation>設定の作成</translation>
    </message>
    <message>
        <location filename="../qml/components/CreateConfigDialog.qml" line="75"/>
        <source>A new snapper configuration is created with the btrfs filesystem type and the default template. A .snapshots subvolume is created automatically beneath the given subvolume. The subvolume must already be mounted and must not be the filesystem root.</source>
        <translation>新しいsnapper設定をbtrfsのファイルシステム種別とdefaultテンプレートで作成します。指定したサブボリュームの下に.snapshotsサブボリュームが自動的に作成されます。サブボリュームは既にマウントされている必要があり、ファイルシステムのルートにはできません</translation>
    </message>
    <message>
        <location filename="../qml/components/CreateConfigDialog.qml" line="79"/>
        <source>Name:</source>
        <translation>名前:</translation>
    </message>
    <message>
        <location filename="../qml/components/CreateConfigDialog.qml" line="83"/>
        <source>Enter a configuration name</source>
        <translation>設定名を入力</translation>
    </message>
    <message>
        <location filename="../qml/components/CreateConfigDialog.qml" line="92"/>
        <source>The name must start with a letter or digit and may contain only letters, digits, _ and - (up to 64 characters).</source>
        <translation>名前は英数字で始まり、英数字と_と-だけを使用できます(64文字まで)</translation>
    </message>
    <message>
        <location filename="../qml/components/CreateConfigDialog.qml" line="96"/>
        <source>Subvolume:</source>
        <translation>サブボリューム:</translation>
    </message>
    <message>
        <location filename="../qml/components/CreateConfigDialog.qml" line="100"/>
        <source>/path/to/btrfs/subvolume</source>
        <translation>/path/to/btrfs/subvolume</translation>
    </message>
    <message>
        <location filename="../qml/components/CreateConfigDialog.qml" line="109"/>
        <source>Enter an absolute path of a mounted Btrfs subvolume. The path must not contain //, . or .. components, must not end with /, and must not be /.</source>
        <translation>マウント済みのBtrfsサブボリュームの絶対パスを入力してください。パスに//、.、..を含めることはできず、/で終わることも、/そのものであることもできません</translation>
    </message>
</context>
<context>
    <name>DeleteConfigDialog</name>
    <message>
        <location filename="../qml/components/DeleteConfigDialog.qml" line="33"/>
        <source>Delete Configuration</source>
        <translation>設定の削除</translation>
    </message>
    <message>
        <location filename="../qml/components/DeleteConfigDialog.qml" line="77"/>
        <source>Delete configuration &quot;%1&quot;?</source>
        <translation>設定「%1」を削除しますか?</translation>
    </message>
    <message>
        <location filename="../qml/components/DeleteConfigDialog.qml" line="86"/>
        <source>All snapshots that belong to this configuration will be deleted.</source>
        <translation>この設定に属するすべてのスナップショットが削除されます</translation>
    </message>
    <message>
        <location filename="../qml/components/DeleteConfigDialog.qml" line="94"/>
        <source>This operation cannot be undone.</source>
        <translation>この操作は元に戻せません。</translation>
    </message>
    <message>
        <location filename="../qml/components/DeleteConfigDialog.qml" line="102"/>
        <source>Administrator privileges are required.</source>
        <translation>管理者権限が必要です</translation>
    </message>
    <message>
        <location filename="../qml/components/DeleteConfigDialog.qml" line="113"/>
        <source>The root configuration (subvolume /) cannot be deleted. Create a configuration for a subvolume other than the filesystem root.</source>
        <translation>ルート設定(サブボリュームが/)は削除できません。ファイルシステムのルート以外のサブボリュームに設定を作成してください</translation>
    </message>
    <message>
        <location filename="../qml/components/DeleteConfigDialog.qml" line="121"/>
        <source>Type the configuration name to confirm:</source>
        <translation>確認のため設定名を入力:</translation>
    </message>
</context>
<context>
    <name>FileChangeModel</name>
    <message>
        <location filename="../src/filechangemodel_restore.cpp" line="116"/>
        <source>The restore service stopped unexpectedly</source>
        <translation>復元サービスが予期せず停止しました</translation>
    </message>
    <message>
        <location filename="../src/filechangemodel_restore.cpp" line="143"/>
        <location filename="../src/filechangemodel_restore.cpp" line="161"/>
        <source>Invalid parameters for restore</source>
        <translation>復元のパラメータが不正です</translation>
    </message>
    <message>
        <location filename="../src/filechangemodel_restore.cpp" line="151"/>
        <location filename="../src/filechangemodel_restore.cpp" line="400"/>
        <source>File changes are still loading</source>
        <translation>ファイルの変更を読み込み中です</translation>
    </message>
    <message>
        <source>D-Bus connection failed</source>
        <translation type="vanished">D-Bus 接続に失敗しました</translation>
    </message>
    <message>
        <source>Restore failed: %1</source>
        <translation type="vanished">復元に失敗しました: %1</translation>
    </message>
    <message>
        <location filename="../src/filechangemodel_restore.cpp" line="460"/>
        <source>No files selected for restoration</source>
        <translation>復元するファイルが選択されていません</translation>
    </message>
    <message>
        <location filename="../src/filechangemodel_restore.cpp" line="574"/>
        <source>Failed to begin restore plan</source>
        <translation>復元プランを開始できませんでした</translation>
    </message>
    <message>
        <location filename="../src/filechangemodel_restore.cpp" line="600"/>
        <source>Restore plan has no manifest id</source>
        <translation>復元プランにマニフェスト ID がありません</translation>
    </message>
    <message>
        <location filename="../src/filechangemodel_restore.cpp" line="642"/>
        <source>Failed to stage restore entries</source>
        <translation>復元エントリをステージングできませんでした</translation>
    </message>
    <message>
        <location filename="../src/filechangemodel_restore.cpp" line="685"/>
        <source>Failed to commit restore plan</source>
        <translation>復元プランをコミットできませんでした</translation>
    </message>
</context>
<context>
    <name>FilesystemInfoModel</name>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="154"/>
        <source>Global reserve</source>
        <translation>グローバル予約</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="162"/>
        <source>Mixed</source>
        <translation>混在</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="167"/>
        <source>Data</source>
        <translation>データ</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="170"/>
        <source>Metadata</source>
        <translation>メタデータ</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="173"/>
        <source>System</source>
        <translation>システム</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="176"/>
        <source>Unknown</source>
        <translation>不明</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="380"/>
        <source>The filesystem usage data from the service is empty or malformed.</source>
        <translation>サービスから返された使用量データが空か壊れています</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="396"/>
        <location filename="../src/filesysteminfomodel.cpp" line="411"/>
        <location filename="../src/filesysteminfomodel.cpp" line="429"/>
        <source>The filesystem usage data from the service is malformed.</source>
        <translation>サービスから返された使用量データが壊れています</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="403"/>
        <source>The filesystem usage data from the service has an unexpected header.</source>
        <translation>サービスから返された使用量データの先頭行が想定と異なります</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="425"/>
        <location filename="../src/filesysteminfomodel.cpp" line="444"/>
        <location filename="../src/filesysteminfomodel.cpp" line="462"/>
        <source>The filesystem usage data from the service contains invalid numbers.</source>
        <translation>サービスから返された使用量データに不正な数値があります</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="474"/>
        <source>The filesystem usage data from the service contains an unknown row.</source>
        <translation>サービスから返された使用量データに未知の行があります</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="480"/>
        <source>The filesystem usage data from the service is incomplete.</source>
        <translation>サービスから返された使用量データが不完全です</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="512"/>
        <source>The subvolume list from the service is empty or malformed.</source>
        <translation>サービスから返されたサブボリューム一覧が空か壊れています</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="522"/>
        <location filename="../src/filesysteminfomodel.cpp" line="532"/>
        <source>The subvolume list from the service has an unexpected header.</source>
        <translation>サービスから返されたサブボリューム一覧の先頭行が想定と異なります</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="540"/>
        <source>The subvolume list from the service is malformed.</source>
        <translation>サービスから返されたサブボリューム一覧が壊れています</translation>
    </message>
    <message>
        <location filename="../src/filesysteminfomodel.cpp" line="557"/>
        <source>The subvolume list from the service contains invalid values.</source>
        <translation>サービスから返されたサブボリューム一覧に不正な値があります</translation>
    </message>
</context>
<context>
    <name>FilesystemPage</name>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="111"/>
        <source>Failed to load filesystem usage: %1</source>
        <translation>ファイルシステムの使用量を取得できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="115"/>
        <source>Failed to load subvolumes: %1</source>
        <translation>サブボリューム一覧を取得できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="126"/>
        <source>Filesystem: %1</source>
        <translation>ファイルシステム: %1</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="126"/>
        <source>(none)</source>
        <translation>(なし)</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="143"/>
        <source>Refresh</source>
        <translation>更新</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="199"/>
        <source>No configuration is selected. Select a configuration on the Settings tab.</source>
        <translation>設定が選択されていません。設定タブで設定を選択してください</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="209"/>
        <source>Overview</source>
        <translation>概要</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="220"/>
        <source>These values come from the Btrfs space information and can differ from the output of btrfs filesystem usage.</source>
        <translation>これらの値はBtrfsのスペース情報から取得しており、btrfs filesystem usageの出力とは異なる場合があります</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="229"/>
        <source>Size:</source>
        <translation>サイズ:</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="232"/>
        <source>Allocated:</source>
        <translation>割り当て済み:</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="235"/>
        <source>Unallocated:</source>
        <translation>未割り当て:</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="238"/>
        <source>Node size:</source>
        <translation>ノードサイズ:</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="241"/>
        <source>Sector size:</source>
        <translation>セクタサイズ:</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="249"/>
        <source>Size is the total size of all devices. Allocated is the space reserved for chunks on the devices. Unallocated = Size - Allocated.</source>
        <translation>サイズは全デバイスの合計です。割り当て済みはデバイス上でチャンクに確保された領域です。未割り当て = サイズ - 割り当て済みです</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="257"/>
        <source>No usage data has been loaded yet.</source>
        <translation>使用量はまだ取得していません</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="267"/>
        <source>Space by chunk type</source>
        <translation>チャンク種別ごとの領域</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="278"/>
        <source>Total is the space allocated to chunks of this type. Used is the space actually used inside those chunks. The global reserve is part of metadata.</source>
        <translation>合計はこの種別のチャンクに割り当てられた領域です。使用はチャンク内で実際に使われている領域です。グローバル予約はメタデータの一部です</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="286"/>
        <source>Type</source>
        <translation>種類</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="287"/>
        <source>Profile</source>
        <translation>プロファイル</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="288"/>
        <source>Total</source>
        <translation>合計</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="289"/>
        <source>Used</source>
        <translation>使用</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="320"/>
        <source>Devices</source>
        <translation>デバイス</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="330"/>
        <source>Device ID</source>
        <translation>デバイスID</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="331"/>
        <source>Size</source>
        <translation>サイズ</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="332"/>
        <source>Allocated</source>
        <translation>割り当て済み</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="360"/>
        <source>Allocated is the space reserved for chunks on this device.</source>
        <translation>割り当て済みはこのデバイス上でチャンクに確保された領域です</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="372"/>
        <source>Subvolumes</source>
        <translation>サブボリューム</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="383"/>
        <source>Loading the subvolume list requires administrator authentication. This page only displays the list.</source>
        <translation>サブボリューム一覧の読み込みには管理者認証が必要です。この画面は一覧を表示するだけで、変更は行いません</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="391"/>
        <source>Load subvolumes</source>
        <translation>サブボリュームを読み込む</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="410"/>
        <source>Hide Snapper snapshots</source>
        <translation>Snapperのスナップショットを隠す</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="423"/>
        <source>The list was truncated to the maximum number of subvolumes. Some entries are not shown.</source>
        <translation>一覧が最大件数に達したため、一部のエントリは表示されていません</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="430"/>
        <source>ID</source>
        <translation>ID</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="431"/>
        <source>Parent</source>
        <translation>親</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="432"/>
        <source>Created</source>
        <translation>作成日時</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="433"/>
        <source>Read-only</source>
        <translation>読み取り専用</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="434"/>
        <source>Path</source>
        <translation>パス</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="453"/>
        <source>Yes</source>
        <translation>はい</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="453"/>
        <source>No</source>
        <translation>いいえ</translation>
    </message>
    <message>
        <location filename="../qml/pages/FilesystemPage.qml" line="472"/>
        <source>No subvolumes have been loaded yet.</source>
        <translation>サブボリュームはまだ読み込んでいません</translation>
    </message>
</context>
<context>
    <name>Main</name>
    <message>
        <location filename="../qml/Main.qml" line="27"/>
        <source>qSnapper - Snapshot Manager</source>
        <translation>qSnapper - スナップショット管理</translation>
    </message>
    <message>
        <location filename="../qml/Main.qml" line="70"/>
        <source>Snapshots</source>
        <translation>スナップショット</translation>
    </message>
    <message>
        <location filename="../qml/Main.qml" line="71"/>
        <source>Settings</source>
        <translation>設定</translation>
    </message>
    <message>
        <location filename="../qml/Main.qml" line="72"/>
        <source>Filesystem</source>
        <translation>ファイルシステム</translation>
    </message>
</context>
<context>
    <name>RestorePreviewDialog</name>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="148"/>
        <source>Snapshot Overview</source>
        <translation>選択されたスナップショットの概要</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="281"/>
        <source>Snapper configuration: %1</source>
        <translation>Snapper の設定: %1</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="288"/>
        <source>Shows only the changes made between the Pre and Post snapshots that still differ from the current system.
Entries already matching the restore target are hidden.
Restoring reverts just those changes; files modified afterwards are left untouched.</source>
        <translation>PreスナップショットとPostスナップショットの間に行われた変更のうち、現在のシステムとまだ差異があるもののみを表示します。
既に復元先と一致している項目は表示されません。
復元ではそれらの変更だけを戻し、その後に変更されたファイルはそのまま残ります。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="289"/>
        <source>Shows the system state after applying the specified snapshot</source>
        <translation>スナップショットを指定した後のシステムの画像を表示する</translation>
    </message>
    <message>
        <source>Loading diff...</source>
        <translation type="vanished">差分を読み込み中...</translation>
    </message>
    <message>
        <source>New file created.</source>
        <translation type="vanished">新しいファイルが作成されました。</translation>
    </message>
    <message>
        <source>File deleted.</source>
        <translation type="vanished">ファイルが削除されました。</translation>
    </message>
    <message>
        <source>No diff found.</source>
        <translation type="vanished">差分が見つかりませんでした。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="502"/>
        <source>No differences with snapshot</source>
        <translation>スナップショットとの差分がありません</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="534"/>
        <source>The differences are not shown because the file is binary.</source>
        <translation>バイナリファイルのため、差分を表示しません。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="536"/>
        <source>The differences are not shown because the file is too large.</source>
        <translation>ファイルが大きすぎるため、差分を表示しません。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="538"/>
        <source>The differences are not shown because there are too many changes.</source>
        <translation>変更が多すぎるため、差分を表示しません。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="540"/>
        <source>The differences are not shown because the file is not a regular file.</source>
        <translation>通常のファイルではないため、差分を表示しません。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="542"/>
        <source>The differences are not shown.</source>
        <translation>差分を表示しません。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="777"/>
        <source>View only. Choose a Pre / Post difference above to restore.</source>
        <translation>閲覧専用です。復元するには上のPre / Post差分を選択してください。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="789"/>
        <source>Cancel</source>
        <translation>キャンセル</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="797"/>
        <source>Restore Selected to #%1</source>
        <translation>選択項目を#%1へ復元</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="798"/>
        <source>Restore Selected</source>
        <translation>選択したものを復元する</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="816"/>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1084"/>
        <source>Confirmation</source>
        <translation>確認</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="837"/>
        <source>Restore selected files/directories?</source>
        <translation>選択されたファイル/ディレクトリを復元しますか?</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="843"/>
        <source>Only the selected entries of the Pre #%1 / Post #%2 difference will be restored to the snapshot #%3 state. Files changed outside this range are left untouched.</source>
        <translation>Pre #%1 / Post #%2 間の差分から選択した項目だけをスナップショット #%3 の状態へ戻します。この範囲外で変更されたファイルはそのまま残ります。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="847"/>
        <source>This will restore selected files and directories to snapshot #%1 state.</source>
        <translation>この操作により、選択されたファイルおよびディレクトリがスナップショット #%1 の状態に復元されます。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="854"/>
        <source>Warning: This may overwrite current files.</source>
        <translation>注意: この操作により、現在のファイルが上書きされる可能性があります。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="923"/>
        <source>Restoring Files</source>
        <translation>ファイルを復元中</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1021"/>
        <source>Restoring files. Please wait...</source>
        <translation>ファイルを復元しています。しばらくお待ちください...</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1037"/>
        <source>Progress: %1 / %2</source>
        <translation>進捗: %1 / %2</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1145"/>
        <source>Success</source>
        <translation>成功</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1164"/>
        <source>File/directory restoration completed.</source>
        <translation>選択されたファイル/ディレクトリの復元が完了しました。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1181"/>
        <source>No more differences with snapshot.</source>
        <translation>スナップショットとの差分がなくなりました。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1172"/>
        <source>Switched to the differences between snapshot #%1 and the current system.</source>
        <translation>スナップショット #%1 と現在のシステムの差分表示に切り替えました。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="863"/>
        <source>Restore Options</source>
        <translation>復元オプション</translation>
    </message>
    <message>
        <source>Shows only the changes made between the Pre and Post snapshots. Restoring reverts just those changes; files modified afterwards are left untouched.</source>
        <translation type="vanished">PreスナップショットとPostスナップショットの間に行われた変更のみを表示します。復元ではそれらの変更だけを戻し、その後に変更されたファイルはそのまま残ります。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="305"/>
        <source>Revert to Pre #%1: undo the changes made between Pre #%1 and Post #%2</source>
        <translation>Pre #%1 へ戻す: Pre #%1 とPost #%2 の間に行われた変更を取り消す</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="314"/>
        <source>Re-apply to Post #%2: redo the changes made between Pre #%1 and Post #%2</source>
        <translation>Post #%2 を再適用: Pre #%1 とPost #%2 の間に行われた変更を再実行する</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="323"/>
        <source>Show differences between snapshot #%1 (Pre) and the current system (view only)</source>
        <translation>スナップショット #%1 (Pre) と現在のシステムとの差分を表示 (閲覧専用)</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="330"/>
        <source>Show differences between snapshot #%1 (Post) and the current system (view only)</source>
        <translation>スナップショット #%1 (Post) と現在のシステムとの差分を表示 (閲覧専用)</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="873"/>
        <source>Method:</source>
        <translation>方式:</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="877"/>
        <source>Direct copy (fast)</source>
        <translation>ダイレクトコピー (高速)</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="882"/>
        <source>YaST compatible</source>
        <translation>YaST互換</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="892"/>
        <source>Batch size:</source>
        <translation>バッチサイズ:</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="903"/>
        <source>files per batch</source>
        <translation>ファイル/バッチ</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="265"/>
        <source>Loading file changes. Please wait...</source>
        <translation>ファイルの変更を読み込み中です。しばらくお待ちください...</translation>
    </message>
    <message>
        <source>Show differences between Pre #%1 and Post #%2 (view only)</source>
        <translation type="vanished">事前 #%1 と事後 #%2 の差分を表示 (閲覧のみ)</translation>
    </message>
    <message>
        <source>Switch to &apos;Pre vs current&apos; view to restore.</source>
        <translation type="vanished">復元するには「事前 vs 現在」表示に切り替えてください。</translation>
    </message>
    <message>
        <source>Switch to &apos;Post vs current&apos; view to restore.</source>
        <translation type="vanished">復元するには「事後 vs 現在」表示に切り替えてください。</translation>
    </message>
    <message>
        <source>Show differences between snapshot #%1 (Pre) and the current system</source>
        <translation type="vanished">スナップショット #%1 (事前) と現在のシステムとの差分を表示</translation>
    </message>
    <message>
        <source>Show differences between snapshot #%1 (Post) and the current system</source>
        <translation type="vanished">スナップショット #%1 (事後) と現在のシステムとの差分を表示</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="588"/>
        <source>New file was created.</source>
        <translation>新しいファイルが作成されました。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="589"/>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="592"/>
        <source>File content was modified.</source>
        <translation>ファイルの内容が変更されました。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="590"/>
        <source>File was removed.</source>
        <translation>ファイルが削除されました。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="591"/>
        <source>File type was changed.</source>
        <translation>ファイルの種類が変更されました。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="609"/>
        <source>Select a file to view details</source>
        <translation>ファイルを選択して詳細を表示</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="641"/>
        <source>Loading file details...</source>
        <translation>ファイルの詳細を読み込み中...</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="651"/>
        <source>File Status</source>
        <translation>ファイルの状態</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="679"/>
        <source>File mode was changed from &apos;%1&apos; to &apos;%2&apos;.</source>
        <translation>ファイルモードが &apos;%1&apos; から &apos;%2&apos; に変更されました。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="692"/>
        <source>File user ownership was changed from &apos;%1&apos; to &apos;%2&apos;.</source>
        <translation>ファイルのユーザ所有者が &apos;%1&apos; から &apos;%2&apos; に変更されました。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="705"/>
        <source>File group ownership was changed from &apos;%1&apos; to &apos;%2&apos;.</source>
        <translation>ファイルのグループ所有者が &apos;%1&apos; から &apos;%2&apos; に変更されました。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="747"/>
        <source>Remove</source>
        <translation>削除</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="748"/>
        <source>Restore</source>
        <translation>復元</translation>
    </message>
    <message>
        <source>Restore from Pre #%1</source>
        <translation type="vanished">事前 #%1 から復元</translation>
    </message>
    <message>
        <source>Restore from Post #%1</source>
        <translation type="vanished">事後 #%1 から復元</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1104"/>
        <source>Remove this file from the current system?</source>
        <translation>このファイルを現在のシステムから削除しますか？</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1105"/>
        <source>Restore this file from snapshot #%1?</source>
        <translation>このファイルをスナップショット #%1 から復元しますか？</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1122"/>
        <source>Warning: This file will be deleted from the current system.</source>
        <translation>注意: このファイルは現在のシステムから削除されます。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1123"/>
        <source>Warning: The current file will be overwritten.</source>
        <translation>注意: 現在のファイルが上書きされます。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1197"/>
        <source>Restore Failed</source>
        <translation>復元失敗</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1216"/>
        <source>Failed to restore some or all files.</source>
        <translation>一部または全てのファイルの復元に失敗しました。</translation>
    </message>
    <message>
        <location filename="../qml/components/RestorePreviewDialog.qml" line="1223"/>
        <source>The files may already be in sync with the snapshot, or an error occurred during restoration. Check the system log for details.</source>
        <translation>ファイルが既にスナップショットと同期しているか、復元中にエラーが発生した可能性があります。詳細はシステムログを確認してください。</translation>
    </message>
</context>
<context>
    <name>SettingsFieldRow</name>
    <message>
        <location filename="../qml/components/SettingsFieldRow.qml" line="46"/>
        <source>Seconds (0 to 999999999)</source>
        <translation>秒 (0 ～ 999999999)</translation>
    </message>
    <message>
        <location filename="../qml/components/SettingsFieldRow.qml" line="48"/>
        <source>Number or MIN-MAX (for example 10 or 5-10)</source>
        <translation>数値または 最小-最大 (例: 10 または 5-10)</translation>
    </message>
    <message>
        <location filename="../qml/components/SettingsFieldRow.qml" line="50"/>
        <source>0 to 1 (for example 0.5)</source>
        <translation>0 ～ 1 (例: 0.5)</translation>
    </message>
    <message>
        <location filename="../qml/components/SettingsFieldRow.qml" line="72"/>
        <source>Yes</source>
        <translation>はい</translation>
    </message>
    <message>
        <location filename="../qml/components/SettingsFieldRow.qml" line="72"/>
        <source>No</source>
        <translation>いいえ</translation>
    </message>
</context>
<context>
    <name>SettingsPage</name>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="120"/>
        <location filename="../qml/pages/SettingsPage.qml" line="364"/>
        <source>Settings saved.</source>
        <translation>設定を保存しました。</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="125"/>
        <source>Failed to save settings: %1</source>
        <translation>設定の保存に失敗しました: %1</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="130"/>
        <source>Failed to load settings: %1</source>
        <translation>設定の読み込みに失敗しました: %1</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="136"/>
        <source>Quota has been set up.</source>
        <translation>クォータを設定しました</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="141"/>
        <source>Failed to set up quota: %1</source>
        <translation>クォータを設定できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="151"/>
        <source>Configuration %1 has been created.</source>
        <translation>設定%1を作成しました</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="156"/>
        <source>Failed to create a configuration: %1</source>
        <translation>設定を作成できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="161"/>
        <source>Configuration %1 has been deleted.</source>
        <translation>設定%1を削除しました</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="166"/>
        <source>Failed to delete a configuration: %1</source>
        <translation>設定を削除できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="174"/>
        <location filename="../qml/pages/SettingsPage.qml" line="503"/>
        <source>Set up quota</source>
        <translation>クォータを設定</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="182"/>
        <source>Enabling Btrfs quota can slow down some filesystem operations, and it cannot be undone from qSnapper. Do you want to continue?</source>
        <translation>Btrfsのクォータを有効にするとファイルシステム操作が遅くなる場合があり、qSnapperからは元に戻せません。続行しますか?</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="214"/>
        <source>Config:</source>
        <translation>設定:</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="256"/>
        <source>New...</source>
        <translation>新規...</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="266"/>
        <source>Delete...</source>
        <translation>削除...</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="293"/>
        <source>Reload</source>
        <translation>再読み込み</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="327"/>
        <source>Revert</source>
        <translation>元に戻す</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="344"/>
        <source>Save</source>
        <translation>保存</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="388"/>
        <source>The settings could not be loaded. Editing is disabled.</source>
        <translation>設定を読み込めませんでした。編集は無効です。</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="398"/>
        <source>Config information (read only)</source>
        <translation>設定の情報 (読み取り専用)</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="419"/>
        <source>Number cleanup</source>
        <translation>番号によるクリーンアップ</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="426"/>
        <source>Enable number cleanup</source>
        <translation>番号によるクリーンアップを有効にする</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="427"/>
        <location filename="../qml/pages/SettingsPage.qml" line="455"/>
        <location filename="../qml/pages/SettingsPage.qml" line="475"/>
        <source>Minimum age</source>
        <translation>最小経過時間</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="428"/>
        <source>Number limit</source>
        <translation>保持数の上限</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="429"/>
        <source>Number limit (important)</source>
        <translation>保持数の上限 (重要)</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="435"/>
        <source>Timeline</source>
        <translation>タイムライン</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="442"/>
        <source>Create timeline snapshots</source>
        <translation>タイムラインのスナップショットを作成する</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="446"/>
        <source>This setting only takes effect while snapper-timeline.timer is enabled. Manage the timer in the timer section below.</source>
        <translation>この設定は、snapper-timeline.timer が有効な間だけ効果があります。タイマーは下のタイマーの欄で管理してください。</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="454"/>
        <source>Enable timeline cleanup</source>
        <translation>タイムラインのクリーンアップを有効にする</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="456"/>
        <source>Hourly limit</source>
        <translation>毎時の上限</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="457"/>
        <source>Daily limit</source>
        <translation>毎日の上限</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="458"/>
        <source>Weekly limit</source>
        <translation>毎週の上限</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="459"/>
        <source>Monthly limit</source>
        <translation>毎月の上限</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="460"/>
        <source>Quarterly limit</source>
        <translation>四半期ごとの上限</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="461"/>
        <source>Yearly limit</source>
        <translation>毎年の上限</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="467"/>
        <source>Empty pre/post pairs</source>
        <translation>差分のない Pre/Post ペア</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="474"/>
        <source>Enable empty pre/post cleanup</source>
        <translation>差分のない Pre/Post ペアのクリーンアップを有効にする</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="481"/>
        <source>Space limits</source>
        <translation>容量の制限</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="496"/>
        <source>Quota: enabled (qgroup %1)</source>
        <translation>クォータ: 有効 (qgroup %1)</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="497"/>
        <source>Quota: not set up</source>
        <translation>クォータ: 未設定</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="516"/>
        <source>SPACE_LIMIT and FREE_LIMIT only take effect when the quota is set up.</source>
        <translation>SPACE_LIMITとFREE_LIMITはクォータを設定した場合だけ有効になります</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="524"/>
        <source>Space limit</source>
        <translation>使用容量の上限</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="525"/>
        <source>Free space limit</source>
        <translation>空き容量の下限</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="531"/>
        <source>Background comparison</source>
        <translation>バックグラウンド比較</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="538"/>
        <source>Enable background comparison</source>
        <translation>バックグラウンド比較を有効にする</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="554"/>
        <source>Appearance</source>
        <translation>外観</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="570"/>
        <source>Light</source>
        <translation>ライト</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="576"/>
        <source>Dark</source>
        <translation>ダーク</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="582"/>
        <source>Follow system</source>
        <translation>システムに従う</translation>
    </message>
    <message>
        <location filename="../qml/pages/SettingsPage.qml" line="589"/>
        <source>The theme is saved for this user only and applies when you press Save.</source>
        <translation>テーマはこのユーザだけに保存され、「保存」を押すと反映されます。</translation>
    </message>
    <message>
        <source>The theme is saved for this user only and applies immediately.</source>
        <translation type="vanished">テーマはこのユーザだけに保存され、すぐに反映されます。</translation>
    </message>
</context>
<context>
    <name>SnapperService</name>
    <message>
        <location filename="../src/snapperservice.cpp" line="53"/>
        <source>This operation cannot be performed while a restore is in progress. Try again after the restore finishes.</source>
        <translation>復元の実行中のため、この操作は実行できません。復元の完了後に再度実行してください。</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="526"/>
        <source>Previous snapshot was not found.</source>
        <translation>前のスナップショットが見つかりませんでした。</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="229"/>
        <location filename="../src/snapperservice.cpp" line="603"/>
        <location filename="../src/snapperservice.cpp" line="643"/>
        <location filename="../src/snapperservice.cpp" line="694"/>
        <location filename="../src/snapperservice.cpp" line="755"/>
        <source>D-Bus connection failed.</source>
        <translation>D-Bus 接続に失敗しました。</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="57"/>
        <source>Another configuration or bulk delete operation is in progress.</source>
        <translation>別の設定操作または一括削除が実行中です</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="61"/>
        <source>The operation did not finish in time. Reload to see the current state.</source>
        <translation>操作が時間内に完了しませんでした。再読み込みして現在の状態を確認してください</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="65"/>
        <source>A restore is being prepared for this configuration.</source>
        <translation>この設定の復元を準備しています</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="69"/>
        <source>The snapshot list is being updated by Snapper. Try again in a minute.</source>
        <translation>Snapperがスナップショット一覧を更新しています。1分ほど待って再試行してください</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="613"/>
        <source>Failed to rollback snapshot: %1</source>
        <translation>スナップショットの復元に失敗しました: %1</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="623"/>
        <source>Rollback operation failed.</source>
        <translation>復元操作に失敗しました。</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="653"/>
        <source>Failed to delete snapshot: %1</source>
        <translation>スナップショットの削除に失敗しました: %1</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="664"/>
        <source>Delete operation failed.</source>
        <translation>削除操作に失敗しました。</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="716"/>
        <source>Failed to create snapshot: %1</source>
        <translation>スナップショットの作成に失敗しました: %1</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="727"/>
        <source>The snapshot was created, but it could not be identified.</source>
        <translation>スナップショットは作成されましたが、特定できませんでした。</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="769"/>
        <source>Failed to modify snapshot: %1</source>
        <translation>スナップショットの変更に失敗しました: %1</translation>
    </message>
    <message>
        <location filename="../src/snapperservice.cpp" line="778"/>
        <source>Modify operation failed.</source>
        <translation>変更操作に失敗しました。</translation>
    </message>
</context>
<context>
    <name>SnapperSettingsModel</name>
    <message>
        <location filename="../src/snappersettingsmodel.cpp" line="334"/>
        <source>Invalid value for %1.</source>
        <translation>%1 の値が不正です。</translation>
    </message>
    <message>
        <location filename="../src/snappersettingsmodel.cpp" line="396"/>
        <source>Failed to set up the quota.</source>
        <translation>クォータを設定できませんでした</translation>
    </message>
    <message>
        <location filename="../src/snappersettingsmodel.cpp" line="494"/>
        <source>Unknown timer unit.</source>
        <translation>不明なタイマーのユニットです。</translation>
    </message>
</context>
<context>
    <name>SnapshotDetailDialog</name>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="35"/>
        <source>Snapshot Details</source>
        <translation>スナップショット詳細</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="94"/>
        <source>Basic Information</source>
        <translation>基本情報</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="104"/>
        <source>Number:</source>
        <translation>番号:</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="118"/>
        <source>Type:</source>
        <translation>タイプ:</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="127"/>
        <source>Single</source>
        <translation>単一</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="131"/>
        <source>Pre</source>
        <translation>プリ</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="133"/>
        <source>Post</source>
        <translation>ポスト</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="142"/>
        <source>Date/Time:</source>
        <translation>日時:</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="161"/>
        <source>User:</source>
        <translation>ユーザ:</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="166"/>
        <source>Unknown</source>
        <translation>不明</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="171"/>
        <source>Cleanup:</source>
        <translation>クリーンアップ:</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="182"/>
        <source>Previous Snapshot:</source>
        <translation>前のスナップショット:</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="195"/>
        <source>Description</source>
        <translation>説明</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="201"/>
        <source>(No description)</source>
        <translation>(説明なし)</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="209"/>
        <source>User Data</source>
        <translation>ユーザデータ</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="254"/>
        <source>Show Changes</source>
        <translation>変更点の表示</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="338"/>
        <source>A reboot is required after the rollback. Changes made after the rollback and before the reboot are not booted by default, because they remain in the currently running subvolume.</source>
        <translation>ロールバックの後には再起動が必要です。ロールバックの後、再起動までに行った変更は、現在動作中のサブボリュームに残るため、既定では起動されません。</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="396"/>
        <source>Snapshot rollback completed.
Please reboot the system.</source>
        <translation>スナップショットへの復元が完了しました。
システムを再起動してください。</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="129"/>
        <source>Pre and Post</source>
        <translation>事前および事後</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="142"/>
        <source>Start Date:</source>
        <translation>開始日:</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="151"/>
        <source>End Date:</source>
        <translation>終了日:</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="266"/>
        <source>System Rollback</source>
        <translation>システムロールバック</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="284"/>
        <source>Close</source>
        <translation>閉じる</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="294"/>
        <source>Confirmation</source>
        <translation>確認</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="315"/>
        <source>Rollback to snapshot #%1?</source>
        <translation>スナップショット #%1 に復元しますか？</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="320"/>
        <source>This operation will restore the system to the selected snapshot state.</source>
        <translation>この操作により、システムは選択したスナップショットの状態に戻ります。</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="328"/>
        <source>Note: This operation requires administrator privileges.</source>
        <translation>注意: この操作には管理者権限が必要です。</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="378"/>
        <source>Success</source>
        <translation>成功</translation>
    </message>
    <message>
        <source>Snapshot rollback completed.\nPlease reboot the system.</source>
        <translation type="vanished">スナップショットへの復元が完了しました。\nシステムを再起動してください。</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="404"/>
        <source>Error</source>
        <translation>エラー</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotDetailDialog.qml" line="424"/>
        <source>Rollback failed: %1</source>
        <translation>復元に失敗しました: %1</translation>
    </message>
</context>
<context>
    <name>SnapshotEditDialog</name>
    <message>
        <location filename="../qml/components/SnapshotEditDialog.qml" line="26"/>
        <source>Modify Snapshot #%1</source>
        <translation>スナップショット #%1 を編集</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotEditDialog.qml" line="62"/>
        <source>Description:</source>
        <translation>説明:</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotEditDialog.qml" line="66"/>
        <source>Enter snapshot description</source>
        <translation>スナップショットの説明を入力してください</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotEditDialog.qml" line="70"/>
        <source>Cleanup algorithm:</source>
        <translation>クリーンアップアルゴリズム:</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotEditDialog.qml" line="80"/>
        <source>User data (key=value per line):</source>
        <translation>ユーザデータ (1行ごとに key=value):</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotEditDialog.qml" line="101"/>
        <source>e.g.
important=yes
reason=manual</source>
        <translation>例:
important=yes
reason=manual</translation>
    </message>
</context>
<context>
    <name>SnapshotListModel</name>
    <message>
        <location filename="../src/snapshotlistmodel.cpp" line="286"/>
        <source>Failed to modify snapshot #%1</source>
        <translation>スナップショット #%1 の変更に失敗しました</translation>
    </message>
    <message>
        <location filename="../src/snapshotlistmodel.cpp" line="352"/>
        <source>No valid snapshot numbers were selected.</source>
        <translation>有効なスナップショット番号が選択されていません</translation>
    </message>
</context>
<context>
    <name>SnapshotListPage</name>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="20"/>
        <source>Snapshot List</source>
        <translation>スナップショット一覧</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="77"/>
        <source>Snapshot created successfully</source>
        <translation>スナップショットが作成されました</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="88"/>
        <source>Snapshot #%1 deleted</source>
        <translation>スナップショット #%1 を削除しました</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="93"/>
        <source>Failed to delete snapshot #%1: %2</source>
        <translation>スナップショット #%1 の削除に失敗しました: %2</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="103"/>
        <source>Deleted %1 snapshot(s)</source>
        <translation>%1個のスナップショットを削除しました</translation>
    </message>
    <message>
        <source>Deletion completed: %1 succeeded, %2 failed</source>
        <translation type="vanished">削除完了: 成功 %1個、失敗 %2個</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="150"/>
        <source>qSnapper</source>
        <translation>qSnapper</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="209"/>
        <source>%1 selected</source>
        <translation>%1個選択中</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="216"/>
        <source>Refresh</source>
        <translation>更新</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="227"/>
        <source>Delete Selected</source>
        <translation>選択を削除</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="237"/>
        <location filename="../qml/pages/SnapshotListPage.qml" line="392"/>
        <source>Create Snapshot</source>
        <translation>スナップショット作成</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="157"/>
        <source>Config:</source>
        <translation>設定:</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="245"/>
        <source>Modify</source>
        <translation>編集</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="265"/>
        <source>Compare</source>
        <translation>比較</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="525"/>
        <source>User data (key=value per line):</source>
        <translation>ユーザデータ (1行ごとに key=value):</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="544"/>
        <source>e.g.
important=yes
reason=manual-test</source>
        <translation>例:
important=yes
reason=manual-test</translation>
    </message>
    <message>
        <source>Dark Mode</source>
        <translation type="vanished">ダークモード</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="105"/>
        <source>Deleted %1 of %2 snapshots. %3 could not be deleted.</source>
        <translation>%2件中%1件のスナップショットを削除しました。%3件は削除できませんでした</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="116"/>
        <source>Failed to delete snapshots: %1</source>
        <translation>スナップショットを削除できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="288"/>
        <source>Total Snapshots: %1</source>
        <translation>スナップショット総数: %1</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="323"/>
        <source>No snapshots available</source>
        <translation>スナップショットがありません</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="362"/>
        <source>About qSnapper</source>
        <translation>qSnapper について</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="373"/>
        <source>Qt License</source>
        <translation>Qt ライセンス</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="420"/>
        <source>Description:</source>
        <translation>説明:</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="427"/>
        <source>Enter snapshot description</source>
        <translation>スナップショットの説明を入力してください</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="438"/>
        <source>Single Snapshot</source>
        <translation>単一スナップショット</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="439"/>
        <source>Pre Snapshot</source>
        <translation>プリスナップショット</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="440"/>
        <source>Post Snapshot</source>
        <translation>Postスナップショット</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="465"/>
        <source>Pre snapshot can be paired with post snapshot later</source>
        <translation>プリスナップショットは後でポストスナップショットとペアにできます</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="475"/>
        <source>To create a post snapshot, first select a pre snapshot that has no post snapshot.</source>
        <translation>Postスナップショットを作成するには、Postスナップショットを持たないPreスナップショットを先に選択してください</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="484"/>
        <source>This snapshot will be paired with pre snapshot #%1</source>
        <translation>このスナップショットはPreスナップショット#%1と対になります</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="496"/>
        <source>Cleanup:</source>
        <translation>クリーンアップ:</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="506"/>
        <source>None</source>
        <translation>なし</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="516"/>
        <source>Important</source>
        <translation>重要</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="604"/>
        <source>Please enter a description</source>
        <translation>説明を入力してください</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="621"/>
        <source>Select a pre snapshot that has no post snapshot to create a post snapshot</source>
        <translation>Postスナップショットを作成するには、Postスナップショットを持たないPreスナップショットを選択してください</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="649"/>
        <source>Confirmation</source>
        <translation>確認</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="670"/>
        <source>Delete selected snapshot?</source>
        <translation>選択したスナップショットを削除しますか？</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="671"/>
        <source>Delete %1 snapshots?</source>
        <translation>%1個のスナップショットを削除しますか？</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="679"/>
        <source>This operation cannot be undone.</source>
        <translation>この操作は元に戻せません。</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="687"/>
        <source>Note: This operation requires administrator privileges.</source>
        <translation>注意: この操作には管理者権限が必要です。</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="742"/>
        <source>Error</source>
        <translation>エラー</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="709"/>
        <source>Please wait</source>
        <translation>しばらくお待ちください</translation>
    </message>
    <message>
        <source>Authentication failed.</source>
        <translation type="vanished">認証に失敗しました。</translation>
    </message>
    <message>
        <location filename="../qml/pages/SnapshotListPage.qml" line="730"/>
        <source>Deleting snapshots, please wait...</source>
        <translation>スナップショットを削除しています。しばらくお待ちください...</translation>
    </message>
</context>
<context>
    <name>SnapshotTableHeader</name>
    <message>
        <location filename="../qml/components/SnapshotTableHeader.qml" line="38"/>
        <source>ID</source>
        <translation>ID</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotTableHeader.qml" line="48"/>
        <source>Type</source>
        <translation>種類</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotTableHeader.qml" line="70"/>
        <source>Important snapshot</source>
        <translation>重要なスナップショット</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotTableHeader.qml" line="75"/>
        <source>Start Date</source>
        <translation>開始日</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotTableHeader.qml" line="85"/>
        <source>End Date</source>
        <translation>終了日</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotTableHeader.qml" line="95"/>
        <source>Description</source>
        <translation>説明</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotTableHeader.qml" line="105"/>
        <source>User Data</source>
        <translation>ユーザデータ</translation>
    </message>
</context>
<context>
    <name>SnapshotTableRow</name>
    <message>
        <location filename="../qml/components/SnapshotTableRow.qml" line="105"/>
        <source>Single</source>
        <translation>単一</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotTableRow.qml" line="107"/>
        <source>Pre and Post</source>
        <translation>事前および事後</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotTableRow.qml" line="109"/>
        <source>Pre</source>
        <translation>事前</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotTableRow.qml" line="111"/>
        <source>Post</source>
        <translation>事後</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotTableRow.qml" line="154"/>
        <source>Important snapshot</source>
        <translation>重要なスナップショット</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotTableRow.qml" line="179"/>
        <source>(No description)</source>
        <translation>(説明なし)</translation>
    </message>
    <message>
        <location filename="../qml/components/SnapshotTableRow.qml" line="208"/>
        <source>Details</source>
        <translation>詳細</translation>
    </message>
</context>
<context>
    <name>TimerSettingsSection</name>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="26"/>
        <source>Systemd timers</source>
        <translation>systemd のタイマー</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="36"/>
        <source>Timeline snapshots (hourly)</source>
        <translation>タイムラインのスナップショット (毎時)</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="38"/>
        <source>Snapshot cleanup (daily)</source>
        <translation>スナップショットのクリーンアップ (毎日)</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="40"/>
        <source>Snapshot at boot</source>
        <translation>起動時のスナップショット</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="72"/>
        <source>These timers are shared by all snapper configs. Changing them affects every config, not only the selected one.</source>
        <translation>これらのタイマーは、すべての snapper の設定で共有されます。変更すると、選択中の設定だけでなく、すべての設定に影響します。</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="119"/>
        <source>Autostart (unit file state): %1</source>
        <translation>自動起動 (ユニットファイルの状態): %1</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="120"/>
        <source>Not available on this system</source>
        <translation>このシステムでは利用できません</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="127"/>
        <source>Current state (active state): %1</source>
        <translation>現在の状態 (稼働状態): %1</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="133"/>
        <source>This state cannot be changed here.</source>
        <translation>この状態は、ここでは変更できません。</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="148"/>
        <source>Disable and stop</source>
        <translation>無効にして停止</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="148"/>
        <source>Enable and start</source>
        <translation>有効にして開始</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="159"/>
        <source>Timer states are not available.</source>
        <translation>タイマーの状態を取得できません。</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="185"/>
        <source>Refresh</source>
        <translation>更新</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="201"/>
        <source>Enabled and started %1.</source>
        <translation>%1 を有効にして開始しました。</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="202"/>
        <source>Disabled and stopped %1.</source>
        <translation>%1 を無効にして停止しました。</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="207"/>
        <source>Failed to change %1: %2</source>
        <translation>%1 の変更に失敗しました: %2</translation>
    </message>
    <message>
        <location filename="../qml/components/TimerSettingsSection.qml" line="212"/>
        <source>Failed to get timer states: %1</source>
        <translation>タイマーの状態の取得に失敗しました: %1</translation>
    </message>
</context>
</TS>
