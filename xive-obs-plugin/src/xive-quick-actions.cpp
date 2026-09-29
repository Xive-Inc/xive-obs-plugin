#include "xive-quick-actions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QtNumeric>

#include <cmath>

namespace XiveQuickActions {

namespace {

const char *kPollPrefix = "XIVE_POLL:";
const char *kVotePrefix = "XIVE_POLL_VOTE:";
const char *kAdBreakMessage = "XIVE_AD_BREAK";

QStringList cleanOptions(const QStringList &options)
{
	QStringList out;
	for (const QString &option : options) {
		const QString trimmed = option.trimmed();
		if (!trimmed.isEmpty())
			out.append(trimmed);
	}
	return out;
}

} // namespace

QString adBreakMessage()
{
	return QString::fromUtf8(kAdBreakMessage);
}

QString buildPollMessage(const QString &question, const QStringList &options)
{
	const QString trimmedQuestion = question.trimmed();
	const QStringList opts = cleanOptions(options);

	if (trimmedQuestion.isEmpty() || opts.size() < kMinPollOptions)
		return QString();

	QJsonArray optionArray;
	/* The parser keeps only the first 8, so send only what will survive the round trip. */
	for (int i = 0; i < opts.size() && i < kMaxPollOptions; ++i)
		optionArray.append(opts.at(i));

	QJsonObject payload;
	payload[QStringLiteral("question")] = trimmedQuestion;
	payload[QStringLiteral("options")] = optionArray;

	/* Compact, because this has to fit inside a chat message. */
	const QString message = QString::fromUtf8(kPollPrefix) +
				QString::fromUtf8(QJsonDocument(payload).toJson(
					QJsonDocument::Compact));

	if (message.length() > kChatMessageLimit)
		return QString();

	return message;
}

QString pollValidationError(const QString &question, const QStringList &options)
{
	if (question.trimmed().isEmpty())
		return QObject::tr("Enter a question for the poll.");

	const QStringList opts = cleanOptions(options);
	if (opts.size() < kMinPollOptions)
		return QObject::tr("A poll needs at least %1 options.").arg(kMinPollOptions);

	if (buildPollMessage(question, options).isEmpty())
		return QObject::tr(
			"That poll is too long to fit in a chat message. Shorten the question or "
			"the options.");

	return QString();
}

bool parsePoll(const QString &content, Poll *out)
{
	/* Not trimmed first, deliberately: parsePollMessage() tests startsWith on the raw string,
	 * so a leading space makes it ordinary chat on the web too. */
	const QString prefix = QString::fromUtf8(kPollPrefix);
	if (!out || !content.startsWith(prefix))
		return false;

	const QByteArray raw = content.mid(prefix.length()).trimmed().toUtf8();

	QJsonParseError error{};
	const QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
	if (error.error != QJsonParseError::NoError || !doc.isObject())
		return false;

	const QJsonObject payload = doc.object();
	const QString question = payload.value(QStringLiteral("question")).toString().trimmed();

	QStringList options;
	const QJsonArray array = payload.value(QStringLiteral("options")).toArray();
	for (const QJsonValue &value : array) {
		const QString option = value.toString().trimmed();
		if (!option.isEmpty())
			options.append(option);
	}

	if (question.isEmpty() || options.size() < kMinPollOptions)
		return false;

	out->question = question;
	out->options = options.mid(0, kMaxPollOptions);
	return true;
}

bool parsePollVote(const QString &content, QString *pollMessageId, int *optionIndex)
{
	const QString prefix = QString::fromUtf8(kVotePrefix);
	if (!content.startsWith(prefix))
		return false;

	const QString rest = content.mid(prefix.length());
	const int lastColon = rest.lastIndexOf(QLatin1Char(':'));
	if (lastColon <= 0)
		return false;

	const QString id = rest.left(lastColon);
	if (id.isEmpty())
		return false;

	/*
	 * The web coerces with Number(), which accepts "2.7" and floors it, and turns "" into 0.
	 * toInt() would reject both outright and leak the raw marker into chat as text, so the
	 * coercion is reproduced rather than tightened.
	 */
	const QString digits = rest.mid(lastColon + 1);
	double parsed = 0;
	if (!digits.isEmpty()) {
		bool ok = false;
		parsed = digits.toDouble(&ok);
		if (!ok || !qIsFinite(parsed))
			return false;
	}
	if (parsed < 0)
		return false;

	if (pollMessageId)
		*pollMessageId = id;
	if (optionIndex)
		*optionIndex = int(std::floor(parsed));
	return true;
}

bool isAdBreak(const QString &content)
{
	return content.trimmed() == QString::fromUtf8(kAdBreakMessage);
}

} // namespace XiveQuickActions
