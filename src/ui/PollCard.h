#pragma once

#include "core/MessageStore.h"

#include <QDateTime>
#include <QString>

// A poll in the chat, drawn the way Discord draws one: the question, each
// answer as a row with its emoji, its votes and a bar for its share, your
// vote marked, and how long it has left.
//
// This is rich text for the chat's QTextBrowser, which has no rounded boxes
// and cannot lay one thing over another, so each answer's bar runs under it
// rather than behind it.
namespace PollCard {

// `messageId` empty draws the poll to read only, as inside a forward. While
// the poll is open, a click on an answer is
// "singularity-poll://vote?m=<message id>&a=<answer id>".
QString html(const PollInfo &poll, const QString &messageId, const QDateTime &now);

// The notice Discord posts when a poll ends (message type 46), as one row of
// the chat like a call's: whose poll closed, what it asked, and what won.
QString resultHtml(const PollResultInfo &result, const QString &authorName, const QString &messageId,
                   const QDateTime &when);

// "5 hours left", "1 day left", "less than a minute left".
QString timeLeft(const QDateTime &now, const QDateTime &expiry);

// "1 vote", "12 votes".
QString votes(int count);

} // namespace PollCard
