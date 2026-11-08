#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include <mcp/McpCommon.h>
#include <minecraft/MinecraftInstance.h>
#include <settings/INISettingsObject.h>

// Global settings BaseInstance's constructor overrides per instance; nothing else is needed to construct one
static const QStringList INSTANCE_OVERRIDES{ "AutoCloseConsole", "ConsoleMaxLines", "ConsoleOverflowStop", "LogPrePostOutput",
                                             "PostExitCommand",  "PreLaunchCommand", "PreLoadCommand",     "RecordGameTime",
                                             "ShowConsole",      "ShowConsoleOnError", "ShowGameTime",     "WrapperCommand" };

class McpContentTest : public QObject {
    Q_OBJECT

   private slots:
    // An instance only has a data pack model while "global data packs" is on (off by default). instance_get and
    // every content op taking kind=datapacks used the null model and crashed the launcher (2026-11-08).
    void dataPacksOffHasNoModel()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        INISettingsObject global(dir.filePath("global.cfg"));
        for (const auto& id : INSTANCE_OVERRIDES) {
            global.registerSetting(id, QVariant());
        }
        MinecraftInstance instance(&global, std::make_unique<INISettingsObject>(dir.filePath("instance.cfg")), dir.path());

        auto* model = Mcp::contentModel(&instance, Mcp::ContentKind::DataPacks);
        QVERIFY2(model != nullptr, "contentModel handed out a null data pack model");
    }
};

QTEST_GUILESS_MAIN(McpContentTest)

#include "McpContent_test.moc"
