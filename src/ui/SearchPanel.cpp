#include "ui/SearchPanel.h"

#include "core/MessageStore.h"
#include "core/RestClient.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QFocusEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonValue>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace {

// What the search box's list rows carry.
constexpr int KindRole = Qt::UserRole + 1;    // "option", "person", "channel" or "value"
constexpr int InsertRole = Qt::UserRole + 2;  // what is written into the box
constexpr int IdRole = Qt::UserRole + 3;      // the person or channel picked
constexpr int PickRole = Qt::UserRole + 4;    // "from:alex", the key SearchBox::picked uses

// What a result row carries. The panel works it all out; the painter only draws.
constexpr int ChannelRole = Qt::UserRole + 1;
constexpr int MessageRole = Qt::UserRole + 2;
constexpr int AuthorRole = Qt::UserRole + 3;
constexpr int AvatarRole = Qt::UserRole + 4;   // the picture's address
constexpr int ColourRole = Qt::UserRole + 5;   // the name's role colour, 0 for none
constexpr int TimeRole = Qt::UserRole + 6;
constexpr int PlaceRole = Qt::UserRole + 7;    // "# general" over the first of a run from one channel
constexpr int HtmlRole = Qt::UserRole + 8;     // the words, with the hits marked
constexpr int ExtraRole = Qt::UserRole + 9;    // files and stickers, one line

// A result's measurements.
constexpr int Outer = 8;         // card to the panel's edge
constexpr int Gap = 4;           // between cards
constexpr int Pad = 10;          // inside a card
constexpr int PlaceHeight = 22;
constexpr int Face = 32;
constexpr int FaceGap = 10;
constexpr int NameHeight = 20;
constexpr int ExtraHeight = 18;

QFont uiFont(int pixels, int weight = QFont::Normal)
{
    QFont font;
    font.setFamilies({QStringLiteral("Poppins"), QStringLiteral("Segoe UI")});
    font.setPixelSize(pixels);
    font.setWeight(QFont::Weight(weight));
    return font;
}

// A hit is marked in the accent, mixed into the panel so the words stay
// readable on any seed colour.
QColor hitColour()
{
    const QColor accent{QLatin1String(Theme::Accent)};
    const QColor ground{QLatin1String(Theme::SurfaceChat)};
    const qreal mix = 0.45;
    return QColor::fromRgbF(accent.redF() * mix + ground.redF() * (1 - mix),
                            accent.greenF() * mix + ground.greenF() * (1 - mix),
                            accent.blueF() * mix + ground.blueF() * (1 - mix));
}

// A message's words as the conversation sets them, so a result reads the
// same as the message it leads to.
void fillDocument(QTextDocument &document, const QString &html, int width)
{
    QFont font;
    font.setFamilies({QStringLiteral("Lora"), QStringLiteral("Georgia"), QStringLiteral("Segoe UI Emoji")});
    font.setPixelSize(14);
    document.setDefaultFont(font);
    document.setDocumentMargin(0);
    document.setDefaultStyleSheet(QStringLiteral(".hit { background-color: %1; color: %2; font-weight: 600; }")
                                      .arg(hitColour().name(), QLatin1String(Theme::TextPrimary)));
    document.setHtml(html);
    document.setTextWidth(qMax(40, width));
}

// Discord's clock in a message header: today, yesterday, or the date.
QString whenText(const QDateTime &when)
{
    const QDate day = when.date();
    const QDate today = QDate::currentDate();
    const QString clock = QLocale().toString(when, QStringLiteral("h:mm AP"));
    if (day == today)
        return QStringLiteral("Today at %1").arg(clock);
    if (day == today.addDays(-1))
        return QStringLiteral("Yesterday at %1").arg(clock);
    return QLocale().toString(when, QStringLiteral("M/d/yy, h:mm AP"));
}

// A <t:seconds:style> in a message, written the way the conversation writes
// it. R, "3 days ago", is given as the date: a result is read later, when
// "ago" has moved on.
QString timestampText(qint64 seconds, const QString &style)
{
    const QDateTime when = QDateTime::fromSecsSinceEpoch(seconds).toLocalTime();
    const QLocale locale;
    if (style == QLatin1String("t"))
        return locale.toString(when, QStringLiteral("h:mm AP"));
    if (style == QLatin1String("T"))
        return locale.toString(when, QStringLiteral("h:mm:ss AP"));
    if (style == QLatin1String("d"))
        return locale.toString(when, QStringLiteral("M/d/yyyy"));
    if (style == QLatin1String("D"))
        return locale.toString(when, QStringLiteral("MMMM d, yyyy"));
    if (style == QLatin1String("F"))
        return locale.toString(when, QStringLiteral("dddd, MMMM d, yyyy h:mm AP"));
    return locale.toString(when, QStringLiteral("MMMM d, yyyy h:mm AP"));
}

// Draws one result as a card: who, when and where above, the words with the
// hits marked, and a line for files. A row under the pointer is lit, the way
// Discord lights the result it would jump to.
class ResultDelegate : public QStyledItemDelegate
{
public:
    explicit ResultDelegate(QListWidget *list)
        : QStyledItemDelegate(list)
        , m_list(list)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

