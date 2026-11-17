#include "KateCodeConfigPage.h"
#include "SettingsStore.h"
#include "../acp/AgentInstaller.h"
#include "../util/AgentIcons.h"

#include <KLocalizedString>

#include <QAbstractItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QSysInfo>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

static QJsonValue parseSessionConfigValue(const QString &text);

static QString sessionConfigValueText(const QJsonValue &value)
{
    if (value.isString()) {
        const QString text = value.toString();
        const QJsonValue reparsed = parseSessionConfigValue(text);
        if (reparsed.isString() && reparsed.toString() == text) {
            return text;
        }

        // Quote strings such as "true", "42", or JSON-looking text so an
        // unchanged edit round-trips as a string rather than changing type.
        QJsonArray wrapper;
        wrapper.append(text);
        QString encoded = QString::fromUtf8(QJsonDocument(wrapper).toJson(QJsonDocument::Compact));
        return encoded.mid(1, encoded.size() - 2);
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'g', 15);
    }
    if (value.isNull() || value.isUndefined()) {
        return QStringLiteral("null");
    }
    if (value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
}

static QJsonValue parseSessionConfigValue(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }

    QJsonParseError parseError;
    const QByteArray wrapped = QByteArrayLiteral("{\"value\":") + trimmed.toUtf8() + QByteArrayLiteral("}");
    const QJsonDocument document = QJsonDocument::fromJson(wrapped, &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject()) {
        return document.object().value(QStringLiteral("value"));
    }
    return trimmed;
}

