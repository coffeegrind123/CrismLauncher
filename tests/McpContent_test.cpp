#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include <mcp/McpCommon.h>
#include <minecraft/MinecraftInstance.h>
#include <settings/INISettingsObject.h>

// Global settings an instance overrides or passes through: BaseInstance's constructor and
// MinecraftInstance::loadSpecificSettings (run by settings()) dereference each one
static const QStringList INSTANCE_OVERRIDES{
    "AutoCloseConsole", "ConsoleMaxLines", "ConsoleOverflowStop", "LogPrePostOutput", "PostExitCommand", "PreLaunchCommand",
    "PreLoadCommand", "RecordGameTime", "ShowConsole", "ShowConsoleOnError", "ShowGameTime", "WrapperCommand",
    "JavaPath", "JvmArgs", "IgnoreJavaCompatibility", "JavaSignature", "JavaArchitecture", "JavaRealArchitecture",
    "JavaVersion", "JavaVendor", "LaunchMaximized", "MinecraftWinWidth", "MinecraftWinHeight", "MinMemAlloc",
    "MaxMemAlloc", "PermGen", "LowMemWarning", "UseNativeOpenAL", "CustomOpenALPath", "UseNativeGLFW",
    "CustomGLFWPath", "UseNativeSDL", "CustomSDLPath", "EnableFeralGamemode", "EnableMangoHud", "UseDiscreteGpu",
    "UseZink", "CloseAfterLaunch", "QuitAfterGameStop", "OnlineFixes", "Env",
};

class McpContentTest : public QObject {
    Q_OBJECT

   private slots:
    // An instance only has a data pack model while "global data packs" is on (off by default). instance_get and
    // every content op taking kind=datapacks used the null model and crashed the launcher (2026-11-08).
    void dataPacksOffIsAnError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        INISettingsObject global(dir.filePath("global.cfg"));
        for (const auto& id : INSTANCE_OVERRIDES) {
            global.registerSetting(id, QVariant());
        }
        MinecraftInstance instance(&global, std::make_unique<INISettingsObject>(dir.filePath("instance.cfg")), dir.path());

        auto model = Mcp::contentModel(&instance, Mcp::ContentKind::DataPacks);
        QVERIFY2(!model, "contentModel must report a missing data pack model, not hand out null");
        QVERIFY(model.error().contains("GlobalDataPacksEnabled"));
    }
};

QTEST_GUILESS_MAIN(McpContentTest)

#include "McpContent_test.moc"