        QRect row = option.rect;
        const QString place = index.data(PlaceRole).toString();
        if (!place.isEmpty()) {
            const QRect label(row.left() + Outer + 4, row.top() + 4, row.width() - 2 * Outer - 8, PlaceHeight - 4);
            painter->setFont(uiFont(12, QFont::DemiBold));
            painter->setPen(QColor(QLatin1String(Theme::TextMuted)));
            painter->drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(painter->font()).elidedText(place, Qt::ElideRight, label.width()));
            row.setTop(row.top() + PlaceHeight);
        }

        const QRect card = row.adjusted(Outer, Gap / 2, -Outer, -Gap / 2);
        const bool lit = option.state & QStyle::State_MouseOver;
        // A card a shade off the panel, and lighter under the pointer.
        QColor fill{QLatin1String(lit ? Theme::SurfaceHover : Theme::SurfaceInput)};
        fill.setAlpha(lit ? 240 : 170);
        QPainterPath shape;
        shape.addRoundedRect(QRectF(card), 8, 8);
        painter->fillPath(shape, fill);

        // The face.
        const QString author = index.data(AuthorRole).toString();
        const QRect face(card.left() + Pad, card.top() + Pad, Face, Face);
        const QUrl address(index.data(AvatarRole).toString());
        const QImage picture = address.isEmpty() ? QImage() : MediaCache::instance().image(address);
        painter->drawPixmap(face, picture.isNull() ? MediaCache::initialsAvatar(author, Face)
                                                   : MediaCache::circular(picture, Face));

        // Name, then the time after it.
        const int left = face.right() + 1 + FaceGap;
        const int width = card.right() - Pad - left;
        const quint32 colour = index.data(ColourRole).toUInt();
        const QFont nameFont = uiFont(13, QFont::DemiBold);
        const QFont timeFont = uiFont(11);
        const QString when = index.data(TimeRole).toString();
        const int timeWidth = QFontMetrics(timeFont).horizontalAdvance(when);
        const QString name = QFontMetrics(nameFont).elidedText(author, Qt::ElideRight, qMax(20, width - timeWidth - 8));
        painter->setFont(nameFont);
        painter->setPen(colour ? QColor::fromRgb(colour) : QColor(QLatin1String(Theme::TextPrimary)));
        const QRect nameBox(left, card.top() + Pad - 2, width, NameHeight);
        painter->drawText(nameBox, Qt::AlignLeft | Qt::AlignVCenter, name);
        painter->setFont(timeFont);
        painter->setPen(QColor(QLatin1String(Theme::TextFaint)));
        painter->drawText(nameBox.adjusted(QFontMetrics(nameFont).horizontalAdvance(name) + 8, 0, 0, 0),
                          Qt::AlignLeft | Qt::AlignVCenter, when);

        // The words.
        QTextDocument document;
        fillDocument(document, index.data(HtmlRole).toString(), width);
        const int textTop = card.top() + Pad + NameHeight;
        painter->translate(left, textTop);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, QColor(QLatin1String(Theme::TextPrimary)));
        context.clip = QRectF(0, 0, width, document.size().height());
        document.documentLayout()->draw(painter, context);
        painter->translate(-left, -textTop);

        const QString extra = index.data(ExtraRole).toString();
        if (!extra.isEmpty()) {
            const int top = textTop + int(std::ceil(document.size().height()));
            painter->setFont(uiFont(12));
            painter->setPen(QColor(QLatin1String(Theme::TextMuted)));
            painter->drawText(QRect(left, top, width, ExtraHeight), Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(painter->font()).elidedText(extra, Qt::ElideRight, width));
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        const int rowWidth = m_list->viewport()->width();
        const int width = rowWidth - 2 * Outer - 2 * Pad - Face - FaceGap;
        QTextDocument document;
        fillDocument(document, index.data(HtmlRole).toString(), width);

        int body = NameHeight + int(std::ceil(document.size().height()));
        if (!index.data(ExtraRole).toString().isEmpty())
            body += ExtraHeight;
        int height = Gap + 2 * Pad + qMax(Face, body);
        if (!index.data(PlaceRole).toString().isEmpty())
            height += PlaceHeight;
        return QSize(rowWidth, height);
    }

private:
    QListWidget *m_list = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------
// The box
// ---------------------------------------------------------------------------

SearchBox::SearchBox(MessageStore *store, RestClient *rest, QWidget *parent)
    : QLineEdit(parent)
    , m_store(store)
    , m_rest(rest)
{
    setObjectName(QStringLiteral("SearchBox"));
    setPlaceholderText(QStringLiteral("Search"));
    setClearButtonEnabled(true);
    setFixedWidth(240);

    m_peopleTimer.setSingleShot(true);
    m_peopleTimer.setInterval(300);
    connect(&m_peopleTimer, &QTimer::timeout, this, &SearchBox::askDiscordForPeople);

    connect(this, &QLineEdit::textChanged, this, [this](const QString &text) {
        // Emptied, by the clear mark or by hand: the search is over.
        if (text.trimmed().isEmpty()) {
            m_picked.clear();
            emit cleared();
        }
        if (hasFocus())
            updateSuggestions();
    });
    connect(this, &QLineEdit::cursorPositionChanged, this, [this]() {
        if (hasFocus() && m_popup && m_popup->isVisible())
            updateSuggestions();
    });
}

