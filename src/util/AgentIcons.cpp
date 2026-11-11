#include "AgentIcons.h"

#include "../config/SettingsStore.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIconEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPixmapCache>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSvgRenderer>

#include <utility>

namespace {

const QUrl kRegistryUrl(QStringLiteral("https://cdn.agentclientprotocol.com/registry/v1/latest/registry.json"));
constexpr qint64 kMaxRegistryBytes = 4 * 1024 * 1024;
constexpr qint64 kMaxIconBytes = 256 * 1024;
// A cached registry is used as is for a day, a cached icon for a week; after
// that they are refreshed in the background (the stale copy stays in use).
constexpr qint64 kRegistryMaxAgeSecs = 24 * 3600;
constexpr qint64 kIconMaxAgeSecs = 7 * 24 * 3600;

bool isFresh(const QString &path, qint64 maxAgeSecs)
{
    const QFileInfo info(path);
    return info.exists()
        && info.lastModified().secsTo(QDateTime::currentDateTime()) < maxAgeSecs;
}

// Renders a monochrome SVG drawn with currentColor in the palette colour of
// the requested mode, so the logo follows the theme (and stays visible on a
// selected row).
class CurrentColorSvgEngine : public QIconEngine
{
public:
    explicit CurrentColorSvgEngine(const QByteArray &svg)
        : m_svg(svg)
        , m_key(QString::number(qHash(svg), 16))
    {
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        Q_UNUSED(state);
        QSvgRenderer renderer(colored(mode));
        if (!renderer.isValid()) {
            return;
        }
        // Keep the aspect ratio, centred in the target.
        QSizeF size = renderer.defaultSize();
        if (size.isEmpty()) {
            size = rect.size();
        }
        size.scale(rect.size(), Qt::KeepAspectRatio);
        const QRectF target(rect.x() + (rect.width() - size.width()) / 2.0,
                            rect.y() + (rect.height() - size.height()) / 2.0,
                            size.width(), size.height());
        renderer.render(painter, target);
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        const QSize deviceSize = size * scale;
        const QString key = QStringLiteral("katecode-agent-%1-%2x%3-%4")
                                .arg(m_key)
                                .arg(deviceSize.width())
                                .arg(deviceSize.height())
                                .arg(color(mode).name(QColor::HexArgb));
        QPixmap result;
        if (!QPixmapCache::find(key, &result)) {
            result = QPixmap(deviceSize);
            result.fill(Qt::transparent);
            QPainter painter(&result);
            paint(&painter, QRect(QPoint(0, 0), deviceSize), mode, state);
            painter.end();
            QPixmapCache::insert(key, result);
        }
        result.setDevicePixelRatio(scale);
        return result;
    }

    QIconEngine *clone() const override
    {
        return new CurrentColorSvgEngine(m_svg);
    }

    QString key() const override
    {
        return QStringLiteral("katecode-currentcolor-svg");
    }

private:
    static QColor color(QIcon::Mode mode)
    {
        const QPalette palette = QApplication::palette();
        switch (mode) {
        case QIcon::Selected:
            return palette.color(QPalette::Active, QPalette::HighlightedText);
        case QIcon::Disabled:
            return palette.color(QPalette::Disabled, QPalette::WindowText);
        default:
            return palette.color(QPalette::Active, QPalette::WindowText);
        }
    }

    QByteArray colored(QIcon::Mode mode) const
    {
        QByteArray svg = m_svg;
        return svg.replace("currentColor", color(mode).name().toLatin1());
    }

    QByteArray m_svg;
    QString m_key;
};

QIcon iconFromSvgData(const QByteArray &svg)
{
    if (!QSvgRenderer(svg).isValid()) {
        return QIcon();
    }
    return QIcon(new CurrentColorSvgEngine(svg));
}

QIcon iconFromSvgFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxIconBytes) {
        return QIcon();
    }
    return iconFromSvgData(file.readAll());
}

} // namespace

AgentIcons::AgentIcons(QObject *parent)
    : QObject(parent)
{
}

AgentIcons::~AgentIcons() = default;

QIcon AgentIcons::defaultIcon() const
{
    if (m_defaultIcon.isNull()) {
        m_defaultIcon = QIcon::fromTheme(QStringLiteral("code-context"));
    }
    return m_defaultIcon;
}

bool AgentIcons::isValidRegistryId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9-]{0,63}$"));
    return re.match(id).hasMatch();
}

QIcon AgentIcons::iconFor(const ACPProvider &provider)
{
    QIcon icon = explicitIcon(provider.icon.trimmed());
    if (icon.isNull()) {
        icon = registryIcon(provider.registryId.trimmed());
    }
    return icon.isNull() ? defaultIcon() : icon;
}

QIcon AgentIcons::explicitIcon(const QString &icon) const
{
    if (icon.isEmpty()) {
        return QIcon();
    }
    if (const auto it = m_explicitIcons.constFind(icon); it != m_explicitIcons.constEnd()) {
        return it.value();
    }
    const QIcon result = loadExplicitIcon(icon);
    m_explicitIcons.insert(icon, result);
    return result;
}

QIcon AgentIcons::loadExplicitIcon(const QString &icon)
{
    if (icon.contains(QLatin1Char('/'))) {
        QString path = icon;
        if (path.startsWith(QStringLiteral("~/"))) {
            path = QDir::homePath() + path.mid(1);
        }
        if (!QFileInfo(path).isFile()) {
            return QIcon();
        }
        // SVGs may use currentColor like the registry's icons.
        if (path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
            return iconFromSvgFile(path);
        }
        QIcon fileIcon(path);
        return fileIcon.availableSizes().isEmpty() ? QIcon() : fileIcon;
    }
    return QIcon::hasThemeIcon(icon) ? QIcon::fromTheme(icon) : QIcon();
}

