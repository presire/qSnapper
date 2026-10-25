# qSnapper - インストール

[READMEへ戻る](../README_JP.md) | [English](INSTALL.md)

## 要件

### 実行時依存関係

- Linuxオペレーティングシステム (必須)
- Qt6 (>= 6.5)
  - Qt6 Core
  - Qt6 GUI
  - Qt6 Quick
  - Qt6 QuickControls2
  - Qt6 Qml
  - Qt6 DBus
- Snapper (>= 0.8.0)
- SnapperのD-Busサービス (`org.opensuse.Snapper`、`snapper`パッケージが提供。設定の作成 / 削除と一括削除に必要)
- libbtrfsutil (ファイルシステムタブに必要)
- PolicyKit (polkit)
- D-Bus

### ビルド依存関係

- CMake (>= 3.16)
- C++17対応コンパイラ (GCC、Clang)
- Qt6開発パッケージ
- PolicyKit-Qt6開発ファイル
- Snapper開発ヘッダ
- libbtrfsutil開発ヘッダ
- scdoc (オプション、manページ生成用。未インストールの場合はmanページ生成をスキップ)

## インストール

### ソースからビルド

#### 1. 依存関係のインストール

**openSUSE Leap 16 / SUSE Linux Enterprise 16**  

```bash
sudo zypper install cmake gcc-c++ \
                    qt6-base-devel qt6-declarative-devel qt6-quickcontrols2-devel qt6-linguist-devel \
                    polkit-devel libpolkit-qt6-1-devel \
                    libsnapper-devel libbtrfsutil-devel scdoc
```

**RHEL 10**

```bash
sudo dnf install cmake gcc-c++ \
                 qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtquickcontrols2-devel qt6-linguist-devel \
                 polkit-devel polkit-qt6-1-devel \
                 snapper-devel btrfs-progs-devel scdoc
```

**Debian 13 (trixie)**  

```bash
sudo apt install cmake g++ make pkg-config \
                 qt6-base-dev qt6-declarative-dev qt6-tools-dev \
                 libpolkit-qt6-1-dev libpolkit-gobject-1-dev \
                 libsnapper-dev libboost-dev libbtrfs-dev libbtrfsutil-dev scdoc
```

**Note:**  
D-Busシステムサービスはpolkit-gobject-1 APIを直接使用しており、ビルド時には `pkg-config` (`polkit-gobject-1.pc`) で検出されます。  
このモジュールはopenSUSEおよびRHEL/Fedoraでは `polkit-devel` に、Debianでは `libpolkit-gobject-1-dev` に同梱されているため (上記コマンドに既に含まれています)、追加のパッケージは不要です。  

#### 2. ビルドとインストール

```bash
git clone https://github.com/presire/qSnapper.git
cd qSnapper
mkdir build && cd build

cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make -j$(nproc)
sudo make install
```

**ビルドオプション:**  

- **SELinuxサポート** (オプション、デフォルト: 無効):  
  
  ```bash
  cmake -DCMAKE_INSTALL_PREFIX=/usr -DENABLE_SELINUX=ON ..
  ```

  SELinux Mandatory Access Control (MAC) ポリシーモジュールのインストールを有効化します。  

  **SELinuxの要件:**  
  - openSUSE / SUSE Linux Enterprise:

    ```bash
    sudo zypper install selinux-policy-devel policycoreutils
    ```

  - RHEL 10:

    ```bash
    sudo dnf install selinux-policy-devel policycoreutils-python-utils
    ```

  SELinux設定の詳細については、[selinux/README_JP.md](../selinux/README_JP.md) を参照してください。

- **ログディレクトリ** (オプション、デフォルト: `/var/log/qsnapper`):

  ```bash
  cmake -DCMAKE_INSTALL_PREFIX=/usr -DQSNAPPER_LOG_DIR=/path/to/log/dir ..
  ```

  D-Busサービスがログファイルを出力するディレクトリを変更します。
  ログファイル名 (`qsnapper-dbus.log`)は変更できません。
  未指定の場合、ログは `/var/log/qsnapper` に出力されます。

#### 3. インストール後の手順

インストールプロセスは自動的に以下をインストールします：  
- D-Busサービスファイルを `/usr/share/dbus-1/system-services/` に
- D-Bus設定ファイルを `/usr/share/dbus-1/system.d/` に
- PolicyKitポリシーを `/usr/share/polkit-1/actions/` に
- デスクトップエントリを `/usr/share/applications/` に
- アプリケーションアイコンを `/usr/share/icons/hicolor/128x128/apps/` に
- manページを `/usr/share/man/man1/` に (`man qsnapper` で参照可能。ビルド時にscdocが利用可能な場合のみ)

D-BusとPolicyKitをリロード：  

```bash
sudo systemctl reload dbus
```
