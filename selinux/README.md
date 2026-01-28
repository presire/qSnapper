# qSnapper SELinux Policy Module

qSnapperは、SELinux Mandatory Access Control (MAC)に対応したQt6/QMLベースのBtrfs/Snapperスナップショット管理GUIアプリケーションです。  
このドキュメントでは、SELinuxポリシーモジュールのインストールと使用方法について説明します。  

## 目次

- [qSnapper SELinux Policy Module](#qsnapper-selinux-policy-module)
  - [目次](#目次)
  - [概要](#概要)
    - [セキュリティドメイン](#セキュリティドメイン)
    - [通信フロー](#通信フロー)
  - [サポート対象ディストリビューション](#サポート対象ディストリビューション)
  - [インストール](#インストール)
    - [前提条件](#前提条件)
    - [CMake経由でのインストール](#cmake経由でのインストール)
    - [スタンドアロンインストール](#スタンドアロンインストール)
  - [設定](#設定)
    - [ブール値の設定](#ブール値の設定)
      - [1. `qsnapper_manage_all_snapshots` (デフォルト: ON)](#1-qsnapper_manage_all_snapshots-デフォルト-on)
      - [2. `qsnapper_user_access` (デフォルト: ON)](#2-qsnapper_user_access-デフォルト-on)
  - [検証](#検証)
    - [ポリシーモジュールのインストール確認](#ポリシーモジュールのインストール確認)
    - [ファイルコンテキストの確認](#ファイルコンテキストの確認)
    - [ドメイン遷移の確認](#ドメイン遷移の確認)
  - [トラブルシューティング](#トラブルシューティング)
    - [AVC denialへの対処](#avc-denialへの対処)
      - [1. AVC denialの確認](#1-avc-denialの確認)
      - [2. 不足しているルールの特定](#2-不足しているルールの特定)
      - [3. 問題報告](#3-問題報告)
    - [ファイルコンテキストの再適用](#ファイルコンテキストの再適用)
    - [D-Busサービスが起動しない](#d-busサービスが起動しない)
      - [SELinuxが原因か切り分け](#selinuxが原因か切り分け)
      - [D-Busログの確認](#d-busログの確認)
    - [カスタムスナップショット場所の追加](#カスタムスナップショット場所の追加)
  - [アンインストール](#アンインストール)
    - [ポリシーモジュールの削除](#ポリシーモジュールの削除)
    - [ファイルコンテキストのリセット(オプション)](#ファイルコンテキストのリセットオプション)
  - [参考リソース](#参考リソース)
  - [ライセンス](#ライセンス)

---

## 概要

qSnapperのSELinuxポリシーモジュールは、2プロセスアーキテクチャ(GUIアプリケーション + D-Busサービス)に対して最小権限原則に基づいた厳格なアクセス制御を提供します。  

### セキュリティドメイン

- **qsnapper_t**: GUIアプリケーションドメイン(非特権ユーザー権限)  
  - Qt6 Quick/QML GUI実行
  - D-Bus経由でのサービス通信(クライアント)
  - Snapper設定ファイルの読み取り(読み取り専用)
  - スナップショットディレクトリのブラウズ(読み取り専用)

- **qsnapper_dbus_t**: D-Busサービスドメイン(root権限)  
  - PolicyKit認可チェックの実施
  - libsnapper経由でのスナップショット操作
  - Btrfs ioctl操作(スナップショット作成/削除)
  - ファイル比較・復元操作

### 通信フロー

```
QML UI (qsnapper_t)
    ↓ D-Bus system bus
SnapshotOperations (qsnapper_dbus_t)
    ↓ PolicyKit認可
libsnapper → Btrfs filesystem
```

---

## サポート対象ディストリビューション

- **openSUSE Leap 16以降** (SELinux標準採用)  
- **openSUSE Tumbleweed** (SELinux対応)  
- **RHEL 8/9** (SELinux標準搭載)  
- **Fedora 35以降** (SELinux標準搭載)  
- **CentOS Stream** (SELinux標準搭載)  

**ポリシータイプ**:  
`targeted` (標準的な設定で動作)  

---

## インストール

### 前提条件

SELinuxが有効化されていることを確認してください:  

```bash
# SELinuxステータス確認
sestatus

# 期待される出力:
# SELinux status:                 enabled
# Current mode:                   enforcing
```

SELinuxポリシー開発ツールをインストール:  

**openSUSE / SUSE Linux Enterprise:**  

```bash
sudo zypper install selinux-policy-devel policycoreutils
```

**RHEL/Fedora:**  

```bash
sudo dnf install selinux-policy-devel policycoreutils-python-utils
```

### CMake経由でのインストール

qSnapperをビルドする際、SELinuxポリシーモジュールはデフォルトで自動的にビルド・インストールされます。  

```bash
# プロジェクトルートディレクトリで実行
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make -j$(nproc)
sudo make install
```

インストール時に自動的に以下が実行されます:  
- ポリシーモジュール(.pp)のビルド
- `semodule -i`によるモジュールの登録
- `restorecon`によるファイルラベルの適用

**SELinuxサポートを無効化する場合:**  

```bash
cmake -DCMAKE_INSTALL_PREFIX=/usr -DENABLE_SELINUX=OFF ..
```

### スタンドアロンインストール

CMakeを使用せず、ポリシーモジュールのみをインストールする場合:  

```bash
cd /path/to/qSnapper/selinux

# ポリシーパッケージのビルド
make

# ポリシーのインストール(root権限必要)
sudo make install
```

---

## 設定

### ブール値の設定

qSnapperポリシーは、動作をカスタマイズするための2つのブール値を提供します。  

#### 1. `qsnapper_manage_all_snapshots` (デフォルト: ON)

D-Busサービスがスナップショットから任意のファイルを復元できるかを制御します。  

**有効化(デフォルト):**  

```bash
sudo setsebool -P qsnapper_manage_all_snapshots on
```

**無効化(ユーザーホームディレクトリのみ復元可能):**  

```bash
sudo setsebool -P qsnapper_manage_all_snapshots off
```

#### 2. `qsnapper_user_access` (デフォルト: ON)

一般ユーザーがqSnapper GUIアプリケーションを起動できるかを制御します。  

**有効化(デフォルト):**  

```bash
sudo setsebool -P qsnapper_user_access on
```

**無効化(管理者のみ起動可能):**  

```bash
sudo setsebool -P qsnapper_user_access off
```

**現在の設定を確認:**  

```bash
getsebool qsnapper_manage_all_snapshots
getsebool qsnapper_user_access
```

---

## 検証

### ポリシーモジュールのインストール確認

```bash
# モジュールが登録されているか確認
sudo semodule -l | grep qsnapper

# 期待される出力:
# qsnapper	1.0.0
```

### ファイルコンテキストの確認

```bash
# バイナリのラベル確認
ls -Z /usr/bin/qsnapper
# 期待: system_u:object_r:qsnapper_exec_t:s0

ls -Z /usr/libexec/qsnapper-dbus-service
# 期待: system_u:object_r:qsnapper_dbus_exec_t:s0

# スナップショットディレクトリ
ls -Z -d /.snapshots
# 期待: system_u:object_r:qsnapper_snapshot_t:s0

# Snapper設定
ls -Z /etc/snapper/
# 期待: qsnapper_conf_t
```

### ドメイン遷移の確認

```bash
# GUIアプリケーション起動
qsnapper &
QSNAPPER_PID=$!

# プロセスのドメイン確認
ps -eZ | grep qsnapper

# 期待される出力例:
# unconfined_u:unconfined_r:qsnapper_t:s0 12345 pts/0 qsnapper
# system_u:system_r:qsnapper_dbus_t:s0   12346 ?     qsnapper-dbus-service
```

---

## トラブルシューティング

### AVC denialへの対処

#### 1. AVC denialの確認

```bash
# 最近のAVC denialを表示
sudo ausearch -m avc -ts recent | grep qsnapper

# リアルタイム監視
sudo ausearch -m avc -ts recent | tail -f
```

#### 2. 不足しているルールの特定

```bash
# denialから推奨ルールを生成
sudo ausearch -m avc -ts recent | grep qsnapper | audit2allow -R

# ローカルポリシーモジュールとして生成(一時的な回避策)
sudo ausearch -m avc -ts recent | audit2allow -M qsnapper-local
sudo semodule -i qsnapper-local.pp
```

#### 3. 問題報告

audit2allowの出力を含めて、GitHubリポジトリにissueを作成してください:  
https://github.com/presire/qSnapper/issues  

### ファイルコンテキストの再適用

ファイルコンテキストが不一致の場合:  

```bash
# 現在のコンテキスト確認
ls -Z /usr/bin/qsnapper

# 期待されるコンテキスト確認
sudo matchpathcon /usr/bin/qsnapper

# 強制再ラベル
sudo restorecon -F -R -v /usr/bin/qsnapper
sudo restorecon -F -R -v /usr/libexec/qsnapper-dbus-service
sudo restorecon -F -R -v /.snapshots
sudo restorecon -F -R -v /etc/snapper
```

### D-Busサービスが起動しない

#### SELinuxが原因か切り分け

```bash
# 一時的にPermissiveモードに切り替え
sudo setenforce 0

# 操作を再試行
qsnapper

# Enforcingモードに戻す
sudo setenforce 1
```

- **Permissiveで動作する**:  
  SELinuxポリシーの問題 → AVC denialログを確認  
- **Permissiveでも動作しない**:  
  アプリケーション自体の問題 → D-Busログを確認  

#### D-Busログの確認

```bash
# D-Busシステムログ
sudo journalctl -u dbus --since "10 minutes ago" | grep qsnapper

# アプリケーションログ
journalctl --user -e | grep qsnapper
```

### カスタムスナップショット場所の追加

`/.snapshots`以外の場所にスナップショットを保存する場合:  

```bash
# ファイルコンテキストルールを追加
sudo semanage fcontext -a -t qsnapper_snapshot_t "/custom/snapshots(/.*)?"

# ラベル適用
sudo restorecon -R -v /custom/snapshots
```

---

## アンインストール

### ポリシーモジュールの削除

```bash
sudo semodule -r qsnapper
```

### ファイルコンテキストのリセット(オプション)

```bash
# デフォルトのコンテキストに戻す
sudo restorecon -F -R -v /usr/bin/qsnapper
sudo restorecon -F -R -v /usr/libexec/qsnapper-dbus-service
```

---

## 参考リソース

- **SELinux Project**:  
  https://github.com/SELinuxProject  
- **openSUSE SELinux**:  
  https://en.opensuse.org/SDB:SELinux  
- **Red Hat SELinux ガイド**:  
  https://access.redhat.com/documentation/ja-jp/red_hat_enterprise_linux/9/html/using_selinux/  

**システム管理者向けの詳細情報**:  
[ADMIN.md](ADMIN.md)を参照してください。  

---

## ライセンス

このSELinuxポリシーモジュールは、qSnapperプロジェクトと同じライセンス(GPL-3.0)で配布されます。  

