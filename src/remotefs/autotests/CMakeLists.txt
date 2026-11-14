ecm_add_test(RemoteLocationTest.cpp
    TEST_NAME remotelocationtest
    LINK_LIBRARIES kateremotelocation Qt6::Test
)

if(BUILD_REMOTE_FS)
    # Mounts a sandboxed local sftp-server through sshfs; skipped (77) when
    # sshfs, bwrap or sftp-server is missing.
    add_test(NAME kateremotefstest
        COMMAND dbus-run-session -- python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/test_remotefs.py $<TARGET_FILE:kate-remote-fs>)
    set_tests_properties(kateremotefstest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
