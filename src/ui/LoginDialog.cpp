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
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include "ui/AuroraWidget.h"

#include <QFrame>

namespace {

QLabel *makeHeading(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600; color: %1;")
                             .arg(QLatin1String(Theme::TextPrimary)));
    return label;
}

QLabel *makeHint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));
    return label;
}

// The button that actually does the thing, styled on the widget itself.
//
// It was relying on an application wide rule for #PrimaryButton, and on this
// screen it came out as dark text on a dark ground: present, clickable, and
// almost invisible. Rules that reach a widget through a cascade can be lost
// to anything else that matches, and the one control somebody has to find on
// a sign-in screen is a poor place to discover that. These colours are set
// where they cannot be overruled.
QPushButton *makePrimaryButton(const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(44);
    button->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border: none; border-radius: 10px; "
        "font-size: 14px; font-weight: 600; padding: 0 18px; } "
        "QPushButton:hover { background-color: %3; } "
        "QPushButton:pressed { background-color: %4; } "
        "QPushButton:disabled { background-color: %5; color: %6; }")
            .arg(QLatin1String(Theme::Accent), QLatin1String(Theme::Dark),
                 QLatin1String(Theme::AccentHover), QLatin1String(Theme::LightGray),
                 QLatin1String(Theme::SurfaceHover), QLatin1String(Theme::TextFaint)));
    return button;
}

QPushButton *makeLinkButton(const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(QStringLiteral("QPushButton { background: transparent; border: none; color: %1; "
                                         "text-align: left; padding: 2px 0; } "
                                         "QPushButton:hover { color: %2; }")
                              .arg(QLatin1String(Theme::Highlight), QLatin1String(Theme::Accent)));
    return button;
}

} // namespace