static QString providerToolTip(const ACPProvider &provider)
{
    QStringList lines = {
        i18n("Executable: %1", provider.executable),
        i18n("CLI options: %1", provider.options.isEmpty() ? i18n("(none)") : provider.options),
        i18n("MCP config JSON: %1", provider.mcpConfigPath.isEmpty() ? i18n("(none)") : provider.mcpConfigPath),
        i18n("True resume: %1", provider.trueResume ? i18n("Yes") : i18n("No")),
        i18n("Icon: %1", provider.icon.isEmpty() ? i18n("(from the ACP registry)") : provider.icon),
        i18n("ACP registry id: %1", provider.registryId.isEmpty() ? i18n("(none)") : provider.registryId),
    };
    if (provider.sessionConfig.isEmpty()) {
        lines.append(i18n("ACP session configuration: (none)"));
    } else {
        lines.append(i18n("ACP session configuration:"));
        for (auto it = provider.sessionConfig.constBegin(); it != provider.sessionConfig.constEnd(); ++it) {
            lines.append(QStringLiteral("  %1 = %2").arg(it.key(), sessionConfigValueText(it.value())));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

KateCodeConfigPage::KateCodeConfigPage(SettingsStore *settings, QWidget *parent)
    : KTextEditor::ConfigPage(parent)
    , m_settings(settings)
    , m_hasChanges(false)
{
    setupUi();

    // Load current settings
    reset();

    // Registry logos arrive in the background.
    connect(m_settings->agentIcons(), &AgentIcons::iconsChanged, this, [this]() {
        for (int row = 0; row < m_providerList->count(); ++row) {
            QListWidgetItem *item = m_providerList->item(row);
            item->setIcon(m_settings->agentIcons()->iconFor(
                m_settings->providerById(item->data(Qt::UserRole).toString())));
        }
    });
}

KateCodeConfigPage::~KateCodeConfigPage() = default;

QString KateCodeConfigPage::name() const
{
    return i18n("Kate Code");
}

QString KateCodeConfigPage::fullName() const
{
    return i18n("Kate Code Plugin Settings");
}

QIcon KateCodeConfigPage::icon() const
{
    return QIcon::fromTheme(QStringLiteral("code-context"));
}

void KateCodeConfigPage::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);

    // Create tab widget
    m_tabWidget = new QTabWidget(this);

    // Create General tab
    auto *generalTab = new QWidget();
    setupGeneralTab(generalTab);
    m_tabWidget->addTab(generalTab, i18n("General"));

    // Create Advanced tab
    auto *advancedTab = new QWidget();
    setupAdvancedTab(advancedTab);
    m_tabWidget->addTab(advancedTab, i18n("Advanced"));

    mainLayout->addWidget(m_tabWidget);
}

void KateCodeConfigPage::setupGeneralTab(QWidget *tab)
{
    auto *tabLayout = new QVBoxLayout(tab);

    // ACP Providers Group
    auto *providerGroup = new QGroupBox(i18n("ACP Providers"), tab);
    auto *providerLayout = new QVBoxLayout(providerGroup);

    m_providerList = new QListWidget(tab);
    m_providerList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_providerList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_providerList->setDragDropMode(QAbstractItemView::InternalMove);
    m_providerList->setDefaultDropAction(Qt::MoveAction);
    m_providerList->setDropIndicatorShown(true);
    m_providerList->setToolTip(i18n("Drag providers to reorder them. Hover over a provider to see its full configuration."));
    providerLayout->addWidget(m_providerList);

    auto *buttonLayout = new QHBoxLayout();
    m_addProviderButton = new QPushButton(i18n("Add..."), tab);
    m_editProviderButton = new QPushButton(i18n("Edit..."), tab);
    m_removeProviderButton = new QPushButton(i18n("Remove"), tab);
    m_moveProviderUpButton = new QPushButton(i18n("Move Up"), tab);
    m_moveProviderDownButton = new QPushButton(i18n("Move Down"), tab);
    m_addFromRegistryButton = new QPushButton(i18n("Add from Registry..."), tab);
    m_addFromRegistryButton->setToolTip(i18n("Add an agent listed in the ACP registry (agentclientprotocol.com)"));
    m_installProviderButton = new QPushButton(i18n("Install..."), tab);
    m_installProviderButton->setToolTip(i18n("Install the selected agent on this computer from the ACP registry"));
    m_installProviderButton->setEnabled(false);
    m_editProviderButton->setEnabled(false);
    m_removeProviderButton->setEnabled(false);
    m_moveProviderUpButton->setEnabled(false);
    m_moveProviderDownButton->setEnabled(false);
    buttonLayout->addWidget(m_addProviderButton);
    buttonLayout->addWidget(m_addFromRegistryButton);
    buttonLayout->addWidget(m_editProviderButton);
    buttonLayout->addWidget(m_removeProviderButton);
    buttonLayout->addWidget(m_moveProviderUpButton);
    buttonLayout->addWidget(m_moveProviderDownButton);
    buttonLayout->addWidget(m_installProviderButton);
    buttonLayout->addStretch();
    providerLayout->addLayout(buttonLayout);

    connect(m_addProviderButton, &QPushButton::clicked, this, &KateCodeConfigPage::onAddProvider);
    connect(m_addFromRegistryButton, &QPushButton::clicked, this, &KateCodeConfigPage::onAddFromRegistry);
    connect(m_installProviderButton, &QPushButton::clicked, this, &KateCodeConfigPage::onInstallProvider);
    connect(m_editProviderButton, &QPushButton::clicked, this, &KateCodeConfigPage::onEditProvider);
    connect(m_removeProviderButton, &QPushButton::clicked, this, &KateCodeConfigPage::onRemoveProvider);
    connect(m_moveProviderUpButton, &QPushButton::clicked, this, &KateCodeConfigPage::onMoveProviderUp);
    connect(m_moveProviderDownButton, &QPushButton::clicked, this, &KateCodeConfigPage::onMoveProviderDown);
    connect(m_providerList, &QListWidget::currentRowChanged, this, [this](int) {
        updateProviderButtons();
    });
    connect(m_providerList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        onEditProvider();
    });
    connect(m_providerList->model(), &QAbstractItemModel::rowsMoved, this, [this]() {
        saveProviderOrder();
        updateProviderButtons();
    });

    auto *providerNote = new QLabel(i18n("Drag providers to set their order; only descriptions are shown here. Edit a provider to see its full launch, MCP, resume, and ACP session configuration. At least one provider must remain. Agents that are not installed on this computer are not offered in chats (except for remote folders); Install... installs them from the ACP registry."), tab);
    providerNote->setWordWrap(true);
    providerNote->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
    providerLayout->addWidget(providerNote);

    tabLayout->addWidget(providerGroup);

    // Diff Colors Group
    auto *diffGroup = new QGroupBox(i18n("Diff Highlighting"), tab);
    auto *diffLayout = new QFormLayout(diffGroup);

    m_diffColorSchemeCombo = new QComboBox(tab);
    m_diffColorSchemeCombo->addItem(
        SettingsStore::schemeDisplayName(DiffColorScheme::RedGreen),
        static_cast<int>(DiffColorScheme::RedGreen));
    m_diffColorSchemeCombo->addItem(
        SettingsStore::schemeDisplayName(DiffColorScheme::BlueOrange),
        static_cast<int>(DiffColorScheme::BlueOrange));
    m_diffColorSchemeCombo->addItem(
        SettingsStore::schemeDisplayName(DiffColorScheme::PurpleGreen),
        static_cast<int>(DiffColorScheme::PurpleGreen));
    connect(m_diffColorSchemeCombo, &QComboBox::currentIndexChanged,
            this, &KateCodeConfigPage::onSettingChanged);
    diffLayout->addRow(i18n("Color scheme:"), m_diffColorSchemeCombo);

    auto *diffNote = new QLabel(i18n("Choose a colorblind-friendly scheme if you have difficulty distinguishing red and green."), tab);
    diffNote->setWordWrap(true);
    diffNote->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
    diffLayout->addRow(diffNote);

    tabLayout->addWidget(diffGroup);

    // Remote hosts Group
    auto *remoteGroup = new QGroupBox(i18n("Remote Hosts"), tab);
    auto *remoteLayout = new QVBoxLayout(remoteGroup);
    m_remoteHelperCheck = new QCheckBox(i18n("Install and use Kate Code's helper on hosts where agents run"), tab);
    connect(m_remoteHelperCheck, &QCheckBox::toggled, this, &KateCodeConfigPage::onSettingChanged);
    remoteLayout->addWidget(m_remoteHelperCheck);
    auto *remoteNote = new QLabel(i18n("For agents of remote folders: a small program (kate-code-helper, about 1 MB, the version pinned in this "
                                       "Kate Code) is installed in ~/.local/share/kate-code on the host and tells Kate when open documents "
                                       "change there, within about 2 seconds instead of 20. Applies to newly connected chats."),
                                  tab);
    remoteNote->setWordWrap(true);
    remoteNote->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
    remoteLayout->addWidget(remoteNote);
    tabLayout->addWidget(remoteGroup);

    // Debugging Group
    auto *debugGroup = new QGroupBox(i18n("Debugging"), tab);
    auto *debugLayout = new QVBoxLayout(debugGroup);

    m_debugLoggingCheck = new QCheckBox(i18n("Log ACP protocol JSON to Output view"), tab);
    connect(m_debugLoggingCheck, &QCheckBox::toggled,
            this, &KateCodeConfigPage::onSettingChanged);
    debugLayout->addWidget(m_debugLoggingCheck);

    auto *debugNote = new QLabel(i18n("When enabled, all JSON-RPC messages sent to and received from the ACP server are logged to Kate's Output panel."), tab);
    debugNote->setWordWrap(true);
    debugNote->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
    debugLayout->addWidget(debugNote);

    tabLayout->addWidget(debugGroup);

    // Stretch to push everything to top
    tabLayout->addStretch();
}