void SearchBox::setScope(const QString &guildId, const QString &channelId)
{
    if (guildId == m_guildId && channelId == m_channelId)
        return;
    m_guildId = guildId;
    m_channelId = channelId;
    m_picked.clear();
    m_peopleAsked.clear();
    hideSuggestions();

    // "Search Server Name", as Discord words it; a chat is just "Search".
    const QString place = guildId.isEmpty() ? QString() : m_store->guild(guildId).name;
    setPlaceholderText(place.isEmpty()
                           ? QStringLiteral("Search")
                           : fontMetrics().elidedText(QStringLiteral("Search ") + place, Qt::ElideRight, width() - 48));
}

SearchBox::Word SearchBox::wordAtCursor() const
{
    const QString line = text();
    const int cursor = cursorPosition();
    int start = cursor;
    while (start > 0 && !line.at(start - 1).isSpace())
        --start;
    int end = cursor;
    while (end < line.size() && !line.at(end).isSpace())
        ++end;

    const auto filterOf = [](const QString &token, QString *key, QString *value) {
        const int colon = token.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            return false;
        QString name = token.left(colon).toLower();
        if (name == QLatin1String("on"))
            name = QStringLiteral("during");
        if (!SearchQuery::filterKeys().contains(name))
            return false;
        *key = name;
        *value = token.mid(colon + 1);
        return true;
    };

    Word word;
    word.start = start;
    word.end = end;
    const QString here = line.mid(start, end - start);
    QString key;
    QString value;
    if (filterOf(here, &key, &value)) {
        word.isFilter = true;
        word.key = key;
        word.value = value;
        return word;
    }

    // "from: al": the filter is the word before, written with a space, as
    // Discord's box shows a chosen filter.
    int previousEnd = start;
    while (previousEnd > 0 && line.at(previousEnd - 1).isSpace())
        --previousEnd;
    if (previousEnd < start) {
        int previousStart = previousEnd;
        while (previousStart > 0 && !line.at(previousStart - 1).isSpace())
            --previousStart;
        if (filterOf(line.mid(previousStart, previousEnd - previousStart), &key, &value) && value.isEmpty()) {
            word.isFilter = true;
            word.key = key;
            word.value = here;
            word.start = previousStart;
            return word;
        }
    }

    word.text = here;
    return word;
}

QStringList SearchBox::peopleHere(const QString &text) const
{
    QStringList ids;
    if (m_guildId.isEmpty()) {
        // A direct or group chat: the people in it, and you.
        QStringList members = m_store->channel(m_channelId).recipientIds;
        if (!m_store->selfUserId().isEmpty())
            members.append(m_store->selfUserId());
        for (const QString &id : std::as_const(members)) {
            const UserInfo info = m_store->user(id);
            if (text.isEmpty() || info.username.contains(text, Qt::CaseInsensitive)
                || info.globalName.contains(text, Qt::CaseInsensitive))
                ids.append(id);
        }
        return ids;
    }
    for (const UserInfo &info : m_store->findUsers(text, m_guildId, 8))
        ids.append(info.id);
    return ids;
}