LoginDialog::LoginDialog(RestClient *rest, QWidget *parent)
    : QDialog(parent)
    , m_rest(rest)
{
    setWindowTitle(QStringLiteral("Singularity"));
    setMinimumSize(760, 620);

    // The hole is the window here too, exactly as in the main window. The
    // sign-in used to be a flat panel, which made the first thing anybody saw
    // the one screen that looked like nothing else in the program.
    //
    // Two rules make this work, both learned the hard way in MainWindow: a
    // translucent child of a QOpenGLWidget paints what is behind the GL
    // surface rather than the rendered frame, so the card has to be opaque;
    // and any pixel with no widget on it is the shader, so margins and
    // stretch are how the hole gets seen at all.
    m_aurora = new AuroraWidget(this);
    m_aurora->setAttribute(Qt::WA_OpaquePaintEvent, true);
    m_aurora->setAttribute(Qt::WA_NoSystemBackground, true);
    m_aurora->setAutoFillBackground(false);
    m_aurora->setHoleColors(Theme::holeAccent(), Theme::holeDisk(), Theme::holeGrade());

    auto *shell = new QVBoxLayout(this);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    shell->addWidget(m_aurora);

    // No card. The form sits on the hole itself.
    //
    // A panel with a border is the honest way to guarantee legibility, and it
    // looked like a dialog pasted over a wallpaper. What replaces it is a
    // gradient that starts nearly solid at the left edge and reaches zero
    // before it runs out of widget, so there is no edge anywhere for the eye
    // to catch: the words sit in the dark part of the picture and the disk
    // carries on through where they stop.
    //
    // This works at all because AuroraWidget renders its overlay into an
    // image with an alpha channel and composites that over the rendered
    // frame. A plain child of a GL widget cannot be see-through; one that
    // goes through setOverlay can.
    auto *centre = new QHBoxLayout(m_aurora);
    centre->setContentsMargins(0, 0, 0, 0);
    centre->setSpacing(0);
    centre->addStretch(1);

    auto *card = new QWidget(m_aurora);
    card->setObjectName(QStringLiteral("LoginPanel"));
    card->setFixedWidth(460);

    // Symmetric now that the column is centred. A one-sided fade only works
    // against an edge; in the middle of the window it would be a bright seam
    // down one side of the text and nothing down the other.
    card->setStyleSheet(QStringLiteral(
        "#LoginPanel { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 rgba(5, 7, 12, 0), stop:0.22 rgba(5, 7, 12, 214), "
        "stop:0.78 rgba(5, 7, 12, 214), stop:1 rgba(5, 7, 12, 0)); }"));
    centre->addWidget(card);
    centre->addStretch(1);

    m_aurora->setOverlay(card);

    // Margins wide enough on both sides that no word is ever read against the
    // part of the gradient that has started to fade.
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(70, 0, 70, 0);
    layout->setSpacing(14);
    layout->addStretch(1);

    auto *title = new QLabel(QStringLiteral("Singularity"), card);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size: 34px; font-weight: 600; letter-spacing: 0.4px; color: %1;")
                             .arg(QLatin1String(Theme::TextPrimary)));
    layout->addWidget(title);
    layout->addWidget(makeHint(QStringLiteral("A Discord client."), card));

    auto *warning = new QLabel(
        QStringLiteral("Discord does not allow third party clients on a normal account. "
                       "Using one can get the account banned. Your password is sent only to "
                       "discord.com and is never saved."),
        card);
    warning->setWordWrap(true);
    // A rule down the left rather than a box around everything. It marks the
    // text as a warning without drawing another rectangle on a screen whose
    // whole point is that it has none.
    warning->setStyleSheet(QStringLiteral("color: %1; background: transparent; "
                                          "border-left: 2px solid %2; padding: 2px 0 2px 12px;")
                               .arg(QLatin1String(Theme::TextMuted), QLatin1String(Theme::Red)));
    layout->addWidget(warning);

    m_pages = new QStackedWidget(card);
    m_pages->addWidget(buildCredentialsPage());
    m_pages->addWidget(buildMfaPage());
    m_pages->addWidget(buildTokenPage());
    m_pages->addWidget(buildQrPage());
    m_pages->addWidget(buildDevicePage());
    layout->addWidget(m_pages);

    m_statusLabel = new QLabel(card);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(m_statusLabel);

    auto *bottomRow = new QHBoxLayout;
    auto *quitButton = new QPushButton(QStringLiteral("Quit"), card);
    connect(quitButton, &QPushButton::clicked, this, &QDialog::reject);
    bottomRow->addStretch(1);
    bottomRow->addWidget(quitButton);
    bottomRow->addStretch(1);
    layout->addLayout(bottomRow);

    // Balances the stretch above, so the whole column sits in the middle of
    // the window rather than against the top of it.
    layout->addStretch(1);

    connect(&m_auth, &AuthClient::succeeded, this, &LoginDialog::onAuthSucceeded);
    connect(&m_auth, &AuthClient::mfaRequired, this, &LoginDialog::onMfaRequired);
    connect(&m_auth, &AuthClient::captchaRequired, this, &LoginDialog::onCaptchaRequired);
    connect(&m_auth, &AuthClient::failed, this, &LoginDialog::onAuthFailed);
    connect(&m_auth, &AuthClient::smsCodeSent, this, [this]() {
        setStatus(QStringLiteral("Text message sent. Type the code above."));
        setBusy(false);
    });

    // Discord stopped a correct password to check this device.
    connect(&m_auth, &AuthClient::phoneCheckRequired, this, [this]() { showDeviceCheck(true, QString()); });
    connect(&m_auth, &AuthClient::emailCheckRequired, this,
            [this](const QString &message) { showDeviceCheck(false, message); });
    connect(&m_auth, &AuthClient::deviceAuthorized, this, [this]() {
        setStatus(QStringLiteral("Device approved. Signing in..."));
        submitCredentials();
    });

    // QR sign-in. No password crosses this window at all; the phone approves.
    connect(&m_remote, &RemoteAuth::qrCodeReady, this, [this](const QImage &code, const QString &) {
        m_qrImage->setPixmap(QPixmap::fromImage(code).scaled(220, 220, Qt::KeepAspectRatio,
                                                             Qt::FastTransformation));
        m_qrStatus->setText(QStringLiteral("Open Discord on your phone, go to Settings and Scan QR Code, "
                                           "then point it here."));
    });
    connect(&m_remote, &RemoteAuth::scanned, this, [this](const QString &username) {
        m_qrStatus->setText(QStringLiteral("Confirm on your phone to sign in as %1.").arg(username));
    });
    connect(&m_remote, &RemoteAuth::succeeded, this, [this](const QString &token) {
        m_qrStatus->setText(QStringLiteral("Approved. Loading..."));
        m_rest->setToken(token);
        finishWith(token);
    });
    connect(&m_remote, &RemoteAuth::declined, this, [this]() {
        m_qrStatus->setText(QStringLiteral("The sign-in was declined on the phone."));
    });
    connect(&m_remote, &RemoteAuth::expired, this, [this]() {
        m_qrStatus->setText(QStringLiteral("This code timed out. Fetching a fresh one..."));
        if (m_pages->currentIndex() == QrPage)
            m_remote.start();
    });
    connect(&m_remote, &RemoteAuth::failed, this, [this](const QString &reason) {
        m_qrStatus->setText(reason);
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

    m_logInButton = makePrimaryButton(QStringLiteral("Log in"), page);
    m_logInButton->setDefault(true);
    layout->addWidget(m_logInButton);

    auto *qrLink = makeLinkButton(QStringLiteral("Sign in with a QR code (scan with your phone)"), page);
    layout->addWidget(qrLink);

    auto *tokenLink = makeLinkButton(QStringLiteral("Use a token instead"), page);
    layout->addWidget(tokenLink);

    connect(m_logInButton, &QPushButton::clicked, this, &LoginDialog::submitCredentials);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &LoginDialog::submitCredentials);
    connect(m_loginEdit, &QLineEdit::returnPressed, this, &LoginDialog::submitCredentials);
    connect(qrLink, &QPushButton::clicked, this, &LoginDialog::useQrInstead);
    connect(tokenLink, &QPushButton::clicked, this, &LoginDialog::useTokenInstead);

    return page;
}

