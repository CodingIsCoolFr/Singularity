#include "ui/LoginDialog.h"

#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/PhoneCountries.h"
#include "core/RestClient.h"
#include "ui/CaptchaDialog.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
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
    setMinimumSize(940, 640);

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

    // Laid out the way Discord's own sign-in is: one card, the form on the
    // left, the QR code on the right already showing. Nobody has to find the
    // phone option, and nobody meets a token box unless they go looking.
    auto *card = new QWidget(m_aurora);
    card->setObjectName(QStringLiteral("LoginPanel"));
    card->setFixedWidth(880);

    // Symmetric: fades out at both sides so there is no edge for the eye to
    // catch, with the words in the solid middle.
    card->setStyleSheet(QStringLiteral(
        "#LoginPanel { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 rgba(5, 7, 12, 0), stop:0.08 rgba(5, 7, 12, 220), "
        "stop:0.92 rgba(5, 7, 12, 220), stop:1 rgba(5, 7, 12, 0)); }"));
    centre->addWidget(card);
    centre->addStretch(1);

    m_aurora->setOverlay(card);

    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(90, 0, 90, 0);
    layout->setSpacing(12);
    layout->addStretch(1);

    // The program's own name, small, over the card - this is Singularity, not
    // a copy of Discord's page, even where the layout follows it.
    auto *brand = new QLabel(QStringLiteral("Singularity"), card);
    brand->setAlignment(Qt::AlignCenter);
    brand->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: 600; letter-spacing: 0.6px; color: %1;")
                             .arg(QLatin1String(Theme::TextPrimary)));
    layout->addWidget(brand);
    layout->addSpacing(10);

    m_pages = new QStackedWidget(card);
    m_pages->addWidget(buildCredentialsPage());
    m_pages->addWidget(buildMfaPage());
    m_pages->addWidget(buildTokenPage());
    m_pages->addWidget(buildDevicePage());
    layout->addWidget(m_pages);

    m_statusLabel = new QLabel(card);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(m_statusLabel);

    layout->addSpacing(10);

    // Still said, because it is true, but as a footnote rather than the first
    // thing on the screen.
    auto *warning = new QLabel(
        QStringLiteral("Discord does not allow third-party clients on a normal account, and using one can get "
                       "an account banned. Your password goes only to discord.com and is never saved."),
        card);
    warning->setWordWrap(true);
    warning->setAlignment(Qt::AlignCenter);
    warning->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                               .arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(warning);

    // Balances the stretch above, so the card sits in the middle of the window.
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

    connect(&m_auth, &AuthClient::passwordResetSent, this, [this]() {
        setBusy(false);
        setStatus(QStringLiteral("Discord emailed you a link to reset your password."));
    });

    // QR sign-in, always on beside the form. No password crosses this window
    // at all; the phone approves.
    connect(&m_remote, &RemoteAuth::qrCodeReady, this, [this](const QImage &code, const QString &) {
        m_qrImage->setPixmap(QPixmap::fromImage(code).scaled(176, 176, Qt::KeepAspectRatio,
                                                             Qt::FastTransformation));
        m_qrTitle->setText(QStringLiteral("Log in with QR code"));
        m_qrStatus->setText(QStringLiteral("Scan this with the Discord mobile app to log in instantly."));
    });
    connect(&m_remote, &RemoteAuth::scanned, this,
            [this](const QString &userId, const QString &avatarHash, const QString &username) {
                // What Discord's page does: the code gives way to the face of
                // the account that scanned it.
                m_scannedAvatar = MediaCache::avatarUrl(userId, avatarHash, 128);
                const QImage face = m_scannedAvatar.isEmpty() ? QImage() : MediaCache::instance().image(m_scannedAvatar);
                m_qrImage->setPixmap(face.isNull() ? MediaCache::initialsAvatar(username, 120)
                                                   : MediaCache::circular(face, 120));
                m_qrTitle->setText(QStringLiteral("Check your phone!"));
                m_qrStatus->setText(QStringLiteral("Logging in as %1").arg(username));
            });
    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &url) {
        if (url.isEmpty() || url != m_scannedAvatar)
            return;
        const QImage face = MediaCache::instance().image(url);
        if (!face.isNull())
            m_qrImage->setPixmap(MediaCache::circular(face, 120));
    });
    connect(&m_remote, &RemoteAuth::succeeded, this, [this](const QString &token) {
        m_qrStatus->setText(QStringLiteral("Approved. Loading..."));
        m_rest->setToken(token);
        finishWith(token);
    });
    connect(&m_remote, &RemoteAuth::declined, this, [this]() {
        m_scannedAvatar.clear();
        m_qrTitle->setText(QStringLiteral("Log in with QR code"));
        m_qrStatus->setText(QStringLiteral("Declined on the phone. Here is a fresh code."));
        if (m_pages->currentIndex() == CredentialsPage)
            m_remote.start();
    });
    // RemoteAuth fetches the fresh code itself; this only puts the screen
    // back to "scan me" while it arrives.
    connect(&m_remote, &RemoteAuth::expired, this, [this]() {
        m_scannedAvatar.clear();
        m_qrTitle->setText(QStringLiteral("Log in with QR code"));
        m_qrStatus->setText(QStringLiteral("Getting a fresh code..."));
    });
    connect(&m_remote, &RemoteAuth::failed, this, [this](const QString &reason) {
        m_qrStatus->setText(reason);
    });

    showPage(CredentialsPage);
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

