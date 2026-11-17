/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#include "../config/SettingsStore.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

// SettingsStore's providers in a scratch XDG_CONFIG_HOME: the default list
// and how existing configurations get new defaults.
class SettingsStoreTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_config;

    static QStringList ids(const QList<ACPProvider> &providers)
    {
        QStringList list;
        for (const ACPProvider &p : providers) {
            list.append(p.id);
        }
        return list;
    }

    // A configuration as an older Kate Code left it: the given providers
    // (id, executable), seeded, without the newer keys.
    static void writeOld(const QList<std::pair<QString, QString>> &providers, bool trueResume = false)
    {
        QSettings settings(QStringLiteral("kate-code"), QStringLiteral("kate-code"));
        settings.clear();
        settings.beginWriteArray(QStringLiteral("ACP/customProviders"), providers.size());
        for (int i = 0; i < providers.size(); ++i) {
            settings.setArrayIndex(i);
            settings.setValue(QStringLiteral("id"), providers.at(i).first);
            settings.setValue(QStringLiteral("description"), providers.at(i).first);
            settings.setValue(QStringLiteral("executable"), providers.at(i).second);
            settings.setValue(QStringLiteral("trueResume"), trueResume);
        }
        settings.endArray();
        settings.setValue(QStringLiteral("ACP/seeded"), true);
        settings.sync();
    }

    static QStringList offered()
    {
        QSettings settings(QStringLiteral("kate-code"), QStringLiteral("kate-code"));
        return settings.value(QStringLiteral("ACP/offeredDefaults")).toStringList();
    }

    const QStringList m_defaults{QStringLiteral("claude-code"),
                                 QStringLiteral("vibe-mistral"),
                                 QStringLiteral("codex"),
                                 QStringLiteral("github-copilot"),
                                 QStringLiteral("gemini-cli"),
                                 QStringLiteral("opencode")};

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_config.isValid());
        // Before any QSettings: Qt reads it once.
        qputenv("XDG_CONFIG_HOME", m_config.path().toLocal8Bit());
        qputenv("XDG_CACHE_HOME", (m_config.path() + QStringLiteral("/cache")).toLocal8Bit());
        QCOMPARE(QSettings(QStringLiteral("kate-code"), QStringLiteral("kate-code")).fileName(),
                 m_config.path() + QStringLiteral("/kate-code/kate-code.conf"));
    }

    void init()
    {
        QSettings settings(QStringLiteral("kate-code"), QStringLiteral("kate-code"));
        settings.clear();
        settings.sync();
    }

    void freshConfiguration()
    {
        SettingsStore store;
        const QList<ACPProvider> providers = store.providers();
        QCOMPARE(ids(providers), m_defaults);
        QCOMPARE(offered(), m_defaults);
        for (const ACPProvider &p : providers) {
            QVERIFY2(p.trueResume, qPrintable(p.id));
            QVERIFY2(!p.registryId.isEmpty(), qPrintable(p.id));
        }
        const ACPProvider copilot = store.providerById(QStringLiteral("github-copilot"));
        QCOMPARE(copilot.executable, QStringLiteral("copilot"));
        QCOMPARE(copilot.options, QStringLiteral("--acp"));
        QCOMPARE(copilot.registryId, QStringLiteral("github-copilot-cli"));
        QCOMPARE(store.providerById(QStringLiteral("opencode")).options, QStringLiteral("acp"));
        QCOMPARE(store.providerById(QStringLiteral("gemini-cli")).executable, QStringLiteral("gemini"));
        QCOMPARE(store.providerById(QStringLiteral("codex")).executable, QStringLiteral("codex-acp"));
    }

    void olderConfigurationGetsNewDefaults()
    {
        writeOld({{QStringLiteral("claude-code"), QStringLiteral("claude-agent-acp")},
                  {QStringLiteral("vibe-mistral"), QStringLiteral("vibe-acp")},
                  {QStringLiteral("custom-1"), QStringLiteral("my-agent")}});
        SettingsStore store;
        // After the user's own entries.
        QCOMPARE(ids(store.providers()),
                 QStringList({QStringLiteral("claude-code"),
                              QStringLiteral("vibe-mistral"),
                              QStringLiteral("custom-1"),
                              QStringLiteral("codex"),
                              QStringLiteral("github-copilot"),
                              QStringLiteral("gemini-cli"),
                              QStringLiteral("opencode")}));
        QCOMPARE(offered(), m_defaults);
    }

    void removedOldDefaultsStayRemoved()
    {
        writeOld({{QStringLiteral("claude-code"), QStringLiteral("claude-agent-acp")}});
        SettingsStore store;
        QCOMPARE(ids(store.providers()),
                 QStringList({QStringLiteral("claude-code"),
                              QStringLiteral("codex"),
                              QStringLiteral("github-copilot"),
                              QStringLiteral("gemini-cli"),
                              QStringLiteral("opencode")}));
    }

    void agentsAddedByHandCountAsDefaults()
    {
        writeOld({{QStringLiteral("claude-code"), QStringLiteral("claude-agent-acp")},
                  {QStringLiteral("custom-1"), QStringLiteral("codex-acp")}});
        {
            SettingsStore store;
            QCOMPARE(ids(store.providers()),
                     QStringList({QStringLiteral("claude-code"),
                                  QStringLiteral("custom-1"),
                                  QStringLiteral("github-copilot"),
                                  QStringLiteral("gemini-cli"),
                                  QStringLiteral("opencode")}));
            // The hand-made one removed later: the default does not appear.
            store.removeCustomProvider(QStringLiteral("custom-1"));
        }
        SettingsStore store;
        QVERIFY(!ids(store.providers()).contains(QStringLiteral("codex")));
        QCOMPARE(offered(), m_defaults);
    }

    void removedDefaultsStayRemoved()
    {
        {
            SettingsStore store;
            store.removeCustomProvider(QStringLiteral("gemini-cli"));
            store.removeCustomProvider(QStringLiteral("claude-code"));
        }
        SettingsStore store;
        QCOMPARE(ids(store.providers()),
                 QStringList({QStringLiteral("vibe-mistral"),
                              QStringLiteral("codex"),
                              QStringLiteral("github-copilot"),
                              QStringLiteral("opencode")}));
    }

    void trueResumeTurnedOnOnce()
    {
        writeOld({{QStringLiteral("claude-code"), QStringLiteral("claude-agent-acp")}}, false);
        {
            SettingsStore store;
            for (const ACPProvider &p : store.providers()) {
                QVERIFY2(p.trueResume, qPrintable(p.id));
            }
            ACPProvider claude = store.providerById(QStringLiteral("claude-code"));
            claude.trueResume = false;
            store.updateCustomProvider(claude.id, claude);
        }
        SettingsStore store;
        QVERIFY(!store.providerById(QStringLiteral("claude-code")).trueResume);
    }
};

QTEST_MAIN(SettingsStoreTest)
#include "SettingsStoreTest.moc"