// The second-factor page, laid out the way Discord's is.
//
// It used to open with a drop-down of every method the account had, which put
// the choice on the person - and "Authenticator app" sat on top whether or not
// they had one to hand. Discord's client does not ask. Its AuthenticationStore
// lists the methods in a fixed order (security key, authenticator app, backup
// code, text message), shows the first, and tucks the others behind a "verify
// with something else" link. This page does the same. The code box also
// takes a backup code while the authenticator app is showing, so nobody has
// to switch methods just because they reached for the wrong code.
QWidget *LoginDialog::buildMfaPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    m_mfaHeading = makeHeading(QStringLiteral("Two step check"), page);
    layout->addWidget(m_mfaHeading);

    m_codeHint = makeHint(QString(), page);
    layout->addWidget(m_codeHint);

    m_codeEdit = new QLineEdit(page);
    m_codeEdit->setPlaceholderText(QStringLiteral("Code"));
    m_codeEdit->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_codeEdit);

    m_verifyButton = makePrimaryButton(QStringLiteral("Verify"), page);
    layout->addWidget(m_verifyButton);

    m_resendSmsLink = makeLinkButton(QStringLiteral("Send the text again"), page);
    m_resendSmsLink->setVisible(false);
    layout->addWidget(m_resendSmsLink);

    // Filled per account by selectMfaMethod(): one link per other method.
    m_otherMethods = new QWidget(page);
    auto *others = new QVBoxLayout(m_otherMethods);
    others->setContentsMargins(0, 4, 0, 0);
    others->setSpacing(0);
    layout->addWidget(m_otherMethods);

    auto *qrLink = makeLinkButton(QStringLiteral("Sign in with a QR code instead"), page);
    layout->addWidget(qrLink);

    auto *backLink = makeLinkButton(QStringLiteral("Back to sign in"), page);
    layout->addWidget(backLink);

    connect(m_verifyButton, &QPushButton::clicked, this, &LoginDialog::submitSecondFactor);
    connect(m_codeEdit, &QLineEdit::returnPressed, this, &LoginDialog::submitSecondFactor);
    connect(m_resendSmsLink, &QPushButton::clicked, this, [this]() {
        setBusy(true);
        setStatus(QStringLiteral("Asking Discord to send a text..."));
        m_auth.requestSmsCode(m_mfa.ticket);
    });
    connect(qrLink, &QPushButton::clicked, this, &LoginDialog::useQrInstead);
    connect(backLink, &QPushButton::clicked, this, [this]() {
        m_codeEdit->clear();
        setStatus(QString());
        showPage(CredentialsPage);
    });

    return page;
}

