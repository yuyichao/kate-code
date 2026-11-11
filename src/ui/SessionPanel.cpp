#include "SessionPanel.h"

#include <QFont>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

SessionPanel::SessionPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *newButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")),
                                      QStringLiteral("New Session"), this);
    newButton->setToolTip(QStringLiteral("Open a new Kate Code chat in the editor area"));
    connect(newButton, &QPushButton::clicked, this, &SessionPanel::newSessionRequested);
    layout->addWidget(newButton);

    auto *heading = new QLabel(QStringLiteral("Open sessions"), this);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setWordWrap(true);
    m_list->setUniformItemSizes(false);
    m_list->setSpacing(3);
    auto activate = [this](QListWidgetItem *item) {
        const int row = m_list->row(item);
        if (row >= 0 && row < m_entryCount) {
            Q_EMIT entryActivated(row);
        }
    };
    connect(m_list, &QListWidget::itemActivated, this, activate);
    connect(m_list, &QListWidget::itemClicked, this, activate);
    layout->addWidget(m_list, 1);

    setEntries({});
}

void SessionPanel::setEntries(const QList<Entry> &entries)
{
    m_list->clear();
    for (const Entry &entry : entries) {
        QString text = entry.title;
        if (!entry.location.isEmpty()) {
            text += QLatin1Char('\n') + entry.location;
        }
        QString secondLine = entry.status;
        if (!entry.detail.isEmpty()) {
            secondLine += QStringLiteral(" · ") + entry.detail;
        }
        if (!secondLine.isEmpty()) {
            text += QLatin1Char('\n') + secondLine;
        }
        auto *item = new QListWidgetItem(QIcon::fromTheme(entry.iconName), text, m_list);
        item->setToolTip(text);
        if (entry.active) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }
    }
    m_entryCount = entries.size();
    if (entries.isEmpty()) {
        auto *hint = new QListWidgetItem(QStringLiteral("No open sessions. Use \"New Session\" to start a chat."), m_list);
        hint->setFlags(Qt::NoItemFlags);
    }
}
