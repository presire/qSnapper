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

![2プロセスモデル](qsnapper-architecture.png)

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
- ioctl操作の制限(Btrfs ioctl範囲のみ許可)

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

### 型宣言

ポリシーモジュール(`qsnapper.te`)で宣言される型:  

| 型名 | 属性 | 用途 |
|---|---|---|
| `qsnapper_t` | `domain` | GUIアプリケーションプロセスドメイン |
| `qsnapper_exec_t` | `exec_type`, `file_type` | GUIアプリケーション実行ファイル |
| `qsnapper_dbus_t` | `domain` | D-Busサービスプロセスドメイン |
| `qsnapper_dbus_exec_t` | `exec_type`, `file_type` | D-Busサービス実行ファイル |
| `qsnapper_tmp_t` | `file_type` | 一時ファイル(`/tmp`内に自動遷移) |
| `qsnapper_tmpfs_t` | `file_type` | tmpfs共有メモリファイル |

### ファイルコンテキスト

ファイルコンテキスト定義(`qsnapper.fc.in`)は、2つの実行ファイルのみにカスタムラベルを付与します:  

```
/usr/bin/qsnapper                       -- system_u:object_r:qsnapper_exec_t:s0
/usr/libexec/qsnapper-dbus-service      -- system_u:object_r:qsnapper_dbus_exec_t:s0
```

**重要:**  
システムディレクトリ(`/etc/snapper`, `/.snapshots`, `/var/lib/snapper`等)には独自のラベルを付与せず、  
既存のシステムラベル(`etc_t`, `unlabeled_t`, `fs_t`, `var_lib_t`等)をそのまま使用します。  
これはopenSUSEの既存ファイルコンテキストルールとの競合を回避するための設計判断です。  

### qsnapper_t ドメイン(GUIアプリケーション)

#### 許可される操作

1. **プロセス管理**
   - 自己プロセスへのシグナル送信(`fork`, `signal`, `signull`, `sigkill`, `sigstop`, `sigchld`)
   - プロセスグループ操作(`setpgid`, `getpgid`)
   - スケジューラパラメータの取得・設定(`getsched`, `setsched`)
   - FIFO、UNIXストリームソケット、UNIXデータグラムソケット
   - 共有メモリ(SHM)操作
   - セマフォ操作

2. **GUI要件**
   - Qt6 Quick/QML実行環境(`usr_t`経由でQtプラグイン、アイコン、テーマにアクセス)
   - フォントファイルアクセス(`fonts_t`, `fonts_cache_t`)
   - ローカライゼーションファイル読み取り(`locale_t`)
   - GPU描画アクセラレーション(`dri_device_t`へのioctl含む)
   - tmpfs共有メモリ(`tmpfs_t`でX11/Wayland連携)
   - デバイスアクセス(`null_device_t`, `zero_device_t`, `random_device_t`, `urandom_device_t`)

3. **D-Bus通信**
   - システムバス接続(クライアント: `system_dbusd_t`への`send_msg`)
   - `qsnapper_dbus_t`ドメインとのメッセージ送受信

4. **ファイルアクセス**
   - `/etc`以下の設定ファイル読み取り(`etc_t` - `/etc/snapper`含む、読み取り専用)
   - `/var/lib`以下のSnapperメタデータ読み取り(`var_lib_t` - 読み取り専用)
   - スナップショットディレクトリブラウズ(`unlabeled_t`, `fs_t` - `/.snapshots`、読み取り専用)
   - ユーザーホームディレクトリの設定管理(`user_home_t`, `user_home_dir_t` - `~/.config/Presire/`、読み書き)
   - 一時ファイル作成(`tmp_t`, `qsnapper_tmp_t`)
   - `/proc`, `/sys`ファイルシステムの読み取り(`proc_t`, `sysfs_t`)
   - カーネルsysctl値の読み取り(`sysctl_t`, `sysctl_kernel_t`)
   - 共有ライブラリのロード(`lib_t`, `ld_so_t`, `ld_so_cache_t`, `textrel_shlib_t`)

