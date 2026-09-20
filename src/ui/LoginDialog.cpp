#include "ui/LoginDialog.h"

#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/RestClient.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QLabel *makeHeading(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600; color: %1;")
                             .arg(QLatin1String(Theme::TextPrimary)));
    return label;
}

QLabel *makeHint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));
    return label;
}

QPushButton *makeLinkButton(const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(QStringLiteral("QPushButton { background: transparent; border: none; color: %1; "
                                         "text-align: left; padding: 2px 0; } "
                                         "QPushButton:hover { color: %2; }")
                              .arg(QLatin1String(Theme::Blue), QLatin1String(Theme::Accent)));
    return button;
}

} // namespace

LoginDialog::LoginDialog(RestClient *rest, QWidget *parent)
    : QDialog(parent)
    , m_rest(rest)
{
    setWindowTitle(QStringLiteral("Wisp - sign in"));
    setMinimumWidth(460);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("Wisp"), this);
    title->setStyleSheet(QStringLiteral("font-size: 28px; font-weight: 600; color: %1;")
                             .arg(QLatin1String(Theme::Accent)));
    layout->addWidget(title);
    layout->addWidget(makeHint(QStringLiteral("A small Discord client."), this));

    auto *warning = new QLabel(
        QStringLiteral("Discord does not allow third party clients on a normal account. "
                       "Using one can get the account banned. Your password is sent only to "
                       "discord.com and is never saved."),
        this);
    warning->setWordWrap(true);
    warning->setStyleSheet(QStringLiteral("color: %1; background-color: %2; border: 1px solid %3; "
                                          "border-radius: 10px; padding: 10px;")
                               .arg(QLatin1String(Theme::LightGray), QLatin1String(Theme::SurfaceInput),
                                    QLatin1String(Theme::Border)));
    layout->addWidget(warning);

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildCredentialsPage());
    m_pages->addWidget(buildMfaPage());
    m_pages->addWidget(buildTokenPage());
    layout->addWidget(m_pages);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(m_statusLabel);

    auto *bottomRow = new QHBoxLayout;
    auto *quitButton = new QPushButton(QStringLiteral("Quit"), this);
    connect(quitButton, &QPushButton::clicked, this, &QDialog::reject);
    bottomRow->addWidget(quitButton);
    bottomRow->addStretch(1);
    layout->addLayout(bottomRow);

    connect(&m_auth, &AuthClient::succeeded, this, &LoginDialog::onAuthSucceeded);
    connect(&m_auth, &AuthClient::mfaRequired, this, &LoginDialog::onMfaRequired);
    connect(&m_auth, &AuthClient::captchaRequired, this, &LoginDialog::onCaptchaRequired);
    connect(&m_auth, &AuthClient::failed, this, &LoginDialog::onAuthFailed);
    connect(&m_auth, &AuthClient::smsCodeSent, this, [this]() {
        setStatus(QStringLiteral("Text message sent. Type the code above."));
        setBusy(false);
    });

    showPage(CredentialsPage);
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

QWidget *LoginDialog::buildCredentialsPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(makeHeading(QStringLiteral("Sign in"), page));

    m_loginEdit = new QLineEdit(page);
    m_loginEdit->setPlaceholderText(QStringLiteral("Email or phone number"));
    layout->addWidget(m_loginEdit);

    m_passwordEdit = new QLineEdit(page);
    m_passwordEdit->setPlaceholderText(QStringLiteral("Password"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    layout->addWidget(m_passwordEdit);

    m_rememberBox = new QCheckBox(QStringLiteral("Stay signed in on this machine"), page);
    m_rememberBox->setChecked(true);
    layout->addWidget(m_rememberBox);

    m_logInButton = new QPushButton(QStringLiteral("Log in"), page);
    m_logInButton->setObjectName(QStringLiteral("PrimaryButton"));
    m_logInButton->setDefault(true);
    layout->addWidget(m_logInButton);

    auto *tokenLink = makeLinkButton(QStringLiteral("Use a token instead"), page);
    layout->addWidget(tokenLink);

    connect(m_logInButton, &QPushButton::clicked, this, &LoginDialog::submitCredentials);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &LoginDialog::submitCredentials);
    connect(m_loginEdit, &QLineEdit::returnPressed, this, &LoginDialog::submitCredentials);
    connect(tokenLink, &QPushButton::clicked, this, &LoginDialog::useTokenInstead);

    return page;
}

