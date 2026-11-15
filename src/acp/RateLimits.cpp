/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#include "RateLimits.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>

#include <algorithm>

namespace
{
const QString kSettingsGroup = QStringLiteral("RateLimits");

QSettings settings()
{
    return QSettings(QStringLiteral("kate-code"), QStringLiteral("kate-code"));
}

QDateTime fromUnixSeconds(const QJsonValue &value)
{
    const double seconds = value.toDouble(-1);
    return seconds > 0 ? QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds)) : QDateTime();
}

std::optional<double> utilizationOf(const QJsonValue &value)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    // A fraction (0.04 = 4%).
    return std::clamp(value.toDouble(), 0.0, 1.0);
}
}

bool RateLimitWindow::hasReset(const QDateTime &now) const
{
    return resetsAt.isValid() && resetsAt <= now;
}

double RateLimitWindow::currentUtilization(const QDateTime &now) const
{
    if (utilization < 0) {
        return utilization;
    }
    return hasReset(now) ? 0.0 : utilization;
}

const RateLimitWindow *RateLimits::mostImmediate() const
{
    for (const RateLimitWindow &window : windows) {
        if (window.utilization >= 0) {
            return &window;
        }
    }
    return nullptr;
}

QDateTime RateLimits::nextReset(const QDateTime &now) const
{
    QDateTime next;
    for (const RateLimitWindow &window : windows) {
        if (window.resetsAt.isValid() && window.resetsAt > now && (!next.isValid() || window.resetsAt < next)) {
            next = window.resetsAt;
        }
    }
    return next;
}

void RateLimits::merge(const RateLimits &newer)
{
    for (const RateLimitWindow &window : newer.windows) {
        auto it = std::find_if(windows.begin(), windows.end(), [&](const RateLimitWindow &w) {
            return w.id == window.id;
        });
        if (it != windows.end()) {
            *it = window;
        } else {
            windows.append(window);
        }
    }
    std::stable_sort(windows.begin(), windows.end(), [](const RateLimitWindow &a, const RateLimitWindow &b) {
        return windowRank(a.id) < windowRank(b.id);
    });
    usingOverage = newer.usingOverage;
    received = newer.received;
}

RateLimits RateLimits::fromClaudeRateLimit(const QJsonObject &info, const QDateTime &received)
{
    RateLimits result;
    result.received = received;
    result.usingOverage = info.value(QStringLiteral("isUsingOverage")).toBool();
    const QJsonObject unified = info.value(QStringLiteral("unifiedWindows")).toObject();
    for (auto it = unified.begin(); it != unified.end(); ++it) {
        const QJsonObject window = it.value().toObject();
        RateLimitWindow entry;
        entry.id = it.key();
        entry.utilization = utilizationOf(window.value(QStringLiteral("utilization"))).value_or(-1.0);
        entry.resetsAt = fromUnixSeconds(window.value(QStringLiteral("resetsAt")));
        entry.status = window.value(QStringLiteral("status")).toString();
        result.windows.append(entry);
    }
    // Older agents: the one window the limit is about.
    const QString type = info.value(QStringLiteral("rateLimitType")).toString();
    const bool listed = std::any_of(result.windows.cbegin(), result.windows.cend(), [&](const RateLimitWindow &w) {
        return w.id == type;
    });
    if (!type.isEmpty() && !listed) {
        RateLimitWindow entry;
        entry.id = type;
        entry.utilization = utilizationOf(info.value(QStringLiteral("utilization"))).value_or(-1.0);
        entry.resetsAt = fromUnixSeconds(info.value(QStringLiteral("resetsAt")));
        entry.status = info.value(QStringLiteral("status")).toString();
        result.windows.append(entry);
    }
    std::stable_sort(result.windows.begin(), result.windows.end(), [](const RateLimitWindow &a, const RateLimitWindow &b) {
        return windowRank(a.id) < windowRank(b.id);
    });
    return result;
}

