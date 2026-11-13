/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2025 Kate Code contributors
*/

#include "EditorDBusService.h"
#include "EditorSession.h"

#include <kde_terminal_interface.h>
#include <KTextEditor/Application>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/MainWindow>
#include <KTextEditor/View>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringDecoder>
#include <QStringEncoder>
#include <QTimer>
#include <QUrl>
#include <QWidget>

namespace {
// The plugin's instance, for documentChangedOnDisk().
EditorDBusService *s_instance = nullptr;
}

EditorDBusService::~EditorDBusService()
{
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

EditorDBusService::EditorDBusService(QObject *parent)
    : QObject(parent)
{
    s_instance = this;
    // Follow which documents Kate has flagged as changed on disk (see
    // changeDocument()).
    if (KTextEditor::Application *app = KTextEditor::Editor::instance()->application()) {
        const QList<KTextEditor::Document *> docs = app->documents();
        for (KTextEditor::Document *doc : docs) {
            trackDocument(doc);
        }
        connect(app, &KTextEditor::Application::documentCreated, this, &EditorDBusService::trackDocument);
    }
}

bool EditorDBusService::registerOnBus()
{
    QDBusConnection bus = QDBusConnection::sessionBus();

    // Primary name is unique per process so multiple Kate instances can coexist.
    // The "p" prefix on the pid is required: a D-Bus name element must not
    // begin with a digit, so a bare pid would be an invalid bus name.
    // ACPSession::buildMcpServers() must build the SAME name for the child.
    const QString primaryName = QStringLiteral("org.kde.katecode.editor.p")
                                + QString::number(QCoreApplication::applicationPid());
    if (!bus.registerService(primaryName)) {
        qWarning() << "[KateCode] Failed to register DBus service:" << bus.lastError().message();
        return false;
    }

    // Best-effort: also claim the bare legacy name so the first process to start
    // is reachable by older tooling.  A second process silently skips this.
    if (!bus.registerService(QStringLiteral("org.kde.katecode.editor"))) {
        qDebug() << "[KateCode] Legacy DBus service name already claimed by another process (harmless)";
    }

    if (!bus.registerObject(QStringLiteral("/KateCode/Editor"), this, QDBusConnection::ExportAllSlots)) {
        qWarning() << "[KateCode] Failed to register DBus object:" << bus.lastError().message();
        return false;
    }

    qDebug() << "[KateCode] DBus service registered:" << primaryName;
    return true;
}

QStringList EditorDBusService::listDocuments()
{
    QStringList result;

    KTextEditor::Application *app = KTextEditor::Editor::instance()->application();
    if (!app) {
        return result;
    }

    const QList<KTextEditor::Document *> docs = app->documents();
    for (KTextEditor::Document *doc : docs) {
        const QString path = doc->url().toLocalFile();
        if (!path.isEmpty()) {
            result.append(path);
        } else {
            // Untitled documents — use document name
            result.append(QStringLiteral("untitled:%1").arg(doc->documentName()));
        }
    }

    return result;
}

namespace {

// The checksum KTextEditor keeps for a document (Document::checksum()): the
// git blob hash of the file as last loaded or saved.
QByteArray gitBlobDigest(const QByteArray &data)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    const QByteArray header = QByteArray("blob ") + QByteArray::number(data.size());
    hash.addData(header);
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(data);
    return hash.result();
}

bool readFileBytes(const QString &path, QByteArray *bytes, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("ERROR: Cannot open file: %1").arg(file.errorString());
        return false;
    }
    *bytes = file.readAll();
    return true;
}

// Text of a file in the given encoding (UTF-8 if empty); a byte order mark
// is dropped and reported in *bom so it can be written back.
bool decodeText(const QByteArray &bytes, const QString &encoding, QString *text, bool *bom)
{
    QStringDecoder decoder(encoding.isEmpty() ? "UTF-8" : encoding.toUtf8().constData());
    if (!decoder.isValid()) {
        return false;
    }
    *bom = QStringConverter::encodingForData(bytes).has_value();
    *text = decoder.decode(bytes);
    return !decoder.hasError();
}

