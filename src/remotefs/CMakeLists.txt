# URL <-> path mapping shared by kate-remote-fs, the plugins and Kate Code.
add_library(kateremotelocation STATIC RemoteLocation.cpp)
set_target_properties(kateremotelocation PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(kateremotelocation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kateremotelocation PUBLIC Qt6::Core KF6::ConfigCore)

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