void KateCodeConfigPage::setupAdvancedTab(QWidget *tab)
{
    auto *tabLayout = new QVBoxLayout(tab);

    // Session Summaries Group
    auto *summaryGroup = new QGroupBox(i18n("Session Summaries"), tab);
    auto *summaryLayout = new QFormLayout(summaryGroup);

    m_enableSummariesCheck = new QCheckBox(i18n("Generate summaries when sessions end"), tab);
    connect(m_enableSummariesCheck, &QCheckBox::toggled,
            this, &KateCodeConfigPage::onSettingChanged);
    summaryLayout->addRow(m_enableSummariesCheck);

    m_summaryProviderCombo = new QComboBox(tab);
    connect(m_summaryProviderCombo, &QComboBox::currentIndexChanged,
            this, &KateCodeConfigPage::onSettingChanged);
    summaryLayout->addRow(i18n("Summary agent:"), m_summaryProviderCombo);

    auto *summaryNote = new QLabel(i18n("The selected agent generates the summary when a session ends. "
                                        "\"Current agent\" asks the live session to summarise itself before it disconnects; "
                                        "other agents run briefly in the background. "
                                        "Summaries are stored in ~/.kate-code/summaries/ and can be used as context when resuming sessions."), tab);
    summaryNote->setWordWrap(true);
    summaryNote->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
    summaryLayout->addRow(summaryNote);

    tabLayout->addWidget(summaryGroup);

    // Session Resume Group
    auto *sessionGroup = new QGroupBox(i18n("Session Resume"), tab);
    auto *sessionLayout = new QVBoxLayout(sessionGroup);

    m_autoResumeCheck = new QCheckBox(i18n("Prompt to resume previous session when connecting"), tab);
    connect(m_autoResumeCheck, &QCheckBox::toggled,
            this, &KateCodeConfigPage::onSettingChanged);
    sessionLayout->addWidget(m_autoResumeCheck);

    m_summariseOnResumeCheck = new QCheckBox(i18n("Summarise an abandoned (raw) session before resuming"), tab);
    connect(m_summariseOnResumeCheck, &QCheckBox::toggled,
            this, &KateCodeConfigPage::onSettingChanged);
    sessionLayout->addWidget(m_summariseOnResumeCheck);

    auto *resumeNote = new QLabel(i18n("When resuming a session that has no summary yet, generate one with the summary agent first. Otherwise the raw transcript is used as context."), tab);
    resumeNote->setWordWrap(true);
    resumeNote->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
    sessionLayout->addWidget(resumeNote);

    tabLayout->addWidget(sessionGroup);

    // ACP File Logging Group
    auto *logGroup = new QGroupBox(i18n("ACP Session Logging"), tab);
    auto *logLayout = new QFormLayout(logGroup);

    m_acpLogEnableCheck = new QCheckBox(i18n("Write raw ACP JSON traffic to a file"), tab);
    connect(m_acpLogEnableCheck, &QCheckBox::toggled,
            this, &KateCodeConfigPage::onSettingChanged);
    logLayout->addRow(m_acpLogEnableCheck);

    m_acpLogDirEdit = new QLineEdit(tab);
    m_acpLogDirEdit->setPlaceholderText(i18n("e.g. /tmp"));
    connect(m_acpLogDirEdit, &QLineEdit::textChanged,
            this, &KateCodeConfigPage::onSettingChanged);
    logLayout->addRow(i18n("Base directory:"), m_acpLogDirEdit);

    m_acpLogSubdirEdit = new QLineEdit(tab);
    m_acpLogSubdirEdit->setPlaceholderText(i18n("e.g. kate_code_sessions"));
    connect(m_acpLogSubdirEdit, &QLineEdit::textChanged,
            this, &KateCodeConfigPage::onSettingChanged);
    logLayout->addRow(i18n("Subdirectory name:"), m_acpLogSubdirEdit);

    auto *logNote = new QLabel(i18n("Each session writes one timestamped .json file (one JSON object per line, flushed immediately) into the subdirectory created inside the base directory. This is separate from the on-screen chat."), tab);
    logNote->setWordWrap(true);
    logNote->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
    logLayout->addRow(logNote);

    tabLayout->addWidget(logGroup);

    // Command Auto-Approval Group
    auto *approvalGroup = new QGroupBox(i18n("Command Auto-Approval"), tab);
    auto *approvalLayout = new QVBoxLayout(approvalGroup);

    auto *approvalNote = new QLabel(
        i18n("Commands run by the agent whose text matches one of these glob patterns "
             "are approved automatically without prompting. Enter one pattern per line. "
             "Leave empty to always prompt.\n"
             "Examples: git status, ls *, npm run *"),
        tab);
    approvalNote->setWordWrap(true);
    approvalLayout->addWidget(approvalNote);

    m_allowedCommandsEdit = new QPlainTextEdit(tab);
    m_allowedCommandsEdit->setPlaceholderText(i18n("e.g.\ngit status\nls *\nnpm run *"));
    connect(m_allowedCommandsEdit, &QPlainTextEdit::textChanged,
            this, &KateCodeConfigPage::onSettingChanged);
    approvalLayout->addWidget(m_allowedCommandsEdit);

    tabLayout->addWidget(approvalGroup);

    // Stretch to push everything to top
    tabLayout->addStretch();
}

void KateCodeConfigPage::populateProviderList()
{
    const QString selectedId = m_providerList->currentItem()
        ? m_providerList->currentItem()->data(Qt::UserRole).toString()
        : QString();
    const QSignalBlocker blocker(m_providerList);
    m_providerList->clear();
    const auto providerList = m_settings->providers();
    for (const auto &p : providerList) {
        const bool installed = SettingsStore::isExecutableAvailable(p.executable);
        auto *item = new QListWidgetItem(m_settings->agentIcons()->iconFor(p),
                                         installed ? p.description : i18n("%1 (not installed)", p.description),
                                         m_providerList);
        item->setData(Qt::UserRole, p.id);
        item->setData(Qt::UserRole + 1, installed);
        item->setToolTip(providerToolTip(p));
        if (p.id == selectedId) {
            m_providerList->setCurrentItem(item);
        }
    }
    if (!m_providerList->currentItem() && m_providerList->count() > 0) {
        m_providerList->setCurrentRow(0);
    }
    updateProviderButtons();

    // The summary-agent dropdown mirrors the provider set, so refresh it too,
    // keeping the on-screen (possibly unapplied) selection.
    populateSummaryProviderCombo(m_summaryProviderCombo->currentData().toString());
}

