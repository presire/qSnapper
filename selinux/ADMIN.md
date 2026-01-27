# qSnapper SELinux Policy Module - 管理者ガイド

このドキュメントは、qSnapperのSELinuxポリシーモジュールを運用・管理するシステム管理者向けの詳細情報を提供します。  

## 目次

- [アーキテクチャ概要](#アーキテクチャ概要)
- [セキュリティモデル](#セキュリティモデル)
- [ポリシー詳細](#ポリシー詳細)
- [ポリシーカスタマイズ](#ポリシーカスタマイズ)
- [監査とロギング](#監査とロギング)
- [パフォーマンス影響](#パフォーマンス影響)
- [既知の制約と考慮事項](#既知の制約と考慮事項)
- [高度なトラブルシューティング](#高度なトラブルシューティング)

---

## アーキテクチャ概要

### 2プロセスモデル

qSnapperは、権限分離の原則に基づいた2プロセスアーキテクチャを採用しています:  

```
┌─────────────────────────────────────────────────────────┐
│ User Space (non-privileged)                                                                   │
│                                                                                               │
│  ┌──────────────────┐                                                            │
│  │ qsnapper                     │  Domain: qsnapper_t                                        │
│  │ (Qt6/QML GUI)                │  User: <regular user>                                      │
│  └────────┬─────────┘  Capabilities: (none)                                      │
│                  │                                                                            │
│                  │ D-Bus system bus                                                           │
│                  ↓                                                                            │
└───────────┼─────────────────────────────────────────────┘
                    │
                    │ PolicyKit authorization check
                    │
┌───────────┼──────────────────────────────────────────────┐
│ System Space (privileged)                                                                       │
│           ↓                                                                                    │
│  ┌──────────────────────┐                                                       │
│  │ qsnapper-dbus-service               │  Domain: qsnapper_dbus_t                              │
│  │ (D-Bus daemon)                      │  User: root                                           │
│  └──────────┬───────────┘  Capabilities: sys_admin, etc.                        │
│                    │                                                                           │
│                    ↓ libsnapper C++ API                                                        │
│  ┌──────────────────────┐                                                       │
│  │ Btrfs filesystem                    │                                                      │
│  │ (/.snapshots)                       │                                                      │
│  └──────────────────────┘                                                       │
└──────────────────────────────────────────────────────────┘
```

### ドメイン分離のメリット

1. **最小権限原則**: 各プロセスは必要最小限の権限のみを持つ  
2. **攻撃面の削減**: GUI層の脆弱性が特権操作に直接影響しない  
3. **監査の粒度**: 各ドメインのアクションを独立して追跡可能  
4. **ポリシー適用の柔軟性**: ドメインごとに異なるセキュリティポリシーを適用可能  

---

## セキュリティモデル

### 強制アクセス制御(MAC)レイヤー

qSnapperのセキュリティは、以下の3層で構成されています:  

```
Layer 3: SELinux MAC (this policy)
         ↓
Layer 2: PolicyKit authorization
         ↓
Layer 1: D-Bus security policy
         ↓
         Filesystem operations
```

#### Layer 1: D-Bus Security Policy

ファイル: `/usr/share/dbus-1/system.d/com.presire.qsnapper.Operations.conf`  

- D-Busシステムバスへの接続許可
- メソッド呼び出し許可の制御

#### Layer 2: PolicyKit Authorization

ファイル: `/usr/share/polkit-1/actions/com.presire.qsnapper.policy`  

アクションごとの認証要件:
- `com.presire.qsnapper.list-snapshots`: 認証不要(allow_active)
- `com.presire.qsnapper.create-snapshot`: 認証必要
- `com.presire.qsnapper.delete-snapshot`: 認証必要
- `com.presire.qsnapper.rollback-snapshot`: 認証必要
- `com.presire.qsnapper.get-file-changes`: 認証不要
- `com.presire.qsnapper.restore-files`: 認証必要

#### Layer 3: SELinux MAC (This Policy)

- プロセスごとのドメイン分離
- ファイル・ディレクトリアクセスの厳格な制御
- ioctl操作の制限
- ネットワークアクセスの制御

### 認証フロー

```
User action (GUI)
  → D-Bus method call
    → D-Bus policy check (Layer 1)
      → PolicyKit authorization (Layer 2)
        → SELinux domain transition check (Layer 3)
          → SELinux file access check (Layer 3)
            → Operation execution
```

**すべてのレイヤーをパスした場合のみ、操作が実行されます。**  

---

## ポリシー詳細

### qsnapper_t ドメイン(GUIアプリケーション)

#### 許可される操作

1. **プロセス管理**  
   - 自己プロセスへのシグナル送信
   - スケジューラパラメータの取得・設定
   - FIFO、UNIXソケット使用

2. **GUI要件**  
   - Qt6 Quick/QML実行環境
   - フォントファイルアクセス
   - ローカライゼーションファイル読み取り
   - X11/Waylandディスプレイサーバー接続

3. **D-Bus通信**  
   - システムバス接続(クライアント)
   - `qsnapper_dbus_t`ドメインとのメッセージ送受信

4. **ファイルアクセス**  
   - Snapper設定ファイル読み取り(`/etc/snapper`, `/var/lib/snapper`)
   - スナップショットディレクトリブラウズ(`/.snapshots`)
   - ユーザーホーム設定管理(`~/.config/Presire/`)
   - 一時ファイル作成(`/tmp`)

#### 禁止される操作

- ✗ スナップショットディレクトリへの書き込み
- ✗ Snapper設定ファイルへの書き込み
- ✗ Btrfs ioctl操作
- ✗ root権限が必要な操作

### qsnapper_dbus_t ドメイン(D-Busサービス)

#### 許可される操作

1. **Linux Capabilities**  
   - `CAP_SYS_ADMIN`: Btrfsスナップショット操作
   - `CAP_DAC_OVERRIDE`: 所有権チェックのバイパス
   - `CAP_DAC_READ_SEARCH`: 読み取り権限チェックのバイパス
   - `CAP_FOWNER`: ファイル所有権変更
   - `CAP_CHOWN`: ファイル所有者変更
   - `CAP_FSETID`: setuid/setgidビット設定
   - `CAP_SYS_RESOURCE`: リソース制限超過

2. **D-Bus通信**  
   - システムバス接続(サービス)
   - PolicyKitデーモンとの通信

3. **ファイルアクセス**  
   - Snapper設定ファイル読み書き(`/etc/snapper`, `/var/lib/snapper`)
   - スナップショットディレクトリ完全管理(`/.snapshots`)
   - 全ファイル読み取り(diff操作用)
   - tunable制御された全ファイル書き込み(復元操作用)

4. **Btrfs ioctl操作**  
   - `BTRFS_IOC_SNAP_CREATE_V2`: スナップショット作成
   - `BTRFS_IOC_SNAP_DESTROY`: スナップショット削除
   - `BTRFS_IOC_DEFAULT_SUBVOL`: デフォルトサブボリューム設定
   - その他Btrfs関連ioctl(0x9400-0x94ff範囲)

5. **外部コマンド実行**  
   - `/usr/bin/diff`: ファイル比較
   - `/usr/bin/snapper`: Snapper CLIツール

#### 制限される操作

- ファイル復元: `qsnapper_manage_all_snapshots` tunableで制御
  - ON(デフォルト): 任意のファイルを復元可能
  - OFF: ユーザーホームディレクトリのみ復元可能

---

## ポリシーカスタマイズ

### カスタムスナップショット場所の追加

デフォルトでは`/.snapshots`のみがスナップショットストレージとしてラベル付けされています。  
別の場所を使用する場合:  

```bash
# 例: /mnt/btrfs-snapshots を追加
sudo semanage fcontext -a -t qsnapper_snapshot_t "/mnt/btrfs-snapshots(/.*)?"
sudo restorecon -R -v /mnt/btrfs-snapshots
```

複数の場所を追加:  

```bash
sudo semanage fcontext -a -t qsnapper_snapshot_t "/home/.snapshots(/.*)?"
sudo semanage fcontext -a -t qsnapper_snapshot_t "/data/.snapshots(/.*)?"
sudo restorecon -R -v /home/.snapshots /data/.snapshots
```

### カスタムSnapper設定場所

Snapperが標準的でない設定場所を使用する場合:  

```bash
sudo semanage fcontext -a -t qsnapper_conf_t "/opt/snapper/config(/.*)?"
sudo restorecon -R -v /opt/snapper/config
```

### ポリシーモジュールの再コンパイル

ソースコード(`.te`ファイル)を編集した場合:  

```bash
cd /path/to/qSnapper/selinux

# 構文チェック
make check

# 再ビルド
make clean
make

# 更新されたモジュールをインストール
sudo semodule -r qsnapper
sudo semodule -i qsnapper.pp
```

---

## 監査とロギング

### AVC(Access Vector Cache) denial監視

#### リアルタイム監視

```bash
# すべてのAVC denialをリアルタイム表示
sudo ausearch -m avc -ts today | tail -f

# qSnapper関連のみ
sudo ausearch -m avc -c qsnapper -c qsnapper-dbus-service -ts recent
```

#### 統計情報の取得

```bash
# 過去24時間のAVC denial件数
sudo ausearch -m avc -ts today | grep -c denied

# 最も頻繁にdenialされる操作の特定
sudo ausearch -m avc -ts today | \
    grep denied | \
    awk '{print $NF}' | \
    sort | uniq -c | sort -rn | head -10
```

### 正常動作の監査ログ

SELinuxは拒否だけでなく、許可された操作も記録できます(auditallow):  

```bash
# ポリシーに auditallow ルールを追加(例)
# auditallow qsnapper_dbus_t qsnapper_snapshot_t:dir { create rmdir };

# 再コンパイル後、操作を監査
sudo ausearch -m avc -c qsnapper-dbus-service | grep granted
```

### ログ分析ツール

#### sealert (setroubleshoot)

より人間が読みやすい形式でdenialを表示:  

```bash
# インストール(RHEL/Fedora)
sudo dnf install setroubleshoot-server

# インストール(openSUSE)
sudo zypper install setroubleshoot-server

# 最近のdenialを分析
sudo sealert -a /var/log/audit/audit.log
```

#### audit2allow

denialから必要なポリシールールを推奨:  

```bash
# 推奨ルールを表示
sudo ausearch -m avc -c qsnapper -ts recent | audit2allow -R

# ローカルポリシーモジュールとして生成
sudo ausearch -m avc -c qsnapper -ts recent | audit2allow -M qsnapper-local

# インストール(一時的な回避策)
sudo semodule -i qsnapper-local.pp
```

---

## パフォーマンス影響

### SELinux有効化のオーバーヘッド

一般的なSELinuxのパフォーマンス影響:  

- **ファイルアクセス**: 1-3%のオーバーヘッド(ラベルチェック)
- **プロセス起動**: 2-5%のオーバーヘッド(ドメイン遷移)
- **システムコール**: 1-2%のオーバーヘッド(アクセス決定)

qSnapperの場合、以下の操作で影響が現れる可能性があります:

- **スナップショット一覧表示**: ファイルラベルチェックによる軽微な遅延(通常無視できる)
- **ファイル比較**: 全ファイル読み取り権限チェック(数百ファイル以上で数%のオーバーヘッド)
- **スナップショット作成**: Btrfs ioctl権限チェック(無視できる程度)

### ベンチマーク方法

SELinuxの影響を測定:

```bash
# SELinux有効(Enforcingモード)でベンチマーク
time qsnapper-dbus-service --test-operation

# SELinux無効(Permissiveモード)でベンチマーク
sudo setenforce 0
time qsnapper-dbus-service --test-operation
sudo setenforce 1

# 差分を計算
```

### パフォーマンスチューニング

SELinuxのパフォーマンスを最適化するには:  

1. **dontauditルールの活用**  
   - 頻繁に発生するが無害なdenialをログに記録しない
   - ポリシー内で既に適用済み

2. **AVC Cacheの最適化**  
   ```bash
   # AVC統計確認
   cat /sys/fs/selinux/avc/cache_stats

   # キャッシュサイズ調整(必要に応じて)
   echo 1024 > /sys/fs/selinux/avc/cache_threshold
   ```

3. **監査ログの最適化**  
   ```bash
   # auditdの設定を調整
   sudo vi /etc/audit/auditd.conf
   # log_format = ENRICHED から RAW に変更(軽量化)
   ```

---

## 既知の制約と考慮事項

### 1. 既存snapperポリシーとの共存

システムに既存の`snapper_t`ドメインが存在する場合、競合する可能性があります。  

**確認方法:**  
```bash
sudo semodule -l | grep snapper
```

**対処法:**  
- 既存ポリシーが存在する場合、qSnapperポリシー内の`optional_policy`ブロックが自動的に連携を試みます
- 競合が発生する場合は、既存ポリシーの無効化またはqSnapperポリシーの調整が必要

### 2. Btrfs ioctl番号の変動

Btrfs ioctlの番号は、カーネルバージョンによって変わる可能性があります。  

**現在の実装:**  
```selinux
allowxperm qsnapper_dbus_t qsnapper_snapshot_t:dir ioctl {
    0x9400-0x94ff  # Btrfs ioctl範囲
};
```

**問題が発生した場合:**  
```bash
# 実際に使用されているioctl番号を確認
sudo ausearch -m avc -c qsnapper-dbus-service | grep ioctl

# ポリシーを調整してioctlpermsを追加
```

### 3. カーネルLSM(Linux Security Module)スタック

最近のカーネル(5.x以降)では、複数のLSMを同時に有効化できます:  

```bash
# 現在のLSMスタック確認
cat /sys/kernel/security/lsm

# 期待される出力例(openSUSE Leap 16):
# capability,selinux,bpf
```

AppArmorとSELinuxは同時に有効化できないため、openSUSE Leap 16ではSELinuxが標準採用されています。  

### 4. openSUSE Leap 16固有の考慮事項

openSUSE Leap 16でSELinuxが新規採用されたため:  

- 既存のAppArmorポリシーとの重複はありません
- ただし、`snapper`パッケージ自体のSELinuxポリシーが将来提供される可能性があります
- 定期的に`sudo semodule -l | grep snapper`で確認することを推奨

### 5. RHEL/Fedora互換性

RHEL/FedoraとopenSUSEでポリシーインターフェース名が異なる場合があります:  

**現在のポリシーでの対応:**  
- `optional_policy`ブロックを使用して、存在しないインターフェースを無視
- ディストリビューション間で可能な限り共通のインターフェースを使用

**互換性の問題が発生した場合:**  
- ディストリビューション固有のifdef的な条件分岐を`.te`ファイルに追加
- または、ディストリビューションごとに別々のポリシーを提供

---

## 高度なトラブルシューティング

### Permissiveドメインの活用

特定のドメインのみをPermissiveモードにして、他はEnforcingのまま維持:  

```bash
# qsnapper_dbus_tドメインのみPermissive化
sudo semanage permissive -a qsnapper_dbus_t

# 操作をテスト(denialがログに記録されるが許可される)
qsnapper # 操作を実行

# AVC denialを確認
sudo ausearch -m avc -c qsnapper-dbus-service -ts recent | audit2allow -R

# Enforcing に戻す
sudo semanage permissive -d qsnapper_dbus_t
```

### ドメイン遷移のデバッグ

ドメイン遷移が正しく行われているか確認:  

```bash
# 遷移前のドメイン
id -Z

# アプリケーション起動
qsnapper &
PID=$!

# 遷移後のドメイン
ps -eZ | grep $PID

# 期待: qsnapper_t ドメイン
```

遷移が行われない場合:  

```bash
# 実行ファイルのコンテキスト確認
ls -Z /usr/bin/qsnapper

# 遷移ルールの確認
sudo sesearch -A -s unconfined_t -t qsnapper_t -c process -p transition

# ファイルコンテキストの再適用
sudo restorecon -F -v /usr/bin/qsnapper
```

### ファイルコンテキストの完全再ラベル

システム全体のファイルコンテキストを再ラベル(最終手段):  

```bash
# リブート時にすべてのファイルを再ラベル
sudo touch /.autorelabel
sudo reboot

# リブート後、時間がかかるため注意(大規模システムで数時間)
```

### ポリシーソースの取得とカスタマイズ

システムのベースポリシーを取得してカスタマイズ:

**openSUSE:**
```bash
# ベースポリシーソースをインストール
sudo zypper install selinux-policy-targeted-src

# ソース場所
ls /usr/src/selinux-policy/
```

**RHEL/Fedora:**
```bash
# ベースポリシーソースをインストール
sudo dnf install selinux-policy-targeted-sources

# ソース場所
ls /usr/share/selinux/devel/
```

---

## セキュリティベストプラクティス

### 1. 最小権限原則の維持

ポリシーをカスタマイズする際は、必要最小限の権限のみを付与してください。  

**悪い例:**  
```selinux
# すべてのファイルへの書き込みを許可(危険!)
allow qsnapper_dbus_t file_type:file write;
```

**良い例:**  
```selinux
# 特定のタイプのファイルのみ書き込み許可
allow qsnapper_dbus_t qsnapper_snapshot_t:file write;
```

### 2. 監査ログの定期レビュー

毎週/毎月、AVC denialログをレビューして異常な動作を検出:  

```bash
# 週次レポート生成
sudo ausearch -m avc -ts this-week > /var/log/selinux-weekly-report.txt
```

### 3. ポリシー更新の追跡

qSnapperの更新時に、SELinuxポリシーも更新されているか確認:  

```bash
# 現在のポリシーバージョン確認
sudo semodule -l | grep qsnapper

# 更新後、ポリシーバージョンが変わっているか確認
```

### 4. ブール値の慎重な変更

ブール値を変更する際は、セキュリティ影響を理解してから実施:  

```bash
# 変更前に影響を確認
sesearch -A -b qsnapper_manage_all_snapshots

# 変更を恒久化する場合のみ -P フラグを使用
sudo setsebool -P qsnapper_manage_all_snapshots off
```

---

## サポート

このポリシーに関する問題や質問は、以下のリソースを参照してください:  

- **プロジェクトリポジトリ**: https://github.com/presire/qSnapper
- **Issue報告**: https://github.com/presire/qSnapper/issues
- **ユーザーガイド**: [README.md](README.md)

**報告時に含めるべき情報:**  
- ディストリビューションとバージョン(例: openSUSE Leap 16)
- SELinuxモード(`getenforce`の出力)
- AVC denialログ(`ausearch -m avc -ts recent`)
- ポリシーバージョン(`semodule -l | grep qsnapper`)

---

このドキュメントは、qSnapperプロジェクトの一部として提供されます。  
ライセンス: GPL-3.0  
