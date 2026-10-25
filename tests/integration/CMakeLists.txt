# tests/integration/CMakeLists.txt
#
# 統合テストの設定
#
# 通常のCTestとして、D-Bus contract同期チェッカーと認可境界チェッカーを登録する
# あわせて、隔離root D-Busシナリオ (test_restore_plan_owner_binding.py) 用のテスト専用サービスと、
# セキュリティPoCの実行体をビルドする
#
# プロジェクトルートのCMakeLists.txtから、次のように取り込む想定
#
#   if(QSNAPPER_BUILD_TESTS)
#       enable_testing()
#       add_subdirectory(tests/unit)
#       add_subdirectory(tests/integration)
#   endif()
#
# 隔離root D-Busシナリオは、意図的にadd_test()へ登録しない
# tests/integration/run_isolated_restore_plan_test.shから手動で起動する
# namespace rootと専用のsystem相当バスを必要とするため、通常のctest実行には含めない

# --- dbus_contract_sync : 通常のCTest ---
# D-Busのintrospection XMLとbusポリシー (conf) の許可メソッドが一致していることを検査する
# Python3インタプリタが見つからない環境ではconfigureを失敗させず、STATUSメッセージのみでこのテストの登録をスキップする
find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
    add_test(NAME dbus_contract_sync
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/integration/check_dbus_contract_sync.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    message(STATUS "Integration tests: dbus_contract_sync ENABLED (${Python3_EXECUTABLE})")
else()
    message(STATUS "Integration tests: dbus_contract_sync SKIPPED (Python3 interpreter not found)")
endif()

# --- restore_authorization_boundaries : 通常のCTest ---
# 段階的復元計画 (BeginRestorePlanからCancelRestorePlanまで) のD-Bus認可境界が、
# "src/dbusservice/snapshotoperations*.cpp"の実ソース上で変化していないこと、
# および廃止したlegacy (RestoreFiles / RestoreFilesDirect) が再導入されていないことを静的に検証する
# Python3インタプリタが見つからない環境ではconfigureを失敗させず、STATUSメッセージのみでこのテストの登録をスキップする
if(Python3_Interpreter_FOUND)
    add_test(NAME restore_authorization_boundaries
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/integration/check_restore_authorization_boundaries.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    message(STATUS "Integration tests: restore_authorization_boundaries ENABLED (${Python3_EXECUTABLE})")
else()
    message(STATUS "Integration tests: restore_authorization_boundaries SKIPPED (Python3 interpreter not found)")
endif()

# --- qsnapper_restoreplan_test_service : テスト専用D-Busサービス (ビルドのみ) ---
# 本番のsecurity core (restoremanifest / restoreplanexecutor / filesystemhelpers / inputvalidator) を実ソースのままリンクする
# libsnapper / PolkitQt1 / btrfsutilは、user namespace内で動作できないためリンクしない
# 代替の実装は、"restoreplan_test_service.cpp"内のDoxygenコメントに記載している
#
# インストールせず、add_test()にも登録しない
# ("run_isolated_restore_plan_test.sh"経由でのみ手動起動する)
# テスト専用サービスとPoC実行体はQtのCoreとDBusを使う
find_package(Qt6 REQUIRED COMPONENTS Core DBus)

add_executable(qsnapper_restoreplan_test_service
    restoreplan_test_service.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoremanifest.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoreplanexecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_internal.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_beneathroot.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_replace.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/inputvalidator.cpp
)
target_include_directories(qsnapper_restoreplan_test_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(qsnapper_restoreplan_test_service PRIVATE
    Qt6::Core
    Qt6::DBus
)
set_target_properties(qsnapper_restoreplan_test_service PROPERTIES AUTOMOC ON)

# --- qsnapper_restoregate_test_service : 共通ゲートを検証するテスト専用D-Busサービス (ビルドのみ) ---
# 復元の実行中に非復元操作を拒否するゲートは、本番のSnapshotOperationsの認可経路に組み込まれているため、
# 再実装ではなく本番のソースをそのままリンクする
# QSNAPPER_ENABLE_TEST_HOOKSで、polkitへの問い合わせと復元の実行中の状態だけを差し替える
# このフックは認可を迂回できるため、本ターゲット以外では定義しない
#
# インストールせず、add_test()にも登録しない
# ("run_isolated_restore_plan_test.sh"経由でのみ手動起動する)
add_executable(qsnapper_restoregate_test_service
    restoregate_test_service.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations_auth.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations_snapshots.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/systemdtimeroperation.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapperdoperation.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/configpathvalidation.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapshotoperations_configs.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/btrfsinfo.cpp
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
    ${CMAKE_SOURCE_DIR}/src/dbusservice/systemdtimeroperation.h
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapperdoperation.h
    ${CMAKE_SOURCE_DIR}/src/dbusservice/configpathvalidation.h
    ${CMAKE_SOURCE_DIR}/src/dbusservice/btrfsinfo.h
)
target_include_directories(qsnapper_restoregate_test_service PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(qsnapper_restoregate_test_service PRIVATE
    QSNAPPER_ENABLE_TEST_HOOKS
    LIBSNAPPER_VERSION_MAJOR=${SNAPPER_VERSION_MAJOR}
    LIBSNAPPER_VERSION_MINOR=${SNAPPER_VERSION_MINOR}
)
target_link_libraries(qsnapper_restoregate_test_service PRIVATE
    Qt6::Core
    Qt6::DBus
    PolkitQt6-1::Core
    PkgConfig::POLKIT_GOBJECT
    snapper
    btrfsutil
)
set_target_properties(qsnapper_restoregate_test_service PROPERTIES AUTOMOC ON)

# --- snapperdop_test_driver : SnapperdOperationの1シナリオを隔離バス上で実行するドライバ (ビルドのみ) ---
# 偽のsnapperd (fake_snapperd.py) と組み合わせ、転送、事前確認、中断、結果不明、応答期限を検査する
# test_snapperd_operation.pyがシナリオ名を引数に起動し、標準出力のCOMPLETIONとSUMMARYを読む
#
# インストールせず、add_test()にも登録しない
# ("run_isolated_restore_plan_test.sh"経由でのみ手動起動する)
add_executable(snapperdop_test_driver
    snapperdop_test_driver.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapperdoperation.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/snapperdoperation.h
)
target_include_directories(snapperdop_test_driver PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(snapperdop_test_driver PRIVATE
    Qt6::Core
    Qt6::DBus
)
set_target_properties(snapperdop_test_driver PROPERTIES AUTOMOC ON)

# --- qsnapper_change_record_injection_poc : セキュリティPoC実行体 (ビルドのみ) ---
# 変更一覧の行指向出力への改行注入 (carrierディレクトリによるパスの偽装) を再現し、阻止されることを確認する
# 本番のシリアライズガード (inputvalidator)、クライアントパーサ (filechangemodel)、
# 実行段ガード (filesystemhelpers) の実ソースをそのままリンクする
# 全操作は引数で与えたscratch root配下に閉じる
#
# インストールせず、add_test()にも登録しない
# ("tests/security_poc/poc_change_record_injection.sh"経由でのみ手動起動する)
add_executable(qsnapper_change_record_injection_poc
    ${CMAKE_SOURCE_DIR}/tests/security_poc/poc_change_record_injection.cpp
    ${CMAKE_SOURCE_DIR}/src/filechangeitem.cpp
    ${CMAKE_SOURCE_DIR}/src/filechangemodel_loading.cpp
    ${CMAKE_SOURCE_DIR}/src/filechangemodel_model.cpp
    ${CMAKE_SOURCE_DIR}/src/filechangemodel_restore.cpp
    ${CMAKE_SOURCE_DIR}/src/filechangemodel_transport.cpp
    ${CMAKE_SOURCE_DIR}/include/filechangeitem.h
    ${CMAKE_SOURCE_DIR}/include/filechangemodel.h
    ${CMAKE_SOURCE_DIR}/include/restoreplantransport.h
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_internal.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_beneathroot.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_replace.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/inputvalidator.cpp
)
target_include_directories(qsnapper_change_record_injection_poc PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(qsnapper_change_record_injection_poc PRIVATE
    Qt6::Core
    Qt6::DBus
)
set_target_properties(qsnapper_change_record_injection_poc PROPERTIES AUTOMOC ON)

# --- qsnapper_restore_plan_tamper_poc : セキュリティPoC実行体 (ビルドのみ) ---
# 段階的復元計画のcommit時に行うchangeTypeの改ざんを再現し、権威ある比較結果との照合で阻止されることを確認する
# 実際のrestorevalidation / restoremanifest / restoreplanexecutor / filesystemhelpersをリンクする
# 全操作は引数で与えたscratch root配下に閉じる
#
# インストールせず、add_test()にも登録しない
# ("tests/security_poc/poc_restore_plan_tamper.sh"経由でのみ手動起動する)
add_executable(qsnapper_restore_plan_tamper_poc
    ${CMAKE_SOURCE_DIR}/tests/security_poc/poc_restore_plan_tamper.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restorevalidation.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoremanifest.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoreplanexecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_internal.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_beneathroot.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers_replace.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/inputvalidator.cpp
)
target_include_directories(qsnapper_restore_plan_tamper_poc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(qsnapper_restore_plan_tamper_poc PRIVATE
    Qt6::Core
)
set_target_properties(qsnapper_restore_plan_tamper_poc PROPERTIES AUTOMOC ON)