void KateCodeConfigPage::populateSummaryProviderCombo(const QString &selectedId)
{
    const QSignalBlocker blocker(m_summaryProviderCombo);
    m_summaryProviderCombo->clear();
    m_summaryProviderCombo->addItem(i18n("Current agent"), SettingsStore::CURRENT_AGENT_PROVIDER_ID);
    const auto providerList = m_settings->providers();
    for (const auto &p : providerList) {
        m_summaryProviderCombo->addItem(p.description, p.id);
    }

    const int index = m_summaryProviderCombo->findData(selectedId);
    m_summaryProviderCombo->setCurrentIndex(index >= 0 ? index : 0);
}

void KateCodeConfigPage::apply()
{
    if (!m_hasChanges) {
        return;
    }

    // Save settings
    m_settings->setSummariesEnabled(m_enableSummariesCheck->isChecked());
    m_settings->setSummaryProviderId(m_summaryProviderCombo->currentData().toString());
    m_settings->setAutoResumeSessions(m_autoResumeCheck->isChecked());
    m_settings->setSummariseOnResume(m_summariseOnResumeCheck->isChecked());
    m_settings->setAcpLogEnabled(m_acpLogEnableCheck->isChecked());
    m_settings->setAcpLogDirectory(m_acpLogDirEdit->text().trimmed());
    m_settings->setAcpLogSubdirectory(m_acpLogSubdirEdit->text().trimmed());
    m_settings->setDiffColorScheme(static_cast<DiffColorScheme>(m_diffColorSchemeCombo->currentData().toInt()));
    m_settings->setDebugLogging(m_debugLoggingCheck->isChecked());
    m_settings->setRemoteHelperEnabled(m_remoteHelperCheck->isChecked());

    // Save allowed command patterns (split on newlines, trim, drop blanks)
    const QStringList rawLines = m_allowedCommandsEdit->toPlainText().split(QLatin1Char('\n'));
    QStringList patterns;
    for (const QString &line : rawLines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            patterns.append(trimmed);
        }
    }
    m_settings->setAllowedCommandPatterns(patterns);

    m_hasChanges = false;
}

void KateCodeConfigPage::defaults()
{
    m_enableSummariesCheck->setChecked(false);
    m_summaryProviderCombo->setCurrentIndex(0);  // Current agent
    m_autoResumeCheck->setChecked(true);
    m_summariseOnResumeCheck->setChecked(false);
    m_acpLogEnableCheck->setChecked(false);
    m_acpLogDirEdit->setText(QDir::homePath() + QStringLiteral("/.kate-code"));
    m_acpLogSubdirEdit->setText(QStringLiteral("kate_code_sessions"));
    m_diffColorSchemeCombo->setCurrentIndex(0); // RedGreen (default)
    m_debugLoggingCheck->setChecked(false);
    m_remoteHelperCheck->setChecked(true);
    m_allowedCommandsEdit->clear();
    m_hasChanges = true;
    Q_EMIT changed();
}

void KateCodeConfigPage::reset()
{
    // Load current settings
    m_enableSummariesCheck->setChecked(m_settings->summariesEnabled());
    populateSummaryProviderCombo(m_settings->summaryProviderId());

    m_autoResumeCheck->setChecked(m_settings->autoResumeSessions());
    m_summariseOnResumeCheck->setChecked(m_settings->summariseOnResume());

    // Load ACP file-logging settings
    m_acpLogEnableCheck->setChecked(m_settings->acpLogEnabled());
    m_acpLogDirEdit->setText(m_settings->acpLogDirectory());
    m_acpLogSubdirEdit->setText(m_settings->acpLogSubdirectory());

    // Load diff color scheme
    int schemeIndex = m_diffColorSchemeCombo->findData(static_cast<int>(m_settings->diffColorScheme()));
    if (schemeIndex >= 0) {
        m_diffColorSchemeCombo->setCurrentIndex(schemeIndex);
    }

    // Load debug setting
    m_debugLoggingCheck->setChecked(m_settings->debugLogging());
    m_remoteHelperCheck->setChecked(m_settings->remoteHelperEnabled());

    // Load allowed command patterns (join with newlines for display)
    m_allowedCommandsEdit->setPlainText(m_settings->allowedCommandPatterns().join(QLatin1Char('\n')));

    // Load provider list
    populateProviderList();

    m_hasChanges = false;
}

void KateCodeConfigPage::onAddProvider()
{
    ACPProvider provider;
    provider.id = QStringLiteral("custom-%1").arg(QDateTime::currentMSecsSinceEpoch());
    provider.builtin = false;
    if (!editProviderDialog(provider, i18n("Add ACP Provider"))) {
        return;
    }

    m_settings->addCustomProvider(provider);
    populateProviderList();
    for (int row = 0; row < m_providerList->count(); ++row) {
        if (m_providerList->item(row)->data(Qt::UserRole).toString() == provider.id) {
            m_providerList->setCurrentRow(row);
            break;
        }
    }
}

void KateCodeConfigPage::onEditProvider()
{
    QListWidgetItem *item = m_providerList->currentItem();
    if (!item) {
        return;
    }

    ACPProvider provider = providerForItem(item);
    if (provider.id.isEmpty()) {
        return;
    }

    if (!editProviderDialog(provider, i18n("Edit ACP Provider"))) {
        return;
    }

    m_settings->updateCustomProvider(provider.id, provider);
    populateProviderList();
}

void KateCodeConfigPage::onRemoveProvider()
{
    QListWidgetItem *item = m_providerList->currentItem();
    if (!item) {
        return;
    }

    // Always keep at least one provider available.
    if (m_providerList->count() <= 1) {
        QMessageBox::warning(this, i18n("Cannot Remove Provider"),
            i18n("At least one provider must remain."));
        return;
    }

    QString providerId = item->data(Qt::UserRole).toString();
    QString providerName = item->text();

    int result = QMessageBox::question(this,
        i18n("Remove Provider"),
        i18n("Remove provider \"%1\"?", providerName),
        QMessageBox::Yes | QMessageBox::No);

    if (result != QMessageBox::Yes) {
        return;
    }

    m_settings->removeCustomProvider(providerId);
    populateProviderList();
}

