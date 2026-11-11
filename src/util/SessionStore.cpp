#include "SessionStore.h"

#include <QDebug>
#include <QDir>

#include <algorithm>

namespace
{
// Group used by the named-session list. "Sessions" holds the single id per
// project written by older versions and is only read as a fallback.
const QString kListGroup = QStringLiteral("Sessions2");
const QString kLegacyGroup = QStringLiteral("Sessions");
}

SessionStore::SessionStore(QObject *parent)
    : QObject(parent)
    , m_settings(QStringLiteral("katecode"), QStringLiteral("kate-code"))
{
    qDebug() << "[SessionStore] Initialized, config file:" << m_settings.fileName();
}

void SessionStore::addSession(const QString &projectRoot, const QString &sessionId, const QString &name, const QString &note)
{
    if (projectRoot.isEmpty() || sessionId.isEmpty()) {
        qWarning() << "[SessionStore] Cannot add: empty project root or session ID";
        return;
    }

    QString sessionName = name.isEmpty()
        ? QStringLiteral("Session %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd")))
        : name;

    QList<SessionEntry> existing = listSessions(projectRoot);

    // If session already exists, preserve its name/note unless new ones are provided
    QString resolvedNote = note;
    for (const SessionEntry &e : existing) {
        if (e.id == sessionId) {
            if (name.isEmpty()) sessionName = e.name;
            if (note.isEmpty()) resolvedNote = e.note;
            break;
        }
    }

    // Remove duplicate if same ID already in list
    existing.erase(std::remove_if(existing.begin(), existing.end(),
        [&sessionId](const SessionEntry &e) { return e.id == sessionId; }),
        existing.end());

    SessionEntry entry;
    entry.id = sessionId;
    entry.name = sessionName;
    entry.note = resolvedNote;
    entry.timestamp = QDateTime::currentDateTime();
    existing.prepend(entry);

    if (existing.size() > MaxSessions) {
        existing = existing.mid(0, MaxSessions);
    }

    writeAll(projectRoot, existing);
    qDebug() << "[SessionStore] Added session" << sessionId << "for" << projectRoot;
}

QList<SessionEntry> SessionStore::listSessions(const QString &projectRoot) const
{
    if (projectRoot.isEmpty()) {
        return {};
    }

    const QString key = normalizeKey(projectRoot);
    QSettings &settings = const_cast<QSettings &>(m_settings);

    settings.beginGroup(kListGroup);
    const bool hasList = settings.childGroups().contains(key);
    settings.beginGroup(key);
    const int count = settings.beginReadArray(QStringLiteral("sessions"));

    QList<SessionEntry> result;
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        SessionEntry entry;
        entry.id = settings.value(QStringLiteral("id")).toString();
        entry.name = settings.value(QStringLiteral("name")).toString();
        entry.note = settings.value(QStringLiteral("note")).toString();
        entry.timestamp = QDateTime::fromString(
            settings.value(QStringLiteral("timestamp")).toString(), Qt::ISODate);
        if (!entry.id.isEmpty()) {
            result.append(entry);
        }
    }

    settings.endArray();
    settings.endGroup();
    settings.endGroup();

    // Before the first list write for this project, surface the session id
    // stored by older versions so it is not lost.
    if (!hasList) {
        const QString legacyId = legacySessionId(key);
        if (!legacyId.isEmpty()) {
            SessionEntry entry;
            entry.id = legacyId;
            entry.name = QStringLiteral("Previous session");
            result.append(entry);
        }
    }

    return result;
}

bool SessionStore::hasSession(const QString &projectRoot) const
{
    return !listSessions(projectRoot).isEmpty();
}

void SessionStore::removeSession(const QString &projectRoot, const QString &sessionId)
{
    if (projectRoot.isEmpty() || sessionId.isEmpty()) {
        return;
    }
    QList<SessionEntry> sessions = listSessions(projectRoot);
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
        [&sessionId](const SessionEntry &e) { return e.id == sessionId; }),
        sessions.end());
    writeAll(projectRoot, sessions);
}

void SessionStore::renameSession(const QString &projectRoot, const QString &sessionId, const QString &newName)
{
    QList<SessionEntry> sessions = listSessions(projectRoot);
    for (auto &entry : sessions) {
        if (entry.id == sessionId) {
            entry.name = newName;
            break;
        }
    }
    writeAll(projectRoot, sessions);
}

