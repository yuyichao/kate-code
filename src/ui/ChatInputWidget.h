#pragma once

#include "../acp/ACPModels.h"
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QWidget>
#include <QTextEdit>

class QJsonArray;
class QLabel;
class QPushButton;
class QComboBox;
class QCompleter;
class QMenu;
class QToolButton;
class FlowLayout;

// Custom QTextEdit that handles QCompleter for slash commands and file references
class QAbstractItemModel;

class CommandTextEdit : public QTextEdit
{
    Q_OBJECT
public:
    explicit CommandTextEdit(QWidget *parent = nullptr);
    void setCompleter(QCompleter *completer);
    void setModels(QAbstractItemModel *commandModel, QAbstractItemModel *fileModel);
    QCompleter *completer() const { return m_completer; }

Q_SIGNALS:
    void imagePasteDetected(const QMimeData *mimeData);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;

private Q_SLOTS:
    void insertCompletion(const QString &completion);

private:
    enum CompletionType {
        None,
        Command,  // '/' prefix
        File      // '@' prefix
    };

    struct CompletionContext {
        CompletionType type;
        QString prefix;        // Full text including prefix char
        QString filterText;    // Text after prefix char for filtering
        int prefixStart;       // Position where prefix starts
    };

    CompletionContext completionUnderCursor() const;
    QCompleter *m_completer = nullptr;
    QAbstractItemModel *m_commandModel = nullptr;
    QAbstractItemModel *m_fileModel = nullptr;
};

class ChatInputWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ChatInputWidget(QWidget *parent = nullptr);
    ~ChatInputWidget() override;

    void setEnabled(bool enabled);
    void clear();
    QString text() const;
    QString permissionMode() const;

    void setAvailableModes(const QJsonArray &modes);
    void setCurrentMode(const QString &modeId);
    void setAvailableCommands(const QList<SlashCommand> &commands);
    void setAvailableFiles(const QStringList &files);
    void setPromptRunning(bool running);
    // Context window usage in the status row: the percentage, with the token
    // counts in the tooltip. used < 0 hides it.
    void setContextUsage(qint64 used, qint64 size);

    // Session config options from the agent (model, effort, fast mode, ...).
    // "mode" is shown by the mode dropdown and skipped here.
    void setConfigOptions(const QJsonArray &options);

Q_SIGNALS:
    void messageSubmitted(const QString &message);
    void imageAttached(const ImageAttachment &image);
    void permissionModeChanged(const QString &mode);
    void stopClicked();
    void configOptionChanged(const QString &configId, const QJsonValue &value);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private Q_SLOTS:
    void onSendClicked();
    void onStopClicked();
    void onModeChanged(int index);
    void onImagePasteDetected(const QMimeData *mimeData);
    void onAttachFileClicked();

private:
    void updateWaitingIndicator();
    QToolButton *optionButton(const QString &configId);
    void updateOptionButton(const QString &configId);
    void populateOptionMenu(const QString &configId, QMenu *menu);
    void updateOptionsRowHeight();

    CommandTextEdit *m_textEdit;
    QPushButton *m_sendButton;
    QPushButton *m_stopButton;
    QPushButton *m_attachFileButton;
    QComboBox *m_modeComboBox;
    QLabel *m_waitingLabel;
    QLabel *m_contextLabel;
    bool m_promptRunning = false;
    bool m_inputEnabled = false;
    QCompleter *m_completer;
    QList<SlashCommand> m_availableCommands;
    QStringList m_availableFiles;

    // Models for completer (switched based on context)
    QAbstractItemModel *m_commandModel = nullptr;
    QAbstractItemModel *m_fileModel = nullptr;

    // Session option controls. Buttons are kept per option id and only
    // hidden when the agent stops advertising an option, so a button whose
    // menu is open is never deleted.
    QWidget *m_optionsRow = nullptr;
    FlowLayout *m_optionsLayout = nullptr;
    QHash<QString, QToolButton *> m_optionButtons;
    QHash<QString, QJsonObject> m_optionData;
    QStringList m_optionOrder;
};
