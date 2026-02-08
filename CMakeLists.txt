cmake_minimum_required(VERSION 3.16)

project(qsnapper VERSION 1.0.2 LANGUAGES CXX)

# Linux専用プロジェクト - 他のプラットフォームでは明示的にエラーを出す
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "qSnapper is only supported on Linux. Current platform: ${CMAKE_SYSTEM_NAME}")
endif()

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Quick Qml QuickControls2 DBus LinguistTools)
find_package(PolkitQt6-1 REQUIRED)

qt6_standard_project_setup()

set(SOURCES
    src/main.cpp
    src/fssnapshot.cpp
    src/snapperservice.cpp
    src/fssnapshotstore.cpp
    src/snapshotlistmodel.cpp
    src/filechangemodel.cpp
    src/thememanager.cpp
)

set(HEADERS
    include/fssnapshot.h
    include/snapperservice.h
    include/fssnapshotstore.h
    include/snapshotlistmodel.h
    include/filechangemodel.h
    include/thememanager.h
)

qt6_add_executable(qsnapper ${SOURCES} ${HEADERS})

target_include_directories(qsnapper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

qt6_add_qml_module(qsnapper
    URI QSnapper
    VERSION 1.0
    RESOURCE_PREFIX "/qt/qml"
    NO_PLUGIN
    QML_FILES
        qml/Main.qml
        qml/pages/SnapshotListPage.qml
        qml/components/SnapshotItem.qml
        qml/components/SnapshotDetailDialog.qml
        qml/components/RestorePreviewDialog.qml
        qml/components/AboutqSnapperDialog.qml
        qml/components/AboutQtDialog.qml
)

# アイコンリソースを別途追加（QMLモジュールとは独立）
qt6_add_resources(qsnapper "icons"
    PREFIX "/QSnapper"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        icons/directory.svg
        icons/file.svg
        icons/Qt.png
        icons/qSnapper.png
)

# 翻訳ファイルの設定
qt6_add_translations(qsnapper
    TS_FILES translations/qsnapper_ja.ts translations/qsnapper_de.ts
    RESOURCE_PREFIX /i18n
)

target_link_libraries(qsnapper PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::QuickControls2
    Qt6::DBus
)

install(TARGETS qsnapper
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 翻訳ファイルのインストール
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/translations
    DESTINATION ${CMAKE_INSTALL_DATADIR}/qsnapper
    FILES_MATCHING PATTERN "*.qm"
)

# D-Busサービス
set(DBUS_SERVICE_SOURCES
    src/dbusservice/main.cpp
    src/dbusservice/snapshotoperations.cpp
)

set(DBUS_SERVICE_HEADERS
    src/dbusservice/snapshotoperations.h
)

qt6_add_executable(qsnapper-dbus-service
    ${DBUS_SERVICE_SOURCES}
    ${DBUS_SERVICE_HEADERS}
)

target_link_libraries(qsnapper-dbus-service PRIVATE
    Qt6::Core
    Qt6::DBus
    PolkitQt6-1::Core
    snapper
)

# libsnapperバージョン検出（Version.hから抽出）
find_path(SNAPPER_INCLUDE_DIR snapper/Version.h)
if(SNAPPER_INCLUDE_DIR)
    file(STRINGS "${SNAPPER_INCLUDE_DIR}/snapper/Version.h" _ver_major
         REGEX "#define[ \t]+LIBSNAPPER_MAJOR[= \t]+\"[0-9]+\"")
    file(STRINGS "${SNAPPER_INCLUDE_DIR}/snapper/Version.h" _ver_minor
         REGEX "#define[ \t]+LIBSNAPPER_MINOR[= \t]+\"[0-9]+\"")
    string(REGEX REPLACE ".*\"([0-9]+)\".*" "\\1" SNAPPER_VERSION_MAJOR "${_ver_major}")
    string(REGEX REPLACE ".*\"([0-9]+)\".*" "\\1" SNAPPER_VERSION_MINOR "${_ver_minor}")
    message(STATUS "Detected libsnapper version: ${SNAPPER_VERSION_MAJOR}.${SNAPPER_VERSION_MINOR}")
else()
    message(WARNING "snapper/Version.h not found, assuming old libsnapper")
    set(SNAPPER_VERSION_MAJOR 0)
    set(SNAPPER_VERSION_MINOR 0)
endif()

# ログ出力ディレクトリ（-DQSNAPPER_LOG_DIR=/path/to/dir で変更可能）
set(QSNAPPER_LOG_DIR "/var/log/qsnapper" CACHE PATH "Log directory for qsnapper-dbus-service")
message(STATUS "Log directory: ${QSNAPPER_LOG_DIR}")

target_compile_definitions(qsnapper-dbus-service PRIVATE
    LIBSNAPPER_VERSION_MAJOR=${SNAPPER_VERSION_MAJOR}
    LIBSNAPPER_VERSION_MINOR=${SNAPPER_VERSION_MINOR}
    QSNAPPER_LOG_DIR="${QSNAPPER_LOG_DIR}"
)

install(TARGETS qsnapper-dbus-service
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}
)

# D-Busサービス設定ファイル（ビルド時にパスを動的生成）
configure_file(
    dbus/com.presire.qsnapper.Operations.service.in
    com.presire.qsnapper.Operations.service
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/com.presire.qsnapper.Operations.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system-services
)

# D-Busセキュリティ設定ファイル
install(FILES dbus/com.presire.qsnapper.Operations.conf
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d
)

# Polkitポリシーファイル
install(FILES polkit/com.presire.qsnapper.policy
    DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions
)

# アプリケーションアイコン
install(FILES icons/qSnapper.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps
)

# デスクトップエントリファイル
configure_file(qsnapper.desktop.in qsnapper.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qsnapper.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

# QMLプラグインのインポートと実行ファイルの最終化
# Qt 6でQMLモジュールを正しくリンクするために必要
if(QT_VERSION_MAJOR EQUAL 6)
    qt_import_qml_plugins(qsnapper)
    qt_finalize_executable(qsnapper)
endif()

########################################
# SELinux support (optional)
########################################

option(ENABLE_SELINUX "Build and install SELinux policy module" OFF)

if(ENABLE_SELINUX)
    message(STATUS "SELinux support: ENABLED")
    add_subdirectory(selinux)
else()
    message(STATUS "SELinux support: DISABLED")
    message(STATUS "To enable: cmake -DENABLE_SELINUX=ON")
endif()