bool encodeText(const QString &text, const QString &encoding, bool bom, QByteArray *bytes)
{
    QStringEncoder encoder(encoding.isEmpty() ? "UTF-8" : encoding.toUtf8().constData(),
                           bom ? QStringConverter::Flag::WriteBom : QStringConverter::Flag::Default);
    if (!encoder.isValid()) {
        return false;
    }
    *bytes = encoder.encode(text);
    return !encoder.hasError();
}

// Atomically replace (or create) the file, creating missing directories.
QString writeFileBytes(const QString &path, const QByteArray &bytes)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        return QStringLiteral("ERROR: Cannot create directory: %1").arg(info.absolutePath());
    }
    QSaveFile file(path);
    // A writable file in a directory we cannot create files in.
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        return QStringLiteral("ERROR: Cannot write file: %1").arg(file.errorString());
    }
    return QString();
}

// Replace the unique occurrence of oldText in text. Files with Windows line
// endings are matched with the agent's "\n" line endings.
QString replaceUnique(QString *text, QString oldText, QString newText)
{
    if (oldText.isEmpty()) {
        return QStringLiteral("ERROR: old_text must not be empty");
    }
    if (text->contains(QLatin1String("\r\n")) && !oldText.contains(QLatin1Char('\r'))) {
        oldText.replace(QLatin1Char('\n'), QLatin1String("\r\n"));
        newText.replace(QLatin1Char('\n'), QLatin1String("\r\n"));
    }
    const qsizetype pos = text->indexOf(oldText);
    if (pos < 0) {
        return QStringLiteral("ERROR: old_text not found in document");
    }
    if (text->indexOf(oldText, pos + 1) >= 0) {
        return QStringLiteral("ERROR: old_text is not unique in document (found at multiple positions)");
    }
    text->replace(pos, oldText.size(), newText);
    return QString();
}

KTextEditor::Cursor cursorAt(const QString &text, qsizetype offset)
{
    const int line = int(text.left(offset).count(QLatin1Char('\n')));
    // (lastIndexOf() with -1 would search from the end.)
    const qsizetype lineStart = offset > 0 ? text.lastIndexOf(QLatin1Char('\n'), offset - 1) + 1 : 0;
    return KTextEditor::Cursor(line, int(offset - lineStart));
}

} // namespace

KTextEditor::Document *EditorDBusService::findDocument(const QString &filePath) const
{
    KTextEditor::Application *app = KTextEditor::Editor::instance()->application();
    if (!app) {
        return nullptr;
    }
    if (KTextEditor::Document *doc = app->findUrl(QUrl::fromLocalFile(filePath))) {
        return doc;
    }
    const QString canonical = QFileInfo(filePath).canonicalFilePath();
    if (!canonical.isEmpty() && canonical != filePath) {
        return app->findUrl(QUrl::fromLocalFile(canonical));
    }
    return nullptr;
}

void EditorDBusService::trackDocument(KTextEditor::Document *doc)
{
    if (!doc || m_modifiedOnDisk.contains(doc)) {
        return;
    }
    m_modifiedOnDisk.insert(doc, false);
    connect(doc, &KTextEditor::Document::modifiedOnDisk, this,
            [this](KTextEditor::Document *document, bool isModified) {
                m_modifiedOnDisk.insert(document, isModified);
            });
    connect(doc, &QObject::destroyed, this, [this, doc]() {
        m_modifiedOnDisk.remove(doc);
    });
}

bool EditorDBusService::flaggedModifiedOnDisk(KTextEditor::Document *doc) const
{
    return m_modifiedOnDisk.value(doc, false);
}

bool EditorDBusService::documentChangedOnDisk(KTextEditor::Document *doc, const QString &filePath)
{
    if (s_instance && s_instance->flaggedModifiedOnDisk(doc)) {
        return true;
    }
    // Before Kate notices, its checksum is still the one of the old file.
    QByteArray bytes;
    QString error;
    return !QFileInfo::exists(filePath) || !readFileBytes(filePath, &bytes, &error)
        || gitBlobDigest(bytes) != doc->checksum();
}

