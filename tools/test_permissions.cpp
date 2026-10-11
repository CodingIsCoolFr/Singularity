#include "core/DiscordPermissions.h"

#include <cstdio>

static int g_failed = 0;
static int g_passed = 0;

static void expect(bool ok, const char *name)
{
    if (ok) {
        ++g_passed;
        std::printf("ok   %s\n", name);
    } else {
        ++g_failed;
        std::printf("FAIL %s\n", name);
    }
}

int main()
{
    using namespace DiscordPermissions;
    const QString guild = QStringLiteral("guild");
    const QString user = QStringLiteral("user");
    const QString role = QStringLiteral("role");

    expect(computeBase(true, 0, {}) == All, "owner has every bit");
    expect(computeBase(false, Administrator, {}) == All, "administrator role has every bit");
    expect(computeBase(false, ViewChannel, {}) == ViewChannel, "@everyone view only");
    expect(computeBase(false, 0, {ViewChannel | SendMessages}) == (ViewChannel | SendMessages),
           "held role is OR'd onto @everyone");

    const quint64 base = computeBase(false, ViewChannel | ReadMessageHistory, {});
    const Overwrite denyEveryone{guild, 0, 0, ViewChannel};
    const quint64 hidden = applyOverwrites(base, guild, user, {}, {denyEveryone});
    expect(!has(hidden, ViewChannel), "@everyone deny VIEW_CHANNEL hides the channel");
    expect(has(hidden, ReadMessageHistory), "deny VIEW_CHANNEL does not clear READ_MESSAGE_HISTORY bits");

    const Overwrite allowRole{role, 0, ViewChannel, 0};
    const quint64 shown = applyOverwrites(base, guild, user, {role}, {denyEveryone, allowRole});
    expect(has(shown, ViewChannel), "a role allow beats @everyone deny");

    const Overwrite denyMember{user, 1, 0, ViewChannel};
    const quint64 memberDenied = applyOverwrites(base, guild, user, {role},
                                                 {denyEveryone, allowRole, denyMember});
    expect(!has(memberDenied, ViewChannel), "a member deny beats a role allow");

    const quint64 adminBase = computeBase(false, Administrator, {});
    const quint64 adminOver = applyOverwrites(adminBase, guild, user, {}, {denyEveryone});
    expect(adminOver == All, "administrator ignores channel overwrites");

    const Overwrite denyHistory{guild, 0, 0, ReadMessageHistory};
    const quint64 noHistory = applyOverwrites(base, guild, user, {}, {denyHistory});
    expect(has(noHistory, ViewChannel), "missing READ_MESSAGE_HISTORY still leaves VIEW_CHANNEL");
    expect(!has(noHistory, ReadMessageHistory), "channel overwrite can strip READ_MESSAGE_HISTORY");

    expect(classifyHistoryFailure(429, 0) == HistoryFailure::RateLimit, "429 is a rate limit");
    expect(classifyHistoryFailure(401, 0) == HistoryFailure::Auth, "401 is auth, not a channel permission");
    expect(classifyHistoryFailure(404, 10003) == HistoryFailure::MissingChannel, "404 is a missing channel");
    expect(classifyHistoryFailure(403, 50001) == HistoryFailure::MissingAccess, "403/50001 is Missing Access");
    expect(classifyHistoryFailure(403, 50013) == HistoryFailure::MissingPermissions,
           "403/50013 is Missing Permissions");
    expect(classifyHistoryFailure(403, 0) == HistoryFailure::Forbidden, "bare 403 is not assumed to be 50001");
    expect(classifyHistoryFailure(500, 0) == HistoryFailure::Other, "500 is not a permission error");

    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
