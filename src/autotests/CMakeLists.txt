# Kate Code's settings (SettingsStore) with a scratch configuration
# directory: the default providers and their migrations.
ecm_add_test(SettingsStoreTest.cpp
    ${CMAKE_SOURCE_DIR}/src/config/SettingsStore.cpp
    ${CMAKE_SOURCE_DIR}/src/util/AgentIcons.cpp
    ${CMAKE_SOURCE_DIR}/src/acp/AgentInstaller.cpp
    ${CMAKE_SOURCE_DIR}/src/acp/ACPService.cpp
    ${CMAKE_SOURCE_DIR}/src/acp/RemoteHelper.cpp
    TEST_NAME settingsstoretest
    LINK_LIBRARIES kateremotelocation KF6::Archive Qt6::Test Qt6::Widgets Qt6::Svg Qt6::Network Qt6::DBus
)
target_include_directories(settingsstoretest PRIVATE ${CMAKE_SOURCE_DIR}/src/acp)
set_tests_properties(settingsstoretest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