using PhoneCountries::looksLikePhone;

static const PhoneCountries::Country *countryFor(const QString &alpha2)
{
    return PhoneCountries::find(alpha2);
}

void LoginDialog::refreshCountryButton()
{
    if (const PhoneCountries::Country *country = countryFor(m_countryAlpha2))
        m_countryButton->setText(QStringLiteral("%1 %2").arg(m_countryAlpha2, QLatin1String(country->code)));
}

QString LoginDialog::loginText() const
{
    return PhoneCountries::toLogin(m_loginEdit->text(), m_countryAlpha2);
}

// A small upper-case caption over a field, the way Discord labels its form.
static QLabel *makeFieldLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700; letter-spacing: 0.4px;")
                             .arg(QLatin1String(Theme::TextMuted)));
    return label;
}

QWidget *LoginDialog::buildCredentialsPage()
{
    auto *page = new QWidget(this);
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(40);

    // Left: the form.
    auto *form = new QVBoxLayout;
    form->setSpacing(6);

    auto *welcome = new QLabel(QStringLiteral("Welcome back!"), page);
    welcome->setAlignment(Qt::AlignCenter);
    welcome->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 600; color: %1;")
                               .arg(QLatin1String(Theme::TextPrimary)));
    form->addWidget(welcome);
    form->addWidget(makeHint(QStringLiteral("We're so excited to see you again!"), page));
    form->addSpacing(14);

    form->addWidget(makeFieldLabel(QStringLiteral("EMAIL OR PHONE NUMBER *"), page));

    // The country code, shown in front of the box as soon as what is typed
    // looks like a phone number - as Discord's form does. Discord only knows
    // a number in full international form.
    auto *loginRow = new QHBoxLayout;
    loginRow->setSpacing(6);
    m_countryButton = new QPushButton(page);
    m_countryButton->setCursor(Qt::PointingHandCursor);
    m_countryButton->setMinimumHeight(40);
    m_countryButton->setToolTip(QStringLiteral("Country code for your phone number"));
    m_countryButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: none; border-radius: 8px; padding: 0 10px; "
        "font-weight: 600; } QPushButton:hover { background: %3; }")
                                       .arg(QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::TextPrimary),
                                            QLatin1String(Theme::SurfaceHover)));
    m_countryButton->setVisible(false);
    loginRow->addWidget(m_countryButton);

    m_loginEdit = new QLineEdit(page);
    m_loginEdit->setObjectName(QStringLiteral("LoginEdit"));
    m_loginEdit->setMinimumHeight(40);
    loginRow->addWidget(m_loginEdit, 1);
    form->addLayout(loginRow);
    form->addSpacing(10);

    // Where the person is, from Windows, unless they picked one before.
    m_countryAlpha2 = AppConfig::instance().value(QStringLiteral("login/phoneCountry")).toString();
    if (m_countryAlpha2.isEmpty())
        m_countryAlpha2 = QLocale::territoryToCode(QLocale::system().territory());
    if (!countryFor(m_countryAlpha2))
        m_countryAlpha2 = QStringLiteral("US");
    refreshCountryButton();

    connect(m_loginEdit, &QLineEdit::textChanged, this,
            [this](const QString &text) { m_countryButton->setVisible(looksLikePhone(text) && !text.trimmed().startsWith(QLatin1Char('+'))); });
    connect(m_countryButton, &QPushButton::clicked, this, [this]() {
        QMenu menu(this);
        menu.setStyleSheet(QStringLiteral("QMenu { menu-scrollable: 1; }"));
        for (const PhoneCountries::Country &country : PhoneCountries::All) {
            QAction *action = menu.addAction(QStringLiteral("%1   %2")
                                                 .arg(QString::fromUtf8(country.name), QLatin1String(country.code)));
            action->setData(QLatin1String(country.alpha2));
            action->setCheckable(true);
            action->setChecked(m_countryAlpha2 == QLatin1String(country.alpha2));
        }
        if (QAction *picked = menu.exec(m_countryButton->mapToGlobal(QPoint(0, m_countryButton->height())))) {
            m_countryAlpha2 = picked->data().toString();
            AppConfig::instance().setValue(QStringLiteral("login/phoneCountry"), m_countryAlpha2);
            refreshCountryButton();
        }
    });

    form->addWidget(makeFieldLabel(QStringLiteral("PASSWORD *"), page));
    m_passwordEdit = new QLineEdit(page);
    m_passwordEdit->setMinimumHeight(40);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    form->addWidget(m_passwordEdit);

    auto *forgotLink = makeLinkButton(QStringLiteral("Forgot your password?"), page);
    form->addWidget(forgotLink);
    form->addSpacing(8);

    m_logInButton = makePrimaryButton(QStringLiteral("Log In"), page);
    m_logInButton->setDefault(true);
    form->addWidget(m_logInButton);

    m_rememberBox = new QCheckBox(QStringLiteral("Stay signed in on this computer"), page);
    m_rememberBox->setChecked(true);
    form->addWidget(m_rememberBox);

    auto *tokenLink = makeLinkButton(QStringLiteral("Sign in with a token"), page);
    form->addWidget(tokenLink);

    row->addLayout(form, 1);

    // A thin rule between the two halves.
    auto *divider = new QFrame(page);
    divider->setFixedWidth(1);
    divider->setStyleSheet(QStringLiteral("background: %1;").arg(QLatin1String(Theme::Border)));
    row->addWidget(divider);

    // Right: the QR code, already running.
    auto *qr = new QVBoxLayout;
    qr->setSpacing(10);
    qr->addStretch(1);

    m_qrImage = new QLabel(page);
    m_qrImage->setAlignment(Qt::AlignCenter);
    m_qrImage->setFixedSize(192, 192);
    // White behind the code because a camera needs a light border around a
    // dark code; when the code gives way to an avatar the plate stays, which
    // is how Discord's page looks too.
    m_qrImage->setStyleSheet(QStringLiteral("background: white; border-radius: 8px; color: #555;"));
    m_qrImage->setText(QStringLiteral("..."));
    auto *imageRow = new QHBoxLayout;
    imageRow->addStretch(1);
    imageRow->addWidget(m_qrImage);
    imageRow->addStretch(1);
    qr->addLayout(imageRow);

    m_qrTitle = new QLabel(QStringLiteral("Log in with QR code"), page);
    m_qrTitle->setAlignment(Qt::AlignCenter);
    m_qrTitle->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: 600; color: %1;")
                                 .arg(QLatin1String(Theme::TextPrimary)));
    qr->addWidget(m_qrTitle);

    m_qrStatus = makeHint(QStringLiteral("Getting a code..."), page);
    qr->addWidget(m_qrStatus);
    qr->addStretch(1);

    auto *qrColumn = new QWidget(page);
    qrColumn->setFixedWidth(250);
    qrColumn->setLayout(qr);
    row->addWidget(qrColumn);

    connect(m_logInButton, &QPushButton::clicked, this, &LoginDialog::submitCredentials);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &LoginDialog::submitCredentials);
    connect(m_loginEdit, &QLineEdit::returnPressed, this, &LoginDialog::submitCredentials);
    connect(tokenLink, &QPushButton::clicked, this, &LoginDialog::useTokenInstead);
    connect(forgotLink, &QPushButton::clicked, this, [this]() {
        setBusy(true);
        setStatus(QStringLiteral("Asking Discord for a reset email..."));
        m_auth.forgotPassword(loginText());
    });

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
    // Centred at a comfortable reading width inside the wide card.
    layout->setContentsMargins(150, 0, 150, 0);
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
    // Centred at a comfortable reading width inside the wide card.
    layout->setContentsMargins(150, 0, 150, 0);
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
        m_auth.resendPhoneCode(loginText());
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
    // Centred at a comfortable reading width inside the wide card.
    layout->setContentsMargins(150, 0, 150, 0);
    layout->setSpacing(8);

    layout->addWidget(makeHeading(QStringLiteral("Sign in with a token"), page));
    layout->addWidget(makeHint(
        QStringLiteral("For people who already have their account token. Most people want the email "
                       "and password, or the QR code."),
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

// ---------------------------------------------------------------------------
// Flow
// ---------------------------------------------------------------------------

void LoginDialog::showPage(Page page)
{
    const bool wasFront = m_pages->currentIndex() == CredentialsPage;
    m_pages->setCurrentIndex(page);

    // The QR code lives on the front page and runs whenever it is showing,
    // as Discord's does. Leaving the page hangs up, so a code is never left
    // live behind a screen nobody is looking at.
    if (page == CredentialsPage) {
        if (!wasFront || !m_remoteStarted) {
            m_scannedAvatar.clear();
            m_qrImage->setText(QStringLiteral("..."));
            m_qrTitle->setText(QStringLiteral("Log in with QR code"));
            m_qrStatus->setText(QStringLiteral("Getting a code..."));
            m_remote.start();
            m_remoteStarted = true;
        }
    } else if (m_remoteStarted) {
        m_remote.stop();
        m_remoteStarted = false;
    }

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
    // Red for a problem, as Discord does. It used the theme's accent colour,
    // which with a grey theme is grey - an error that looked like a hint.
    m_statusLabel->setStyleSheet(isError ? QStringLiteral("color: %1; font-weight: 600;").arg(QLatin1String(Theme::Red))
                                         : QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextFaint)));
}

