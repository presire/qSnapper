# qSnapper - Development

[Back to README](../README.md) | [日本語](DEVELOPMENT_JP.md)

## Development

### Building for Development

```bash
mkdir build-debug && cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

### Project Structure

```
qSnapper/
├── CMakeLists.txt                                  # Top-level build configuration (GUI, D-Bus service, tests, packaging)
├── qsnapper.desktop.in                             # Desktop entry template
├── LICENSE.md                                      # Project license
├── README.md / README_JP.md                        # Overview (English / Japanese)
├── docs/                                           # Detailed documentation (English / Japanese)
│   ├── INSTALL.md / INSTALL_JP.md                  # Requirements and installation
│   ├── USAGE.md / USAGE_JP.md                      # Usage and configuration
│   ├── TROUBLESHOOTING.md / TROUBLESHOOTING_JP.md  # Troubleshooting
│   └── DEVELOPMENT.md / DEVELOPMENT_JP.md          # Development and project structure
├── .github/workflows/                              # CI workflows
│   └── release.yml                                 # CI: tests, RPM/DEB builds and release on version tags
├── src/                                            # C++ sources
│   ├── main.cpp                                    # GUI application entry point
│   ├── snapperservice.cpp                          # Client-side D-Bus interface to the privileged helper
│   ├── fssnapshot.cpp                              # Snapshot data model (one snapshot)
│   ├── snapshotlistmodel.cpp                       # Snapshot list model
│   ├── snapshotgroupmodel.cpp                      # Pre/Post snapshot grouping model
│   ├── configmanagermodel.cpp                      # Snapper configuration list model
│   ├── snappersettingsmodel.cpp                    # Configuration values and timer state model
│   ├── filesysteminfomodel.cpp                     # Filesystem allocation, usage and subvolume model
│   ├── filechangemodel_loading.cpp                 # File change loading (libsnapper comparison input)
│   ├── filechangemodel_model.cpp                   # File change tree model
│   ├── filechangemodel_restore.cpp                 # Restore plan staging and execution
│   ├── filechangemodel_transport.cpp               # D-Bus transport for restore plans
│   ├── thememanager.cpp                            # Theme management (Light, Dark, System)
│   ├── windowstatemanager.cpp                      # Persistence of window size and maximized state
│   ├── singleinstanceguard.cpp                     # Single-instance guard (lock file and local socket)
│   └── dbusservice/                                # Privileged D-Bus service (runs as root)
│       ├── main.cpp                                # Service entry point (object registration, logging, idle timeout)
│       ├── snapshotoperations.{cpp,h}              # D-Bus method entry points and polkit authorization
│       ├── snapshotoperations_*.cpp                # Method groups (snapshots, changes, configs, settings, restore)
│       ├── snapperdoperation.{cpp,h}               # Calls org.opensuse.Snapper for config and bulk delete operations
│       ├── btrfsinfo.{cpp,h}                       # Kernel Btrfs ioctls and libbtrfsutil wrappers
│       ├── systemdtimeroperation.{cpp,h}           # systemd timer state and enable/disable
│       ├── inputvalidator.{cpp,h}                  # Validation of untrusted D-Bus input
│       ├── configpathvalidation.{cpp,h}            # Validation of configuration names and subvolume paths
│       ├── filesystemhelpers.{cpp,h}               # Safe file operations beneath a root (no symlink following)
│       ├── restoremanifest.{cpp,h}                 # Restore plan registry (owner binding, budgets, retention)
│       ├── restoreplanexecutor.{cpp,h}             # Chunked execution of frozen restore plans
│       ├── restorevalidation.{cpp,h}               # Restore destination construction and entry validation
│       └── comparisoncache.h                       # Single-entry cache of a libsnapper comparison
├── include/                                        # Headers for the GUI application
│   ├── configmanagermodel.h                        # Configuration list model
│   ├── csvrecord.h                                 # CSV quoting and parsing (shared by service and client)
│   ├── externalurlopener.h                         # Opens http/https links via the desktop portal
│   ├── filechangemodel.h                           # File change model and restore plan transport
│   ├── filesysteminfomodel.h                       # Filesystem information model
│   ├── fssnapshot.h                                # Snapshot data model
│   ├── singleinstanceguard.h                       # Single-instance guard
│   ├── snapperservice.h                            # Client-side D-Bus interface
│   ├── snappersettingsmodel.h                      # Configuration values and timer state model
│   ├── snappertimerstate.h                         # Timer unit state record
│   ├── snapshotgroupmodel.h                        # Pre/Post grouping model
│   ├── snapshotlistmodel.h                         # Snapshot list model
│   ├── thememanager.h                              # Theme manager
│   └── windowstatemanager.h                        # Window state manager
├── qml/                                            # QML user interface
│   ├── Main.qml                                    # Main window with Snapshots, Settings and Filesystem tabs
│   ├── pages/                                      # Page components
│   │   ├── SnapshotListPage.qml                    # Snapshot list page
│   │   ├── SettingsPage.qml                        # Configuration editor, timers and appearance
│   │   └── FilesystemPage.qml                      # Btrfs allocation, usage and subvolumes
│   └── components/                                 # Reusable components
│       ├── AboutQtDialog.qml                       # About Qt dialog
│       ├── AboutqSnapperDialog.qml                 # About qSnapper dialog
│       ├── BorderedDialog.qml                      # Dialog base type with a theme-aware border
│       ├── CompareSnapshotsDialog.qml              # File differences between two snapshots
│       ├── CreateConfigDialog.qml                  # Create a Snapper configuration
│       ├── DeleteConfigDialog.qml                  # Delete a configuration with typed-name confirmation
│       ├── RestorePreviewDialog.qml                # Restore preview, options and progress
│       ├── SettingsFieldRow.qml                    # One editable configuration value
│       ├── SnapshotDetailDialog.qml                # Snapshot details, file restore and rollback
│       ├── SnapshotEditDialog.qml                  # Edit snapshot metadata
│       ├── SnapshotTableHeader.qml                 # Snapshot table header row
│       ├── SnapshotTableRow.qml                    # Snapshot table row delegate
│       └── TimerSettingsSection.qml                # systemd timer state and enable/disable
├── dbus/                                           # D-Bus configuration
│   ├── com.presire.qsnapper.Operations.conf        # Bus policy (allowed methods)
│   ├── com.presire.qsnapper.Operations.service.in  # Service activation template
│   └── com.presire.qsnapper.Operations.xml         # Interface introspection XML
├── polkit/                                         # PolicyKit policy
│   └── com.presire.qsnapper.policy                 # Authorization actions
├── systemd/tmpfiles.d/                             # systemd tmpfiles configuration
│   └── qsnapper.conf                               # Creates the log directory /var/log/qsnapper (0700)
├── selinux/                                        # SELinux policy module
│   ├── qsnapper.te                                 # Type enforcement rules
│   ├── qsnapper.if                                 # Interface definitions
│   ├── qsnapper.fc.in                              # File context template (generated by CMake)
│   ├── qsnapper.fc                                 # File context for the standalone Makefile build
│   ├── CMakeLists.txt                              # Policy build (CMake)
│   ├── Makefile                                    # Policy build (standalone)
│   ├── ADMIN.md / ADMIN_JP.md                      # Administrator guide (English / Japanese)
│   ├── README.md / README_JP.md                    # Policy overview (English / Japanese)
│   └── qsnapper-architecture.*                     # Architecture diagram (.drawio / .png)
├── cmake/                                          # CMake helpers and packaging scripts
│   ├── packaging.cmake                             # CPack packaging configuration (RPM/DEB)
│   ├── rpm-post-install.sh                         # RPM post-install (load SELinux module, create log directory)
│   └── rpm-pre-uninstall.sh                        # RPM pre-uninstall (remove SELinux module)
├── man/                                            # Manual page source (scdoc)
│   └── qsnapper.1.scd                              # qsnapper(1) man page
├── icons/                                          # Application and UI icons
├── translations/                                   # Translation files
│   ├── qsnapper_ja.ts                              # Japanese translation
│   └── qsnapper_de.ts                              # German translation
├── tests/                                          # Tests (enabled with QSNAPPER_BUILD_TESTS=ON)
│   ├── unit/                                       # Qt Test unit tests (tst_*.cpp, one per component)
│   ├── integration/                                # D-Bus contract checks and isolated D-Bus tests
│   ├── security_poc/                               # Vulnerability PoC scripts (VM only)
│   └── TESTREPORT_v1.3.3.md                        # Test report template
├── Licenses/                                       # Third-party licenses
│   ├── Qt.md, D-Bus.md, PolicyKit.md               # License texts (Qt, D-Bus, PolicyKit)
│   └── Polkit-Qt.md, Snapper.md, Btrfs-progs.md    # License texts (Polkit-Qt, Snapper, Btrfs-progs)
└── ScreenShot/                                     # Screenshots used in the README
```
