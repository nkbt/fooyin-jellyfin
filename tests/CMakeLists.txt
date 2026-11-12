find_package(GTest REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Sql Test)
include(GoogleTest)

add_library(jellyfin_test_support STATIC fakejellyfinserver.cpp fakejellyfinserver.h syntheticnetworkaccessmanager.cpp syntheticnetworkaccessmanager.h testsupport.cpp testsupport.h)
target_include_directories(jellyfin_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(jellyfin_test_support PUBLIC JELLYFIN_TEST_CERT_DIR="${CMAKE_CURRENT_BINARY_DIR}/tls")
target_link_libraries(jellyfin_test_support PUBLIC jellyfin_library Qt6::Test GTest::gtest)

function(jellyfin_add_test name)
    add_executable(${name} ${ARGN} testmain.cpp)
    target_link_libraries(${name} PRIVATE jellyfin_test_support)
    gtest_discover_tests(
        ${name}
        DISCOVERY_MODE PRE_TEST
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endfunction()

jellyfin_add_test(test_jellyfinrequest jellyfinrequesttest.cpp)
jellyfin_add_test(test_jellyfintrack jellyfintracktest.cpp)
jellyfin_add_test(test_jellyfinaccount jellyfinaccounttest.cpp)
jellyfin_add_test(test_jellyfincredentialrace jellyfincredentialracetest.cpp)
jellyfin_add_test(test_jellyfincredentialstate jellyfincredentialstatetest.cpp)
jellyfin_add_test(test_jellyfincatalogsync jellyfincatalogsynctest.cpp)
jellyfin_add_test(test_jellyfinstreamdevice jellyfinstreamdevicetest.cpp)
jellyfin_add_test(test_jellyfininput jellyfininputtest.cpp)
jellyfin_add_test(test_jellyfinplaybackreporter jellyfinplaybackreportertest.cpp)
jellyfin_add_test(test_jellyfinplaylistmirror jellyfinplaylistmirrortest.cpp)
target_link_libraries(test_jellyfinplaylistmirror PRIVATE Qt6::Sql)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME JellyfinLauncher.DoesNotContactAnotherProfile COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launchercollisiontest.py ${CMAKE_SOURCE_DIR}/scripts/launch-candidate.sh)
add_test(NAME JellyfinLauncher.RejectsAMissingOptionValue COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launchermissingvaluetest.py ${CMAKE_SOURCE_DIR}/scripts/launch-candidate.sh)

set(FOOYIN_SOURCE_DIR "" CACHE PATH "Matching host source tree for native integration tests")
if(FOOYIN_SOURCE_DIR)
    pkg_check_modules(FFMPEG_TEST REQUIRED IMPORTED_TARGET libavutil)
    add_executable(test_jellyfinintegration jellyfinintegrationtest.cpp ${FOOYIN_SOURCE_DIR}/data/data.qrc ${FOOYIN_SOURCE_DIR}/data/icons.qrc ${FOOYIN_SOURCE_DIR}/data/layouts.qrc)
    set_target_properties(test_jellyfinintegration PROPERTIES AUTORCC ON)
    target_compile_definitions(test_jellyfinintegration PRIVATE JELLYFIN_HOST_CORE_LIBRARY="$<TARGET_FILE:Fooyin::Core>")
    target_include_directories(test_jellyfinintegration PRIVATE ${FOOYIN_SOURCE_DIR}/src ${FOOYIN_SOURCE_DIR}/src/core ${FOOYIN_SOURCE_DIR}/src/gui)
    target_link_libraries(test_jellyfinintegration PRIVATE jellyfin_test_support PkgConfig::FFMPEG_TEST)
    gtest_discover_tests(test_jellyfinintegration DISCOVERY_MODE PRE_TEST PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The host settings tree model is not exported, so the test compiles the matching host source.
    jellyfin_add_test(test_jellyfinsettingspage jellyfinsettingspagetest.cpp ${FOOYIN_SOURCE_DIR}/src/utils/settings/settingsmodel.cpp)
    target_include_directories(test_jellyfinsettingspage PRIVATE ${FOOYIN_SOURCE_DIR}/src)
endif()

find_program(OPENSSL_EXECUTABLE openssl REQUIRED)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tls)
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/tls/certificate.pem)
    execute_process(
        COMMAND ${OPENSSL_EXECUTABLE} req -x509 -newkey rsa:2048 -nodes
                -keyout ${CMAKE_CURRENT_BINARY_DIR}/tls/key.pem
                -out ${CMAKE_CURRENT_BINARY_DIR}/tls/certificate.pem
                -days 3650 -subj /CN=127.0.0.1 -addext subjectAltName=IP:127.0.0.1
                -addext basicConstraints=critical,CA:TRUE
        RESULT_VARIABLE tls_fixture_result OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT tls_fixture_result EQUAL 0)
        message(FATAL_ERROR "Could not create the synthetic TLS fixture")
    endif()
endif()
