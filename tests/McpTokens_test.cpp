#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <mcp/McpTokens.h>

using namespace Mcp;

class McpTokensTest : public QObject {
    Q_OBJECT

   private slots:
    void createVerifyRevoke()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TokenStore store(dir.filePath("mcp/tokens.json"));

        const auto [token, secret] = store.create("Claude Code", Scope::Read | Scope::Content, ConfirmPolicy::AskAllWrites);
        QVERIFY(secret.startsWith(TokenStore::SECRET_PREFIX));
        QVERIFY(secret.size() > 40);
        QVERIFY(!secret.contains('='));

        const auto match = store.verify(secret);
        QVERIFY(match.has_value());
        QCOMPARE(match->id, token.id);
        QVERIFY(match->scopes.testFlag(Scope::Content));
        QVERIFY(!match->scopes.testFlag(Scope::Launch));

        QVERIFY(!store.verify(secret + "x").has_value());
        QVERIFY(!store.verify("not-a-token").has_value());
        QVERIFY(!store.verify(QString()).has_value());

        QVERIFY(store.revoke(token.id));
        QVERIFY(!store.verify(secret).has_value());
        QVERIFY(!store.revoke(token.id));
    }

    void secretsAreUnique()
    {
        QTemporaryDir dir;
        TokenStore store(dir.filePath("tokens.json"));
        const auto first = store.create("a", Scope::Read, ConfirmPolicy::NeverAsk).second;
        const auto second = store.create("b", Scope::Read, ConfirmPolicy::NeverAsk).second;
        QVERIFY(first != second);
    }

    void persistsHashesOnly()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("mcp/tokens.json");
        QString secret;
        QString id;
        {
            TokenStore store(path);
            auto created = store.create("VS Code", Scope::Read | Scope::Launch, ConfirmPolicy::AskDestructive);
            secret = created.second;
            id = created.first.id;
            QVERIFY(store.save().has_value());
        }

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto contents = file.readAll();
        QVERIFY(!contents.contains(secret.toUtf8()));
        QVERIFY(contents.contains(TokenStore::hashSecret(secret).toHex()));
#ifdef Q_OS_UNIX
        QVERIFY((file.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == QFileDevice::Permissions());
#endif

        TokenStore reloaded(path);
        QVERIFY(reloaded.load().has_value());
        QCOMPARE(reloaded.tokens().size(), 1);
        const auto match = reloaded.verify(secret);
        QVERIFY(match.has_value());
        QCOMPARE(match->id, id);
        QCOMPARE(match->name, QString("VS Code"));
        QVERIFY(match->policy == ConfirmPolicy::AskDestructive);
        QVERIFY(match->scopes == Scopes(Scope::Read | Scope::Launch));
    }

    void update()
    {
        QTemporaryDir dir;
        TokenStore store(dir.filePath("tokens.json"));
        const auto id = store.create("x", Scope::Read, ConfirmPolicy::NeverAsk).first.id;
        QVERIFY(store.update(id, "renamed", Scope::Read | Scope::Files, ConfirmPolicy::AskAllWrites));
        const auto token = store.find(id);
        QVERIFY(token.has_value());
        QCOMPARE(token->name, QString("renamed"));
        QVERIFY(token->scopes.testFlag(Scope::Files));
        QVERIFY(token->policy == ConfirmPolicy::AskAllWrites);
        QVERIFY(!store.update("missing", "y", Scope::Read, ConfirmPolicy::NeverAsk));
    }

    void scopeNamesRoundTrip()
    {
        for (auto scope : allScopes()) {
            QVERIFY(scopeFromName(scopeName(scope)) == std::optional<Scope>(scope));
        }
        QVERIFY(scopesFromNames({ "read", "bogus", "files" }) == Scopes(Scope::Read | Scope::Files));
        QVERIFY(!scopeFromName("bogus").has_value());
    }
};

QTEST_GUILESS_MAIN(McpTokensTest)

#include "McpTokens_test.moc"