QIcon AgentIcons::registryIcon(const QString &id)
{
    if (!isValidRegistryId(id)) {
        return QIcon();
    }
    if (const auto it = m_icons.constFind(id); it != m_icons.constEnd()) {
        return it.value();
    }
    const QString path = iconCachePath(id);
    if (QFileInfo::exists(path)) {
        const QIcon icon = iconFromSvgFile(path);
        if (!icon.isNull()) {
            m_icons.insert(id, icon);
            if (!isFresh(path, kIconMaxAgeSecs)) {
                requestRegistryIcon(id);
            }
            return icon;
        }
    }
    requestRegistryIcon(id);
    return QIcon();
}

void AgentIcons::requestRegistryIcon(const QString &id)
{
    // One attempt per run: an unknown id or a failed download is not retried
    // on every refresh.
    if (m_requested.contains(id)) {
        return;
    }
    m_requested.insert(id);
    m_pending.insert(id);
    ensureRegistry();
    if (m_registryLoaded) {
        processPending();
    }
}

void AgentIcons::ensureRegistry()
{
    if (m_registryLoaded || m_registryRequested) {
        return;
    }
    if (loadRegistryFromDisk(true)) {
        return;
    }
    m_registryRequested = true;
    QNetworkReply *reply = get(kRegistryUrl, kMaxRegistryBytes);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            const QByteArray data = reply->readAll();
            QDir().mkpath(cacheDir());
            QSaveFile file(registryCachePath());
            if (file.open(QIODevice::WriteOnly)) {
                file.write(data);
                file.commit();
            }
            parseRegistry(data);
        } else {
            qDebug() << "[AgentIcons] Could not fetch the ACP registry:" << reply->errorString();
            // A stale copy is better than nothing.
            loadRegistryFromDisk(false);
        }
        if (m_registryLoaded) {
            processPending();
        } else {
            m_pending.clear();
        }
        Q_EMIT iconsChanged();
    });
}

bool AgentIcons::loadRegistryFromDisk(bool requireFresh)
{
    const QString path = registryCachePath();
    if (requireFresh ? !isFresh(path, kRegistryMaxAgeSecs) : !QFileInfo::exists(path)) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxRegistryBytes) {
        return false;
    }
    parseRegistry(file.readAll());
    return m_registryLoaded;
}

void AgentIcons::parseRegistry(const QByteArray &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    const QJsonArray agents = doc.isObject() ? doc.object()[QStringLiteral("agents")].toArray()
                                             : doc.array();
    if (agents.isEmpty()) {
        return;
    }
    m_registryIcons.clear();
    m_registryAgents.clear();
    for (const QJsonValue &value : agents) {
        const QJsonObject agent = value.toObject();
        const QString id = agent[QStringLiteral("id")].toString();
        if (!isValidRegistryId(id)) {
            continue;
        }
        m_registryAgents.append({id, agent[QStringLiteral("name")].toString(id)});
        const QString icon = agent[QStringLiteral("icon")].toString();
        if (!icon.isEmpty()) {
            // The icon may be given relative to the registry.
            const QUrl url = kRegistryUrl.resolved(QUrl(icon));
            if (url.scheme() == QStringLiteral("https")) {
                m_registryIcons.insert(id, url);
            }
        }
    }
    m_registryLoaded = true;
}

QList<QPair<QString, QString>> AgentIcons::registryAgents()
{
    ensureRegistry();
    return m_registryAgents;
}

void AgentIcons::processPending()
{
    const QSet<QString> pending = std::exchange(m_pending, {});
    for (const QString &id : pending) {
        const QUrl url = m_registryIcons.value(id);
        if (url.isValid()) {
            downloadIcon(id, url);
        }
    }
}

void AgentIcons::downloadIcon(const QString &id, const QUrl &url)
{
    QNetworkReply *reply = get(url, kMaxIconBytes);
    connect(reply, &QNetworkReply::finished, this, [this, reply, id]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "[AgentIcons] Could not fetch the icon of" << id << ":" << reply->errorString();
            return;
        }
        const QByteArray svg = reply->readAll();
        const QIcon icon = iconFromSvgData(svg);
        if (icon.isNull()) {
            qDebug() << "[AgentIcons] The registry icon of" << id << "is not a valid SVG";
            return;
        }
        QDir().mkpath(cacheDir() + QStringLiteral("/icons"));
        QSaveFile file(iconCachePath(id));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(svg);
            file.commit();
        }
        m_icons.insert(id, icon);
        Q_EMIT iconsChanged();
    });
}

QNetworkReply *AgentIcons::get(const QUrl &url, qint64 maxBytes)
{
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    QNetworkReply *reply = m_network->get(request);
    // Registry content is untrusted: give up on anything oversized.
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply, maxBytes](qint64 received, qint64 total) {
        if (received > maxBytes || total > maxBytes) {
            reply->abort();
        }
    });
    return reply;
}

QString AgentIcons::cacheDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/kate-code/acp-registry");
}

QString AgentIcons::iconCachePath(const QString &id) const
{
    return cacheDir() + QStringLiteral("/icons/") + id + QStringLiteral(".svg");
}

QString AgentIcons::registryCachePath() const
{
    return cacheDir() + QStringLiteral("/registry.json");
}
