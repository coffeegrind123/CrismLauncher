// SPDX-License-Identifier: GPL-3.0-only
#include "MirrorSettingsWidget.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QUrl>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

#include <algorithm>

#include "Application.h"
#include "net/DownloadMirror.h"
#include "settings/SettingsObject.h"

namespace {
QLabel* makeNote(QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(true);
    return label;
}

void selectData(QComboBox* comboBox, int value)
{
    comboBox->setCurrentIndex(std::max(0, comboBox->findData(value)));
}
}  // namespace

MirrorSettingsWidget::MirrorSettingsWidget(QWidget* parent) : QWidget(parent)
{
    static const QRegularExpression s_validUrlRegExp("https?://.+");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_gameFilesGroup = new QGroupBox(this);
    auto* gameFilesLayout = new QVBoxLayout(m_gameFilesGroup);
    m_gameFilesNote = makeNote(m_gameFilesGroup);
    m_gameFilesMode = new QComboBox(m_gameFilesGroup);
    m_gameFilesURL = new QLineEdit(m_gameFilesGroup);
    m_gameFilesURL->setValidator(new QRegularExpressionValidator(s_validUrlRegExp, m_gameFilesURL));
    m_gameFilesURL->setPlaceholderText(Net::DownloadMirror::DEFAULT_BASE_URL);
    gameFilesLayout->addWidget(m_gameFilesNote);
    gameFilesLayout->addWidget(m_gameFilesMode);
    gameFilesLayout->addWidget(m_gameFilesURL);
    layout->addWidget(m_gameFilesGroup);

    m_modsGroup = new QGroupBox(this);
    auto* modsLayout = new QVBoxLayout(m_modsGroup);
    m_modsNote = makeNote(m_modsGroup);
    modsLayout->addWidget(m_modsNote);
    auto* platformsLayout = new QFormLayout();
    m_modrinthLabel = new QLabel(m_modsGroup);
    m_modrinthMirror = new QComboBox(m_modsGroup);
    m_curseForgeLabel = new QLabel(m_modsGroup);
    m_curseForgeMirror = new QComboBox(m_modsGroup);
    m_modrinthLabel->setBuddy(m_modrinthMirror);
    m_curseForgeLabel->setBuddy(m_curseForgeMirror);
    platformsLayout->addRow(m_modrinthLabel, m_modrinthMirror);
    platformsLayout->addRow(m_curseForgeLabel, m_curseForgeMirror);
    modsLayout->addLayout(platformsLayout);
    layout->addWidget(m_modsGroup);

    connect(m_gameFilesMode, &QComboBox::currentIndexChanged, this, [this] {
        m_gameFilesURL->setEnabled(m_gameFilesMode->currentData().toInt() != static_cast<int>(Net::DownloadMirror::Mode::Off));
    });

    retranslateUi();
    loadSettings();
}

void MirrorSettingsWidget::fillComboBoxes()
{
    using Net::DownloadMirror::Mode;
    using Net::DownloadMirror::ModPlatformMirror;

    // Refilling for a language change must keep the selection
    const auto gameFilesMode = m_gameFilesMode->currentData();
    const auto modrinth = m_modrinthMirror->currentData();
    const auto curseForge = m_curseForgeMirror->currentData();

    m_gameFilesMode->clear();
    m_gameFilesMode->addItem(tr("Off: download from the official servers"), static_cast<int>(Mode::Off));
    m_gameFilesMode->addItem(tr("Prefer the mirror, fall back to the official servers"), static_cast<int>(Mode::PreferMirror));
    m_gameFilesMode->addItem(tr("Mirror only (fails when the mirror does; Java downloads may not work)"),
                             static_cast<int>(Mode::MirrorOnly));

    for (auto* comboBox : { m_modrinthMirror, m_curseForgeMirror }) {
        comboBox->clear();
        comboBox->addItem(tr("Official servers"), static_cast<int>(ModPlatformMirror::Official));
        comboBox->addItem(tr("MCIM, fall back to the official servers"), static_cast<int>(ModPlatformMirror::Mcim));
    }

    if (gameFilesMode.isValid()) {
        selectData(m_gameFilesMode, gameFilesMode.toInt());
        selectData(m_modrinthMirror, modrinth.toInt());
        selectData(m_curseForgeMirror, curseForge.toInt());
    }
}

void MirrorSettingsWidget::loadSettings()
{
    auto* s = APPLICATION->settings();
    selectData(m_gameFilesMode, s->get("DownloadMirrorMode").toInt());
    m_gameFilesURL->setText(s->get("DownloadMirrorURL").toString());
    m_gameFilesURL->setEnabled(m_gameFilesMode->currentData().toInt() != static_cast<int>(Net::DownloadMirror::Mode::Off));
    selectData(m_modrinthMirror, s->get("ModrinthMirror").toInt());
    selectData(m_curseForgeMirror, s->get("CurseForgeMirror").toInt());
}

void MirrorSettingsWidget::applySettings()
{
    auto* s = APPLICATION->settings();

    QUrl url(m_gameFilesURL->text());
    if (!url.isEmpty() && !url.path().endsWith('/')) {
        url.setPath(url.path() + '/');
    }
    const bool isLocalhost = url.host() == "localhost" || url.host() == "127.0.0.1" || url.host() == "::1";
    if (!url.isEmpty() && url.scheme() == "http" && !isLocalhost) {
        url.setScheme("https");
    }

    s->set("DownloadMirrorMode", m_gameFilesMode->currentData().toInt());
    s->set("DownloadMirrorURL", url.toString());
    s->set("ModrinthMirror", m_modrinthMirror->currentData().toInt());
    s->set("CurseForgeMirror", m_curseForgeMirror->currentData().toInt());
}

void MirrorSettingsWidget::retranslateUi()
{
    m_gameFilesGroup->setTitle(tr("Game Files"));
    m_gameFilesNote->setText(
        tr("Download Minecraft, its libraries and assets, Java, Forge and NeoForge from a mirror, which can be much faster in some "
           "regions. Files are still checked against the official checksums, and files without one always come from the official "
           "servers. Uses <a href=\"https://bmclapidoc.bangbang93.com/\">BMCLAPI</a> by bangbang93 unless another server with the same "
           "layout is set below."));

    m_modsGroup->setTitle(tr("Mods"));
    m_modsNote->setText(
        tr("Search, update checks and mod downloads for Modrinth and CurseForge can go through "
           "<a href=\"https://github.com/mcmod-info-mirror/mcim\">MCIM</a>, a community cache aimed at players in mainland China. "
           "Downloaded files are still checked against the official checksums, your API keys are never sent to it, and anything it "
           "can't answer is fetched from the official servers. Its data can lag behind by a few hours. Requests to it always use the "
           "launcher's standard User Agent, ignoring a custom one."));
    m_modrinthLabel->setText(tr("&Modrinth:"));
    m_curseForgeLabel->setText(tr("&CurseForge:"));

    fillComboBoxes();
}
