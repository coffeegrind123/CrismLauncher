// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QVBoxLayout>

#include "Application.h"
#include "BaseWizardPage.h"
#include "settings/SettingsObject.h"
#include "ui/widgets/MirrorSettingsWidget.h"

//! Last setup page: download mirrors. The defaults (official servers) suit most people
class MirrorWizardPage : public BaseWizardPage {
    Q_OBJECT

   public:
    explicit MirrorWizardPage(QWidget* parent = nullptr) : BaseWizardPage(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(&m_widget);
        layout->addStretch();
        layout->setContentsMargins(0, 0, 0, 0);
        retranslate();
    }

    bool validatePage() override
    {
        m_widget.applySettings();
        APPLICATION->settings()->set("UserAskedAboutMirrors", true);
        return true;
    }

    void retranslate() override
    {
        setTitle(tr("Download Mirrors"));
        setSubTitle(tr("Mirrors can speed up downloads where the official servers are slow, for example in mainland China. "
                       "If downloads work fine for you, leave everything as it is."));
        m_widget.retranslateUi();
    }

   private:
    MirrorSettingsWidget m_widget;
};