bool LoginDialog::shouldRemember() const
{
    return m_rememberBox->isChecked();
}

void LoginDialog::submitCredentials()
{
    setBusy(true);
    setStatus(QStringLiteral("Signing in..."));
    const QString login = loginText();
    // Logged so a "wrong login" can be told apart from a wrong password: the
    // shape only, never the number or address itself.
    wlog(QStringLiteral("login"), looksLikePhone(login)
                                      ? QStringLiteral("signing in with a phone number (%1 digits, international form)")
                                            .arg(login.size() - 1)
                                      : QStringLiteral("signing in with an email address"));
    m_auth.logIn(login, m_passwordEdit->text());
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
                                  .arg(loginText()));
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
        m_auth.verifyPhoneForDevice(loginText(), text);
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

// Discord wants a captcha before it will let this sign-in through.
//
// What Discord's own client does for any request: show the captcha, then send
// the same request again with the answer attached. The captcha is Discord's
// real hCaptcha, in a small browser window, and the person solves it - this
// program never answers one by itself.
void LoginDialog::onCaptchaRequired(const QString &service, const QString &siteKey, const QString &rqdata)
{
    setStatus(QStringLiteral("Discord wants a quick check that you are a person."));

    const QString answer = CaptchaDialog::solve(this, siteKey, rqdata);
    if (answer.isEmpty()) {
        setBusy(false);
        setStatus(QStringLiteral("The %1 check was closed before it finished. Press Log In to try again, "
                                 "or scan the QR code instead.")
                      .arg(service),
                  true);
        return;
    }

    setStatus(QStringLiteral("Thanks. Signing in..."));
    m_auth.retryWithCaptcha(answer);
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
    // The code is on the front page, running whenever that page shows.
    setStatus(QStringLiteral("Scan the code on the right with the Discord mobile app."));
    showPage(CredentialsPage);
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
