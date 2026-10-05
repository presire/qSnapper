# CPack packaging configuration for qSnapper
# RPM and DEB package generation

set(CPACK_PACKAGE_NAME "qsnapper")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Qt6/QML GUI for Btrfs/Snapper snapshot management")
set(CPACK_PACKAGE_DESCRIPTION "A modern Qt6/QML graphical user interface for managing Btrfs filesystem snapshots via Snapper. Supports creating, browsing, comparing, and restoring snapshots with Light/Dark theme and multi-language support.")
set(CPACK_PACKAGE_VENDOR "Presire")
set(CPACK_PACKAGE_CONTACT "Presire")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE.md")

# Detect distribution from /etc/os-release for distro-specific dependencies
set(DISTRO_ID "unknown")
set(DISTRO_ID_LIKE "")
if(EXISTS "/etc/os-release")
    file(STRINGS "/etc/os-release" OS_RELEASE_LINES)
    foreach(line ${OS_RELEASE_LINES})
        if(line MATCHES "^ID=(.*)")
            string(STRIP "${CMAKE_MATCH_1}" DISTRO_ID)
            string(REPLACE "\"" "" DISTRO_ID "${DISTRO_ID}")
        endif()
        if(line MATCHES "^ID_LIKE=(.*)")
            string(STRIP "${CMAKE_MATCH_1}" DISTRO_ID_LIKE)
            string(REPLACE "\"" "" DISTRO_ID_LIKE "${DISTRO_ID_LIKE}")
        endif()
    endforeach()
endif()
message(STATUS "Detected distribution: ${DISTRO_ID} (ID_LIKE: ${DISTRO_ID_LIKE})")

# RPM settings
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "System/Monitoring")
set(CPACK_RPM_PACKAGE_URL "https://github.com/presire/qSnapper")
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
set(CPACK_RPM_PACKAGE_AUTOPROV ON)
set(CPACK_RPM_PACKAGE_AUTOREQ ON)

# RPM dependencies (distro-specific for QML runtime modules)
if(DISTRO_ID MATCHES "opensuse" OR DISTRO_ID_LIKE MATCHES "suse")
    set(CPACK_RPM_PACKAGE_REQUIRES
        "snapper, qt6-declarative-imports, qt6-quickcontrols2-imports, polkit")
elseif(DISTRO_ID STREQUAL "fedora" OR DISTRO_ID_LIKE MATCHES "fedora")
    set(CPACK_RPM_PACKAGE_REQUIRES
        "snapper, qt6-qtdeclarative, qt6-qtquickcontrols2, polkit")
else()
    set(CPACK_RPM_PACKAGE_REQUIRES "snapper, polkit")
endif()

set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_APPEND
    /usr/share/applications
    /usr/share/icons
    /usr/share/icons/hicolor
    /usr/share/icons/hicolor/128x128
    /usr/share/icons/hicolor/128x128/apps
    /usr/share/dbus-1
    /usr/share/dbus-1/system-services
    /usr/share/dbus-1/system.d
    /usr/share/polkit-1
    /usr/share/polkit-1/actions
    /usr/share/selinux
    /usr/share/selinux/packages
)

# RPMスクリプトレット
# post-installはログディレクトリの作成 (systemd-tmpfiles) も行うため、SELinuxの有無にかかわらず登録する
# (SELinuxポリシーの読み込みは、ポリシーパッケージがインストールされている場合のみ行われる)
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_SOURCE_DIR}/cmake/rpm-post-install.sh")
if(ENABLE_SELINUX)
    set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${CMAKE_SOURCE_DIR}/cmake/rpm-pre-uninstall.sh")
endif()

# DEB settings
set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/presire/qSnapper")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "snapper, qml6-module-qtquick, qml6-module-qtquick-controls, qml6-module-qtquick-layouts, polkitd | policykit-1")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

include(CPack)
