ecm_add_test(RemoteLocationTest.cpp
    TEST_NAME remotelocationtest
    LINK_LIBRARIES kateremotelocation Qt6::Test
)

if(BUILD_REMOTE_FS)
    add_executable(kpasswdtool kpasswdtool.cpp)
    target_link_libraries(kpasswdtool KF6::KIOCore Qt6::Core)

    # Kate Code's ACPService with agents on remote hosts.
    add_executable(acpdriver acpdriver.cpp ${CMAKE_SOURCE_DIR}/src/acp/ACPService.cpp)
    target_include_directories(acpdriver PRIVATE ${CMAKE_SOURCE_DIR}/src/acp)
    target_link_libraries(acpdriver kateremotelocation Qt6::Core Qt6::DBus)

    # Mounts a sandboxed local sftp-server through sshfs; skipped (77) when
    # sshfs, bwrap or sftp-server is missing.
    add_test(NAME kateremotefstest
        COMMAND dbus-run-session -- python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/test_remotefs.py $<TARGET_FILE:kate-remote-fs> $<TARGET_FILE:kpasswdtool> $<TARGET_FILE:acpdriver>)
    set_tests_properties(kateremotefstest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
