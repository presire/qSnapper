# qSnapper - Installation

[Back to README](../README.md) | [日本語](INSTALL_JP.md)

## Requirements

### Runtime Dependencies

- Linux operating system (required)
- Qt6 (>= 6.5)
  - Qt6 Core
  - Qt6 GUI
  - Qt6 Quick
  - Qt6 QuickControls2
  - Qt6 Qml
  - Qt6 DBus
- Snapper (>= 0.8.0)
- Snapper D-Bus service (`org.opensuse.Snapper`, provided by the `snapper` package; required for configuration creation and deletion and for bulk deletion)
- libbtrfsutil (required for the Filesystem tab)
- PolicyKit (polkit)
- D-Bus

### Build Dependencies

- CMake (>= 3.16)
- C++17 compatible compiler (GCC, Clang)
- Qt6 development packages
- PolicyKit-Qt6 development files
- Snapper development headers
- libbtrfsutil development headers
- scdoc (optional, for generating the man page; if absent, the build skips man page generation)

## Installation

### From Source

#### 1. Install Dependencies

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
The D-Bus system service uses the polkit-gobject-1 API directly, and the build locates it via `pkg-config` (`polkit-gobject-1.pc`).  
This module is shipped in `polkit-devel` on openSUSE and RHEL/Fedora,  
and in `libpolkit-gobject-1-dev` on Debian (all listed above), so no additional package is required.  

#### 2. Build and Install

```bash
git clone https://github.com/presire/qSnapper.git
cd qSnapper
mkdir build && cd build

cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make -j$(nproc)
sudo make install
```

**Build Options:**  

- **SELinux Support** (Optional, disabled by default):
  ```bash
  cmake -DCMAKE_INSTALL_PREFIX=/usr -DENABLE_SELINUX=ON ..
  ```

  Enables SELinux Mandatory Access Control (MAC) policy module installation.  

  **Requirements for SELinux:**  

  - openSUSE / SUSE Linux Enterprise:
    ```bash
    sudo zypper install selinux-policy-devel policycoreutils
    ```
  - RHEL 10:
    ```bash
    sudo dnf install selinux-policy-devel policycoreutils-python-utils
    ```

  See [selinux/README.md](../selinux/README.md) for detailed SELinux configuration.

- **Log Directory** (Optional, default: `/var/log/qsnapper`):  
  ```bash
  cmake -DCMAKE_INSTALL_PREFIX=/usr -DQSNAPPER_LOG_DIR=/path/to/log/dir ..
  ```

  Changes the directory where the D-Bus service writes log files.  
  The log filename (`qsnapper-dbus.log`) cannot be changed.  
  If not specified, logs are written to `/var/log/qsnapper`.  

#### 3. Post-Installation Steps

The installation process automatically installs:  
- D-Bus service files to `/usr/share/dbus-1/system-services/`
- D-Bus configuration to `/usr/share/dbus-1/system.d/`
- PolicyKit policy to `/usr/share/polkit-1/actions/`
- Desktop entry to `/usr/share/applications/`
- Application icon to `/usr/share/icons/hicolor/128x128/apps/`
- Manual page to `/usr/share/man/man1/` (view with `man qsnapper`; only when scdoc is available at build time)

Reload D-Bus and PolicyKit:  

```bash
sudo systemctl reload dbus
```
