#pragma once

#include <QHash>
#include <QIcon>
#include <QObject>
#include <QSet>
#include <QUrl>

struct ACPProvider;
class QNetworkAccessManager;
class QNetworkReply;

/**
 * AgentIcons - Logos for ACP providers.
 *
 * A provider's icon is, in order: its explicit icon (a theme icon name or an
 * image file), the icon its ACP registry entry publishes
 * (https://agentclientprotocol.com/get-started/registry), or the generic
 * code-context icon.
 *
 * Registry icons are monochrome SVGs drawn with currentColor; they are
 * rendered in the palette's text colour so they follow the theme. They are
 * fetched in the background and cached on disk; iconsChanged() fires when one
 * arrives. Failures (offline, unknown id) silently keep the generic icon.
 */
class AgentIcons : public QObject
{
    Q_OBJECT

public:
    explicit AgentIcons(QObject *parent = nullptr);
    ~AgentIcons() override;

    QIcon iconFor(const ACPProvider &provider);
    QIcon defaultIcon() const;

    // Registry ids are lowercase letters, digits and hyphens.
    static bool isValidRegistryId(const QString &id);

    // Agents listed in the registry (id -> display name), from the cached
    // copy. Loads the registry in the background (iconsChanged() fires) if
    // it is not known yet.
    QList<QPair<QString, QString>> registryAgents();

Q_SIGNALS:
    void iconsChanged();

private:
    QIcon explicitIcon(const QString &icon) const;
    static QIcon loadExplicitIcon(const QString &icon);
    QIcon registryIcon(const QString &id);
    void requestRegistryIcon(const QString &id);
    void ensureRegistry();
    bool loadRegistryFromDisk(bool requireFresh);
    void parseRegistry(const QByteArray &json);
    void processPending();
    void downloadIcon(const QString &id, const QUrl &url);
    QNetworkReply *get(const QUrl &url, qint64 maxBytes);

    QString cacheDir() const;
    QString iconCachePath(const QString &id) const;
    QString registryCachePath() const;

    QNetworkAccessManager *m_network = nullptr;
    QHash<QString, QIcon> m_icons;          // registry id -> icon
    // Explicit icons, kept so a provider's icon stays the same object
    // (cacheKey) between lookups.
    mutable QHash<QString, QIcon> m_explicitIcons;
    mutable QIcon m_defaultIcon;
    QHash<QString, QUrl> m_registryIcons;   // registry id -> icon URL
    QList<QPair<QString, QString>> m_registryAgents;
    bool m_registryLoaded = false;
    bool m_registryRequested = false;       // network fetch started this run
    QSet<QString> m_pending;                // ids waiting for the registry
    QSet<QString> m_requested;              // ids fetched (or tried) this run
};
