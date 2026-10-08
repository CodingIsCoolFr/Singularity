#include "core/AppCommands.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QSet>

#include <cmath>

namespace AppCommands {

namespace {

constexpr quint64 Administrator = 1ull << 3;
constexpr qint64 DiscordEpochMs = 1420070400000LL;

// Discord's index sends the name you would see in your language beside the
// one the app registered. The registered one is what is sent back.
QString untranslated(const QJsonObject &raw)
{
    const QString name = raw.value(QStringLiteral("name_default")).toString();
    return name.isEmpty() ? raw.value(QStringLiteral("name")).toString() : name;
}

QString shown(const QJsonObject &raw, const char *key)
{
    const QString localized = raw.value(QLatin1String(key) + QStringLiteral("_localized")).toString();
    return localized.isEmpty() ? raw.value(QLatin1String(key)).toString() : localized;
}

Option parseOption(const QJsonObject &raw)
{
    Option option;
    option.type = raw.value(QStringLiteral("type")).toInt(String);
    option.name = untranslated(raw);
    option.displayName = shown(raw, "name");
    option.description = shown(raw, "description");
    option.required = raw.value(QStringLiteral("required")).toBool();
    option.autocomplete = raw.value(QStringLiteral("autocomplete")).toBool();
    for (const QJsonValue &value : raw.value(QStringLiteral("choices")).toArray()) {
        const QJsonObject choice = value.toObject();
        option.choices.append({shown(choice, "name"), choice.value(QStringLiteral("value"))});
    }
    if (raw.contains(QStringLiteral("min_value"))) {
        option.hasMin = true;
        option.minValue = raw.value(QStringLiteral("min_value")).toDouble();
    }
    if (raw.contains(QStringLiteral("max_value"))) {
        option.hasMax = true;
        option.maxValue = raw.value(QStringLiteral("max_value")).toDouble();
    }
    option.minLength = raw.value(QStringLiteral("min_length")).toInt(-1);
    option.maxLength = raw.value(QStringLiteral("max_length")).toInt(-1);
    for (const QJsonValue &value : raw.value(QStringLiteral("options")).toArray())
        option.options.append(parseOption(value.toObject()));
    return option;
}

QList<int> intList(const QJsonValue &value)
{
    QList<int> out;
    for (const QJsonValue &item : value.toArray())
        out.append(item.toInt());
    return out;
}

// A permission override: true, false, or not said (null).
QJsonValue overrideFor(const QJsonObject &permissions, const char *kind, const QString &id)
{
    const QJsonValue value = permissions.value(QLatin1String(kind)).toObject().value(id);
    return value.isBool() ? value : QJsonValue();
}

// Discord's "all channels" key is the server id minus one.
QString allChannelsKey(const QString &guildId)
{
    bool ok = false;
    const quint64 id = guildId.toULongLong(&ok);
    return ok && id > 0 ? QString::number(id - 1) : QString();
}

QJsonValue channelOverride(const QJsonObject &permissions, const Place &place)
{
    if (permissions.isEmpty())
        return {};
    const QString channel = place.parentId.isEmpty() ? place.channelId : place.parentId;
    QJsonValue value = overrideFor(permissions, "channels", channel);
    if (value.isBool())
        return value;
    return overrideFor(permissions, "channels", allChannelsKey(place.guildId));
}

QJsonValue memberOverride(const QJsonObject &permissions, const Place &place)
{
    if (permissions.isEmpty())
        return {};
    // The person themselves wins outright.
    if (permissions.value(QStringLiteral("user")).isBool())
        return permissions.value(QStringLiteral("user"));
    QJsonValue value = overrideFor(permissions, "users", place.selfId);
    if (value.isBool())
        return value;
    // Then any role: one allow is enough, a deny counts only when nothing allows.
    bool denied = false;
    for (const QString &role : place.roleIds) {
        value = overrideFor(permissions, "roles", role);
        if (value.isBool()) {
            if (value.toBool())
                return true;
            denied = true;
        }
    }
    if (denied)
        return false;
    // Then @everyone, whose id is the server's.
    return overrideFor(permissions, "roles", place.guildId);
}

bool parseInteger(QString text, qint64 *out)
{
    text.remove(QLatin1Char(','));
    text.remove(QLatin1Char('_'));
    text.remove(QLatin1Char(' '));
    bool ok = false;
    const qint64 value = text.toLongLong(&ok);
    if (!ok)
        return false;
    *out = value;
    return true;
}

QString numberText(double value)
{
    return QString::number(value, 'g', 15);
}

} // namespace

Index parseIndex(const QJsonObject &raw, bool userInstalled)
{
    Index index;
    for (const QJsonValue &value : raw.value(QStringLiteral("applications")).toArray()) {
        const QJsonObject app = value.toObject();
        App out;
        out.id = app.value(QStringLiteral("id")).toString();
        out.name = app.value(QStringLiteral("name")).toString();
        out.icon = app.value(QStringLiteral("icon")).toString();
        out.botId = app.value(QStringLiteral("bot_id")).toString();
        if (out.botId.isEmpty())
            out.botId = app.value(QStringLiteral("bot")).toObject().value(QStringLiteral("id")).toString();
        out.permissions = app.value(QStringLiteral("permissions")).toObject();
        if (!out.id.isEmpty())
            index.apps.append(out);
    }

    for (const QJsonValue &value : raw.value(QStringLiteral("application_commands")).toArray()) {
        const QJsonObject root = value.toObject();
        Command base;
        base.rootId = root.value(QStringLiteral("id")).toString();
        base.applicationId = root.value(QStringLiteral("application_id")).toString();
        if (base.rootId.isEmpty() || base.applicationId.isEmpty())
            continue;
        base.version = root.value(QStringLiteral("version")).toString();
        base.guildId = root.value(QStringLiteral("guild_id")).toString();
        base.type = root.value(QStringLiteral("type")).toInt(1);
        base.root = root;
        const QJsonValue dmp = root.value(QStringLiteral("default_member_permissions"));
        if (dmp.isString() || dmp.isDouble()) {
            base.hasDefaultPermissions = true;
            base.defaultMemberPermissions = dmp.isString() ? dmp.toString() : numberText(dmp.toDouble());
        }
        base.dmPermission = !root.contains(QStringLiteral("dm_permission"))
            || root.value(QStringLiteral("dm_permission")).isNull()
            || root.value(QStringLiteral("dm_permission")).toBool();
        base.nsfw = root.value(QStringLiteral("nsfw")).toBool();
        base.hasContexts = root.value(QStringLiteral("contexts")).isArray();
        base.contexts = intList(root.value(QStringLiteral("contexts")));
        base.integrationTypes = intList(root.value(QStringLiteral("integration_types")));
        base.permissions = root.value(QStringLiteral("permissions")).toObject();
        base.userInstalled = userInstalled;

        const QString rootName = untranslated(root);
        const QString rootShown = shown(root, "name");
        const QString rootDescription = shown(root, "description");

        QList<Option> options;
        for (const QJsonValue &option : root.value(QStringLiteral("options")).toArray())
            options.append(parseOption(option.toObject()));

        bool hasSub = false;
        for (const Option &option : options) {
            if (option.type == SubCommand || option.type == SubCommandGroup)
                hasSub = true;
        }

        if (!hasSub) {
            Command command = base;
            command.id = base.rootId;
            command.name = rootName;
            command.displayName = rootShown;
            command.description = rootDescription;
            command.options = options;
            index.commands.append(command);
            continue;
        }

        const auto addSub = [&](const QList<QPair<QString, int>> &path, const QStringList &shownPath,
                                const Option &leaf) {
            Command command = base;
            command.subPath = path;
            command.id = base.rootId;
            QStringList names{rootName};
            for (const auto &step : path) {
                command.id += QChar(0) + step.first;
                names << step.first;
            }
            command.name = names.join(QLatin1Char(' '));
            command.displayName = (QStringList{rootShown} + shownPath).join(QLatin1Char(' '));
            command.description = leaf.description;
            command.options = leaf.options;
            index.commands.append(command);
        };

        for (const Option &option : options) {
            if (option.type == SubCommand) {
                addSub({{option.name, SubCommand}}, {option.displayName}, option);
            } else if (option.type == SubCommandGroup) {
                for (const Option &sub : option.options) {
                    if (sub.type != SubCommand)
                        continue;
                    addSub({{option.name, SubCommandGroup}, {sub.name, SubCommand}},
                           {option.displayName, sub.displayName}, sub);
                }
            }
        }
    }
    return index;
}

bool allowed(const Command &command, const App *app, const Place &place)
{
    if (command.builtIn)
        return true;

    const bool inGuild = !place.guildId.isEmpty();

    // Where it may be used. A DM with only the app's own bot is its own kind
    // of place; any other DM or group is a "private channel".
    int context = 0;
    if (!inGuild) {
        context = 2;
        if (app && !app->botId.isEmpty() && place.recipientIds.size() == 1
            && place.recipientIds.first() == app->botId)
            context = 1;
    }
    if (command.hasContexts) {
        if (!command.contexts.contains(context))
            return false;
    } else if (!inGuild && (context == 2 || !command.dmPermission)) {
        return false;
    }

    if (!inGuild)
        return true;
    if (place.owner || (place.permissions & Administrator))
        return true;
    if (command.userInstalled && command.integrationTypes.contains(1))
        return true;

    const QJsonObject appPermissions = app ? app->permissions : QJsonObject();

    const QJsonValue channel = channelOverride(command.permissions, place);
    if (channel.isBool() && !channel.toBool())
        return false;
    if (!channel.isBool()) {
        const QJsonValue appChannel = channelOverride(appPermissions, place);
        if (appChannel.isBool() && !appChannel.toBool())
            return false;
    }

    const QJsonValue member = memberOverride(command.permissions, place);
    if (member.isBool())
        return member.toBool();
    const QJsonValue appMember = memberOverride(appPermissions, place);
    if (appMember.isBool() && !appMember.toBool())
        return false;

    if (command.hasDefaultPermissions) {
        bool ok = false;
        const quint64 needed = command.defaultMemberPermissions.toULongLong(&ok);
        if (!ok || needed == 0)
            return false;   // "0" means administrators only
        return (place.permissions & needed) == needed;
    }
    return true;
}

QList<Span> findSpans(const QString &text, const QList<Option> &options, int *looseEnd)
{
    // Discord's option names: letters, digits, "-" and "_", up to 32.
    static const QRegularExpression label(QStringLiteral(R"((?:^|(?<=\s))([\p{L}\p{N}_-]{1,32}):)"),
                                          QRegularExpression::UseUnicodePropertiesOption);
    QList<Span> spans;
    QSet<int> used;
    auto it = label.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString name = match.captured(1);
        int found = -1;
        for (int i = 0; i < options.size(); ++i) {
            if (used.contains(i))
                continue;
            if (options.at(i).name.compare(name, Qt::CaseInsensitive) == 0
                || options.at(i).displayName.compare(name, Qt::CaseInsensitive) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0)
            continue;
        used.insert(found);
        Span span;
        span.option = found;
        span.labelStart = match.capturedStart(1);
        span.valueStart = match.capturedEnd(0);
        spans.append(span);
    }
    for (int i = 0; i < spans.size(); ++i)
        spans[i].valueEnd = i + 1 < spans.size() ? spans.at(i + 1).labelStart : int(text.size());
    if (looseEnd)
        *looseEnd = spans.isEmpty() ? int(text.size()) : spans.first().labelStart;
    return spans;
}

QJsonValue toValue(const Option &option, const QString &rawText, const Resolver &resolver, QString *error)
{
    const QString text = rawText.trimmed();
    QString dummy;
    QString &why = error ? *error : dummy;
    why.clear();

    static const QRegularExpression userMention(QStringLiteral(R"(^<@!?(\d+)>$)"));
    static const QRegularExpression roleMention(QStringLiteral(R"(^<@&(\d+)>$)"));
    static const QRegularExpression channelMention(QStringLiteral(R"(^<#(\d+)>$)"));
    static const QRegularExpression bareId(QStringLiteral(R"(^\d{15,21}$)"));

    const auto fromChoices = [&](QJsonValue *out) {
        if (option.choices.isEmpty())
            return false;
        for (const Choice &choice : option.choices) {
            const QString valueText = choice.value.isDouble() ? numberText(choice.value.toDouble())
                                                              : choice.value.toString();
            if (choice.name.compare(text, Qt::CaseInsensitive) == 0
                || valueText.compare(text, Qt::CaseInsensitive) == 0) {
                *out = choice.value;
                return true;
            }
        }
        QStringList names;
        for (const Choice &choice : option.choices)
            names << choice.name;
        why = QStringLiteral("%1 has to be one of: %2").arg(option.displayName, names.join(QStringLiteral(", ")));
        return true;
    };

    switch (option.type) {
    case String: {
        QJsonValue value;
        if (fromChoices(&value))
            return value;
        if (option.minLength >= 0 && text.size() < option.minLength)
            why = QStringLiteral("%1 needs at least %2 characters.").arg(option.displayName).arg(option.minLength);
        else if (option.maxLength >= 0 && text.size() > option.maxLength)
            why = QStringLiteral("%1 takes at most %2 characters.").arg(option.displayName).arg(option.maxLength);
        return text;
    }
    case Integer:
    case Number: {
        QJsonValue value;
        if (fromChoices(&value))
            return value;
        double number = 0;
        if (option.type == Integer) {
            qint64 whole = 0;
            if (!parseInteger(text, &whole) || std::llabs(whole) > (1LL << 53)) {
                why = QStringLiteral("%1 has to be a whole number.").arg(option.displayName);
                return {};
            }
            number = double(whole);
        } else {
            QString cleaned = text;
            cleaned.remove(QLatin1Char(','));
            bool ok = false;
            number = cleaned.toDouble(&ok);
            if (!ok || !std::isfinite(number)) {
                why = QStringLiteral("%1 has to be a number.").arg(option.displayName);
                return {};
            }
        }
        if (option.hasMin && number < option.minValue)
            why = QStringLiteral("%1 has to be at least %2.").arg(option.displayName, numberText(option.minValue));
        else if (option.hasMax && number > option.maxValue)
            why = QStringLiteral("%1 has to be at most %2.").arg(option.displayName, numberText(option.maxValue));
        return number;
    }
    case Boolean: {
        const QString lower = text.toLower();
        if (lower == QLatin1String("true") || lower == QLatin1String("yes") || lower == QLatin1String("on")
            || lower == QLatin1String("1"))
            return true;
        if (lower == QLatin1String("false") || lower == QLatin1String("no") || lower == QLatin1String("off")
            || lower == QLatin1String("0"))
            return false;
        why = QStringLiteral("%1 has to be True or False.").arg(option.displayName);
        return {};
    }
    case User:
    case Role:
    case Mentionable: {
        QRegularExpressionMatch match = userMention.match(text);
        if (match.hasMatch() && option.type != Role)
            return match.captured(1);
        match = roleMention.match(text);
        if (match.hasMatch() && option.type != User)
            return match.captured(1);
        if (option.type != User && text == QLatin1String("@everyone") && !resolver.guildId.isEmpty())
            return resolver.guildId;
        if (bareId.match(text).hasMatch())
            return text;
        why = option.type == Role ? QStringLiteral("Pick a role for %1 from the list.").arg(option.displayName)
                                  : QStringLiteral("Pick someone for %1 from the list.").arg(option.displayName);
        return {};
    }
    case Channel: {
        const QRegularExpressionMatch match = channelMention.match(text);
        if (match.hasMatch())
            return match.captured(1);
        if (bareId.match(text).hasMatch())
            return text;
        if (resolver.channelByName) {
            QString name = text;
            if (name.startsWith(QLatin1Char('#')))
                name.remove(0, 1);
            const QString id = resolver.channelByName(name);
            if (!id.isEmpty())
                return id;
        }
        why = QStringLiteral("Pick a channel for %1 from the list.").arg(option.displayName);
        return {};
    }
    case Attachment:
        why = QStringLiteral("This command wants a file. Singularity cannot attach files to commands yet.");
        return {};
    default:
        why = QStringLiteral("Singularity does not know option type %1.").arg(option.type);
        return {};
    }
}

QJsonObject buildData(const Command &command, const QJsonArray &options, const QString &targetId)
{
    QJsonArray wrapped = options;
    for (int i = int(command.subPath.size()) - 1; i >= 0; --i) {
        wrapped = QJsonArray{QJsonObject{{QStringLiteral("type"), command.subPath.at(i).second},
                                         {QStringLiteral("name"), command.subPath.at(i).first},
                                         {QStringLiteral("options"), wrapped}}};
    }

    // Discord sends the command back as its client holds it. Overrides are
    // kept in a different shape there, so they are left out rather than
    // sent in the wrong one.
    QJsonObject root = command.root;
    root.remove(QStringLiteral("permissions"));

    QJsonObject data{
        {QStringLiteral("version"), command.version},
        {QStringLiteral("id"), command.rootId},
        {QStringLiteral("name"), untranslated(command.root)},
        {QStringLiteral("type"), command.type},
        {QStringLiteral("options"), wrapped},
        {QStringLiteral("application_command"), root},
        {QStringLiteral("attachments"), QJsonArray()},
    };
    if (!command.guildId.isEmpty())
        data.insert(QStringLiteral("guild_id"), command.guildId);
    if (!targetId.isEmpty())
        data.insert(QStringLiteral("target_id"), targetId);
    return data;
}

QString makeNonce()
{
    const qint64 ms = QDateTime::currentMSecsSinceEpoch() - DiscordEpochMs;
    return QString::number(quint64(ms) << 22);
}

QList<Command> builtIns()
{
    struct Spec { const char *name; QString description; bool required; };
    const Spec specs[] = {
        {"shrug", QStringLiteral("Adds ¯\\_(ツ)_/¯ to your message."), false},
        {"tableflip", QStringLiteral("Adds (╯°□°)╯︵ ┻━┻ to your message."), false},
        {"unflip", QStringLiteral("Adds ┬─┬ノ( º _ ºノ) to your message."), false},
        {"me", QStringLiteral("Shows your words in italics."), true},
        {"spoiler", QStringLiteral("Hides your message until someone clicks it."), true},
    };
    QList<Command> out;
    for (const Spec &spec : specs) {
        Command command;
        command.builtIn = true;
        command.id = QStringLiteral("-1:") + QLatin1String(spec.name);
        command.rootId = command.id;
        command.applicationId = QStringLiteral("-1");
        command.name = QLatin1String(spec.name);
        command.displayName = command.name;
        command.description = spec.description;
        Option message;
        message.type = String;
        message.name = QStringLiteral("message");
        message.displayName = message.name;
        message.description = QStringLiteral("Your message");
        message.required = spec.required;
        command.options.append(message);
        out.append(command);
    }
    return out;
}

QString runBuiltIn(const Command &command, const QString &rawText)
{
    const QString text = rawText.trimmed();
    const auto append = [&](const QString &face) {
        return text.isEmpty() ? face : text + QLatin1Char(' ') + face;
    };
    if (command.name == QLatin1String("shrug"))
        return append(QStringLiteral("¯\\_(ツ)_/¯"));
    if (command.name == QLatin1String("tableflip"))
        return append(QStringLiteral("(╯°□°)╯︵ ┻━┻"));
    if (command.name == QLatin1String("unflip"))
        return append(QStringLiteral("┬─┬ノ( º _ ºノ)"));
    if (command.name == QLatin1String("me"))
        return text.isEmpty() ? QString() : QStringLiteral("_%1_").arg(text);
    if (command.name == QLatin1String("spoiler"))
        return text.isEmpty() ? QString() : QStringLiteral("||%1||").arg(text);
    return text;
}

QString frecencyKey(const Command &command)
{
    return command.guildId.isEmpty() ? command.id : command.id + QLatin1Char(':') + command.guildId;
}

} // namespace AppCommands