// Discord checking a device it has not seen, instead of asking for a second
// factor. Same page for both kinds; only the words and the box change.
QWidget *LoginDialog::buildDevicePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(makeHeading(QStringLiteral("Approve this device"), page));

    m_deviceHint = makeHint(QString(), page);
    layout->addWidget(m_deviceHint);

    m_deviceEdit = new QLineEdit(page);
    layout->addWidget(m_deviceEdit);

    m_deviceButton = makePrimaryButton(QStringLiteral("Verify"), page);
    layout->addWidget(m_deviceButton);

    m_deviceResendLink = makeLinkButton(QStringLiteral("Send the text again"), page);
    layout->addWidget(m_deviceResendLink);

    auto *qrLink = makeLinkButton(QStringLiteral("Sign in with a QR code instead"), page);
    layout->addWidget(qrLink);

    auto *backLink = makeLinkButton(QStringLiteral("Back to sign in"), page);
    layout->addWidget(backLink);

    connect(m_deviceButton, &QPushButton::clicked, this, &LoginDialog::submitDeviceCheck);
    connect(m_deviceEdit, &QLineEdit::returnPressed, this, &LoginDialog::submitDeviceCheck);
    connect(m_deviceResendLink, &QPushButton::clicked, this, [this]() {
        setBusy(true);
        setStatus(QStringLiteral("Asking Discord to send a text..."));
        m_auth.resendPhoneCode(m_loginEdit->text());
    });
    connect(qrLink, &QPushButton::clicked, this, &LoginDialog::useQrInstead);
    connect(backLink, &QPushButton::clicked, this, [this]() {
        m_deviceEdit->clear();
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
        QStringLiteral("Use this when Discord asks for a captcha. Singularity cannot answer a captcha, "
                       "so the password path stops there."),
        page));

    m_tokenEdit = new QLineEdit(page);
    m_tokenEdit->setPlaceholderText(QStringLiteral("Paste your account token"));
    m_tokenEdit->setEchoMode(QLineEdit::Password);
    m_tokenEdit->setText(AppConfig::instance().token());
    layout->addWidget(m_tokenEdit);

    m_tokenButton = makePrimaryButton(QStringLiteral("Connect"), page);
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

