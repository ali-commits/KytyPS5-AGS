cmake_minimum_required(VERSION 3.24)
project(KytyLauncherTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Concurrent Network Widgets Test)
enable_testing()

# Standalone build against an existing native build tree: pass -DKYTY_LAUNCHER_WARM_BUILD=<dir>.
# The in-tree build (src/launcher/CMakeLists.txt, BUILD_TESTING) includes the same lists.
if(KYTY_LAUNCHER_WARM_BUILD)
    find_package(SDL3 CONFIG REQUIRED)
    include(preservation.cmake)
endif()