void SearchBox::updateSuggestions()
{
    if (!hasFocus()) {
        hideSuggestions();
        return;
    }
    // Only a name still being typed is put to Discord.
    m_peopleTimer.stop();

    struct Row
    {
        QString kind;
        QString label;
        QString insert;
        QString id;
        QString pick;
    };
    QList<Row> rows;
    QString heading;
    const Word word = wordAtCursor();

    if (!word.isFilter) {
        // Discord's list of filters, while nothing or the start of one is
        // typed. Plain words get no list: Enter searches for them.
        const QString start = word.text.toLower();
        for (const QString &key : SearchQuery::filterKeys()) {
            if (key == QLatin1String("in") && m_guildId.isEmpty())
                continue;
            if (!start.isEmpty() && !key.startsWith(start))
                continue;
            rows.append({QStringLiteral("option"),
                         QStringLiteral("%1:  %2").arg(key, SearchQuery::filterHint(key)),
                         key + QLatin1Char(':'), {}, {}});
        }
        heading = QStringLiteral("Search options");
    } else if (word.key == QLatin1String("from") || word.key == QLatin1String("mentions")) {
        heading = word.key == QLatin1String("from") ? QStringLiteral("From user") : QStringLiteral("Mentions user");
        for (const QString &id : peopleHere(word.value)) {
            const UserInfo info = m_store->user(id);
            if (info.username.isEmpty())
                continue;
            QString shown = m_guildId.isEmpty() ? QString() : m_store->memberLook(m_guildId, id).nickname;
            if (shown.isEmpty())
                shown = info.displayName();
            const QString label = shown.compare(info.username, Qt::CaseInsensitive) == 0
                ? shown
                : QStringLiteral("%1   %2").arg(shown, info.username);
            const QString insert = word.key + QLatin1Char(':') + info.username;
            rows.append({QStringLiteral("person"), label, insert, id, insert.toLower()});
        }
        // People this client has not met yet are Discord's to find.
        if (!m_guildId.isEmpty() && !word.value.trimmed().isEmpty()
            && !m_peopleAsked.contains(word.value.trimmed().toLower())) {
            m_peopleWanted = word.value;
            m_peopleTimer.start();
        }
    } else if (word.key == QLatin1String("in")) {
        heading = QStringLiteral("In channel");
        const QString wanted = word.value;
        for (const ChannelInfo &channel : m_store->channelsOfGuild(m_guildId)) {
            if (channel.isCategory() || channel.name.isEmpty() || rows.size() >= 8)
                continue;
            if (!wanted.isEmpty() && !channel.name.contains(wanted, Qt::CaseInsensitive))
                continue;
            const QString written = channel.name.contains(QLatin1Char(' '))
                ? QStringLiteral("\"%1\"").arg(channel.name)
                : channel.name;
            rows.append({QStringLiteral("channel"), QStringLiteral("# ") + channel.name,
                         QStringLiteral("in:") + written, channel.id,
                         QStringLiteral("in:") + channel.name.toLower()});
        }
    } else if (word.key == QLatin1String("has")) {
        heading = QStringLiteral("Message contains");
        for (const QString &kind : SearchQuery::hasValues()) {
            if (kind.startsWith(word.value.toLower()))
                rows.append({QStringLiteral("value"), kind, QStringLiteral("has:") + kind, {}, {}});
        }
    } else if (word.key == QLatin1String("pinned")) {
        heading = QStringLiteral("Pinned");
        for (const QString &answer : {QStringLiteral("true"), QStringLiteral("false")}) {
            if (answer.startsWith(word.value.toLower()))
                rows.append({QStringLiteral("value"), answer, QStringLiteral("pinned:") + answer, {}, {}});
        }
    } else {
        // before:, during:, after:. A date already typed is offered as it
        // reads; otherwise the two everybody wants.
        heading = QStringLiteral("Date, like %1").arg(QDate::currentDate().toString(Qt::ISODate));
        const QDate typed = SearchQuery::readDate(word.value);
        if (typed.isValid() && !word.value.isEmpty()) {
            rows.append({QStringLiteral("value"), QLocale().toString(typed, QLocale::LongFormat),
                         word.key + QLatin1Char(':') + typed.toString(Qt::ISODate), {}, {}});
        } else {
            const QDate today = QDate::currentDate();
            const QList<QPair<QString, QDate>> days{{QStringLiteral("Today"), today},
                                                     {QStringLiteral("Yesterday"), today.addDays(-1)}};
            for (const auto &day : days) {
                if (!day.first.startsWith(word.value, Qt::CaseInsensitive))
                    continue;
                rows.append({QStringLiteral("value"),
                             QStringLiteral("%1   %2").arg(day.first, day.second.toString(Qt::ISODate)),
                             word.key + QLatin1Char(':') + day.second.toString(Qt::ISODate), {}, {}});
            }
        }
    }

    if (rows.isEmpty()) {
        hideSuggestions();
        return;
    }

    if (!m_popup) {
        m_popup = new QListWidget(window());
        m_popup->setObjectName(QStringLiteral("SearchPopup"));
        m_popup->setFocusPolicy(Qt::NoFocus);
        m_popup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_popup->setStyleSheet(QStringLiteral(
            "QListWidget#SearchPopup { background-color: %1; color: %2; border: 1px solid %3; "
            "border-radius: 10px; padding: 4px; font-family: \"Segoe UI\"; font-size: 13px; outline: none; }"
            "QListWidget#SearchPopup::item { padding: 5px 10px; border-radius: 6px; }"
            "QListWidget#SearchPopup::item:selected { background: %4; color: %2; }"
            "QListWidget#SearchPopup::item:hover { background: %4; }")
                                   .arg(QLatin1String(Theme::SurfaceSidebar), QLatin1String(Theme::TextPrimary),
                                        QLatin1String(Theme::Border), QLatin1String(Theme::SurfaceHover)));
        connect(m_popup, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { applySuggestion(item); });
    }

    m_popup->clear();
    auto *title = new QListWidgetItem(heading.toUpper(), m_popup);
    title->setFlags(Qt::NoItemFlags);
    title->setFont(uiFont(11, QFont::Bold));
    title->setForeground(QColor(QLatin1String(Theme::TextFaint)));
    for (const Row &row : std::as_const(rows)) {
        auto *item = new QListWidgetItem(row.label, m_popup);
        item->setData(KindRole, row.kind);
        item->setData(InsertRole, row.insert);
        item->setData(IdRole, row.id);
        item->setData(PickRole, row.pick);
    }

    // A filter's values: the first is chosen, so Enter takes it. The list of
    // filters: nothing is, so Enter searches for what was typed.
    if (word.isFilter)
        m_popup->setCurrentRow(1);
    else {
        m_popup->setCurrentRow(-1);
        m_popup->clearSelection();
    }

    const int rowHeight = 28;
    const int height = qMin(10, int(m_popup->count())) * rowHeight + 10;
    const int width = qMax(this->width(), 300);
    const QPoint corner = mapTo(window(), QPoint(this->width(), this->height() + 4));
    m_popup->setFixedSize(width, height);
    m_popup->move(qMax(8, corner.x() - width), corner.y());
    m_popup->raise();
    m_popup->show();
}

