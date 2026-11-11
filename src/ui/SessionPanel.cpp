#include "SessionPanel.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace {
constexpr int KindRole = Qt::UserRole;
constexpr int KeyRole = Qt::UserRole + 1; // open: index, resumable: session id
constexpr int RootRole = Qt::UserRole + 2; // resumable: project root
}

SessionPanel::SessionPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *buttons = new QHBoxLayout();
    buttons->setSpacing(4);
    auto *newButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")),
                                      QStringLiteral("New Session"), this);
    newButton->setToolTip(QStringLiteral("Open a new Kate Code chat in the editor area"));
    connect(newButton, &QPushButton::clicked, this, &SessionPanel::newSessionRequested);
    buttons->addWidget(newButton, 1);

    auto *browseButton = new QToolButton(this);
    browseButton->setObjectName(QStringLiteral("browseSessionsButton"));
    browseButton->setIcon(QIcon::fromTheme(QStringLiteral("view-history")));
    browseButton->setToolTip(QStringLiteral("Browse previous sessions of this project with their summaries"));
    connect(browseButton, &QToolButton::clicked, this, &SessionPanel::browseRequested);
    buttons->addWidget(browseButton);

    // The resumable list follows the current directory unless this is on.
    auto *allButton = new QToolButton(this);
    allButton->setObjectName(QStringLiteral("allDirectoriesButton"));
    allButton->setIcon(QIcon::fromTheme(QStringLiteral("folder-open")));
    allButton->setCheckable(true);
    allButton->setToolTip(QStringLiteral("List previous sessions from all directories, not only the current one"));
    connect(allButton, &QToolButton::toggled, this, &SessionPanel::showAllDirectoriesChanged);
    buttons->addWidget(allButton);
    layout->addLayout(buttons);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setWordWrap(true);
    m_list->setUniformItemSizes(false);
    m_list->setSpacing(3);
    // Switching to an open chat is cheap, so a click does it. Resuming
    // starts an agent in a new tab, so it takes an activation (double-click
    // or Enter).
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (item->data(KindRole).toInt() == OpenItem) {
            Q_EMIT entryActivated(item->data(KeyRole).toInt());
        }
    });
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        switch (item->data(KindRole).toInt()) {
        case OpenItem:
            Q_EMIT entryActivated(item->data(KeyRole).toInt());
            break;
        case ResumableItem:
            Q_EMIT resumeRequested(item->data(KeyRole).toString(), item->data(RootRole).toString());
            break;
        }
    });
    layout->addWidget(m_list, 1);

    rebuild();
}

void SessionPanel::setEntries(const QList<Entry> &entries)
{
    m_entries = entries;
    rebuild();
}

void SessionPanel::setResumable(const QString &heading, const QString &headingToolTip,
                                const QList<ResumableEntry> &entries)
{
    m_resumableHeading = heading;
    m_resumableHeadingToolTip = headingToolTip;
    m_resumable = entries;
    rebuild();
}

QListWidgetItem *SessionPanel::addHeading(const QString &text, const QString &toolTip, bool divider)
{
    if (divider) {
        // A thin rule between the sections.
        auto *rule = new QListWidgetItem(m_list);
        rule->setFlags(Qt::NoItemFlags);
        rule->setSizeHint(QSize(1, 9));
        auto *line = new QWidget(m_list);
        auto *lineLayout = new QVBoxLayout(line);
        lineLayout->setContentsMargins(0, 4, 0, 4);
        auto *frame = new QFrame(line);
        frame->setFrameShape(QFrame::HLine);
        frame->setFrameShadow(QFrame::Sunken);
        lineLayout->addWidget(frame);
        m_list->setItemWidget(rule, line);
    }
    auto *item = new QListWidgetItem(text, m_list);
    item->setFlags(Qt::ItemIsEnabled);
    item->setToolTip(toolTip);
    QFont font = item->font();
    font.setBold(true);
    item->setFont(font);
    return item;
}

void SessionPanel::addHint(const QString &text)
{
    auto *hint = new QListWidgetItem(text, m_list);
    hint->setFlags(Qt::NoItemFlags);
}

void SessionPanel::rebuild()
{
    m_list->clear();

    addHeading(QStringLiteral("Open sessions"), QString(), false);
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &entry = m_entries.at(i);
        QString text = entry.title;
        QString secondLine = entry.status;
        if (!entry.detail.isEmpty()) {
            secondLine += QStringLiteral(" · ") + entry.detail;
        }
        if (!secondLine.isEmpty()) {
            text += QLatin1Char('\n') + secondLine;
        }
        auto *item = new QListWidgetItem(withStatusBadge(entry.icon, entry.statusColor), text, m_list);
        item->setData(KindRole, OpenItem);
        item->setData(KeyRole, i);
        item->setToolTip(text);
        if (entry.active) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }
    }
    if (m_entries.isEmpty()) {
        addHint(QStringLiteral("No open sessions. Use \"New Session\" to start a chat."));
    }

    if (m_resumableHeading.isEmpty()) {
        return;
    }
    addHeading(m_resumableHeading, m_resumableHeadingToolTip, true);
    for (const ResumableEntry &entry : std::as_const(m_resumable)) {
        QString text = entry.title;
        if (!entry.detail.isEmpty()) {
            text += QLatin1Char('\n') + entry.detail;
        }
        auto *item = new QListWidgetItem(entry.icon, text, m_list);
        item->setData(KindRole, ResumableItem);
        item->setData(KeyRole, entry.sessionId);
        item->setData(RootRole, entry.projectRoot);
        item->setToolTip(entry.toolTip.isEmpty() ? text : entry.toolTip);
    }
    if (m_resumable.isEmpty()) {
        addHint(QStringLiteral("No previous sessions to resume here."));
    }
}

QIcon SessionPanel::withStatusBadge(const QIcon &icon, const QColor &color) const
{
    if (!color.isValid()) {
        return icon;
    }
    QIcon result;
    const qreal dpr = devicePixelRatioF();
    for (const int size : {16, 22, 32}) {
        for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Selected}) {
            QPixmap pixmap = icon.pixmap(QSize(size, size), dpr, mode);
            if (pixmap.isNull()) {
                pixmap = QPixmap(QSize(size, size) * dpr);
                pixmap.setDevicePixelRatio(dpr);
                pixmap.fill(Qt::transparent);
            }
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            // A dot in the bottom-right corner, ringed in the background
            // colour so it stands apart from the logo.
            const qreal diameter = size * 0.45;
            const QRectF dot(size - diameter, size - diameter, diameter, diameter);
            painter.setPen(QPen(palette().color(mode == QIcon::Selected ? QPalette::Highlight : QPalette::Base),
                                qMax(1.0, size / 16.0)));
            painter.setBrush(color);
            painter.drawEllipse(dot.adjusted(0.5, 0.5, -0.5, -0.5));
            painter.end();
            result.addPixmap(pixmap, mode);
        }
    }
    return result;
}
