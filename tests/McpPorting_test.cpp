#include <QTest>
#include <QtEndian>

#include <mcp/McpPortingOps.h>

using namespace Mcp;

namespace {
QByteArray u16(int value)
{
    QByteArray out(2, '\0');
    qToBigEndian<quint16>(static_cast<quint16>(value), out.data());
    return out;
}

QByteArray utf8Entry(const QByteArray& text)
{
    return QByteArray(1, '\x01') + u16(static_cast<int>(text.size())) + text;
}

QByteArray classEntry(int nameIndex)
{
    return QByteArray(1, '\x07') + u16(nameIndex);
}
}  // namespace

class McpPortingTest : public QObject {
    Q_OBJECT

   private slots:
    void readsClassReferences()
    {
        // A minimal class file: just the header and a constant pool, which is all the scanner reads
        QByteArray pool;
        pool += utf8Entry("net/minecraft/core/BlockPos");                                                    // #1
        pool += classEntry(1);                                                                               // #2
        pool += QByteArray(1, '\x05') + QByteArray(8, '\0');                                                 // #3-#4 Long
        pool += utf8Entry("(Lnet/minecraft/world/level/Level;[Lcom/mojang/blaze3d/vertex/PoseStack;)V");      // #5
        pool += utf8Entry("[Lnet/minecraft/core/Direction;");                                                // #6
        pool += classEntry(6);                                                                               // #7
        pool += utf8Entry("java/lang/Object");                                                               // #8
        pool += classEntry(8);                                                                               // #9
        QByteArray file = QByteArray::fromHex("CAFEBABE") + u16(0) + u16(69) + u16(10) + pool;

        const auto references = readClassReferences(file);
        QVERIFY2(references.has_value(), qPrintable(references ? QString() : references.error()));
        QCOMPARE(references->majorVersion, 69);
        QVERIFY(references->classes.contains("net/minecraft/core/BlockPos"));
        QVERIFY(references->classes.contains("net/minecraft/core/Direction"));
        QVERIFY(references->classes.contains("net/minecraft/world/level/Level"));
        QVERIFY(references->classes.contains("com/mojang/blaze3d/vertex/PoseStack"));
        QVERIFY(references->classes.contains("java/lang/Object"));
    }

    void rejectsBrokenFiles()
    {
        QVERIFY(!readClassReferences("not a class").has_value());
        QVERIFY(!readClassReferences(QByteArray::fromHex("CAFEBABE00000045000501")).has_value());
    }

    void javaReleases()
    {
        QCOMPARE(javaRelease(52), 8);
        QCOMPARE(javaRelease(65), 21);
        QCOMPARE(javaRelease(69), 25);
    }

    void primers()
    {
        const auto backport = primersBetween("26.3", "1.21.1");
        QVERIFY(!backport.isEmpty());
        QVERIFY(backport.first().contains("/1.21.2/"));
        QVERIFY(backport.last().contains("/26.3/"));
        QVERIFY(!backport.join(' ').contains("/1.21.1/"));
        QCOMPARE(primersBetween("1.21.1", "26.3"), backport);
        QVERIFY(primersBetween("1.21.1", "1.21.1").isEmpty());
    }
};

QTEST_GUILESS_MAIN(McpPortingTest)

#include "McpPorting_test.moc"
