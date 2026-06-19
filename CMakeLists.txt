cmake_minimum_required(VERSION 3.16)

project(qsnapper VERSION 1.3.3 LANGUAGES CXX)

# Linux専用プロジェクト: 他のプラットフォームでは明示的にエラーを出す
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "qSnapper is only supported on Linux. Current platform: ${CMAKE_SYSTEM_NAME}")
endif()

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install path prefix" FORCE)
endif()

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Quick Qml QuickControls2 DBus LinguistTools)
find_package(PolkitQt6-1 REQUIRED)

qt6_standard_project_setup()

# QTP0004: サブディレクトリ (qml/pages, qml/components) のqmldir要求を抑制
# NO_PLUGIN指定でリソース埋め込みのためqmldirは不要
qt6_policy(SET QTP0004 OLD)

set(SOURCES
    src/main.cpp
    src/fssnapshot.cpp
    src/snapperservice.cpp
    src/fssnapshotstore.cpp
    src/snapshotlistmodel.cpp
    src/filechangemodel.cpp
    src/thememanager.cpp
    src/snapshotgroupmodel.cpp
)

set(HEADERS
    include/fssnapshot.h
    include/snapperservice.h
    include/fssnapshotstore.h
    include/snapshotlistmodel.h
    include/filechangemodel.h
    include/thememanager.h
    include/snapshotgroupmodel.h
)

qt6_add_executable(qsnapper ${SOURCES} ${HEADERS})

target_include_directories(qsnapper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 注意: NO_IMPORT_SCANは共有ライブラリQt向け
# QMLプラグインは実行時にロードされるため、finalize段でのスキャン/リンクは不要
# 静的Qtビルドに切り替える場合はこのオプションを外すこと
qt6_add_qml_module(qsnapper
    URI QSnapper
    VERSION 1.0
    RESOURCE_PREFIX "/qt/qml"
    NO_PLUGIN
    NO_IMPORT_SCAN
    QML_FILES
        qml/Main.qml
        qml/pages/SnapshotListPage.qml
        qml/components/SnapshotItem.qml
        qml/components/SnapshotDetailDialog.qml
        qml/components/RestorePreviewDialog.qml
        qml/components/AboutqSnapperDialog.qml
        qml/components/AboutQtDialog.qml
        qml/components/SnapshotTableHeader.qml
        qml/components/SnapshotTableRow.qml
        qml/components/SnapshotEditDialog.qml
        qml/components/CompareSnapshotsDialog.qml
)

# アイコンリソースを別途追加 (QMLモジュールとは独立)
qt6_add_resources(qsnapper "icons"
    PREFIX "/QSnapper"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        icons/directory.svg
        icons/file.svg
        icons/Qt.png
        icons/qSnapper@64.png
        icons/qSnapper@128.png
        icons/qSnapper@256.png
)

# 翻訳ファイルの設定
qt6_add_translations(qsnapper
    TS_FILES translations/qsnapper_ja.ts translations/qsnapper_de.ts
    RESOURCE_PREFIX /i18n
)

target_compile_definitions(qsnapper PRIVATE
    QSNAPPER_VERSION="${PROJECT_VERSION}"
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
    src/dbusservice/inputvalidator.cpp
)

set(DBUS_SERVICE_HEADERS
    src/dbusservice/snapshotoperations.h
    src/dbusservice/inputvalidator.h
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
    btrfsutil
)

# libsnapperバージョン検出 (Version.hから抽出)
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

# ログ出力ディレクトリ (-DQSNAPPER_LOG_DIR=/path/to/dir で変更可能)
set(QSNAPPER_LOG_DIR "/var/log/qsnapper" CACHE PATH "Log directory for qsnapper-dbus-service")
message(STATUS "Log directory: ${QSNAPPER_LOG_DIR}")

target_compile_definitions(qsnapper-dbus-service PRIVATE
    QSNAPPER_VERSION="${PROJECT_VERSION}"
    LIBSNAPPER_VERSION_MAJOR=${SNAPPER_VERSION_MAJOR}
    LIBSNAPPER_VERSION_MINOR=${SNAPPER_VERSION_MINOR}
    QSNAPPER_LOG_DIR="${QSNAPPER_LOG_DIR}"
)

install(TARGETS qsnapper-dbus-service
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}
)

# D-Busサービス設定ファイル (ビルド時にパスを動的生成)
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

# systemd-tmpfiles設定 (ログディレクトリを0700に恒久設定)
install(FILES systemd/tmpfiles.d/qsnapper.conf
    DESTINATION lib/tmpfiles.d
)

# アプリケーションアイコン (hicolor theme, 複数解像度)
install(FILES icons/qSnapper@64.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/64x64/apps
    RENAME qSnapper.png
)
install(FILES icons/qSnapper@128.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps
    RENAME qSnapper.png
)
install(FILES icons/qSnapper@256.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
    RENAME qSnapper.png
)
install(FILES icons/qSnapper@512.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
    RENAME qSnapper.png
)

# デスクトップエントリファイル
configure_file(qsnapper.desktop.in qsnapper.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qsnapper.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

# 注意:
# qt_import_qml_plugins / qt_finalize_executableは静的Qtビルド向け
# 共有ライブラリのQtでは、qt6_standard_project_setup() + qt6_add_executable() が自動でfinalizeするため明示呼び出しは不要
# (呼ぶと存在しないプラグインターゲットのリンク警告が発生する)

########################################
# Unit tests (optional)
########################################
option(QSNAPPER_BUILD_TESTS "Build unit tests (Qt Test based)" OFF)
option(QSNAPPER_TEST_SANITIZE "Enable ASan/UBSan for unit tests" OFF)

if(QSNAPPER_BUILD_TESTS)
    message(STATUS "Unit tests: ENABLED (sanitizers=${QSNAPPER_TEST_SANITIZE})")
    enable_testing()
    add_subdirectory(tests/unit)
else()
    message(STATUS "Unit tests: DISABLED (enable with -DQSNAPPER_BUILD_TESTS=ON)")
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

########################################
# Manual page (scdoc)
########################################
find_program(SCDOC_EXECUTABLE scdoc)
if(SCDOC_EXECUTABLE)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/qsnapper.1
        COMMAND ${SCDOC_EXECUTABLE} < ${CMAKE_CURRENT_SOURCE_DIR}/man/qsnapper.1.scd
                                    > ${CMAKE_CURRENT_BINARY_DIR}/qsnapper.1
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/man/qsnapper.1.scd
        VERBATIM
    )
    add_custom_target(manpage ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/qsnapper.1)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qsnapper.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
    )
    message(STATUS "Manual page: ENABLED (scdoc found at ${SCDOC_EXECUTABLE})")
else()
    message(STATUS "Manual page: DISABLED (scdoc not found)")
endif()

########################################
# Packaging (CPack)
########################################
include(cmake/packaging.cmake)
