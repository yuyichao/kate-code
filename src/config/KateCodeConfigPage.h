#pragma once

#include <KTextEditor/ConfigPage>

#include <QJsonObject>
#include <QPointer>

class AgentInstaller;
class QNetworkAccessManager;
class QProgressDialog;
class SettingsStore;
struct ACPProvider;
class QLineEdit;
class QCheckBox;
class QComboBox;
class QPushButton;
class QLabel;
class QPlainTextEdit;
class QListWidget;
class QListWidgetItem;
class QTabWidget;
class QTableWidget;

class KateCodeConfigPage : public KTextEditor::ConfigPage
{
    Q_OBJECT

public:
    explicit KateCodeConfigPage(SettingsStore *settings, QWidget *parent = nullptr);
    ~KateCodeConfigPage() override;

    // KTextEditor::ConfigPage interface
    QString name() const override;
    QString fullName() const override;
    QIcon icon() const override;

public Q_SLOTS:
    void apply() override;
    void defaults() override;
    void reset() override;

private Q_SLOTS:
    void onSettingChanged();

    void onAddProvider();
    void onEditProvider();
    void onRemoveProvider();
    void onMoveProviderUp();
    void onMoveProviderDown();
    void onAddFromRegistry();
    void onInstallProvider();

private:
    void setupUi();
    void setupGeneralTab(QWidget *tab);
    void setupAdvancedTab(QWidget *tab);
    void populateProviderList();
    void populateSummaryProviderCombo(const QString &selectedId);
    bool editProviderDialog(ACPProvider &provider, const QString &title);
    ACPProvider providerForItem(const QListWidgetItem *item) const;
    void updateProviderButtons();
    void saveProviderOrder();
    // The ACP registry's agents to choose from; false when cancelled.
    bool pickRegistryAgent(QJsonObject *agent);
    // Adds agent as a provider (its npm program looked up first when it
    // runs with npx), then offers to install it when it is not installed.
    void addRegistryAgent(const QJsonObject &agent);
    void finishAddingRegistryAgent(const QJsonObject &agent, const QString &program, const QStringList &args);
    // Installs the provider's agent on this computer after asking (offer:
    // asked right after adding it; otherwise from the Install button).
    void installProvider(const QString &providerId, bool offer);

    SettingsStore *m_settings;

    // Tab widget
    QTabWidget *m_tabWidget;

    // General tab - ACP Providers section
    QListWidget *m_providerList;
    QPushButton *m_addProviderButton;
    QPushButton *m_editProviderButton;
    QPushButton *m_removeProviderButton;
    QPushButton *m_moveProviderUpButton;
    QPushButton *m_moveProviderDownButton;
    QPushButton *m_addFromRegistryButton;
    QPushButton *m_installProviderButton;
    QPointer<AgentInstaller> m_installer;
    QPointer<QProgressDialog> m_installProgress;
    QNetworkAccessManager *m_network = nullptr;

    // General tab - Diff colors section
    QComboBox *m_diffColorSchemeCombo;

    // Advanced tab - Summary options
    QCheckBox *m_enableSummariesCheck;
    QComboBox *m_summaryProviderCombo;

    // Advanced tab - Session resume
    QCheckBox *m_autoResumeCheck;
    QCheckBox *m_summariseOnResumeCheck;

    // Advanced tab - ACP file logging
    QCheckBox *m_acpLogEnableCheck;
    QLineEdit *m_acpLogDirEdit;
    QLineEdit *m_acpLogSubdirEdit;

    // General tab - Remote hosts section
    QCheckBox *m_remoteHelperCheck;

    // General tab - Debug section
    QCheckBox *m_debugLoggingCheck;

    // Advanced tab - Command auto-approval section
    QPlainTextEdit *m_allowedCommandsEdit;

    bool m_hasChanges;
};