QWidget *LoginDialog::buildQrPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    layout->addWidget(makeHeading(QStringLiteral("Scan to sign in"), page));

    // The code itself. White quiet zone kept, on a small white plate, because a
    // camera needs the light border and a dark QR on the dark hole would not
    // read. Placed on its own so it sits centred whatever the code's size.
    m_qrImage = new QLabel(page);
    m_qrImage->setAlignment(Qt::AlignCenter);
    m_qrImage->setFixedSize(240, 240);
    m_qrImage->setStyleSheet(QStringLiteral("background: white; border-radius: 10px;"));
    m_qrImage->setText(QStringLiteral("..."));
    auto *imageRow = new QHBoxLayout;
    imageRow->addStretch(1);
    imageRow->addWidget(m_qrImage);
    imageRow->addStretch(1);
    layout->addLayout(imageRow);

    m_qrStatus = makeHint(QStringLiteral("Getting a code..."), page);
    layout->addWidget(m_qrStatus);

    auto *backLink = makeLinkButton(QStringLiteral("Back to sign in"), page);
    layout->addWidget(backLink);
    connect(backLink, &QPushButton::clicked, this, [this]() {
        m_remote.stop();
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
    // Leaving the QR page tears the connection down, so a code is never left
    // live behind a screen nobody is looking at.
    if (m_pages->currentIndex() == QrPage && page != QrPage)
        m_remote.stop();

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
    case QrPage:
        break;
    case DevicePage:
        m_deviceButton->setDefault(true);
        m_deviceEdit->setFocus();
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
    m_resendSmsLink->setEnabled(!busy);
    m_otherMethods->setEnabled(!busy);
    m_deviceEdit->setEnabled(!busy);
    m_deviceButton->setEnabled(!busy);
    m_deviceResendLink->setEnabled(!busy);
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

    // Tell the two kinds of code apart by their shape, so the box takes
    // whichever one the person has. An authenticator code is six digits; a
    // backup code is eight letters and digits, shown by Discord as xxxx-xxxx.
    QString compact = code;
    compact.remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
    static const QRegularExpression sixDigits(QStringLiteral("^\\d{6}$"));
    static const QRegularExpression backupShape(QStringLiteral("^[A-Za-z0-9]{8}$"));

    QString method = m_mfaMethod;
    if (method == QLatin1String("totp") && m_mfa.has(QStringLiteral("backup"))
        && backupShape.match(compact).hasMatch())
        method = QStringLiteral("backup");
    else if (method == QLatin1String("backup") && m_mfa.has(QStringLiteral("totp"))
             && sixDigits.match(compact).hasMatch())
        method = QStringLiteral("totp");

    setBusy(true);
    setStatus(QStringLiteral("Checking the code..."));
    m_auth.submitCode(method, m_mfa, compact);
}

void LoginDialog::selectMfaMethod(const QString &method)
{
    m_mfaMethod = method;
    m_codeEdit->clear();

    if (method == QLatin1String("totp")) {
        m_mfaHeading->setText(QStringLiteral("Enter your authenticator code"));
        m_codeHint->setText(m_mfa.has(QStringLiteral("backup"))
                                ? QStringLiteral("Type the 6-digit code from your authenticator app. "
                                                 "A backup code works in this box too.")
                                : QStringLiteral("Type the 6-digit code from your authenticator app."));
        m_codeEdit->setPlaceholderText(QStringLiteral("6-digit code"));
    } else if (method == QLatin1String("backup")) {
        m_mfaHeading->setText(QStringLiteral("Enter a backup code"));
        m_codeHint->setText(QStringLiteral("One of the 8-character codes Discord gave you when you set up "
                                           "two-step. Each one works once."));
        m_codeEdit->setPlaceholderText(QStringLiteral("xxxx-xxxx"));
    } else if (method == QLatin1String("sms")) {
        m_mfaHeading->setText(QStringLiteral("Enter the code we texted you"));
        m_codeHint->setText(QStringLiteral("Discord sends a 6-digit code to the phone on this account."));
        m_codeEdit->setPlaceholderText(QStringLiteral("6-digit code"));
    }

    // A text has to be asked for; the other codes already exist. Send it the
    // moment the method is chosen rather than making someone find a button.
    const bool sms = method == QLatin1String("sms");
    m_resendSmsLink->setVisible(sms);
    if (sms && !m_smsSent) {
        m_smsSent = true;
        setBusy(true);
        setStatus(QStringLiteral("Asking Discord to send a text..."));
        m_auth.requestSmsCode(m_mfa.ticket);
    }

    // The other ways in, as links, the way Discord's "verify with something
    // else" offers them. Rebuilt each time so the current one never appears.
    auto *layout = static_cast<QVBoxLayout *>(m_otherMethods->layout());
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const QString &other : m_mfa.methods) {
        if (other == method || other == QLatin1String("webauthn"))
            continue;
        QString label;
        if (other == QLatin1String("totp"))
            label = QStringLiteral("Use my authenticator app instead");
        else if (other == QLatin1String("backup"))
            label = QStringLiteral("Use a backup code instead");
        else if (other == QLatin1String("sms"))
            label = QStringLiteral("Text me a code instead");
        if (label.isEmpty())
            continue;
        auto *link = makeLinkButton(label, m_otherMethods);
        layout->addWidget(link);
        connect(link, &QPushButton::clicked, this, [this, other]() {
            setStatus(QString());
            selectMfaMethod(other);
            m_codeEdit->setFocus();
        });
    }

    adjustSize();
}