QString EditorDBusService::readDocument(const QString &filePath)
{
    KTextEditor::Document *doc = findDocument(filePath);
    // The buffer is what the user sees (with unsaved changes), unless the
    // file changed on disk behind an unmodified buffer that Kate has not
    // reloaded (or not even noticed) yet.
    if (doc && (doc->isModified() || !documentChangedOnDisk(doc, filePath))) {
        return doc->text();
    }

    QFile file(filePath);
    if (!file.exists()) {
        return QStringLiteral("ERROR: File not found: %1").arg(filePath);
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QStringLiteral("ERROR: Cannot open file: %1").arg(file.errorString());
    }
    return QString::fromUtf8(file.readAll());
}

QString EditorDBusService::editDocument(const QString &filePath, const QString &oldText, const QString &newText)
{
    return changeDocument(filePath, oldText, newText, false);
}

QString EditorDBusService::writeDocument(const QString &filePath, const QString &content)
{
    return changeDocument(filePath, QString(), content, true);
}

QString EditorDBusService::changeDocument(const QString &filePath, const QString &oldText, const QString &newText, bool whole)
{
    if (!QFileInfo(filePath).isAbsolute()) {
        return QStringLiteral("ERROR: file_path must be absolute: %1").arg(filePath);
    }
    const bool exists = QFileInfo::exists(filePath);
    if (!whole && !exists) {
        return QStringLiteral("ERROR: File not found: %1").arg(filePath);
    }
    KTextEditor::Document *doc = findDocument(filePath);

    // Read the file on disk; for an open document also decide whether it
    // still has the content the buffer was loaded from or last saved as.
    QByteArray bytes;
    QString error;
    if (exists && !readFileBytes(filePath, &bytes, &error)) {
        return error;
    }
    const bool changedOnDisk = doc && (!exists || flaggedModifiedOnDisk(doc) || gitBlobDigest(bytes) != doc->checksum());

    if (doc && !changedOnDisk) {
        // Edit the buffer (the change can be undone there) and save it. The
        // checks avoid every dialog KTextEditor would show while saving.
        if (!QFileInfo(filePath).isWritable()) {
            return QStringLiteral("ERROR: File is not writable: %1").arg(filePath);
        }
        QByteArray encoded;
        if (!encodeText(newText, doc->encoding(), false, &encoded)) {
            return QStringLiteral("ERROR: The new text cannot be saved in the file's encoding (%1)").arg(doc->encoding());
        }
        if (whole) {
            if (!doc->setText(newText)) {
                return QStringLiteral("ERROR: Failed to replace text");
            }
        } else {
            const QString content = doc->text();
            const qsizetype pos = content.indexOf(oldText);
            if (oldText.isEmpty() || pos < 0) {
                return QStringLiteral("ERROR: old_text not found in document");
            }
            if (content.indexOf(oldText, pos + 1) >= 0) {
                return QStringLiteral("ERROR: old_text is not unique in document (found at multiple positions)");
            }
            const KTextEditor::Range range(cursorAt(content, pos), cursorAt(content, pos + oldText.size()));
            if (!doc->replaceText(range, newText)) {
                return QStringLiteral("ERROR: Failed to replace text");
            }
        }
        if (!doc->save()) {
            return QStringLiteral("ERROR: The change was made in Kate's buffer but saving the file failed");
        }
        return QStringLiteral("OK");
    }

    if (doc && doc->isModified()) {
        return QStringLiteral(
                   "ERROR: %1 has unsaved changes in Kate and was also changed on disk. "
                   "Ask the user to save or reload it in Kate first.")
            .arg(filePath);
    }

    // Change the file on disk: it is not open, or its buffer is unmodified
    // and out of date (it is reloaded afterwards).
    const QString encoding = doc ? doc->encoding() : QString();
    QString text;
    bool bom = false;
    if (exists && !decodeText(bytes, encoding, &text, &bom)) {
        if (!whole) {
            return QStringLiteral("ERROR: %1 is not valid %2 text").arg(filePath, encoding.isEmpty() ? QStringLiteral("UTF-8") : encoding);
        }
        bom = false;
    }
    if (whole) {
        text = newText;
    } else {
        error = replaceUnique(&text, oldText, newText);
        if (!error.isEmpty()) {
            return error;
        }
    }
    QByteArray encoded;
    if (!encodeText(text, encoding, bom, &encoded)) {
        return QStringLiteral("ERROR: The new text cannot be saved in the file's encoding (%1)").arg(encoding);
    }
    error = writeFileBytes(filePath, encoded);
    if (!error.isEmpty()) {
        return error;
    }
    if (doc) {
        doc->documentReload();
    }
    return QStringLiteral("OK");
}