void SearchBox::hideSuggestions()
{
    m_peopleTimer.stop();
    if (m_popup)
        m_popup->hide();
}

void SearchBox::applySuggestion(QListWidgetItem *item)
{
    if (!item || !(item->flags() & Qt::ItemIsEnabled))
        return;

    const QString kind = item->data(KindRole).toString();
    const QString insert = item->data(InsertRole).toString();
    const Word word = wordAtCursor();
    QString line = text();

    // A filter's name is followed by its value, so it takes no space yet.
    const bool valueNext = kind == QLatin1String("option");
    const bool spaceAlready = word.end < line.size() && line.at(word.end).isSpace();
    const QString written = valueNext || spaceAlready ? insert : insert + QLatin1Char(' ');

    const QString pick = item->data(PickRole).toString();
    if (!pick.isEmpty())
        m_picked.insert(pick, item->data(IdRole).toString());

    line.replace(word.start, word.end - word.start, written);
    setText(line);
    setCursorPosition(word.start + written.size() + (spaceAlready && !valueNext ? 1 : 0));
    updateSuggestions();
}

bool SearchBox::moveSelection(int step)
{
    if (!m_popup || !m_popup->isVisible())
        return false;
    int row = m_popup->currentRow();
    if (row < 0)
        row = step > 0 ? 0 : m_popup->count();
    for (int next = row + step; next >= 0 && next < m_popup->count(); next += step) {
        if (m_popup->item(next)->flags() & Qt::ItemIsSelectable) {
            m_popup->setCurrentRow(next);
            return true;
        }
    }
    return true;
}

void SearchBox::askDiscordForPeople()
{
    const QString wanted = m_peopleWanted.trimmed();
    if (wanted.isEmpty() || m_guildId.isEmpty() || !m_rest)
        return;
    m_peopleAsked.insert(wanted.toLower());
    const QString guildId = m_guildId;
    m_rest->searchGuildMembers(
        guildId, wanted,
        [this, guildId](const QJsonArray &members) {
            for (const QJsonValue &member : members)
                m_store->rememberMember(guildId, member.toObject());
            if (guildId == m_guildId && hasFocus())
                updateSuggestions();
        },
        [](const RestClient::Error &) {});
}

void SearchBox::keyPressEvent(QKeyEvent *event)
{
    const bool listed = m_popup && m_popup->isVisible();
    switch (event->key()) {
    case Qt::Key_Down:
    case Qt::Key_Up:
        if (listed && moveSelection(event->key() == Qt::Key_Down ? 1 : -1))
            return;
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        QListWidgetItem *chosen = listed ? m_popup->currentItem() : nullptr;
        if (chosen && chosen->isSelected()) {
            applySuggestion(chosen);
            return;
        }
        hideSuggestions();
        if (!text().trimmed().isEmpty())
            emit searchRequested(text().trimmed());
        return;
    }
    case Qt::Key_Escape:
        if (listed) {
            hideSuggestions();
            return;
        }
        if (!text().isEmpty()) {
            clear();
            return;
        }
        clearFocus();
        return;
    default:
        break;
    }
    QLineEdit::keyPressEvent(event);
}

void SearchBox::focusInEvent(QFocusEvent *event)
{
    QLineEdit::focusInEvent(event);
    updateSuggestions();
}

void SearchBox::focusOutEvent(QFocusEvent *event)
{
    QLineEdit::focusOutEvent(event);
    hideSuggestions();
}

// ---------------------------------------------------------------------------
// The results
// ---------------------------------------------------------------------------

