# Remote Projects: open sftp:// folders as projects through kate-remote-fs.
qt_add_resources(kateremoteprojects_RESOURCES kateremoteprojects.qrc)

add_library(kateremoteprojects MODULE
    RemoteProjectsPlugin.cpp
    RemoteProjectsView.cpp
    RemoteTerminal.cpp
    ${kateremoteprojects_RESOURCES}
)

target_link_libraries(kateremoteprojects
    kateremotelocation
    KF6::TextEditor
    KF6::I18n
    KF6::CoreAddons
    KF6::XmlGui
    KF6::ConfigCore
    KF6::KIOCore
    KF6::Parts
    Qt6::Widgets
    Qt6::DBus
)

install(TARGETS kateremoteprojects DESTINATION ${KDE_INSTALL_PLUGINDIR}/kf6/ktexteditor)
