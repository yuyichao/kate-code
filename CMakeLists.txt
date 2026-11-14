cmake_minimum_required(VERSION 3.16)
project(katecode VERSION 1.5.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REQUIRED_KF_VERSION "6.0.0")
find_package(ECM ${REQUIRED_KF_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)

find_package(KF6 ${REQUIRED_KF_VERSION} REQUIRED COMPONENTS
    I18n
    TextEditor
    CoreAddons
    XmlGui
    SyntaxHighlighting
    Pty
    Parts
    Config
    KIO
)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    DBus
    Widgets
    WebEngineWidgets
    QuickWidgets
    WebChannel
    Network
    Svg
)

# kate-remote-fs (remote folders under ~/.kate-remote) needs libfuse 3.12+
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(FUSE3 IMPORTED_TARGET fuse3>=3.12)
endif()
include(CMakeDependentOption)
cmake_dependent_option(BUILD_REMOTE_FS "Build the kate-remote-fs daemon" ON "FUSE3_FOUND" OFF)
add_feature_info(kate-remote-fs BUILD_REMOTE_FS "remote folders (needs fuse3 >= 3.12)")

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    include(ECMAddTests)
endif()

add_subdirectory(src)

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