SearchPanel::SearchPanel(RestClient *rest, MessageStore *store, QWidget *parent)
    : QWidget(parent)
    , m_rest(rest)
    , m_store(store)
{
    setObjectName(QStringLiteral("SearchPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    // Wider than the member list it stands in for: a result is a message.
    setMinimumWidth(300);
    setMaximumWidth(440);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(4);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(16, 6, 10, 0);
    m_count = new QLabel(QStringLiteral("Search"), this);
    m_count->setObjectName(QStringLiteral("SearchCount"));
    auto *close = new QPushButton(QStringLiteral("Close"), this);
    close->setObjectName(QStringLiteral("TabButton"));
    close->setFlat(true);
    close->setCursor(Qt::PointingHandCursor);
    close->setToolTip(QStringLiteral("End this search"));
    connect(close, &QPushButton::clicked, this, &SearchPanel::closeRequested);
    header->addWidget(m_count, 1);
    header->addWidget(close);
    layout->addLayout(header);

    // Discord's three orders.
    auto *tabs = new QHBoxLayout;
    tabs->setContentsMargins(10, 0, 10, 2);
    tabs->setSpacing(4);
    const auto makeTab = [this, tabs](const QString &text, SearchQuery::Sort sort) {
        auto *button = new QPushButton(text, this);
        button->setObjectName(QStringLiteral("TabButton"));
        button->setFlat(true);
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::clicked, this, [this, sort]() { setSort(sort); });
        tabs->addWidget(button);
        return button;
    };
    m_newest = makeTab(QStringLiteral("Newest"), SearchQuery::Sort::Newest);
    m_oldest = makeTab(QStringLiteral("Oldest"), SearchQuery::Sort::Oldest);
    m_relevant = makeTab(QStringLiteral("Most relevant"), SearchQuery::Sort::Relevant);
    tabs->addStretch(1);
    layout->addLayout(tabs);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("SearchStatus"));
    m_status->setWordWrap(true);
    m_status->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_status->setContentsMargins(18, 24, 18, 0);
    m_status->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; background: transparent;")
                                .arg(QLatin1String(Theme::TextMuted)));
    layout->addWidget(m_status);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("SearchResults"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMouseTracking(true);
    m_list->viewport()->setCursor(Qt::PointingHandCursor);
    m_list->setItemDelegate(new ResultDelegate(m_list));
    Theme::showThrough(m_list);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        emit jumpRequested(item->data(ChannelRole).toString(), item->data(MessageRole).toString());
    });
    layout->addWidget(m_list, 1);

    m_pager = new QWidget(this);
    auto *pager = new QHBoxLayout(m_pager);
    pager->setContentsMargins(10, 0, 10, 0);
    m_previous = new QPushButton(QStringLiteral("Previous"), m_pager);
    m_next = new QPushButton(QStringLiteral("Next"), m_pager);
    for (QPushButton *button : {m_previous, m_next}) {
        button->setObjectName(QStringLiteral("TabButton"));
        button->setFlat(true);
        button->setCursor(Qt::PointingHandCursor);
    }
    m_pageLabel = new QLabel(m_pager);
    m_pageLabel->setAlignment(Qt::AlignCenter);
    m_pageLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; background: transparent;")
                                   .arg(QLatin1String(Theme::TextMuted)));
    pager->addWidget(m_previous);
    pager->addWidget(m_pageLabel, 1);
    pager->addWidget(m_next);
    connect(m_previous, &QPushButton::clicked, this, [this]() { turnPage(-1); });
    connect(m_next, &QPushButton::clicked, this, [this]() { turnPage(1); });
    layout->addWidget(m_pager);
    // Only once there is more than one page.
    m_pager->hide();

    // Faces arrive after the rows do.
    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &) {
        if (isVisible() && m_list->count() > 0)
            m_list->viewport()->update();
    });

    setSort(SearchQuery::Sort::Newest);
    showStatus(QString());
}

void SearchPanel::search(const QString &guildId, const QString &channelId, const QString &text,
                         const QHash<QString, QString> &picked)
{
    ++m_serial;
    m_namesAsked.clear();
    m_guildId = guildId;
    m_channelId = channelId;
    m_picked = picked;

    const SearchQuery::Sort sort = m_request.sort;
    m_request = SearchQuery::Request{};
    m_request.sort = sort;
    m_request.query = SearchQuery::parse(text);
    m_request.includeNsfw = m_includeNsfw && !guildId.isEmpty();
    m_terms = SearchQuery::highlightTerms(m_request.query.content);
    m_total = 0;
    m_count->setText(QStringLiteral("Searching"));
    resolveNames();
}

void SearchPanel::clear()
{
    ++m_serial;
    m_guildId.clear();
    m_channelId.clear();
    m_picked.clear();
    m_total = 0;
    m_list->clear();
    m_pager->hide();
    m_count->setText(QStringLiteral("Search"));
    showStatus(QString());
}

QString SearchPanel::personNamed(const QString &name) const
{
    QList<UserInfo> candidates;
    if (m_guildId.isEmpty()) {
        QStringList ids = m_store->channel(m_channelId).recipientIds;
        ids.append(m_store->selfUserId());
        for (const QString &id : std::as_const(ids))
            candidates.append(m_store->user(id));
    } else {
        candidates = m_store->findUsers(name, m_guildId, 20);
    }

    for (const UserInfo &info : std::as_const(candidates)) {
        const QString nickname = m_guildId.isEmpty() ? QString() : m_store->memberLook(m_guildId, info.id).nickname;
        for (const QString &known : {info.username, info.globalName, nickname}) {
            if (!known.isEmpty() && known.compare(name, Qt::CaseInsensitive) == 0)
                return info.id;
        }
    }
    return {};
}