void SessionStore::setSessionNote(const QString &projectRoot, const QString &sessionId, const QString &note)
{
    QList<SessionEntry> sessions = listSessions(projectRoot);
    for (auto &entry : sessions) {
        if (entry.id == sessionId) {
            entry.note = note;
            break;
        }
    }
    writeAll(projectRoot, sessions);
}

void SessionStore::clearSession(const QString &projectRoot)
{
    if (projectRoot.isEmpty()) {
        return;
    }

    // Write an empty list rather than removing the group, so the legacy
    // single-session fallback does not resurface a cleared session.
    writeAll(projectRoot, {});
    qDebug() << "[SessionStore] Cleared sessions for" << projectRoot;
}

QString SessionStore::getLastSession(const QString &projectRoot) const
{
    QList<SessionEntry> sessions = listSessions(projectRoot);
    return sessions.isEmpty() ? QString() : sessions.first().id;
}

void SessionStore::saveSession(const QString &projectRoot, const QString &sessionId)
{
    addSession(projectRoot, sessionId, QString());
}

void SessionStore::writeAll(const QString &projectRoot, const QList<SessionEntry> &sessions)
{
    if (projectRoot.isEmpty()) {
        return;
    }

    const QString key = normalizeKey(projectRoot);
    m_settings.beginGroup(kListGroup);
    m_settings.beginGroup(key);
    m_settings.remove(QString());
    // Keep the real path: the key encoding ('/' -> "__") is not reversible.
    m_settings.setValue(QStringLiteral("root"), QDir::cleanPath(projectRoot));
    m_settings.beginWriteArray(QStringLiteral("sessions"));
    for (int i = 0; i < sessions.size(); ++i) {
        m_settings.setArrayIndex(i);
        m_settings.setValue(QStringLiteral("id"), sessions.at(i).id);
        m_settings.setValue(QStringLiteral("name"), sessions.at(i).name);
        m_settings.setValue(QStringLiteral("note"), sessions.at(i).note);
        m_settings.setValue(QStringLiteral("timestamp"), sessions.at(i).timestamp.toString(Qt::ISODate));
    }
    m_settings.endArray();
    m_settings.endGroup();
    m_settings.endGroup();
    m_settings.sync();
}

QStringList SessionStore::listAllProjectRoots() const
{
    QSettings &settings = const_cast<QSettings &>(m_settings);
    settings.beginGroup(kListGroup);
    const QStringList keys = settings.childGroups();

    QStringList roots;
    for (const QString &key : keys) {
        settings.beginGroup(key);
        const int count = settings.beginReadArray(QStringLiteral("sessions"));
        settings.endArray();
        QString path = settings.value(QStringLiteral("root")).toString();
        settings.endGroup();

        if (count == 0) {
            continue;  // cleared project
        }
        if (path.isEmpty()) {
            path = key;
            path.replace(QStringLiteral("__"), QStringLiteral("/"));
        }
        if (!path.isEmpty() && !roots.contains(path)) {
            roots.append(path);
        }
    }
    settings.endGroup();

    // Projects that only have a session id from older versions.
    settings.beginGroup(kLegacyGroup);
    const QStringList legacyKeys = settings.childKeys();
    settings.endGroup();
    for (const QString &key : legacyKeys) {
        if (keys.contains(key) || legacySessionId(key).isEmpty()) {
            continue;
        }
        QString path = key;
        path.replace(QStringLiteral("__"), QStringLiteral("/"));
        if (!path.isEmpty() && !roots.contains(path)) {
            roots.append(path);
        }
    }

    roots.sort();
    return roots;
}

QString SessionStore::legacySessionId(const QString &key) const
{
    QSettings &settings = const_cast<QSettings &>(m_settings);
    settings.beginGroup(kLegacyGroup);
    const QString sessionId = settings.value(key).toString();
    settings.endGroup();
    return sessionId;
}

QString SessionStore::normalizeKey(const QString &projectRoot) const
{
    // Use cleaned absolute path as key, but replace slashes for QSettings compatibility
    QString normalized = QDir::cleanPath(projectRoot);
    normalized.replace(QLatin1Char('/'), QLatin1String("__"));
    return normalized;
}
