# URL <-> path mapping shared by kate-remote-fs, the plugins and Kate Code.
add_library(kateremotelocation STATIC RemoteLocation.cpp BuildVersion.cpp)
set_target_properties(kateremotelocation PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(kateremotelocation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kateremotelocation PUBLIC Qt6::Core KF6::ConfigCore)

# The daemon and the plugins compare versions (a daemon left running across
# an upgrade); the git commit tells builds of one version apart.
set(KATE_REMOTE_BUILD_VERSION "${PROJECT_VERSION}")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE KATE_REMOTE_GIT_COMMIT
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE KATE_REMOTE_GIT_RESULT
                    ERROR_QUIET)
    if(KATE_REMOTE_GIT_RESULT EQUAL 0 AND KATE_REMOTE_GIT_COMMIT)
        set(KATE_REMOTE_BUILD_VERSION "${PROJECT_VERSION}-${KATE_REMOTE_GIT_COMMIT}")
    endif()
endif()
target_compile_definitions(kateremotelocation PRIVATE KATE_REMOTE_BUILD_VERSION="${KATE_REMOTE_BUILD_VERSION}")

if(BUILD_REMOTE_FS)
    add_executable(kate-remote-fs
        daemon/main.cpp
        daemon/RemoteFs.cpp
        daemon/RemoteFsService.cpp
    )
    target_link_libraries(kate-remote-fs kateremotelocation Qt6::Core Qt6::DBus PkgConfig::FUSE3)
    install(TARGETS kate-remote-fs DESTINATION ${KDE_INSTALL_LIBEXECDIR})

    # SSH_ASKPASS for the mounts: login passwords from KDE's password cache
    add_executable(kate-remote-fs-askpass askpass/main.cpp)
    target_link_libraries(kate-remote-fs-askpass kateremotelocation KF6::KIOCore KF6::I18n Qt6::Core)
    install(TARGETS kate-remote-fs-askpass DESTINATION ${KDE_INSTALL_LIBEXECDIR})
    target_compile_definitions(kate-remote-fs PRIVATE
        KATE_REMOTE_ASKPASS_PATH="${KDE_INSTALL_FULL_LIBEXECDIR}/kate-remote-fs-askpass")

    configure_file(daemon/kate-remote-fs.service.in ${CMAKE_CURRENT_BINARY_DIR}/kate-remote-fs.service @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kate-remote-fs.service DESTINATION ${KDE_INSTALL_SYSTEMDUSERUNITDIR})
    configure_file(daemon/org.kde.KateRemoteFS.service.in ${CMAKE_CURRENT_BINARY_DIR}/org.kde.KateRemoteFS.service @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.KateRemoteFS.service DESTINATION ${KDE_INSTALL_DBUSSERVICEDIR})
endif()

if(BUILD_TESTING)
    add_subdirectory(autotests)
endif()
