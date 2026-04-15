# tests/unit/CMakeLists.txt
# Qt Test ベースのセキュリティ修正向け単体テスト。
# ルート CMakeLists.txt 側で次のように取り込む想定:
#
#   if(QSNAPPER_BUILD_TESTS)
#       enable_testing()
#       add_subdirectory(tests/unit)
#   endif()
#
# 実装本体 (inputvalidator.cpp) は src/dbusservice/ に P0-3/P0-4 で追加され、
# 本テストはそれを直接リンクする (D-Bus層を介さずに純粋関数を検証)。

find_package(Qt6 REQUIRED COMPONENTS Core Test)

set(INPUTVALIDATOR_SOURCES
    ${CMAKE_SOURCE_DIR}/src/dbusservice/inputvalidator.cpp
)

# --- tst_configname ---------------------------------------------------------
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

# --- tst_filepaths ----------------------------------------------------------
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

# ASan/UBSan を全テストに適用 (Debugビルド時に-DQSNAPPER_TEST_SANITIZE=ONで)
if(QSNAPPER_TEST_SANITIZE)
    foreach(tgt tst_configname tst_filepaths)
        target_compile_options(${tgt} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${tgt} PRIVATE
            -fsanitize=address,undefined)
    endforeach()
endif()