void LoginDialog::onMfaRequired(const AuthClient::MfaOptions &options)
{
    m_mfa = options;
    m_smsSent = false;
    setBusy(false);

    // Discord shows the first method in its list. A security key is first
    // when the account has one, and that needs the browser's WebAuthn, which
    // this window does not have - so the next method stands in, and an
    // account with only a security key is sent to the QR code, which the
    // phone can approve instead.
    QString first;
    for (const QString &method : options.methods) {
        if (method != QLatin1String("webauthn")) {
            first = method;
            break;
        }
    }

    if (first.isEmpty()) {
        setStatus(options.has(QStringLiteral("webauthn"))
                      ? QStringLiteral("This account signs in with a security key or passkey. Scan the QR "
                                       "code with the Discord app on your phone instead.")
                      : QStringLiteral("Discord asked for a second step but named no method. Scan the QR "
                                       "code with the Discord app on your phone instead."));
        useQrInstead();
        return;
    }

    setStatus(QStringLiteral("Password accepted. One more step."));
    selectMfaMethod(first);
    showPage(MfaPage);
}

void LoginDialog::showDeviceCheck(bool byPhone, const QString &message)
{
    setBusy(false);
    m_deviceByPhone = byPhone;
    m_deviceEdit->clear();
    m_deviceResendLink->setVisible(byPhone);

    if (byPhone) {
        m_deviceHint->setText(QStringLiteral("Discord does not know this device yet, so it texted a code to "
                                             "%1. Type it here.")
                                  .arg(m_loginEdit->text().trimmed()));
        m_deviceEdit->setPlaceholderText(QStringLiteral("6-digit code"));
        m_deviceButton->setText(QStringLiteral("Verify"));
    } else {
        m_deviceHint->setText(QStringLiteral("Discord does not know this device yet, so it emailed you a link "
                                             "to approve it. Click the link in that email, then press Try "
                                             "again. You can also paste the link here."));
        m_deviceEdit->setPlaceholderText(QStringLiteral("Paste the link from the email (optional)"));
        m_deviceButton->setText(QStringLiteral("Try again"));
    }

    setStatus(message.isEmpty() ? QString() : message);
    showPage(DevicePage);
}

void LoginDialog::submitDeviceCheck()
{
    const QString text = m_deviceEdit->text().trimmed();

    if (m_deviceByPhone) {
        if (text.isEmpty()) {
            setStatus(QStringLiteral("Type the code from the text first."), true);
            return;
        }
        setBusy(true);
        setStatus(QStringLiteral("Checking the code..."));
        m_auth.verifyPhoneForDevice(m_loginEdit->text(), text);
        return;
    }

    // Email. Nothing pasted means the link was already clicked in a browser,
    // which approved the device there; signing in again is all that is left.
    if (text.isEmpty()) {
        submitCredentials();
        return;
    }

    // The emailed link is discord.com/authorize-ip with the token in the query,
    // or after a # on older mails. Take it from whichever, or accept a bare
    // token if that is what was pasted.
    QString token;
    const QUrl url(text);
    if (url.isValid() && !url.host().isEmpty()) {
        token = QUrlQuery(url).queryItemValue(QStringLiteral("token"));
        if (token.isEmpty())
            token = QUrlQuery(url.fragment()).queryItemValue(QStringLiteral("token"));
    } else {
        token = text;
    }

    if (token.isEmpty()) {
        setStatus(QStringLiteral("That link has no approval code in it. Copy the whole link from the email."),
                  true);
        return;
    }

    setBusy(true);
    setStatus(QStringLiteral("Approving this device..."));
    m_auth.authorizeDevice(token);
}

void LoginDialog::onCaptchaRequired(const QString &service, const QString &siteKey)
{
    Q_UNUSED(siteKey)
    setBusy(false);
    setStatus(QStringLiteral("Discord asked for a %1 captcha, which Singularity cannot answer. "
                             "The easiest way in is the QR code: go back and choose \"Sign in with a "
                             "QR code\". Or paste a token below.")
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

void LoginDialog::useQrInstead()
{
    setStatus(QString());
    m_qrImage->setText(QStringLiteral("..."));
    m_qrStatus->setText(QStringLiteral("Getting a code..."));
    showPage(QrPage);
    m_remote.start();
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
        setStatus(QStringLiteral("Signed in, but the session could not be saved, so Singularity will ask "
                                 "again next time. The log has the reason."),
                  true);
    }

    // Clear the password field before the window goes away.
    m_passwordEdit->clear();

    accept();
}
