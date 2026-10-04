#include "LoginWizardPage.h"
#include "minecraft/auth/AccountList.h"
#include "ui/dialogs/AuthlibInjectorLoginDialog.h"
#include "ui/dialogs/ChooseOfflineNameDialog.h"
#include "ui/dialogs/MSALoginDialog.h"
#include "ui_LoginWizardPage.h"

#include "Application.h"

LoginWizardPage::LoginWizardPage(QWidget* parent) : BaseWizardPage(parent), ui(new Ui::LoginWizardPage)
{
    ui->setupUi(this);

    // Xbox authentication won't work without a client identifier
    const bool supportsMsa = APPLICATION->capabilities() & Application::SupportsMSA;
    ui->microsoftButton->setVisible(supportsMsa);
    ui->microsoftLabel->setVisible(supportsMsa);
}

LoginWizardPage::~LoginWizardPage()
{
    delete ui;
}

void LoginWizardPage::initializePage() {}

bool LoginWizardPage::validatePage()
{
    return true;
}

void LoginWizardPage::retranslate()
{
    ui->retranslateUi(this);
}

void LoginWizardPage::on_microsoftButton_clicked()
{
    // The Microsoft login opens a browser; hide the wizard so it doesn't cover the dialog
    wizard()->hide();
    auto account = MSALoginDialog::newAccount(nullptr);
    wizard()->show();
    accountAdded(account);
}

void LoginWizardPage::on_authlibInjectorButton_clicked()
{
    accountAdded(AuthlibInjectorLoginDialog::newAccount(this, tr("Log in with an account from an authlib-injector compatible server.")));
}

void LoginWizardPage::on_offlineButton_clicked()
{
    accountAdded(ChooseOfflineNameDialog::newAccount(this, tr("Please enter your desired username to add your offline account.")));
}

void LoginWizardPage::accountAdded(const MinecraftAccountPtr& account)
{
    if (!account) {
        return;
    }

    APPLICATION->accounts()->addAccount(account);
    APPLICATION->accounts()->setDefaultAccount(account);
    if (wizard()->currentId() == wizard()->pageIds().last()) {
        wizard()->accept();
    } else {
        wizard()->next();
    }
}