QJsonObject RateLimits::toJson() const
{
    QJsonArray list;
    for (const RateLimitWindow &window : windows) {
        list.append(QJsonObject{
            {QStringLiteral("id"), window.id},
            {QStringLiteral("utilization"), window.utilization},
            {QStringLiteral("resetsAt"), window.resetsAt.isValid() ? QJsonValue(double(window.resetsAt.toSecsSinceEpoch())) : QJsonValue()},
            {QStringLiteral("status"), window.status},
        });
    }
    return QJsonObject{
        {QStringLiteral("windows"), list},
        {QStringLiteral("usingOverage"), usingOverage},
        {QStringLiteral("received"), received.isValid() ? QJsonValue(double(received.toSecsSinceEpoch())) : QJsonValue()},
    };
}

RateLimits RateLimits::fromJson(const QJsonObject &json)
{
    RateLimits result;
    for (const QJsonValue &value : json.value(QStringLiteral("windows")).toArray()) {
        const QJsonObject window = value.toObject();
        RateLimitWindow entry;
        entry.id = window.value(QStringLiteral("id")).toString();
        if (entry.id.isEmpty()) {
            continue;
        }
        entry.utilization = window.value(QStringLiteral("utilization")).toDouble(-1);
        entry.resetsAt = fromUnixSeconds(window.value(QStringLiteral("resetsAt")));
        entry.status = window.value(QStringLiteral("status")).toString();
        result.windows.append(entry);
    }
    result.usingOverage = json.value(QStringLiteral("usingOverage")).toBool();
    result.received = fromUnixSeconds(json.value(QStringLiteral("received")));
    return result;
}

QString RateLimits::windowLabel(const QString &id)
{
    static const QHash<QString, QString> labels{
        {QStringLiteral("five_hour"), QStringLiteral("5-hour")},
        {QStringLiteral("seven_day"), QStringLiteral("Weekly (all models)")},
        {QStringLiteral("seven_day_opus"), QStringLiteral("Weekly (Opus)")},
        {QStringLiteral("seven_day_sonnet"), QStringLiteral("Weekly (Sonnet)")},
        {QStringLiteral("seven_day_oauth_apps"), QStringLiteral("Weekly (apps)")},
        {QStringLiteral("seven_day_overage_included"), QStringLiteral("Weekly (with extra usage)")},
        {QStringLiteral("overage"), QStringLiteral("Extra usage")},
    };
    return labels.value(id, id);
}

int RateLimits::windowRank(const QString &id)
{
    if (id == QLatin1String("five_hour")) {
        return 0;
    }
    if (id == QLatin1String("seven_day")) {
        return 1;
    }
    if (id.startsWith(QLatin1String("seven_day"))) {
        return 2;
    }
    if (id == QLatin1String("overage")) {
        return 3;
    }
    return 4;
}

RateLimitStore::RateLimitStore()
{
    QSettings s = settings();
    s.beginGroup(kSettingsGroup);
    for (const QString &key : s.childKeys()) {
        const QJsonObject json = QJsonDocument::fromJson(s.value(key).toByteArray()).object();
        const RateLimits limits = RateLimits::fromJson(json);
        if (!limits.isEmpty()) {
            m_limits.insert(QString::fromUtf8(QByteArray::fromPercentEncoding(key.toUtf8())), limits);
        }
    }
}

RateLimitStore *RateLimitStore::instance()
{
    // Destroyed with the plugin library (not owned by the application,
    // which outlives an unloaded plugin).
    static RateLimitStore store;
    return &store;
}

RateLimits RateLimitStore::limits(const QString &providerId) const
{
    return m_limits.value(providerId);
}

void RateLimitStore::update(const QString &providerId, const RateLimits &limits)
{
    if (limits.isEmpty()) {
        return;
    }
    RateLimits &stored = m_limits[providerId];
    stored.merge(limits);
    QSettings s = settings();
    s.beginGroup(kSettingsGroup);
    // Provider ids may contain characters QSettings treats specially.
    s.setValue(QString::fromLatin1(providerId.toUtf8().toPercentEncoding()), QJsonDocument(stored.toJson()).toJson(QJsonDocument::Compact));
    Q_EMIT changed(providerId);
}
