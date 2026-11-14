#pragma once

#include <KXMLGUIClient>

#include <QObject>
#include <QPointer>
#include <QUrl>

#include <optional>

class KActionMenu;
class RemoteProjectsPlugin;

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

private:
    void askRemoteFolder();
    void openUrl(const QUrl &url);
    void rebuildRecentMenu();
    void showMessage(const QString &text, const QString &type);

    RemoteProjectsPlugin *m_plugin;
    QPointer<KTextEditor::MainWindow> m_mainWindow;
    KActionMenu *m_recentMenu;
};
