# tests/integration/CMakeLists.txt
#
# 統合テスト:
# D-Bus contract同期チェッカー (通常CTest) と、隔離root D-Busシナリオ (test_restore_plan_owner_binding.py) 用のテスト専用サービス実行体
#
# プロジェクトルートのCMakeLists.txt側で次のように取り込む想定:
#
#   if(QSNAPPER_BUILD_TESTS)
#       enable_testing()
#       add_subdirectory(tests/unit)
#       add_subdirectory(tests/integration)
#   endif()
#
# 隔離root D-Busシナリオは、意図的にadd_test()へ登録しない
# tests/integration/run_isolated_restore_plan_test.sh から手動起動する
# (namespace root + private system-like busを要するため通常のctest実行に含めない)

# --- dbus_contract_sync: 通常CTest ---
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

# --- restore_authorization_boundaries: 通常CTest ---
# legacy (RestoreFiles / RestoreFilesDirect) と
# staged (BeginRestorePlan..CancelRestorePlan) の2つのD-Bus認可境界が"snapshotoperations.cpp"の実ソース上で変化していないことを静的に検証する
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

# --- qsnapper_restoreplan_test_service: テスト専用D-Busサービス (ビルドのみ) ---
# 本番のsecurity core (restoremanifest / restoreplanexecutor / filesystemhelpers / inputvalidator) を実ソースのままリンクする
# libsnapper / PolkitQt1 / btrfsutilは、user namespace内で動作できないためリンクしない (代替は、"restoreplan_test_service.cpp"内のDoxygenコメントで明示する)
#
# インストールせず、"add_test()"にも登録しない
# ("run_isolated_restore_plan_test.sh"経由でのみ手動起動する)
find_package(Qt6 REQUIRED COMPONENTS Core DBus)

add_executable(qsnapper_restoreplan_test_service
    restoreplan_test_service.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoremanifest.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoreplanexecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers.cpp
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

# --- qsnapper_change_record_injection_poc: セキュリティPoC実行体 (ビルドのみ) ---
# 変更一覧の行指向出力への改行注入 (carrierディレクトリによるパス偽装) を、
# 本番のシリアライズガード (inputvalidator) / クライアントパーサ (filechangemodel) / 実行段ガード (filesystemhelpers) の実ソースで再現・阻止確認する
# 全操作は引数で与えたscratch root配下に閉じる
#
# インストールせず、add_test()にも登録しない
# ("tests/security_poc/poc_change_record_injection.sh"経由でのみ手動起動する)
add_executable(qsnapper_change_record_injection_poc
    ${CMAKE_SOURCE_DIR}/tests/security_poc/poc_change_record_injection.cpp
    ${CMAKE_SOURCE_DIR}/src/filechangemodel.cpp
    ${CMAKE_SOURCE_DIR}/include/filechangemodel.h
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers.cpp
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

# --- qsnapper_restore_plan_tamper_poc: セキュリティPoC実行体 (ビルドのみ) ---
# staged restoreのcommit時changeType改ざん (権威ある比較照合) を、
# 実のrestorevalidation / restoremanifest / restoreplanexecutor / filesystemhelpersで
# 再現・阻止確認する。全操作は引数で与えたscratch root配下に閉じる
#
# インストールせず、add_test()にも登録しない
# ("tests/security_poc/poc_restore_plan_tamper.sh"経由でのみ手動起動する)
add_executable(qsnapper_restore_plan_tamper_poc
    ${CMAKE_SOURCE_DIR}/tests/security_poc/poc_restore_plan_tamper.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restorevalidation.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoremanifest.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/restoreplanexecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers.cpp
    ${CMAKE_SOURCE_DIR}/src/dbusservice/inputvalidator.cpp
)
target_include_directories(qsnapper_restore_plan_tamper_poc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dbusservice
)
target_link_libraries(qsnapper_restore_plan_tamper_poc PRIVATE
    Qt6::Core
)
set_target_properties(qsnapper_restore_plan_tamper_poc PROPERTIES AUTOMOC ON)
