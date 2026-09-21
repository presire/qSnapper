# tests/unit/CMakeLists.txt
# Qt Test ベースのセキュリティ修正向け単体テスト
# プロジェクトルートのCMakeLists.txt側で次のように取り込む想定:
#
#   if(QSNAPPER_BUILD_TESTS)
#       enable_testing()
#       add_subdirectory(tests/unit)
#   endif()
#
# 実装本体 (inputvalidator.cpp) は、"src/dbusservice/"にP0-3 / P0-4で追加され、本テストはそれを直接リンクする
# (D-Bus層を介さずに純粋関数を検証)

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

# --- tst_recordsafety ---
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
# ComparisonCache<T>はヘッダオンリのテンプレートなので、テスト本体以外のソース不要
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
# コミット時preflightの純粋関数群 (状態ビット -> changeType写像、path正規化、権威あるmap構築と凍結済みentry検証) をlibsnapperなしで検証する
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
# qsnapper::csv (include/csvrecord.h) はヘッダオンリのため、実装ソースはinputvalidator.cppのみ
# (制御文字が依然としてfail-closedで拒否されることを同じターゲット内で固定する)
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

# ASan / UBSanを全テストに適用 (Debugビルド時に"-DQSNAPPER_TEST_SANITIZE=ON")
if(QSNAPPER_TEST_SANITIZE)
    foreach(tgt tst_configname tst_filepaths tst_recordsafety tst_filesystemhelpers tst_filechangemodel tst_comparisoncache tst_restoremanifest tst_restorevalidation tst_restoreplanexecutor tst_csvrecord)
        target_compile_options(${tgt} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${tgt} PRIVATE
            -fsanitize=address,undefined)
    endforeach()
endif()
