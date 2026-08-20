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

set(FILESYSTEM_HELPER_SOURCES
    ${CMAKE_SOURCE_DIR}/src/dbusservice/filesystemhelpers.cpp
)

# --- tst_configname ---
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

# --- tst_filesystemhelpers ---
add_executable(tst_filesystemhelpers
    tst_filesystemhelpers.cpp
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
add_executable(tst_filechangemodel
    tst_filechangemodel.cpp
    ${CMAKE_SOURCE_DIR}/src/filechangemodel.cpp
    ${CMAKE_SOURCE_DIR}/include/filechangemodel.h
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
# ComparisonCache<T> はヘッダオンリのテンプレートなので、テスト本体以外のソース不要
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

# ASan/UBSan を全テストに適用 (Debugビルド時に-DQSNAPPER_TEST_SANITIZE=ONで)
if(QSNAPPER_TEST_SANITIZE)
    foreach(tgt tst_configname tst_filepaths tst_filesystemhelpers tst_filechangemodel tst_comparisoncache)
        target_compile_options(${tgt} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${tgt} PRIVATE
            -fsanitize=address,undefined)
    endforeach()
endif()
