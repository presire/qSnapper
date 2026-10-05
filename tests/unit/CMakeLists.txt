# tests/unit/CMakeLists.txt
#
# Qt Testベースの単体テストの設定
# セキュリティ修正の回帰防止を主な目的とし、各テストは1つのctestとして登録する
#
# プロジェクトルートのCMakeLists.txtから、次のように取り込む想定
#
#   if(QSNAPPER_BUILD_TESTS)
#       enable_testing()
#       add_subdirectory(tests/unit)
#   endif()
#
# 各テストは"src/dbusservice/"などの実装ソースを直接リンクし、D-Bus層を介さずに関数を検証する
# 入力検証の実装 (inputvalidator.cpp) は、P0-3とP0-4の修正で追加したもの

# 全テスト共通で使うQtモジュール
find_package(Qt6 REQUIRED COMPONENTS Core Test)

# 入力検証の実装ソース (複数のテストで共有する)
set(INPUTVALIDATOR_SOURCES
    ${CMAKE_SOURCE_DIR}/src/dbusservice/inputvalidator.cpp
)

# 安全なファイル操作ヘルパーの実装ソース (分割後の全ファイル)
set(FILESYSTEM_HELPER_SOURCES
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_internal.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_beneathroot.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_replace.cpp
)

# --- tst_configname ---
# 設定名検証 (validateConfigName) の境界と不正入力を確認する
add_executable(tst_configname
    tst_configname.cpp
    ${INPUTVALIDATOR_SOURCES}
)
target_include_directories(tst_configname PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(tst_configname PRIVATE
    Qt6::Core
    Qt6::Test
)
set_target_properties(tst_configname PROPERTIES AUTOMOC ON)
add_test(NAME tst_configname COMMAND tst_configname)

# --- tst_filepaths ---
# スナップショットルート配下の判定 (isPathWithinSnapshotRoot) と、絶対パス検証 (validateAbsoluteFilePath) の境界と不正入力を確認する
add_executable(tst_filepaths
    tst_filepaths.cpp
    ${INPUTVALIDATOR_SOURCES}
)
target_include_directories(tst_filepaths PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(tst_filepaths PRIVATE
    Qt6::Core
    Qt6::Test
)
set_target_properties(tst_filepaths PROPERTIES AUTOMOC ON)
add_test(NAME tst_filepaths COMMAND tst_filepaths)

# --- tst_recordsafety ---
# 改行の注入を許さないレコード安全化 (isRecordSafeText / sanitizeRecordText) を確認する
add_executable(tst_recordsafety
    tst_recordsafety.cpp
    ${INPUTVALIDATOR_SOURCES}
)
target_include_directories(tst_recordsafety PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(tst_recordsafety PRIVATE
    Qt6::Core
    Qt6::Test
)
set_target_properties(tst_recordsafety PROPERTIES AUTOMOC ON)
add_test(NAME tst_recordsafety COMMAND tst_recordsafety)

# --- tst_filesystemhelpers ---
# 安全なファイル操作ヘルパー (src/dbusservice/filesystemhelpers*.cpp) を確認する
# テスト本体は機能別にtst_filesystemhelpers_*.cppへ分け、クラス宣言はtst_filesystemhelpers.hに置く
# テストクラスは1つのQObjectのままのため、ctest名はtst_filesystemhelpersのまま変わらない
add_executable(tst_filesystemhelpers
    tst_filesystemhelpers.cpp
    tst_filesystemhelpers.h
    tst_filesystemhelpers_basic.cpp
    tst_filesystemhelpers_replace.cpp
    tst_filesystemhelpers_beneathroot.cpp
    tst_filesystemhelpers_beneathroot2.cpp
    tst_filesystemhelpers_dirfd.cpp
    ${FILESYSTEM_HELPER_SOURCES}
)
target_include_directories(tst_filesystemhelpers PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(tst_filesystemhelpers PRIVATE
    Qt6::Core
    Qt6::Test
)
set_target_properties(tst_filesystemhelpers PROPERTIES AUTOMOC ON)
add_test(NAME tst_filesystemhelpers COMMAND tst_filesystemhelpers)

# --- tst_filechangemodel ---
# 変更ファイルのツリーモデルと段階的復元計画のクライアント処理 (src/filechangemodel_*.cpp) を確認する
# テストのスロット定義はtst_filechangemodel_*.cppへ分け、宣言はtst_filechangemodel.hにまとめる
add_executable(tst_filechangemodel
    tst_filechangemodel.cpp
    tst_filechangemodel.h
    tst_filechangemodel_staged_a.cpp
    tst_filechangemodel_staged_b.cpp
    tst_filechangemodel_comparison.cpp
    tst_filechangemodel_filtered.cpp
    ${CMAKE_SOURCE_DIR}/src/filechangeitem.cpp
    ${CMAKE_SOURCE_DIR}/src/filechangemodel_loading.cpp
    ${CMAKE_SOURCE_DIR}/src/filechangemodel_model.cpp
    ${CMAKE_SOURCE_DIR}/src/filechangemodel_restore.cpp
    ${CMAKE_SOURCE_DIR}/src/filechangemodel_transport.cpp
    ${CMAKE_SOURCE_DIR}/include/filechangeitem.h
    ${CMAKE_SOURCE_DIR}/include/filechangemodel.h
    ${CMAKE_SOURCE_DIR}/include/restoreplantransport.h
)
target_include_directories(tst_filechangemodel PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(tst_filechangemodel PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Test
)
set_target_properties(tst_filechangemodel PROPERTIES AUTOMOC ON)
add_test(NAME tst_filechangemodel COMMAND tst_filechangemodel)

# --- tst_comparisoncache ---
# 比較結果の単一エントリキャッシュ (ComparisonCache<T>) の再利用と破棄を確認する
# ヘッダーオンリーのテンプレートのため、テスト本体以外のソースは不要
add_executable(tst_comparisoncache
    tst_comparisoncache.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/comparisoncache.h
)
target_include_directories(tst_comparisoncache PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(tst_comparisoncache PRIVATE
    Qt6::Core
    Qt6::Test
)
set_target_properties(tst_comparisoncache PROPERTIES AUTOMOC ON)
add_test(NAME tst_comparisoncache COMMAND tst_comparisoncache)

# --- tst_restoremanifest ---
# RestoreManifest / RestoreManifestRegistryの状態遷移、容量予算、TTLを確認する
add_executable(tst_restoremanifest
    tst_restoremanifest.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoremanifest.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoremanifest.h
)
target_include_directories(tst_restoremanifest PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(tst_restoremanifest PRIVATE
    Qt6::Core
    Qt6::Test
)
set_target_properties(tst_restoremanifest PROPERTIES AUTOMOC ON)
add_test(NAME tst_restoremanifest COMMAND tst_restoremanifest)

# --- tst_restorevalidation ---
# commit時のpreflightで使う純粋関数群をlibsnapperなしで検証する
# 対象は、状態ビットからchangeTypeへの写像、パスの正規化、権威あるmapの構築、凍結済みentryの検証
add_executable(tst_restorevalidation
    tst_restorevalidation.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restorevalidation.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restorevalidation.h
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoremanifest.cpp
    ${INPUTVALIDATOR_SOURCES}
)
target_include_directories(tst_restorevalidation PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(tst_restorevalidation PRIVATE
    Qt6::Core
    Qt6::Test
)
set_target_properties(tst_restorevalidation PROPERTIES AUTOMOC ON)
add_test(NAME tst_restorevalidation COMMAND tst_restorevalidation)

# --- tst_restoreplanexecutor ---
# RestorePlanExecutorのowner境界、chunk単位の実行、TTL、終端通知を確認する
add_executable(tst_restoreplanexecutor
    tst_restoreplanexecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoreplanexecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoremanifest.cpp
)
target_include_directories(tst_restoreplanexecutor PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(tst_restoreplanexecutor PRIVATE
    Qt6::Core
    Qt6::Test
)
set_target_properties(tst_restoreplanexecutor PROPERTIES AUTOMOC ON)
add_test(NAME tst_restoreplanexecutor COMMAND tst_restoreplanexecutor)

# --- tst_csvrecord ---
# CSVのquoteと解析 (qsnapper::csv、include/csvrecord.h) を確認する
# ヘッダーオンリーのため、実装ソースはinputvalidator.cppだけをリンクする
# inputvalidatorをリンクするのは、制御文字が引き続きfail-closedで拒否されることを同じターゲット内で確認するため
add_executable(tst_csvrecord
    tst_csvrecord.cpp
    ${INPUTVALIDATOR_SOURCES}
)
target_include_directories(tst_csvrecord PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(tst_csvrecord PRIVATE
    Qt6::Core
    Qt6::Test
)
set_target_properties(tst_csvrecord PROPERTIES AUTOMOC ON)
add_test(NAME tst_csvrecord COMMAND tst_csvrecord)

# --- tst_fssnapshotstore ---
# クライアント側のPreスナップショット番号ストアを確認する
# 特に、保存先がユーザー単位のデータディレクトリであることを確認する
add_executable(tst_fssnapshotstore
    tst_fssnapshotstore.cpp
    ${CMAKE_SOURCE_DIR}/src/fssnapshotstore.cpp
)
target_include_directories(tst_fssnapshotstore PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(tst_fssnapshotstore PRIVATE
    Qt6::Core
    Qt6::Test
)
set_target_properties(tst_fssnapshotstore PROPERTIES AUTOMOC ON)
add_test(NAME tst_fssnapshotstore COMMAND tst_fssnapshotstore)

# --- tst_snapshotoperations ---
# 実際のSnapshotOperationsを生成し、D-Busを介さずにスロットを直接呼んで復元計画の終端処理を検証する
# 特権操作 (rootサブボリュームのrw化) はテスト用の差し替え関数で観測し、実システムには触れない
find_package(Qt6 REQUIRED COMPONENTS DBus)
add_executable(tst_snapshotoperations
    tst_snapshotoperations.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations_auth.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations_snapshots.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations_changes.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations_diff.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations_restore.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations_restore_apply.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/inputvalidator.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_internal.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_beneathroot.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_replace.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoremanifest.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restorevalidation.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoreplanexecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations.h
)
target_include_directories(tst_snapshotoperations PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(tst_snapshotoperations PRIVATE
    LIBSNAPPER_VERSION_MAJOR=${SNAPPER_VERSION_MAJOR}
    LIBSNAPPER_VERSION_MINOR=${SNAPPER_VERSION_MINOR}
)
target_link_libraries(tst_snapshotoperations PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Test
    PolkitQt6-1::Core
    PkgConfig::POLKIT_GOBJECT
    snapper
    btrfsutil
)
set_target_properties(tst_snapshotoperations PROPERTIES AUTOMOC ON)
add_test(NAME tst_snapshotoperations COMMAND tst_snapshotoperations)

# --- tst_singleinstanceguard ---
# GUIクライアントの二重起動防止 (src/singleinstanceguard.cpp) を、一時的なruntimeディレクトリで検証する
find_package(Qt6 REQUIRED COMPONENTS Network)
add_executable(tst_singleinstanceguard
    tst_singleinstanceguard.cpp
    ${CMAKE_SOURCE_DIR}/src/singleinstanceguard.cpp
    ${CMAKE_SOURCE_DIR}/include/singleinstanceguard.h
)
target_include_directories(tst_singleinstanceguard PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(tst_singleinstanceguard PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
)
set_target_properties(tst_singleinstanceguard PROPERTIES AUTOMOC ON)
add_test(NAME tst_singleinstanceguard COMMAND tst_singleinstanceguard)

# --- tst_externalurlopener ---
# 外部ブラウザで開くURLの検証と、ポータル失敗時の直接起動への切り替え判定 (include/externalurlopener.h) を確認する
find_package(Qt6 REQUIRED COMPONENTS Gui DBus)
add_executable(tst_externalurlopener
    tst_externalurlopener.cpp
    ${CMAKE_SOURCE_DIR}/include/externalurlopener.h
)
target_include_directories(tst_externalurlopener PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(tst_externalurlopener PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::DBus
    Qt6::Test
)
set_target_properties(tst_externalurlopener PROPERTIES AUTOMOC ON)
add_test(NAME tst_externalurlopener COMMAND tst_externalurlopener)

# ASan / UBSanを全テストに適用する
# Debugビルドで"-DQSNAPPER_TEST_SANITIZE=ON"を指定した場合だけ有効になる
# テストを追加した場合は、下記のターゲット一覧にも追加すること
if(QSNAPPER_TEST_SANITIZE)
    foreach(tgt tst_configname tst_filepaths tst_recordsafety tst_filesystemhelpers tst_filechangemodel tst_comparisoncache tst_restoremanifest tst_restorevalidation tst_restoreplanexecutor tst_csvrecord tst_snapshotoperations tst_fssnapshotstore tst_singleinstanceguard tst_externalurlopener)
        target_compile_options(${tgt} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${tgt} PRIVATE
            -fsanitize=address,undefined)
    endforeach()
endif()
