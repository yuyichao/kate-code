/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <optional>

// One plan usage window (Claude's 5-hour limit, weekly limits, ...).
struct RateLimitWindow {
    QString id; // "five_hour", "seven_day", "seven_day_opus", ...
    double utilization = -1.0; // 0..1; < 0 = unknown
    QDateTime resetsAt; // invalid = unknown
    QString status; // "allowed", "allowed_warning", "rejected", or empty

    // Utilization now: 0 once the window has reset since it was reported.
    double currentUtilization(const QDateTime &now) const;
    bool hasReset(const QDateTime &now) const;
};

// The plan's usage limits, as the agent last reported them. Account-wide,
// so shared by all chats of a provider (RateLimitStore).
struct RateLimits {
    QList<RateLimitWindow> windows; // most immediate (shortest) first
    bool usingOverage = false;
    QDateTime received;

    bool isEmpty() const
    {
        return windows.isEmpty();
    }
    // The shortest window with a known utilization (the 5-hour one when
    // reported), or nullptr.
    const RateLimitWindow *mostImmediate() const;
    // The earliest reset still ahead of now (invalid if none).
    QDateTime nextReset(const QDateTime &now) const;
    // Windows of newer replace these; the others are kept.
    void merge(const RateLimits &newer);

    // claude-agent-acp: usage_update's _meta["_claude/rateLimit"] (Claude
    // Code's SDKRateLimitInfo): "unifiedWindows" {id: {utilization (0..1),
    // resetsAt (Unix seconds), status}}, else the single window
    // rateLimitType/utilization/resetsAt/status. Empty if neither.
    static RateLimits fromClaudeRateLimit(const QJsonObject &info, const QDateTime &received);

    QJsonObject toJson() const;
    static RateLimits fromJson(const QJsonObject &json);

    // "5-hour", "Weekly (all models)", ...; the id if unknown.
    static QString windowLabel(const QString &id);
    // Shorter windows first; unknown ones last.
    static int windowRank(const QString &id);
};

// The latest rate limits per provider, shared by every chat and kept across
// Kate restarts (the agent reports them only when they change).
class RateLimitStore : public QObject
{
    Q_OBJECT

public:
    static RateLimitStore *instance();

    RateLimits limits(const QString &providerId) const;
    void update(const QString &providerId, const RateLimits &limits);

Q_SIGNALS:
    void changed(const QString &providerId);

private:
    RateLimitStore();
    QHash<QString, RateLimits> m_limits;
};
