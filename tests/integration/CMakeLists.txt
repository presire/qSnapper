# tests/integration/CMakeLists.txt
# 統合テスト: D-Bus contract 同期チェッカー (通常CTest) と、隔離root D-Bus
# シナリオ (test_restore_plan_owner_binding.py) 用のテスト専用サービス実行体。
#
# ルート CMakeLists.txt 側で次のように取り込む想定:
#
#   if(QSNAPPER_BUILD_TESTS)
#       enable_testing()
#       add_subdirectory(tests/unit)
#       add_subdirectory(tests/integration)
#   endif()
#
# 隔離root D-Bus シナリオは意図的に add_test() へ登録しない。
# tests/integration/run_isolated_restore_plan_test.sh から手動起動する
# (namespace root + private system-like busを要するため通常のctest実行に含めない)。

# --- dbus_contract_sync: 通常CTest ---
# Python3インタプリタが見つからない環境ではconfigureを失敗させず、
# STATUSメッセージのみでこのテストの登録をskipする。
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
# legacy (RestoreFiles/RestoreFilesDirect) とstaged (BeginRestorePlan..
# CancelRestorePlan) の2つのD-Bus認可境界がsnapshotoperations.cppの実ソース上で
# 変化していないことを静的に検証する。Python3インタプリタが見つからない環境では
# configureを失敗させず、STATUSメッセージのみでこのテストの登録をskipする。
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
# 本番のsecurity core (restoremanifest / restoreplanexecutor / filesystemhelpers /
# inputvalidator) を実ソースのままリンクする。libsnapper / PolkitQt1 / btrfsutilは
# user namespace内で動作できないためリンクしない (代替はrestoreplan_test_service.cpp
# 内のDoxygenコメントで明示する)。
# インストールせず、add_test()にも登録しない
# (run_isolated_restore_plan_test.sh 経由でのみ手動起動する)。
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