5. **その他**
   - syslogへのログ送信(`devlog_t`, `kernel_t`)
   - ファイルディスクリプタ継承(`init_t`, `unconfined_t`から)
   - 端末アクセス(`user_devpts_t`, `user_tty_device_t`)

#### 禁止される操作

- スナップショットディレクトリへの書き込み
- `/etc/snapper`設定ファイルへの書き込み
- Btrfs ioctl操作
- root権限(Linux capabilities)が必要な操作

### qsnapper_dbus_t ドメイン(D-Busサービス)

#### 許可される操作

1. **Linux Capabilities**
   - `CAP_SYS_ADMIN`: Btrfsスナップショット操作
   - `CAP_DAC_OVERRIDE`: 所有権チェックのバイパス
   - `CAP_DAC_READ_SEARCH`: 読み取り権限チェックのバイパス
   - `CAP_FOWNER`: ファイル所有権変更
   - `CAP_CHOWN`: ファイル所有者変更
   - `CAP_FSETID`: setuid/setgidビット設定
   - `CAP_SETUID`: UID変更
   - `CAP_SETGID`: GID変更
   - `CAP_SYS_RESOURCE`: リソース制限超過(`setrlimit`)

2. **D-Bus通信**
   - システムバス接続(サービス: `send_msg` + `acquire_svc`でサービス名を登録)
   - `qsnapper_t`ドメインとのメッセージ送受信

3. **ファイルアクセス**
   - `/etc`以下の設定ファイル読み書き(`etc_t` - `/etc/snapper`含む)
   - `/var/lib`以下のSnapperメタデータ読み書き(`var_lib_t` - ディレクトリ作成・削除含む)
   - スナップショットディレクトリ完全管理(`unlabeled_t`, `fs_t` - `/.snapshots`、作成・削除・マウント含む)
   - ファイル復元: **ユーザーホームディレクトリのみ**(`user_home_t`, `user_home_dir_t`)
   - ログファイル管理(`var_log_t` - 作成・書き込み・追記)
   - ランタイムデータ(`var_run_t`)
   - 一時ファイル作成(`tmp_t`, `qsnapper_tmp_t`)
   - 共有ライブラリのロード(`lib_t`, `ld_so_t`, `ld_so_cache_t`, `textrel_shlib_t`)
   - `/proc`, `/sys`ファイルシステムの読み取り(`proc_t`, `sysfs_t`)
   - カーネルsysctl値の読み取り(`sysctl_t`, `sysctl_kernel_t`)
   - ローカライゼーションファイル読み取り(`locale_t`)
   - ネットワーク設定ファイル読み取り(`net_conf_t` - ホスト名解決)

4. **Btrfs ioctl操作**
   - `unlabeled_t:dir`および`fs_t:dir`に対するBtrfs ioctl(0x9400-0x94ff範囲)

   ```selinux
   allowxperm qsnapper_dbus_t unlabeled_t:dir ioctl { 0x9400-0x94ff };
   allowxperm qsnapper_dbus_t fs_t:dir ioctl { 0x9400-0x94ff };
   ```

   この範囲には以下が含まれます:
   - `BTRFS_IOC_SNAP_CREATE_V2`: スナップショット作成
   - `BTRFS_IOC_SNAP_DESTROY`: スナップショット削除
   - `BTRFS_IOC_DEFAULT_SUBVOL`: デフォルトサブボリューム設定
   - その他Btrfs関連ioctl

5. **外部コマンド実行**
   - `bin_t`内の実行ファイル(`execute_no_trans`で同一ドメイン内で実行)
   - シェル(`shell_exec_t`)

6. **ブロックデバイスアクセス**
   - 固定ディスクデバイス(`fixed_disk_device_t` - Btrfs操作用)

7. **その他**
   - syslogへのログ送信(`devlog_t`, `kernel_t`)
   - ファイルディスクリプタ継承(`system_dbusd_t`から)
   - Netlinkルートソケット(`netlink_route_socket`)
   - `fs_t`に対するファイルシステム操作(`getattr`, `mount`, `unmount`)

