#pragma once

#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>

class ACPService;
class QTimer;

/**
 * ACPSessionDeleter - Deletes saved sessions on the agent's side.
 *
 * Starts the provider's agent on its own (no chat), sends `initialize`, then
 * `session/delete` for each session when the agent advertises
 * `sessionCapabilities.delete`, and stops the agent. Emits finished() once
 * and deletes itself afterwards.
 */
class ACPSessionDeleter : public QObject
{
    Q_OBJECT

public:
    ACPSessionDeleter(const QString &executable, const QStringList &args, const QString &workingDir,
                      const QStringList &sessionIds, QObject *parent = nullptr);
    ~ACPSessionDeleter() override;

    void start();

Q_SIGNALS:
    // deleted: sessions the agent confirmed; error: why the others were not
    // deleted (empty when all were).
    void finished(int deleted, const QString &error);

private:
    void onResponse(int id, const QJsonObject &result, const QJsonObject &error);
    void finish(const QString &error);

    ACPService *m_service;
    QTimer *m_timeout;
    QString m_workingDir;
    QStringList m_sessionIds;
    int m_initializeId = -1;
    QSet<int> m_pending;
    int m_deleted = 0;
    QStringList m_errors;
    bool m_finished = false;
};
