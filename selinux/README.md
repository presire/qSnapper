# qSnapper SELinux Policy Module

qSnapper is a Qt6/QML-based Btrfs/Snapper snapshot management GUI application with SELinux Mandatory Access Control (MAC) support.
This document describes how to install and use the SELinux policy module.

## Table of Contents

- [qSnapper SELinux Policy Module](#qsnapper-selinux-policy-module)
  - [Table of Contents](#table-of-contents)
  - [Overview](#overview)
    - [Security Domains](#security-domains)
    - [Communication Flow](#communication-flow)
  - [Supported Distributions](#supported-distributions)
  - [Installation](#installation)
    - [Prerequisites](#prerequisites)
    - [Installation via CMake](#installation-via-cmake)
    - [Standalone Installation](#standalone-installation)
  - [Configuration](#configuration)
    - [Boolean Settings](#boolean-settings)
      - [1. `qsnapper_manage_all_snapshots` (Default: ON)](#1-qsnapper_manage_all_snapshots-default-on)
      - [2. `qsnapper_user_access` (Default: ON)](#2-qsnapper_user_access-default-on)
  - [Verification](#verification)
    - [Verify Policy Module Installation](#verify-policy-module-installation)
    - [Verify File Contexts](#verify-file-contexts)
    - [Verify Domain Transitions](#verify-domain-transitions)
  - [Troubleshooting](#troubleshooting)
    - [Handling AVC Denials](#handling-avc-denials)
      - [1. Check AVC Denials](#1-check-avc-denials)
      - [2. Identify Missing Rules](#2-identify-missing-rules)
      - [3. Report Issues](#3-report-issues)
    - [Reapply File Contexts](#reapply-file-contexts)
    - [D-Bus Service Fails to Start](#d-bus-service-fails-to-start)
      - [Isolate Whether SELinux Is the Cause](#isolate-whether-selinux-is-the-cause)
      - [Check D-Bus Logs](#check-d-bus-logs)
    - [Adding Custom Snapshot Locations](#adding-custom-snapshot-locations)
  - [Uninstallation](#uninstallation)
    - [Remove the Policy Module](#remove-the-policy-module)
    - [Reset File Contexts (Optional)](#reset-file-contexts-optional)
  - [References](#references)
  - [License](#license)

---

## Overview

The qSnapper SELinux policy module provides strict access control based on the principle of least privilege for the two-process architecture (GUI application + D-Bus service).

### Security Domains

- **qsnapper_t**: GUI application domain (unprivileged user)
  - Qt6 Quick/QML GUI execution
  - Service communication via D-Bus (client)
  - Reading Snapper configuration files (read-only)
  - Browsing snapshot directories (read-only)

- **qsnapper_dbus_t**: D-Bus service domain (root privileges)
  - PolicyKit authorization checks
  - Snapshot operations via libsnapper
  - Btrfs ioctl operations (snapshot creation/deletion)
  - File comparison and restore operations

### Communication Flow

```
QML UI (qsnapper_t)
    ↓ D-Bus system bus
SnapshotOperations (qsnapper_dbus_t)
    ↓ PolicyKit authorization
libsnapper → Btrfs filesystem
```

---

## Supported Distributions

- **openSUSE Leap 16 or later** (SELinux adopted as standard)
- **RHEL 9 / 10** (SELinux included as standard)

**Policy type**:
`targeted` (works with standard configuration)

---

## Installation

### Prerequisites

Verify that SELinux is enabled:

```bash
# Check SELinux status
sestatus

# Expected output:
# SELinux status:                 enabled
# Current mode:                   enforcing
```

Install SELinux policy development tools:

**openSUSE / SUSE Linux Enterprise:**

```bash
sudo zypper install selinux-policy-devel policycoreutils
```

**RHEL 9 / 10:**

```bash
sudo dnf install selinux-policy-devel policycoreutils-python-utils
```

### Installation via CMake

The SELinux policy module is **not built by default**. To enable it, specify `-DENABLE_SELINUX=ON` when running CMake:

```bash
# Run from the project root directory
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr -DENABLE_SELINUX=ON ..
make -j$(nproc)
sudo make install
```

The following steps are automatically performed during installation:
- Building the policy module (.pp)
- Registering the module via `semodule -i`
- Applying file labels via `restorecon`

### Standalone Installation

To install only the policy module without using CMake:

```bash
cd /path/to/qSnapper/selinux

# Build the policy package
make

# Install the policy (root privileges required)
sudo make install
```

---

## Configuration

### Boolean Settings

The qSnapper policy provides two booleans for customizing behavior.

#### 1. `qsnapper_manage_all_snapshots` (Default: ON)

Controls whether the D-Bus service can restore arbitrary files from snapshots.

**Enable (default):**

```bash
sudo setsebool -P qsnapper_manage_all_snapshots on
```

**Disable (only user home directory files can be restored):**

```bash
sudo setsebool -P qsnapper_manage_all_snapshots off
```

#### 2. `qsnapper_user_access` (Default: ON)

Controls whether regular users can launch the qSnapper GUI application.

**Enable (default):**

```bash
sudo setsebool -P qsnapper_user_access on
```

**Disable (administrators only):**

```bash
sudo setsebool -P qsnapper_user_access off
```

**Check current settings:**

```bash
getsebool qsnapper_manage_all_snapshots
getsebool qsnapper_user_access
```

---

## Verification

### Verify Policy Module Installation

```bash
# Check if the module is registered
sudo semodule -l | grep qsnapper

# Expected output:
# qsnapper	1.0.0
```

### Verify File Contexts

```bash
# Check binary labels
ls -Z /usr/bin/qsnapper
# Expected: system_u:object_r:qsnapper_exec_t:s0

ls -Z /usr/libexec/qsnapper-dbus-service
# Expected: system_u:object_r:qsnapper_dbus_exec_t:s0

# Snapshot directory
ls -Z -d /.snapshots
# Expected: system_u:object_r:qsnapper_snapshot_t:s0

# Snapper configuration
ls -Z /etc/snapper/
# Expected: qsnapper_conf_t
```

### Verify Domain Transitions

```bash
# Launch the GUI application
qsnapper &
QSNAPPER_PID=$!

# Check process domains
ps -eZ | grep qsnapper

# Expected output example:
# unconfined_u:unconfined_r:qsnapper_t:s0 12345 pts/0 qsnapper
# system_u:system_r:qsnapper_dbus_t:s0   12346 ?     qsnapper-dbus-service
```

---

## Troubleshooting

### Handling AVC Denials

#### 1. Check AVC Denials

```bash
# Display recent AVC denials
sudo ausearch -m avc -ts recent | grep qsnapper

# Real-time monitoring
sudo ausearch -m avc -ts recent | tail -f
```

#### 2. Identify Missing Rules

```bash
# Generate recommended rules from denials
sudo ausearch -m avc -ts recent | grep qsnapper | audit2allow -R

# Generate as a local policy module (temporary workaround)
sudo ausearch -m avc -ts recent | audit2allow -M qsnapper-local
sudo semodule -i qsnapper-local.pp
```

#### 3. Report Issues

Please create an issue on the GitHub repository including the audit2allow output:
https://github.com/presire/qSnapper/issues

### Reapply File Contexts

If file contexts are inconsistent:

```bash
# Check current context
ls -Z /usr/bin/qsnapper

# Check expected context
sudo matchpathcon /usr/bin/qsnapper

# Force relabeling
sudo restorecon -F -R -v /usr/bin/qsnapper
sudo restorecon -F -R -v /usr/libexec/qsnapper-dbus-service
sudo restorecon -F -R -v /.snapshots
sudo restorecon -F -R -v /etc/snapper
```

### D-Bus Service Fails to Start

#### Isolate Whether SELinux Is the Cause

```bash
# Temporarily switch to Permissive mode
sudo setenforce 0

# Retry the operation
qsnapper

# Switch back to Enforcing mode
sudo setenforce 1
```

- **Works in Permissive**:
  SELinux policy issue → Check AVC denial logs
- **Does not work in Permissive either**:
  Application issue → Check D-Bus logs

#### Check D-Bus Logs

```bash
# D-Bus system logs
sudo journalctl -u dbus --since "10 minutes ago" | grep qsnapper

# Application logs
journalctl --user -e | grep qsnapper
```

### Adding Custom Snapshot Locations

If you store snapshots in a location other than `/.snapshots`:

```bash
# Add a file context rule
sudo semanage fcontext -a -t qsnapper_snapshot_t "/custom/snapshots(/.*)?"

# Apply labels
sudo restorecon -R -v /custom/snapshots
```

---

## Uninstallation

### Remove the Policy Module

```bash
sudo semodule -r qsnapper
```

### Reset File Contexts (Optional)

```bash
# Restore default contexts
sudo restorecon -F -R -v /usr/bin/qsnapper
sudo restorecon -F -R -v /usr/libexec/qsnapper-dbus-service
```

---

## References

- **SELinux Project**:
  https://github.com/SELinuxProject
- **openSUSE SELinux**:
  https://en.opensuse.org/SDB:SELinux
- **Red Hat SELinux Guide**:
  https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/9/html/using_selinux/

**For detailed information for system administrators**:
See [ADMIN.md](ADMIN.md).

---

## License

This SELinux policy module is distributed under the same license (GPL-3.0) as the qSnapper project.