QWidget *LoginDialog::buildMfaPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(makeHeading(QStringLiteral("Two step check"), page));

    m_methodBox = new QComboBox(page);
    layout->addWidget(m_methodBox);

    m_codeEdit = new QLineEdit(page);
    m_codeEdit->setPlaceholderText(QStringLiteral("Code"));
    layout->addWidget(m_codeEdit);

    m_codeHint = makeHint(QString(), page);
    layout->addWidget(m_codeHint);

    auto *buttonRow = new QHBoxLayout;
    m_sendSmsButton = new QPushButton(QStringLiteral("Send text message"), page);
    m_sendSmsButton->setVisible(false);
    buttonRow->addWidget(m_sendSmsButton);
    buttonRow->addStretch(1);

    m_verifyButton = new QPushButton(QStringLiteral("Verify"), page);
    m_verifyButton->setObjectName(QStringLiteral("PrimaryButton"));
    buttonRow->addWidget(m_verifyButton);
    layout->addLayout(buttonRow);

    auto *backLink = makeLinkButton(QStringLiteral("Back to sign in"), page);
    layout->addWidget(backLink);

    connect(m_methodBox, &QComboBox::currentIndexChanged, this, [this](int) { refreshMfaMethodUi(); });
    connect(m_verifyButton, &QPushButton::clicked, this, &LoginDialog::submitSecondFactor);
    connect(m_codeEdit, &QLineEdit::returnPressed, this, &LoginDialog::submitSecondFactor);
    connect(m_sendSmsButton, &QPushButton::clicked, this, [this]() {
        setBusy(true);
        setStatus(QStringLiteral("Asking Discord to send a text..."));
        m_auth.requestSmsCode(m_mfa.ticket);
    });
    connect(backLink, &QPushButton::clicked, this, [this]() {
        m_codeEdit->clear();
        setStatus(QString());
        showPage(CredentialsPage);
    });

    return page;
}

QWidget *LoginDialog::buildTokenPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(makeHeading(QStringLiteral("Sign in with a token"), page));
    layout->addWidget(makeHint(
        QStringLiteral("Use this when Discord asks for a captcha. Wisp cannot answer a captcha, "
                       "so the password path stops there."),
        page));

    m_tokenEdit = new QLineEdit(page);
    m_tokenEdit->setPlaceholderText(QStringLiteral("Paste your account token"));
    m_tokenEdit->setEchoMode(QLineEdit::Password);
    m_tokenEdit->setText(AppConfig::instance().token());
    layout->addWidget(m_tokenEdit);

    m_tokenButton = new QPushButton(QStringLiteral("Connect"), page);
    m_tokenButton->setObjectName(QStringLiteral("PrimaryButton"));
    layout->addWidget(m_tokenButton);

    auto *backLink = makeLinkButton(QStringLiteral("Back to sign in"), page);
    layout->addWidget(backLink);

    const auto submitToken = [this]() {
        const QString value = m_tokenEdit->text().trimmed();
        if (value.isEmpty()) {
            setStatus(QStringLiteral("Paste a token first."), true);
            return;
        }
        setBusy(true);
        setStatus(QStringLiteral("Checking the token..."));
        m_rest->setToken(value);
        m_rest->fetchCurrentUser(
            [this, value](const QJsonObject &) { finishWith(value); },
            [this](const RestClient::Error &error) {
                setBusy(false);
                if (error.httpStatus == 401)
                    setStatus(QStringLiteral("Discord rejected that token."), true);
                else if (error.isRateLimit())
                    setStatus(QStringLiteral("Rate limited. Wait a moment."), true);
                else
                    setStatus(QStringLiteral("Could not connect: %1").arg(error.message), true);
            });
    };

    connect(m_tokenButton, &QPushButton::clicked, this, submitToken);
    connect(m_tokenEdit, &QLineEdit::returnPressed, this, submitToken);
    connect(backLink, &QPushButton::clicked, this, [this]() {
        setStatus(QString());
        showPage(CredentialsPage);
    });

    return page;
}

// ---------------------------------------------------------------------------
// Flow
// ---------------------------------------------------------------------------

void LoginDialog::showPage(Page page)
{
    m_pages->setCurrentIndex(page);
    adjustSize();

    switch (page) {
    case CredentialsPage:
        m_logInButton->setDefault(true);
        m_loginEdit->setFocus();
        break;
    case MfaPage:
        m_verifyButton->setDefault(true);
        m_codeEdit->setFocus();
        break;
    case TokenPage:
        m_tokenButton->setDefault(true);
        m_tokenEdit->setFocus();
        break;
    }
}

void LoginDialog::setBusy(bool busy)
{
    m_loginEdit->setEnabled(!busy);
    m_passwordEdit->setEnabled(!busy);
    m_logInButton->setEnabled(!busy);
    m_codeEdit->setEnabled(!busy);
    m_verifyButton->setEnabled(!busy);
    m_sendSmsButton->setEnabled(!busy);
    m_tokenEdit->setEnabled(!busy);
    m_tokenButton->setEnabled(!busy);
}