QString SearchPanel::channelNamed(const QString &name) const
{
    if (m_guildId.isEmpty())
        return {};
    for (const ChannelInfo &channel : m_store->channelsOfGuild(m_guildId)) {
        if (!channel.isCategory() && channel.name.compare(name, Qt::CaseInsensitive) == 0)
            return channel.id;
    }
    return {};
}

// Ids for the people and channels the filters name: what the box's list was
// used for, then anyone known here by exactly that name, then, in a server,
// Discord's member search for a name nobody here has met. A name still
// without an id is left for toQuery to explain.
void SearchPanel::resolveNames()
{
    const int serial = m_serial;
    for (SearchQuery::Filter &filter : m_request.query.filters) {
        if (!SearchQuery::needsId(filter.key) || !filter.id.isEmpty())
            continue;

        const QString lower = filter.value.toLower();
        filter.id = m_picked.value(filter.key + QLatin1Char(':') + lower);
        if (!filter.id.isEmpty())
            continue;

        if (filter.key == QLatin1String("in")) {
            filter.id = channelNamed(filter.value);
            continue;
        }

        filter.id = personNamed(filter.value);
        if (!filter.id.isEmpty() || m_guildId.isEmpty() || m_namesAsked.contains(lower))
            continue;

        m_namesAsked.insert(lower);
        showStatus(QStringLiteral("Looking for %1...").arg(filter.value));
        const QString guildId = m_guildId;
        m_rest->searchGuildMembers(
            guildId, filter.value,
            [this, serial, guildId](const QJsonArray &members) {
                for (const QJsonValue &member : members)
                    m_store->rememberMember(guildId, member.toObject());
                if (serial == m_serial)
                    resolveNames();
            },
            [this, serial](const RestClient::Error &) {
                if (serial == m_serial)
                    resolveNames();
            });
        return;
    }
    runPage();
}

void SearchPanel::runPage()
{
    QString problem;
    const QList<QPair<QString, QString>> query = SearchQuery::toQuery(m_request, &problem);
    if (query.isEmpty()) {
        m_count->setText(QStringLiteral("Search"));
        m_list->clear();
        m_pager->hide();
        showStatus(problem);
        return;
    }

    showStatus(QStringLiteral("Searching..."));
    const int serial = m_serial;
    m_rest->searchMessages(
        m_guildId, m_channelId, query,
        [this, serial](const QJsonObject &body) {
            if (serial == m_serial)
                showPage(SearchQuery::readPage(body));
        },
        [this, serial](const RestClient::Error &error) {
            if (serial != m_serial)
                return;
            m_count->setText(QStringLiteral("Search"));
            m_list->clear();
            m_pager->hide();
            if (error.httpStatus == 403)
                showStatus(QStringLiteral("You can't search here."));
            else if (error.httpStatus == 429)
                showStatus(QStringLiteral("Discord says that is too many searches for now. Try again in a minute."));
            else if (error.httpStatus == 202)
                showStatus(error.message);
            else if (error.httpStatus == 400 && !error.message.isEmpty())
                showStatus(QStringLiteral("Discord could not search for that: %1").arg(error.message));
            else
                showStatus(QStringLiteral("The search did not go through (HTTP %1). The log has the details.")
                               .arg(error.httpStatus));
        });
}

void SearchPanel::showStatus(const QString &text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
    m_list->setVisible(text.isEmpty());
    if (!text.isEmpty())
        m_pager->hide();
}

QString SearchPanel::previewText(const QJsonObject &raw) const
{
    const MessageInfo message = MessageStore::parseMessage(raw);
    QString text = message.content;
    if (text.isEmpty() && !message.snapshots.isEmpty())
        text = message.snapshots.first().content;
    if (text.isEmpty()) {
        for (const EmbedInfo &embed : message.embeds) {
            const QString card = QStringList{embed.title, embed.description}.join(QLatin1Char('\n')).trimmed();
            if (!card.isEmpty()) {
                text = card;
                break;
            }
        }
    }

    // Mentions, channels, emoji and timestamps as words, the way the
    // conversation shows them.
    static const QRegularExpression token(
        QStringLiteral(R"(<@!?(\d+)>|<@&(\d+)>|<#(\d+)>|<a?:(\w+):\d+>|<t:(-?\d+)(?::([tTdDfFR]))?>)"));
    QString out;
    int done = 0;
    QRegularExpressionMatchIterator it = token.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += text.mid(done, match.capturedStart() - done);
        done = match.capturedEnd();
        if (!match.captured(1).isEmpty()) {
            const QString id = match.captured(1);
            QString name = m_guildId.isEmpty() ? QString() : m_store->memberLook(m_guildId, id).nickname;
            if (name.isEmpty())
                name = m_store->userName(id);
            out += QLatin1Char('@') + (name.isEmpty() ? QStringLiteral("unknown-user") : name);
        } else if (!match.captured(2).isEmpty()) {
            const QString role = m_store->guild(m_guildId).roles.value(match.captured(2)).name;
            out += QLatin1Char('@') + (role.isEmpty() ? QStringLiteral("unknown-role") : role);
        } else if (!match.captured(3).isEmpty()) {
            const QString channel = m_store->channel(match.captured(3)).name;
            out += QLatin1Char('#') + (channel.isEmpty() ? QStringLiteral("unknown") : channel);
        } else if (!match.captured(4).isEmpty()) {
            out += QLatin1Char(':') + match.captured(4) + QLatin1Char(':');
        } else {
            out += timestampText(match.captured(5).toLongLong(), match.captured(6));
        }
    }
    out += text.mid(done);

    // A wall of text is cut, as Discord cuts a long result.
    constexpr int Longest = 600;
    if (out.size() > Longest)
        out = out.left(Longest).trimmed() + QStringLiteral("...");
    return out;
}