#### ファイル復元の制限

ファイル復元操作は**ユーザーホームディレクトリのみ**に制限されています。
この制限は`.te`ポリシーにハードコードされたセキュリティ制約です。

```selinux
# File restoration - LIMITED TO USER HOME DIRECTORIES ONLY
allow qsnapper_dbus_t user_home_dir_t:dir { ... };
allow qsnapper_dbus_t user_home_t:dir { ... };
allow qsnapper_dbus_t user_home_t:file { ... };
allow qsnapper_dbus_t user_home_t:lnk_file { ... };
```

### ドメイン遷移

ポリシーは以下の2つのドメイン遷移を定義しています:

1. **`unconfined_t` → `qsnapper_t`** (GUIアプリケーション起動)

   ```selinux
   allow unconfined_t qsnapper_exec_t:file { getattr open read execute map };
   allow unconfined_t qsnapper_t:process transition;
   type_transition unconfined_t qsnapper_exec_t:process qsnapper_t;
   ```

2. **`system_dbusd_t` → `qsnapper_dbus_t`** (D-Busによるサービス起動)

   ```selinux
   allow system_dbusd_t qsnapper_dbus_exec_t:file { getattr open read execute map };
   allow system_dbusd_t qsnapper_dbus_t:process transition;
   type_transition system_dbusd_t qsnapper_dbus_exec_t:process qsnapper_dbus_t;
   ```

### ファイル型遷移

`/tmp`内に作成されるファイルは自動的に`qsnapper_tmp_t`に遷移します:

```selinux
type_transition qsnapper_t tmp_t:file qsnapper_tmp_t;
type_transition qsnapper_t tmp_t:dir qsnapper_tmp_t;
type_transition qsnapper_dbus_t tmp_t:file qsnapper_tmp_t;
type_transition qsnapper_dbus_t tmp_t:dir qsnapper_tmp_t;
```

### インターフェース定義(.if)

`qsnapper.if`は、他のSELinuxポリシーモジュールがqSnapperと連携するためのインターフェースを提供します。
これらのインターフェースは外部モジュール用であり、qSnapperの`.te`ポリシー自体では使用されていません。

| インターフェース | 用途 |
|---|---|
| `qsnapper_domtrans` | 指定ドメインから`qsnapper_t`へのドメイン遷移を許可 |
| `qsnapper_run` | ドメイン遷移の許可 + ロールへの`qsnapper_roles`属性付与 |
| `qsnapper_dbus_chat` | 指定ドメインと`qsnapper_t`間のD-Busメッセージ送受信を許可 |
| `qsnapper_read_config` | `qsnapper_conf_t`型の設定ファイル読み取りを許可 |
| `qsnapper_manage_config` | `qsnapper_conf_t`型の設定ファイル管理を許可 |
| `qsnapper_read_snapshots` | `qsnapper_snapshot_t`型のスナップショットファイル読み取りを許可 |
| `qsnapper_manage_snapshots` | `qsnapper_snapshot_t`型のスナップショットファイル管理を許可 |
| `qsnapper_dbus_domtrans` | 指定ドメインから`qsnapper_dbus_t`へのドメイン遷移を許可 |
| `qsnapper_dbus_service_chat` | 指定ドメインと`qsnapper_dbus_t`間のD-Busメッセージ送受信を許可 |
| `qsnapper_read_log` | `qsnapper_log_t`型のログファイル読み取りを許可 |
| `qsnapper_append_log` | `qsnapper_log_t`型のログファイルへの追記を許可 |
| `qsnapper_manage_log` | `qsnapper_log_t`型のログファイル管理を許可 |
| `qsnapper_admin` | qSnapper環境全体の管理(全型への管理アクセス) |