void KateCodeConfigPage::onMoveProviderUp()
{
    const int row = m_providerList->currentRow();
    if (row <= 0) {
        return;
    }
    QListWidgetItem *item = m_providerList->takeItem(row);
    m_providerList->insertItem(row - 1, item);
    m_providerList->setCurrentRow(row - 1);
    saveProviderOrder();
}

void KateCodeConfigPage::onMoveProviderDown()
{
    const int row = m_providerList->currentRow();
    if (row < 0 || row >= m_providerList->count() - 1) {
        return;
    }
    QListWidgetItem *item = m_providerList->takeItem(row);
    m_providerList->insertItem(row + 1, item);
    m_providerList->setCurrentRow(row + 1);
    saveProviderOrder();
}

ACPProvider KateCodeConfigPage::providerForItem(const QListWidgetItem *item) const
{
    if (!item) {
        return {};
    }
    const QString id = item->data(Qt::UserRole).toString();
    for (const ACPProvider &provider : m_settings->providers()) {
        if (provider.id == id) {
            return provider;
        }
    }
    return {};
}

void KateCodeConfigPage::updateProviderButtons()
{
    const int row = m_providerList->currentRow();
    const bool hasSelection = row >= 0;
    m_editProviderButton->setEnabled(hasSelection);
    m_removeProviderButton->setEnabled(hasSelection && m_providerList->count() > 1);
    m_moveProviderUpButton->setEnabled(hasSelection && row > 0);
    m_moveProviderDownButton->setEnabled(hasSelection && row < m_providerList->count() - 1);
    m_installProviderButton->setEnabled(hasSelection && !m_installer && !m_providerList->currentItem()->data(Qt::UserRole + 1).toBool());
}

void KateCodeConfigPage::saveProviderOrder()
{
    QStringList ids;
    ids.reserve(m_providerList->count());
    for (int row = 0; row < m_providerList->count(); ++row) {
        ids.append(m_providerList->item(row)->data(Qt::UserRole).toString());
    }
    m_settings->setProviderOrder(ids);
}

// An argument for ACPProvider::options (split by QProcess::splitCommand()).
static QString commandArgument(QString arg)
{
    static const QRegularExpression special(QStringLiteral("[\\s\"]"));
    if (!arg.isEmpty() && !arg.contains(special)) {
        return arg;
    }
    // Inside quotes, a literal quote is three quotes.
    return QLatin1Char('"') + arg.replace(QLatin1Char('"'), QStringLiteral("\"\"\"")) + QLatin1Char('"');
}

static QString commandOptions(const QStringList &args)
{
    QStringList quoted;
    for (const QString &arg : args) {
        quoted.append(commandArgument(arg));
    }
    return quoted.join(QLatin1Char(' '));
}

static QStringList stringList(const QJsonValue &value)
{
    QStringList list;
    for (const QJsonValue &item : value.toArray()) {
        list.append(item.toString());
    }
    return list;
}

// The registry's build for this computer ("linux-x86_64", "darwin-aarch64";
// the program name is the same in every build of an agent, so a near miss
// such as musl does not matter here).
static QJsonObject localBinaryBuild(const QJsonObject &builds)
{
    QString arch = QSysInfo::currentCpuArchitecture();
    if (arch == QLatin1String("arm64")) {
        arch = QStringLiteral("aarch64");
    }
    const QString key = QSysInfo::kernelType() + QLatin1Char('-') + arch;
    if (builds.contains(key)) {
        return builds.value(key).toObject();
    }
    return builds.isEmpty() ? QJsonObject() : builds.begin().value().toObject();
}

bool KateCodeConfigPage::pickRegistryAgent(QJsonObject *agent)
{
    QDialog dialog(this);
    dialog.setWindowTitle(i18n("Add Agent from the ACP Registry"));
    dialog.resize(640, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(i18n("Search"));
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);
    auto *list = new QListWidget(&dialog);
    layout->addWidget(list, 1);
    auto *status = new QLabel(&dialog);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(i18n("Add"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    AgentIcons *icons = m_settings->agentIcons();
    auto fill = [this, list, status, icons]() {
        QSet<QString> added;
        for (const ACPProvider &provider : m_settings->providers()) {
            added.insert(provider.registryId.trimmed());
        }
        auto agents = icons->registryAgents();
        std::sort(agents.begin(), agents.end(), [](const auto &a, const auto &b) {
            return a.second.compare(b.second, Qt::CaseInsensitive) < 0;
        });
        list->clear();
        for (const auto &[id, name] : agents) {
            const QJsonObject entry = icons->registryEntry(id);
            ACPProvider forIcon;
            forIcon.registryId = id;
            const bool isAdded = added.contains(id);
            auto *item = new QListWidgetItem(icons->iconFor(forIcon),
                                             isAdded ? i18n("%1 %2 (added)", name, entry.value(QStringLiteral("version")).toString())
                                                     : i18n("%1 %2", name, entry.value(QStringLiteral("version")).toString()),
                                             list);
            item->setData(Qt::UserRole, id);
            QStringList tip{entry.value(QStringLiteral("description")).toString()};
            tip << i18n("Runs as: %1", entry.value(QStringLiteral("distribution")).toObject().keys().join(QStringLiteral(", ")));
            if (const QString license = entry.value(QStringLiteral("license")).toString(); !license.isEmpty()) {
                tip << i18n("License: %1", license);
            }
            if (const QString repository = entry.value(QStringLiteral("repository")).toString(); !repository.isEmpty()) {
                tip << i18n("Repository: %1", repository);
            }
            item->setToolTip(tip.join(QLatin1Char('\n')));
            if (isAdded) {
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            }
        }
        status->setText(agents.isEmpty() ? i18n("Loading the ACP registry…")
                                         : i18n("%1 agents. Agents already added are greyed out; hover over an agent for details.", agents.size()));
    };
    fill();
    // Without a cached registry the list fills in once it is fetched;
    // logos arrive one by one.
    connect(icons, &AgentIcons::iconsChanged, &dialog, [list, fill, icons]() {
        if (list->count() == 0) {
            fill();
            return;
        }
        for (int row = 0; row < list->count(); ++row) {
            ACPProvider forIcon;
            forIcon.registryId = list->item(row)->data(Qt::UserRole).toString();
            list->item(row)->setIcon(icons->iconFor(forIcon));
        }
    });
    connect(filter, &QLineEdit::textChanged, &dialog, [list](const QString &text) {
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem *item = list->item(row);
            item->setHidden(!text.isEmpty() && !item->text().contains(text, Qt::CaseInsensitive)
                            && !item->toolTip().contains(text, Qt::CaseInsensitive));
        }
    });
    auto updateOk = [list, buttons]() {
        QListWidgetItem *item = list->currentItem();
        buttons->button(QDialogButtonBox::Ok)->setEnabled(item && (item->flags() & Qt::ItemIsEnabled) && !item->isHidden());
    };
    connect(list, &QListWidget::currentItemChanged, &dialog, updateOk);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, [&dialog](QListWidgetItem *item) {
        if (item->flags() & Qt::ItemIsEnabled) {
            dialog.accept();
        }
    });
    updateOk();
    if (dialog.exec() != QDialog::Accepted || !list->currentItem()) {
        return false;
    }
    *agent = icons->registryEntry(list->currentItem()->data(Qt::UserRole).toString());
    return !agent->isEmpty();
}