QString EditorDBusService::askUserQuestion(const QString &questionsJson)
{
    Q_UNUSED(questionsJson);
    // Reached only by a kate-mcp-server that was not given its chat's
    // endpoint (KATECODE_DBUS_PATH), e.g. one left over from an older build.
    return QStringLiteral("ERROR: This Kate MCP server is not attached to a Kate Code chat, so it cannot "
                          "ask the user. Reconnect the chat session in Kate.");
}

EditorSession *EditorDBusService::createSession(QObject *parent)
{
    const QString path = QStringLiteral("/KateCode/Session/s%1").arg(m_nextSessionId++);
    auto *session = new EditorSession(this, path, parent);
    const bool ok = QDBusConnection::sessionBus().registerObject(path, session, QDBusConnection::ExportAllSlots);
    if (!ok) {
        qWarning() << "[KateCode] Failed to register DBus object" << path << ":"
                   << QDBusConnection::sessionBus().lastError().message();
    }
    session->setRegistered(ok);
    return session;
}

QString EditorDBusService::pasteToTerminal(const QString &text)
{
    return pasteToTerminalIn(text, nullptr);
}

QString EditorDBusService::pasteToTerminalIn(const QString &text, KTextEditor::MainWindow *window)
{
    // sendInput() delivers raw keystrokes: a line break would run the command
    // and other control characters (Ctrl-C/D, escape sequences) act on the
    // shell. Only accept printable single-line text so nothing executes
    // without the user pressing Enter.
    for (const QChar ch : text) {
        const char16_t u = ch.unicode();
        if (u < 0x20 || u == 0x7f || (u >= 0x80 && u < 0xa0) || u == 0x2028 || u == 0x2029) {
            return QStringLiteral("ERROR: text must be a single line without control characters "
                                  "(line breaks would execute it)");
        }
    }

    KTextEditor::MainWindow *mainWindow = window;
    if (!mainWindow) {
        KTextEditor::Application *app = KTextEditor::Editor::instance()->application();
        if (!app)
            return QStringLiteral("ERROR: KTextEditor application not available");
        mainWindow = app->activeMainWindow();
    }
    if (!mainWindow)
        return QStringLiteral("ERROR: No active main window");

    // Kate's terminal plugin registers itself as "katekonsoleplugin"
    QObject *terminalView = mainWindow->pluginView(QStringLiteral("katekonsoleplugin"));
    if (!terminalView)
        return QStringLiteral("ERROR: Kate terminal plugin not available or not enabled");

    // The KonsolePart may not be a direct Qt-child of the plugin view — it is often
    // parented to a container widget deeper in the hierarchy.  Search the entire main
    // window widget tree so we find it wherever it lives.
    TerminalInterface *terminal = nullptr;
    QWidget *win = mainWindow->window();
    if (win) {
        const auto allObjs = win->findChildren<QObject *>(QString(), Qt::FindChildrenRecursively);
        for (QObject *obj : allObjs) {
            terminal = qobject_cast<TerminalInterface *>(obj);
            if (terminal)
                break;
        }
    }

    if (!terminal)
        return QStringLiteral("ERROR: Could not find TerminalInterface in terminal plugin");

    terminal->sendInput(text);
    return QStringLiteral("OK");
}
