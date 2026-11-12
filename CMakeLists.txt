cmake_minimum_required(VERSION 3.25)

project(
    fooyin-jellyfin
    VERSION 0.1.0
    DESCRIPTION "Jellyfin library integration for fooyin"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

option(BUILD_TESTING "Build the plugin tests" ON)

find_package(Fooyin 0.15 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Widgets Concurrent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

add_library(
    jellyfin_library OBJECT
    src/jellyfinaccess.cpp
    src/jellyfinaccess.h
    src/jellyfinaccount.cpp
    src/jellyfinaccount.h
    src/jellyfincatalogsync.cpp
    src/jellyfincatalogsync.h
    src/jellyfincredentialstatestore.cpp
    src/jellyfincredentialstatestore.h
    src/jellyfincredentialstore.cpp
    src/jellyfincredentialstore.h
    src/jellyfininput.cpp
    src/jellyfininput.h
    src/jellyfinpage.cpp
    src/jellyfinpage.h
    src/jellyfinplaybackreporter.cpp
    src/jellyfinplaybackreporter.h
    src/jellyfinplaylistmirror.cpp
    src/jellyfinplaylistmirror.h
    src/jellyfinrequest.cpp
    src/jellyfinrequest.h
    src/jellyfintrack.cpp
    src/jellyfintrack.h
)
set_target_properties(
    jellyfin_library
    PROPERTIES POSITION_INDEPENDENT_CODE ON
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
)
target_include_directories(jellyfin_library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(
    jellyfin_library PUBLIC JELLYFIN_PLUGIN_VERSION="${PROJECT_VERSION}" QT_USE_QSTRINGBUILDER QT_NO_CAST_FROM_ASCII
)
target_link_libraries(
    jellyfin_library
    PUBLIC Fooyin::Core
           Fooyin::Gui
           Fooyin::Utils
           Qt6::Concurrent
           Qt6::Network
           Qt6::Widgets
           PkgConfig::LIBSECRET
)

create_fooyin_plugin(
    jellyfin
    DEPENDS jellyfin_library
    SOURCES src/jellyfinplugin.cpp
            src/jellyfinplugin.h
    JSON_IN src/jellyfin.json.in
)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