void KateCodeConfigPage::onAddFromRegistry()
{
    QJsonObject agent;
    if (pickRegistryAgent(&agent)) {
        addRegistryAgent(agent);
    }
}

void KateCodeConfigPage::addRegistryAgent(const QJsonObject &agent)
{
    const QString name = agent.value(QStringLiteral("name")).toString(agent.value(QStringLiteral("id")).toString());
    const QJsonObject distribution = agent.value(QStringLiteral("distribution")).toObject();
    // A binary build: its program (the same name on every platform), which
    // the install puts into Kate Code's bin directory.
    if (const QJsonObject builds = distribution.value(QStringLiteral("binary")).toObject(); !builds.isEmpty()) {
        const QJsonObject build = localBinaryBuild(builds);
        QString cmd = build.value(QStringLiteral("cmd")).toString();
        cmd.replace(QLatin1Char('\\'), QLatin1Char('/'));
        QString program = QFileInfo(cmd).fileName();
        if (program.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) {
            program.chop(4);
        }
        if (!program.isEmpty()) {
            finishAddingRegistryAgent(agent, program, stringList(build.value(QStringLiteral("args"))));
            return;
        }
    }
    // npx: the package's program, from npm's metadata of that version (the
    // name npx would run).
    if (const QJsonObject npx = distribution.value(QStringLiteral("npx")).toObject(); !npx.isEmpty()) {
        const auto spec = KateRemote::parseNpmPackageSpec(npx.value(QStringLiteral("package")).toString());
        if (!spec) {
            QMessageBox::warning(this, i18n("Add Agent"), i18n("The ACP registry gives no exact npm package version for %1.", name));
            return;
        }
        if (!m_network) {
            m_network = new QNetworkAccessManager(this);
        }
        QNetworkRequest request(QUrl(QStringLiteral("https://registry.npmjs.org/%1/%2").arg(spec->first, spec->second)));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setTransferTimeout(30000);
        QNetworkReply *reply = m_network->get(request);
        auto *progress = new QProgressDialog(i18n("Looking up the npm package %1…", spec->first), i18n("Cancel"), 0, 0, this);
        progress->setWindowTitle(i18n("Add Agent"));
        progress->setAttribute(Qt::WA_DeleteOnClose);
        connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
        progress->open();
        const QStringList args = stringList(npx.value(QStringLiteral("args")));
        const QString package = spec->first;
        connect(reply, &QNetworkReply::finished, this, [this, reply, progress, agent, args, package, name]() {
            reply->deleteLater();
            const bool canceled = progress->wasCanceled();
            progress->close();
            if (canceled) {
                return;
            }
            const QJsonObject metadata = QJsonDocument::fromJson(reply->readAll()).object();
            // npm's rule: a single program, or the one named after the
            // package (without its scope).
            const QString unscoped = package.mid(package.lastIndexOf(QLatin1Char('/')) + 1);
            const QJsonValue bin = metadata.value(QStringLiteral("bin"));
            QString program;
            if (bin.isString()) {
                program = unscoped;
            } else if (const QJsonObject bins = bin.toObject(); bins.size() == 1) {
                program = bins.begin().key();
            } else if (bins.contains(unscoped)) {
                program = unscoped;
            }
            if (reply->error() != QNetworkReply::NoError || program.isEmpty()) {
                QMessageBox::warning(this,
                                     i18n("Add Agent"),
                                     reply->error() != QNetworkReply::NoError
                                         ? i18n("Cannot look up the npm package %1: %2", package, reply->errorString())
                                         : i18n("The npm package %1 has no program that npx would run.", package));
                return;
            }
            finishAddingRegistryAgent(agent, program, args);
        });
        return;
    }
    // uvx: run through uv (nothing to install here).
    if (const QJsonObject uvx = distribution.value(QStringLiteral("uvx")).toObject(); !uvx.isEmpty()) {
        finishAddingRegistryAgent(agent,
                                  QStringLiteral("uvx"),
                                  QStringList{uvx.value(QStringLiteral("package")).toString()} + stringList(uvx.value(QStringLiteral("args"))));
        return;
    }
    QMessageBox::warning(this, i18n("Add Agent"), i18n("The ACP registry does not say how to run %1.", name));
}

