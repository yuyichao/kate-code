#pragma once

#include <KXMLGUIClient>

#include <QObject>
#include <QPointer>
#include <QUrl>

#include <optional>

#include "RemoteLocation.h"

class KActionMenu;
class RemoteProjectsPlugin;
class RemoteTerminalPanel;

namespace KTextEditor
{
class MainWindow;
}

/**
 * Per window: File > Open Remote Folder... and Open Recent Remote Folder.
 */
class RemoteProjectsView : public QObject, public KXMLGUIClient
{
    Q_OBJECT

public:
    RemoteProjectsView(RemoteProjectsPlugin *plugin, KTextEditor::MainWindow *mainWindow);
    ~RemoteProjectsView() override;

    // "sftp://host/path", "host:/path", "user@host:/path" or a local path
    // below the mount root. Sets *error when the text is not usable.
    static std::optional<QUrl> parseInput(const QString &text, const QString &mountRoot, QString *error);

private Q_SLOTS:
    void updateTerminalLocation();

private:
    void askRemoteFolder();
    // userFromKio: a URL without a user gets the one KIO logged in with.
    void openUrl(const QUrl &url, bool userFromKio = false);
    void openUrlAs(const QUrl &url);
    void rebuildRecentMenu();
    // The remote folder of the active project (else the active document's
    // folder), if on an ssh host: the Remote Terminal tool view's.
    std::optional<KateRemote::Location> currentSshLocation() const;
    void watchProjectView(QObject *projectView);
    // The Remote Terminal tool view, created when an ssh remote folder is
    // first active in the window (then kept).
    void createTerminalToolView();
    void showMessage(const QString &text, const QString &type);

    RemoteProjectsPlugin *m_plugin;
    QPointer<KTextEditor::MainWindow> m_mainWindow;
    KActionMenu *m_recentMenu;
    QPointer<QWidget> m_terminalToolView;
    RemoteTerminalPanel *m_terminalPanel = nullptr;
};