void SearchPanel::showPage(const SearchQuery::Page &page)
{
    m_total = page.total;
    m_count->setText(page.total == 1 ? QStringLiteral("1 result")
                                     : QStringLiteral("%1 results").arg(QLocale().toString(page.total)));
    m_list->clear();

    if (page.messages.isEmpty()) {
        showStatus(m_request.offset > 0 ? QStringLiteral("No more results.")
                                        : QStringLiteral("Nothing matched. Try other words, or fewer filters."));
    } else {
        showStatus(QString());
    }

    QString lastPlace;
    for (const QJsonObject &raw : page.messages) {
        m_store->rememberUser(raw.value(QStringLiteral("author")).toObject());
        const MessageInfo message = MessageStore::parseMessage(raw);
        const QString channelId = raw.value(QStringLiteral("channel_id")).toString();

        auto *item = new QListWidgetItem(m_list);
        item->setData(ChannelRole, channelId);
        item->setData(MessageRole, message.id);

        // Where it was said, over the first of each run from one channel. A
        // chat search is all one chat, so it goes without.
        if (!m_guildId.isEmpty()) {
            QString name = m_store->channel(channelId).name;
            if (name.isEmpty())
                name = page.threads.value(channelId).value(QStringLiteral("name")).toString();
            const QString place = QStringLiteral("# ") + (name.isEmpty() ? QStringLiteral("unknown channel") : name);
            if (place != lastPlace)
                item->setData(PlaceRole, place);
            lastPlace = place;
        }

        QString author = m_guildId.isEmpty() ? QString() : m_store->memberLook(m_guildId, message.authorId).nickname;
        if (author.isEmpty())
            author = message.authorName;
        if (author.isEmpty())
            author = m_store->userName(message.authorId);
        item->setData(AuthorRole, author);
        item->setData(AvatarRole, MediaCache::avatarUrl(message.authorId, message.authorAvatar, 64).toString());
        item->setData(ColourRole, m_store->look(message.authorId, m_guildId).roleColour);
        item->setData(TimeRole, whenText(message.timestamp));
        item->setData(HtmlRole, SearchQuery::highlightHtml(previewText(raw), m_terms));

        QStringList extra;
        for (const Attachment &attachment : message.attachments)
            extra.append(attachment.filename);
        for (const StickerInfo &sticker : message.stickers)
            extra.append(QStringLiteral("Sticker: %1").arg(sticker.name));
        if (!extra.isEmpty())
            item->setData(ExtraRole, extra.join(QStringLiteral(", ")));
    }
    m_list->scrollToTop();

    // Discord stops at offset 9975, so at most 400 pages are reachable.
    const int reachable = qMin(m_total, SearchQuery::MaxOffset + SearchQuery::PageSize);
    const int pages = qMax(1, (reachable + SearchQuery::PageSize - 1) / SearchQuery::PageSize);
    const int pageNow = m_request.offset / SearchQuery::PageSize + 1;
    m_pageLabel->setText(QStringLiteral("Page %1 of %2").arg(pageNow).arg(pages));
    m_previous->setEnabled(pageNow > 1);
    m_next->setEnabled(pageNow < pages);
    m_pager->setVisible(pages > 1 || pageNow > 1);
}

void SearchPanel::setSort(SearchQuery::Sort sort)
{
    m_request.sort = sort;
    const QList<QPair<QPushButton *, SearchQuery::Sort>> tabs{
        {m_newest, SearchQuery::Sort::Newest},
        {m_oldest, SearchQuery::Sort::Oldest},
        {m_relevant, SearchQuery::Sort::Relevant},
    };
    for (const auto &tab : tabs) {
        tab.first->setProperty("active", tab.second == sort);
        tab.first->style()->unpolish(tab.first);
        tab.first->style()->polish(tab.first);
    }

    // A new order starts again from its first page.
    if (m_guildId.isEmpty() && m_channelId.isEmpty())
        return;
    m_request.offset = 0;
    ++m_serial;
    resolveNames();
}

void SearchPanel::turnPage(int step)
{
    const int offset = qBound(0, m_request.offset + step * SearchQuery::PageSize, SearchQuery::MaxOffset);
    if (offset == m_request.offset)
        return;
    m_request.offset = offset;
    ++m_serial;
    resolveNames();
}