void KateCodeConfigPage::finishAddingRegistryAgent(const QJsonObject &agent, const QString &program, const QStringList &args)
{
    ACPProvider provider;
    provider.id = QStringLiteral("custom-%1").arg(QDateTime::currentMSecsSinceEpoch());
    provider.description = agent.value(QStringLiteral("name")).toString(agent.value(QStringLiteral("id")).toString());
    provider.executable = program;
    provider.options = commandOptions(args);
    provider.registryId = agent.value(QStringLiteral("id")).toString();
    // Added first: declining or a failed install leaves it there, with the
    // Install button.
    m_settings->addCustomProvider(provider);
    populateProviderList();
    for (int row = 0; row < m_providerList->count(); ++row) {
        if (m_providerList->item(row)->data(Qt::UserRole).toString() == provider.id) {
            m_providerList->setCurrentRow(row);
            break;
        }
    }
    if (!SettingsStore::isExecutableAvailable(provider.executable)) {
        installProvider(provider.id, true);
    }
}

void KateCodeConfigPage::onInstallProvider()
{
    if (QListWidgetItem *item = m_providerList->currentItem()) {
        installProvider(item->data(Qt::UserRole).toString(), false);
    }
}

void KateCodeConfigPage::installProvider(const QString &providerId, bool offer)
{
    if (m_installer) {
        return;
    }
    const ACPProvider provider = m_settings->providerById(providerId);
    const QString registryId = provider.registryId.trimmed();
    const QJsonObject agent = registryId.isEmpty() ? QJsonObject() : m_settings->agentIcons()->registryEntry(registryId);
    QString reason;
    if (registryId.isEmpty()) {
        reason = i18n("%1 has no ACP registry id: set one (Edit...) to install it from the registry.", provider.description);
    } else if (agent.isEmpty()) {
        reason = i18n("The ACP registry has no agent %1 (or it could not be fetched yet).", registryId);
    } else {
        AgentInstaller::canInstallLocally(agent, provider.executable, &reason);
    }
    if (!reason.isEmpty()) {
        if (offer) {
            QMessageBox::information(this, i18n("Agent Added"), i18n("%1 was added but is not installed on this computer.\n\n%2", provider.description, reason));
        } else {
            QMessageBox::warning(this, i18n("Install Agent"), reason);
        }
        return;
    }
    QString question = AgentInstaller::localInstallQuestion(agent, provider.executable);
    if (offer) {
        question = i18n("%1 was added. It is not installed on this computer.", provider.description) + QStringLiteral("\n\n") + question
            + QStringLiteral("\n\n") + i18n("If you do not install it now, Install... in the settings does it later.");
    }
    if (QMessageBox::question(this, i18n("Install Agent"), question, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes) {
        return;
    }

    m_installer = new AgentInstaller(agent, QString(), provider.executable, this);
    auto *progress = new QProgressDialog(i18n("Installing %1…", provider.description), i18n("Cancel"), 0, 0, this);
    m_installProgress = progress;
    progress->setWindowTitle(i18n("Install Agent"));
    progress->setMinimumWidth(480);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    connect(progress, &QProgressDialog::canceled, this, [this]() {
        if (m_installer) {
            m_installer->cancel();
            m_installer->deleteLater();
            m_installer = nullptr;
        }
        updateProviderButtons();
    });
    connect(m_installer, &AgentInstaller::progress, progress, &QProgressDialog::setLabelText);
    connect(m_installer, &AgentInstaller::finished, this, [this, installer = m_installer.data()](bool ok, const QString &message) {
        installer->deleteLater();
        m_installer = nullptr;
        if (m_installProgress) {
            // Not "canceled": the install is over.
            m_installProgress->disconnect(this);
            m_installProgress->close();
        }
        populateProviderList();
        updateProviderButtons();
        // Chats list the agents installed here.
        m_settings->notifyAgentsChanged();
        if (ok) {
            QMessageBox::information(this, i18n("Install Agent"), message);
        } else {
            QMessageBox::warning(this, i18n("Install Agent"), message);
        }
    });
    updateProviderButtons();
    progress->open();
    m_installer->start();
}

bool KateCodeConfigPage::editProviderDialog(ACPProvider &provider, const QString &title)
{
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.resize(760, 560);
    auto *dialogLayout = new QVBoxLayout(&dialog);
    auto *formLayout = new QFormLayout();
    dialogLayout->addLayout(formLayout);

    auto *descEdit = new QLineEdit(provider.description, &dialog);
    descEdit->setPlaceholderText(i18n("e.g. Codex GPT-5.6 Sol"));
    formLayout->addRow(i18n("Description:"), descEdit);

    auto *exeEdit = new QLineEdit(provider.executable, &dialog);
    exeEdit->setPlaceholderText(i18n("e.g. codex-acp"));
    formLayout->addRow(i18n("Executable:"), exeEdit);

    auto *optEdit = new QLineEdit(provider.options, &dialog);
    optEdit->setPlaceholderText(i18n("Arguments passed to the ACP executable (optional)"));
    formLayout->addRow(i18n("CLI options:"), optEdit);

    auto *mcpEdit = new QLineEdit(provider.mcpConfigPath, &dialog);
    mcpEdit->setPlaceholderText(i18n("e.g. ~/.cursor/mcp.json (optional)"));
    formLayout->addRow(i18n("MCP config JSON:"), mcpEdit);

    auto *resumeCheck = new QCheckBox(i18n("Try real ACP session/load, fall back to context"), &dialog);
    resumeCheck->setChecked(provider.trueResume);
    formLayout->addRow(i18n("True resume:"), resumeCheck);

    // The agent's entry in the ACP registry provides its logo.
    auto *registryCombo = new QComboBox(&dialog);
    registryCombo->setEditable(true);
    registryCombo->setInsertPolicy(QComboBox::NoInsert);
    // Full width like the line edits (form layouts in Breeze only stretch
    // expanding fields; a combo box would keep its short contents' width).
    registryCombo->setSizePolicy(QSizePolicy::Expanding, registryCombo->sizePolicy().verticalPolicy());
    auto fillRegistryCombo = [this, registryCombo]() {
        const QString text = registryCombo->currentText();
        registryCombo->clear();
        registryCombo->addItem(QString());
        for (const auto &agent : m_settings->agentIcons()->registryAgents()) {
            registryCombo->addItem(agent.first);
            registryCombo->setItemData(registryCombo->count() - 1, agent.second, Qt::ToolTipRole);
        }
        registryCombo->setCurrentText(text);
    };
    registryCombo->setCurrentText(provider.registryId);
    fillRegistryCombo();
    // Without a cached registry the list fills in once it is fetched.
    connect(m_settings->agentIcons(), &AgentIcons::iconsChanged, registryCombo, [registryCombo, fillRegistryCombo]() {
        if (registryCombo->count() <= 1) {
            fillRegistryCombo();
        }
    });
    registryCombo->lineEdit()->setPlaceholderText(i18n("e.g. claude-acp, codex-acp, gemini (optional)"));
    registryCombo->setToolTip(i18n("Id of the agent in the ACP registry (agentclientprotocol.com); its icon is used for this provider."));
    formLayout->addRow(i18n("ACP registry id:"), registryCombo);

    auto *iconEdit = new QLineEdit(provider.icon, &dialog);
    iconEdit->setPlaceholderText(i18n("Theme icon name or image file (optional, overrides the registry icon)"));
    formLayout->addRow(i18n("Icon:"), iconEdit);

    auto *configLabel = new QLabel(i18n("ACP session configuration"), &dialog);
    QFont labelFont = configLabel->font();
    labelFont.setBold(true);
    configLabel->setFont(labelFont);
    dialogLayout->addWidget(configLabel);

    auto *configHelp = new QLabel(i18n("These key/value pairs are applied after session creation through ACP session/set_config_option when the agent advertises a matching option. Values may be plain text or JSON. For @agentclientprotocol/codex-acp use model (for example gpt-5.6-sol), reasoning_effort (for example xhigh), and mode (read-only, agent, or agent-full-access). The mode controls both approval and sandboxing; Codex config.toml keys such as approval_policy and sandbox_mode are not ACP session option ids."), &dialog);
    configHelp->setWordWrap(true);
    dialogLayout->addWidget(configHelp);

    auto *configTable = new QTableWidget(0, 2, &dialog);
    configTable->setHorizontalHeaderLabels({i18n("Configuration key"), i18n("Value (text or JSON)")});
    configTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    configTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    configTable->verticalHeader()->setVisible(false);
    configTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    configTable->setSelectionMode(QAbstractItemView::SingleSelection);
    dialogLayout->addWidget(configTable, 1);

    auto addConfigRow = [configTable](const QString &key = QString(), const QString &value = QString()) {
        const int row = configTable->rowCount();
        configTable->insertRow(row);
        configTable->setItem(row, 0, new QTableWidgetItem(key));
        configTable->setItem(row, 1, new QTableWidgetItem(value));
    };
    for (auto it = provider.sessionConfig.constBegin(); it != provider.sessionConfig.constEnd(); ++it) {
        addConfigRow(it.key(), sessionConfigValueText(it.value()));
    }

    auto *configButtons = new QHBoxLayout();
    auto *addConfigButton = new QPushButton(i18n("Add Parameter"), &dialog);
    auto *removeConfigButton = new QPushButton(i18n("Remove Parameter"), &dialog);
    removeConfigButton->setEnabled(false);
    configButtons->addWidget(addConfigButton);
    configButtons->addWidget(removeConfigButton);
    configButtons->addStretch();
    dialogLayout->addLayout(configButtons);

    connect(addConfigButton, &QPushButton::clicked, &dialog, [configTable, addConfigRow]() {
        addConfigRow();
        configTable->setCurrentCell(configTable->rowCount() - 1, 0);
        configTable->editItem(configTable->currentItem());
    });
    connect(removeConfigButton, &QPushButton::clicked, &dialog, [configTable]() {
        if (configTable->currentRow() >= 0) {
            configTable->removeRow(configTable->currentRow());
        }
    });
    connect(configTable, &QTableWidget::currentCellChanged, &dialog,
            [removeConfigButton](int row) { removeConfigButton->setEnabled(row >= 0); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialogLayout->addWidget(buttons);

    while (dialog.exec() == QDialog::Accepted) {
        const QString description = descEdit->text().trimmed();
        const QString executable = exeEdit->text().trimmed();
        if (description.isEmpty() || executable.isEmpty()) {
            QMessageBox::warning(&dialog, i18n("Invalid Provider"), i18n("Description and Executable are required."));
            continue;
        }
        const QString registryId = registryCombo->currentText().trimmed();
        if (!registryId.isEmpty() && !AgentIcons::isValidRegistryId(registryId)) {
            QMessageBox::warning(&dialog, i18n("Invalid Provider"),
                                 i18n("An ACP registry id contains only lowercase letters, digits and hyphens."));
            continue;
        }

        QJsonObject sessionConfig;
        bool valid = true;
        for (int row = 0; row < configTable->rowCount(); ++row) {
            const QString key = configTable->item(row, 0) ? configTable->item(row, 0)->text().trimmed() : QString();
            const QString value = configTable->item(row, 1) ? configTable->item(row, 1)->text() : QString();
            if (key.isEmpty() && value.trimmed().isEmpty()) {
                continue;
            }
            if (key.isEmpty()) {
                QMessageBox::warning(&dialog, i18n("Invalid Session Configuration"),
                                     i18n("Every ACP session configuration value requires a key."));
                valid = false;
                break;
            }
            if (sessionConfig.contains(key)) {
                QMessageBox::warning(&dialog, i18n("Invalid Session Configuration"),
                                     i18n("The ACP session configuration key \"%1\" is duplicated.", key));
                valid = false;
                break;
            }
            sessionConfig.insert(key, parseSessionConfigValue(value));
        }
        if (!valid) {
            continue;
        }

        provider.description = description;
        provider.executable = executable;
        provider.options = optEdit->text().trimmed();
        provider.mcpConfigPath = mcpEdit->text().trimmed();
        provider.trueResume = resumeCheck->isChecked();
        provider.sessionConfig = sessionConfig;
        provider.icon = iconEdit->text().trimmed();
        provider.registryId = registryCombo->currentText().trimmed();
        return true;
    }
    return false;
}

void KateCodeConfigPage::onSettingChanged()
{
    m_hasChanges = true;
    Q_EMIT changed();
}
