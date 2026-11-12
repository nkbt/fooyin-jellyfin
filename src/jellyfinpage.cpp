/*
 * fooyin-jellyfin
 * Copyright © 2026, Nikita Butenko
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "jellyfinpage.h"

#include "jellyfinaccount.h"
#include "jellyfincatalogsync.h"

#include <utils/settings/settingsmanager.h>

#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin {
class JellyfinPageWidget : public SettingsPageWidget
{
    Q_OBJECT

public:
    JellyfinPageWidget(Account* account, SettingsManager* settings);

    void load() override;
    void apply() override;
    void reset() override;

private:
    void startQuickConnect();
    void updateState();

    Account* m_account;
    SettingsManager* m_settings;
    QLineEdit* m_serverUrl;
    QPushButton* m_startQuickConnect;
    QPushButton* m_cancelQuickConnect;
    QLabel* m_quickConnectCode;
    QPushButton* m_signOut;
    QPushButton* m_refresh;
    QLabel* m_accountLabel;
    QLabel* m_status;
};

JellyfinPageWidget::JellyfinPageWidget(Account* account, SettingsManager* settings)
    : m_account{account}
    , m_settings{settings}
    , m_serverUrl{new QLineEdit(this)}
    , m_startQuickConnect{new QPushButton(tr("Sign in with Quick Connect"), this)}
    , m_cancelQuickConnect{new QPushButton(tr("Cancel"), this)}
    , m_quickConnectCode{new QLabel(this)}
    , m_signOut{new QPushButton(tr("Sign out"), this)}
    , m_refresh{new QPushButton(tr("Refresh library"), this)}
    , m_accountLabel{new QLabel(this)}
    , m_status{new QLabel(this)}
{
    m_serverUrl->setPlaceholderText(u"https://jellyfin.example.com"_s);
    m_serverUrl->setToolTip(tr("Jellyfin server address, including any base path"));

    QFont codeFont{m_quickConnectCode->font()};
    codeFont.setPointSizeF(codeFont.pointSizeF() * 2);
    codeFont.setBold(true);
    m_quickConnectCode->setFont(codeFont);
    m_quickConnectCode->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_quickConnectCode->setToolTip(tr("Enter this code in Quick Connect on a signed-in Jellyfin client"));

    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* serverGroup  = new QGroupBox(tr("Server"), this);
    auto* serverLayout = new QGridLayout(serverGroup);
    serverLayout->addWidget(new QLabel(tr("Server address") + ":"_L1, this), 0, 0);
    serverLayout->addWidget(m_serverUrl, 0, 1);
    serverLayout->setColumnStretch(1, 1);

    auto* quickConnectButtons = new QHBoxLayout();
    quickConnectButtons->addWidget(m_startQuickConnect);
    quickConnectButtons->addWidget(m_cancelQuickConnect);
    quickConnectButtons->addStretch();
    serverLayout->addLayout(quickConnectButtons, 1, 0, 1, 2);
    serverLayout->addWidget(new QLabel(tr("Quick Connect code") + ":"_L1, this), 2, 0);
    serverLayout->addWidget(m_quickConnectCode, 2, 1);

    auto* accountButtons = new QHBoxLayout();
    accountButtons->addWidget(m_signOut);
    accountButtons->addWidget(m_refresh);
    accountButtons->addStretch();

    auto* statusGroup  = new QGroupBox(tr("Status"), this);
    auto* statusLayout = new QVBoxLayout(statusGroup);
    statusLayout->addWidget(m_accountLabel);
    statusLayout->addWidget(m_status);
    statusLayout->addLayout(accountButtons);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(serverGroup);
    layout->addWidget(statusGroup);
    layout->addStretch();

    QObject::connect(m_startQuickConnect, &QPushButton::clicked, this, &JellyfinPageWidget::startQuickConnect);
    QObject::connect(m_serverUrl, &QLineEdit::returnPressed, this, &JellyfinPageWidget::startQuickConnect);
    QObject::connect(m_serverUrl, &QLineEdit::textChanged, this, &JellyfinPageWidget::updateState);
    QObject::connect(m_cancelQuickConnect, &QPushButton::clicked, m_account, &Account::cancelQuickConnect);
    QObject::connect(m_signOut, &QPushButton::clicked, m_account, &Account::signOut);
    QObject::connect(m_refresh, &QPushButton::clicked, m_account, &Account::refresh);
    QObject::connect(m_account, &Account::statusChanged, m_status, &QLabel::setText);
    QObject::connect(m_account, &Account::connectionChanged, this, &JellyfinPageWidget::updateState);
    QObject::connect(m_account, &Account::quickConnectChanged, this, &JellyfinPageWidget::updateState);
    QObject::connect(m_account->catalogSync(), &CatalogSync::finished, this, &JellyfinPageWidget::updateState);
    QObject::connect(m_account->catalogSync(), &CatalogSync::progressChanged, this, &JellyfinPageWidget::updateState);
}

void JellyfinPageWidget::load()
{
    const auto connection = m_account->connection();
    m_serverUrl->setText(connection ? connection->serverUrl.toString()
                                    : m_settings->fileValue(ServerUrlKey).toString());
    m_status->setText(m_account->status());
    updateState();
}

void JellyfinPageWidget::apply() { }

void JellyfinPageWidget::reset() { }

void JellyfinPageWidget::startQuickConnect()
{
    if(m_account->isQuickConnectPending() || m_serverUrl->text().trimmed().isEmpty()) {
        return;
    }
    m_account->startQuickConnect(m_serverUrl->text().trimmed());
}

void JellyfinPageWidget::updateState()
{
    const auto connection = m_account->connection();
    const bool running    = m_account->catalogSync()->isRunning();
    const bool pending    = m_account->isQuickConnectPending();
    const bool hasServer  = !m_serverUrl->text().trimmed().isEmpty();

    m_accountLabel->setText(
        connection ? tr("Signed in as %1 on %2").arg(connection->userName, connection->serverUrl.toDisplayString())
                   : tr("Not signed in"));

    const QString code = m_account->quickConnectCode();
    m_quickConnectCode->setText(code);
    m_quickConnectCode->setVisible(!code.isEmpty());

    m_serverUrl->setEnabled(!pending);
    m_serverUrl->setToolTip(pending ? tr("Unavailable while a Quick Connect request is pending")
                                    : tr("Jellyfin server address, including any base path"));
    m_startQuickConnect->setEnabled(!pending && hasServer);
    m_startQuickConnect->setToolTip(pending ? tr("Unavailable while a Quick Connect request is pending")
                                    : !hasServer
                                        ? tr("Unavailable until a server address is entered")
                                        : tr("Request a Quick Connect code from the server and wait for approval"));
    m_cancelQuickConnect->setEnabled(pending);
    m_cancelQuickConnect->setToolTip(pending ? tr("Stop waiting for Quick Connect approval")
                                             : tr("Unavailable because no Quick Connect request is pending"));
    m_signOut->setEnabled(connection.has_value());
    m_signOut->setToolTip(connection
                              ? tr("Remove the stored session token. The cached catalog is hidden until reconnection.")
                              : tr("Unavailable because no account is signed in"));
    m_refresh->setEnabled(connection.has_value() && !running);
    m_refresh->setToolTip(!connection ? tr("Unavailable because no account is signed in")
                          : running   ? tr("Unavailable while synchronisation is running")
                                      : tr("Synchronise the Jellyfin catalog, playlists and favourites now"));
}

JellyfinPage::JellyfinPage(Account* account, SettingsManager* settings, QObject* parent)
    : SettingsPage{settings->settingsDialog(), parent}
{
    setId("Fooyin.Page.Integrations.Jellyfin");
    setName(tr("Jellyfin"));
    setCategory({tr("Integrations"), tr("Jellyfin")});
    setWidgetCreator([account, settings] { return new JellyfinPageWidget(account, settings); });
}
} // namespace Fooyin::Jellyfin

#include "jellyfinpage.moc"
#include "moc_jellyfinpage.cpp"
