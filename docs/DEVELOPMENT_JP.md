# qSnapper - 開発

[READMEへ戻る](../README_JP.md) | [English](DEVELOPMENT.md)

## 開発

### 開発用ビルド

```bash
mkdir build-debug && cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

### プロジェクト構造

```
qSnapper/
├── CMakeLists.txt                                  # トップレベルのビルド設定 (GUI、D-Busサービス、テスト、パッケージング)
├── qsnapper.desktop.in                             # デスクトップエントリのテンプレート
├── LICENSE.md                                      # プロジェクトのライセンス
├── README.md / README_JP.md                        # 概要 (英語 / 日本語)
├── docs/                                           # 詳細ドキュメント (英語 / 日本語)
│   ├── INSTALL.md / INSTALL_JP.md                  # 要件とインストール
│   ├── USAGE.md / USAGE_JP.md                      # 使用方法と設定
│   ├── TROUBLESHOOTING.md / TROUBLESHOOTING_JP.md  # トラブルシューティング
│   └── DEVELOPMENT.md / DEVELOPMENT_JP.md          # 開発とプロジェクト構造
├── .github/workflows/                              # CIワークフロー
│   └── release.yml                                 # CI: テスト、RPM/DEBビルド、バージョンタグでのリリース
├── src/                                            # C++ソースファイル
│   ├── main.cpp                                    # GUIアプリケーションのエントリポイント
│   ├── snapperservice.cpp                          # D-Busサービスを呼び出すクライアント側のインターフェース
│   ├── fssnapshot.cpp                              # スナップショットのデータモデル (1件分)
│   ├── snapshotlistmodel.cpp                       # スナップショットのリストモデル
│   ├── snapshotgroupmodel.cpp                      # Pre/Postスナップショットのグループ化モデル
│   ├── configmanagermodel.cpp                      # Snapper設定のリストモデル
│   ├── snappersettingsmodel.cpp                    # 設定値とタイマー状態のモデル
│   ├── filesysteminfomodel.cpp                     # ファイルシステムの割り当て、使用量、サブボリュームのモデル
│   ├── filechangemodel_loading.cpp                 # ファイル変更の読み込み (libsnapperの比較入力)
│   ├── filechangemodel_model.cpp                   # ファイル変更のツリーモデル
│   ├── filechangemodel_restore.cpp                 # 復元計画のステージングと実行
│   ├── filechangemodel_transport.cpp               # 復元計画のD-Bus転送
│   ├── thememanager.cpp                            # テーマ管理 (ライト、ダーク、システム)
│   ├── windowstatemanager.cpp                      # ウィンドウサイズと最大化状態の永続化
│   ├── singleinstanceguard.cpp                     # 多重起動の防止 (ロックファイルとローカルソケット)
│   └── dbusservice/                                # 特権D-Busサービス (rootで動作)
│       ├── main.cpp                                # サービスのエントリポイント (オブジェクト登録、ログ、アイドル終了)
│       ├── snapshotoperations.{cpp,h}              # D-Busメソッドの入口とpolkit認可
│       ├── snapshotoperations_*.cpp                # メソッド群 (スナップショット、変更、設定、タイマー、復元)
│       ├── snapperdoperation.{cpp,h}               # 設定の作成 / 削除と一括削除でorg.opensuse.Snapperを呼び出す
│       ├── btrfsinfo.{cpp,h}                       # カーネルのBtrfs ioctlとlibbtrfsutilのラッパー
│       ├── systemdtimeroperation.{cpp,h}           # systemdタイマーの状態取得と有効化 / 無効化
│       ├── inputvalidator.{cpp,h}                  # D-Bus経由の信頼できない入力の検証
│       ├── configpathvalidation.{cpp,h}            # 設定名とサブボリュームパスの検証
│       ├── filesystemhelpers.{cpp,h}               # ルート配下での安全なファイル操作 (symlinkを辿らない)
│       ├── restoremanifest.{cpp,h}                 # 復元計画の管理 (呼び出し元への束縛、上限、保持期間)
│       ├── restoreplanexecutor.{cpp,h}             # 凍結済み復元計画の分割実行
│       ├── restorevalidation.{cpp,h}               # 復元先の組み立てと復元エントリの検証
│       └── comparisoncache.h                       # libsnapperの比較結果の1件キャッシュ
├── include/                                        # GUIアプリケーションのヘッダーファイル
│   ├── configmanagermodel.h                        # 設定のリストモデル
│   ├── csvrecord.h                                 # CSVのクォートと解析 (サービスとクライアントで共用)
│   ├── externalurlopener.h                         # http/httpsリンクをデスクトップポータル経由で開く
│   ├── filechangemodel.h                           # ファイル変更モデルと復元計画の転送
│   ├── filesysteminfomodel.h                       # ファイルシステム情報のモデル
│   ├── fssnapshot.h                                # スナップショットのデータモデル
│   ├── singleinstanceguard.h                       # 多重起動の防止
│   ├── snapperservice.h                            # クライアント側のD-Busインターフェース
│   ├── snappersettingsmodel.h                      # 設定値とタイマー状態のモデル
│   ├── snappertimerstate.h                         # タイマーユニットの状態
│   ├── snapshotgroupmodel.h                        # Pre/Postのグループ化モデル
│   ├── snapshotlistmodel.h                         # スナップショットのリストモデル
│   ├── thememanager.h                              # テーママネージャ
│   └── windowstatemanager.h                        # ウィンドウ状態の管理
├── qml/                                            # QMLユーザインターフェース
│   ├── Main.qml                                    # スナップショット、設定、ファイルシステムのタブを持つメインウィンドウ
│   ├── pages/                                      # ページコンポーネント
│   │   ├── SnapshotListPage.qml                    # スナップショット一覧ページ
│   │   ├── SettingsPage.qml                        # 設定の編集、タイマー、外観
│   │   └── FilesystemPage.qml                      # Btrfsの割り当て、使用量、サブボリューム
│   └── components/                                 # 再利用可能なコンポーネント
│       ├── AboutQtDialog.qml                       # Qtについてのダイアログ
│       ├── AboutqSnapperDialog.qml                 # qSnapperについてのダイアログ
│       ├── BorderedDialog.qml                      # テーマに追従する枠付きダイアログの基底型
│       ├── CompareSnapshotsDialog.qml              # 任意の2スナップショット間のファイル差分
│       ├── CreateConfigDialog.qml                  # Snapper設定の作成
│       ├── DeleteConfigDialog.qml                  # 名前入力による確認付きの設定削除
│       ├── RestorePreviewDialog.qml                # 復元のプレビュー、オプション、進捗
│       ├── SettingsFieldRow.qml                    # 設定値1項目の編集行
│       ├── SnapshotDetailDialog.qml                # スナップショットの詳細、ファイル復元、ロールバック
│       ├── SnapshotEditDialog.qml                  # スナップショットのメタデータ編集
│       ├── SnapshotTableHeader.qml                 # スナップショットテーブルのヘッダ行
│       ├── SnapshotTableRow.qml                    # スナップショットテーブルの行デリゲート
│       └── TimerSettingsSection.qml                # systemdタイマーの状態と有効化 / 無効化
├── dbus/                                           # D-Bus設定ファイル
│   ├── com.presire.qsnapper.Operations.conf        # バスポリシー (許可するメソッド)
│   ├── com.presire.qsnapper.Operations.service.in  # サービス起動定義のテンプレート
│   └── com.presire.qsnapper.Operations.xml         # インターフェース定義 (introspection XML)
├── polkit/                                         # PolicyKitポリシー
│   └── com.presire.qsnapper.policy                 # 認可アクションの定義
├── systemd/tmpfiles.d/                             # systemd tmpfiles設定
│   └── qsnapper.conf                               # ログディレクトリ /var/log/qsnapper の作成 (0700)
├── selinux/                                        # SELinuxポリシーモジュール
│   ├── qsnapper.te                                 # Type Enforcementルール
│   ├── qsnapper.if                                 # インターフェース定義
│   ├── qsnapper.fc.in                              # ファイルコンテキストのテンプレート (CMakeが生成)
│   ├── qsnapper.fc                                 # 単体のMakefileビルド用ファイルコンテキスト
│   ├── CMakeLists.txt                              # ポリシーのビルド (CMake)
│   ├── Makefile                                    # ポリシーのビルド (単体)
│   ├── ADMIN.md / ADMIN_JP.md                      # 管理者ガイド (英語 / 日本語)
│   ├── README.md / README_JP.md                    # ポリシーの概要 (英語 / 日本語)
│   └── qsnapper-architecture.*                     # アーキテクチャ図 (.drawio / .png)
├── cmake/                                          # CMakeの補助ファイルとパッケージング用スクリプト
│   ├── packaging.cmake                             # CPackパッケージング設定 (RPM/DEB)
│   ├── rpm-post-install.sh                         # RPMのインストール後処理 (SELinuxモジュールのロード、ログディレクトリ作成)
│   └── rpm-pre-uninstall.sh                        # RPMのアンインストール前処理 (SELinuxモジュールの削除)
├── man/                                            # manページのソース (scdoc)
│   └── qsnapper.1.scd                              # qsnapper(1) manページ
├── icons/                                          # アプリケーションとUIのアイコン
├── translations/                                   # 翻訳ファイル
│   ├── qsnapper_ja.ts                              # 日本語翻訳
│   └── qsnapper_de.ts                              # ドイツ語翻訳
├── tests/                                          # テスト (QSNAPPER_BUILD_TESTS=ONで有効)
│   ├── unit/                                       # Qt Testの単体テスト (コンポーネントごとにtst_*.cpp)
│   ├── integration/                                # D-Bus契約の検査と隔離D-Bus環境のテスト
│   ├── security_poc/                               # 脆弱性のPoCスクリプト (専用VMでのみ実行)
│   └── TESTREPORT_v1.3.3.md                        # テストレポートのテンプレート
├── Licenses/                                       # サードパーティのライセンス
│   ├── Qt.md, D-Bus.md, PolicyKit.md               # ライセンス本文 (Qt、D-Bus、PolicyKit)
│   └── Polkit-Qt.md, Snapper.md, Btrfs-progs.md    # ライセンス本文 (Polkit-Qt、Snapper、Btrfs-progs)
└── ScreenShot/                                     # READMEで使用するスクリーンショット
```