**注意:** `qsnapper_conf_t`, `qsnapper_snapshot_t`, `qsnapper_log_t`, `qsnapper_var_run_t`は`.if`のインターフェースで`gen_require`として参照されていますが、現在の`.te`ポリシーでは宣言されていません。
これらの型は、将来的にカスタムラベリングが必要になった場合に外部モジュールで宣言して使用するために設計されています。
現在のポリシーではシステム標準のラベル(`etc_t`, `unlabeled_t`, `fs_t`, `var_lib_t`, `var_log_t`)を使用しています。

---

## ポリシーカスタマイズ

### ファイルコンテキストについての注意

現在のポリシーは、スナップショットディレクトリ(`/.snapshots`)やSnapper設定(`/etc/snapper`)に対して  
システム標準のラベル(`unlabeled_t`, `fs_t`, `etc_t`)を使用しています。  
Btrfsサブボリュームは通常`unlabeled_t`または`fs_t`のラベルが自動的に付与されるため、カスタムスナップショット場所でも追加の設定なしで動作します。  

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
# ポリシーに auditallow ルールを追加する場合の例:
# auditallow qsnapper_dbus_t unlabeled_t:dir { create rmdir };

# 再コンパイル後、操作を監査
sudo ausearch -m avc -c qsnapper-dbus-service | grep granted
```

### ログ分析ツール

#### sealert (setroubleshoot)

より人間が読みやすい形式でdenialを表示:

```bash
# インストール(RHEL 9 / 10)
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
- **ファイル比較**: ファイル読み取り権限チェック(数百ファイル以上で数%のオーバーヘッド)
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

1. **AVC Cacheの最適化**
   
   ```bash
   # AVC統計確認
   cat /sys/fs/selinux/avc/cache_stats

   # キャッシュサイズ調整(必要に応じて)
   echo 1024 > /sys/fs/selinux/avc/cache_threshold
   ```

2. **監査ログの最適化**
   
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
- 既存ポリシーが存在する場合、競合状況を確認
- 競合が発生する場合は、既存ポリシーの無効化またはqSnapperポリシーの調整が必要

### 2. Btrfs ioctl番号の変動

Btrfs ioctlの番号は、カーネルバージョンによって変わる可能性があります。

**現在の実装:**

```selinux
allowxperm qsnapper_dbus_t unlabeled_t:dir ioctl { 0x9400-0x94ff };
allowxperm qsnapper_dbus_t fs_t:dir ioctl { 0x9400-0x94ff };
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

### 5. RHEL互換性

RHELとopenSUSEでポリシーインターフェース名が異なる場合があります:

**互換性の問題が発生した場合:**
- ディストリビューション固有の条件分岐を`.te`ファイルに追加
- または、ディストリビューションごとに別々のポリシーを提供

### 6. システムラベルへの依存

現在のポリシーはシステム標準のラベル(`etc_t`, `unlabeled_t`, `fs_t`, `var_lib_t`, `var_log_t`)に依存しています。
システムのベースポリシーが更新されてこれらの型の定義が変更された場合、qSnapperポリシーに影響する可能性があります。

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

**openSUSE Leap 16 / SUSE Linux Enterprise 16:**
```bash
# ベースポリシーソースをインストール
sudo zypper install selinux-policy-targeted-src

# ソース場所
ls /usr/src/selinux-policy/
```

**RHEL 9 / 10:**
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
allow qsnapper_dbus_t user_home_t:file write;
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

---

## サポート

このポリシーに関する問題や質問は、以下のリソースを参照してください:

- **プロジェクトリポジトリ**: https://github.com/presire/qSnapper
- **Issue報告**: https://github.com/presire/qSnapper/issues
- **ユーザーガイド**: [README_JP.md](README_JP.md)

**報告時に含めるべき情報:**
- ディストリビューションとバージョン(例: openSUSE Leap 16)
- SELinuxモード(`getenforce`の出力)
- AVC denialログ(`ausearch -m avc -ts recent`)
- ポリシーバージョン(`semodule -l | grep qsnapper`)

---

このドキュメントは、qSnapperプロジェクトの一部として提供されます。  
ライセンス: GPL-3.0  