void LoginDialog::setStatus(const QString &text, bool isError)
{
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1;")
                                     .arg(QLatin1String(isError ? Theme::Accent : Theme::TextFaint)));
}

bool LoginDialog::shouldRemember() const
{
    return m_rememberBox->isChecked();
}

void LoginDialog::submitCredentials()
{
    setBusy(true);
    setStatus(QStringLiteral("Signing in..."));
    m_auth.logIn(m_loginEdit->text(), m_passwordEdit->text());
}

void LoginDialog::submitSecondFactor()
{
    const QString code = m_codeEdit->text().trimmed();
    if (code.isEmpty()) {
        setStatus(QStringLiteral("Type the code first."), true);
        return;
    }

    setBusy(true);
    setStatus(QStringLiteral("Checking the code..."));

    const QString method = m_methodBox->currentData().toString();
    if (method == QLatin1String("sms"))
        m_auth.submitSmsCode(m_mfa.ticket, code);
    else if (method == QLatin1String("backup"))
        m_auth.submitBackupCode(m_mfa.ticket, code);
    else
        m_auth.submitTotp(m_mfa.ticket, code);
}

void LoginDialog::refreshMfaMethodUi()
{
    const QString method = m_methodBox->currentData().toString();

    m_sendSmsButton->setVisible(method == QLatin1String("sms"));

    if (method == QLatin1String("sms")) {
        m_codeEdit->setPlaceholderText(QStringLiteral("6 digit code from the text"));
        m_codeHint->setText(QStringLiteral("Press \"Send text message\" first."));
    } else if (method == QLatin1String("backup")) {
        m_codeEdit->setPlaceholderText(QStringLiteral("One backup code"));
        m_codeHint->setText(QStringLiteral("Each backup code works once."));
    } else {
        m_codeEdit->setPlaceholderText(QStringLiteral("6 digit code from your app"));
        m_codeHint->setText(QStringLiteral("Open your authenticator app and read the current code."));
    }
}

void LoginDialog::onMfaRequired(const AuthClient::MfaOptions &options)
{
    m_mfa = options;
    setBusy(false);

    m_methodBox->clear();
    if (options.totp)
        m_methodBox->addItem(QStringLiteral("Authenticator app"), QStringLiteral("totp"));
    if (options.sms)
        m_methodBox->addItem(QStringLiteral("Text message"), QStringLiteral("sms"));
    if (options.backup)
        m_methodBox->addItem(QStringLiteral("Backup code"), QStringLiteral("backup"));

    m_methodBox->setVisible(m_methodBox->count() > 1);
    refreshMfaMethodUi();

    if (options.webauthn) {
        setStatus(QStringLiteral("This account also offers a security key. Wisp does not support "
                                 "security keys, so use one of the other choices."));
    } else {
        setStatus(QStringLiteral("Password accepted. One more step."));
    }

    showPage(MfaPage);
}

void LoginDialog::onCaptchaRequired(const QString &service, const QString &siteKey)
{
    Q_UNUSED(siteKey)
    setBusy(false);
    setStatus(QStringLiteral("Discord asked for a %1 captcha. Wisp cannot answer one. "
                             "Log in at discord.com in a browser, then use a token here.")
                  .arg(service),
              true);
    showPage(TokenPage);
}

void LoginDialog::onAuthFailed(const QString &message)
{
    setBusy(false);
    setStatus(message, true);
}

void LoginDialog::onAuthSucceeded(const QString &token)
{
    setStatus(QStringLiteral("Signed in. Loading..."));
    m_rest->setToken(token);
    finishWith(token);
}

void LoginDialog::useTokenInstead()
{
    setStatus(QString());
    showPage(TokenPage);
}

void LoginDialog::finishWith(const QString &token)
{
    m_token = token;

    // Only the token is kept. The password was never written anywhere.
    const bool remember = shouldRemember();
    bool saved = false;

    if (remember)
        saved = AppConfig::instance().setToken(token);
    else
        AppConfig::instance().clearToken();

    wlog(QStringLiteral("login"),
         QStringLiteral("finished: remember=%1, saved=%2").arg(remember).arg(saved));

    // Say so rather than quietly asking again next time.
    if (remember && !saved) {
        setStatus(QStringLiteral("Signed in, but the session could not be saved, so Wisp will ask "
                                 "again next time. The log has the reason."),
                  true);
    }

    // Clear the password field before the window goes away.
    m_passwordEdit->clear();

    accept();
}
