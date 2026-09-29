#include "ui/xive-alerts-dock.h"

#include "ui/xive-theme.h"
#include "xive-api.h"
#include "xive-auth.h"
#include "xive-channel.h"
#include "xive-config.h"

#include <QFrame>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

const char *XiveAlertsDock::kDockId = "xive_alerts_dock";

XiveAlertsDock::XiveAlertsDock(QWidget *parent) : QWidget(parent)
{
	XiveTheme::applyDockTheme(this);

	auto *shell = new QVBoxLayout(this);
	shell->setContentsMargins(0, 0, 0, 0);
	shell->setSpacing(0);

	shell->addWidget(XiveTheme::makeHeader(this, tr("Gifts"), &m_headerDot, &m_headerState));

	auto *layout = new QVBoxLayout();
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);
	shell->addLayout(layout, 1);

	/* Wallet and broadcast total are the two standing numbers — one card, like a studio stat row. */
	auto *totalsCard = XiveTheme::makeCard(this);
	auto *totals = new QVBoxLayout(totalsCard);
	totals->setContentsMargins(10, 10, 10, 10);
	totals->setSpacing(4);

	m_walletLabel = new QLabel(totalsCard);
	totals->addWidget(m_walletLabel);

	m_totalLabel = new QLabel(totalsCard);
	totals->addWidget(m_totalLabel);

	layout->addWidget(totalsCard);

	layout->addWidget(XiveTheme::makeSectionLabel(this, tr("Top gifters")));

	m_notice = new QLabel(this);
	m_notice->setWordWrap(true);
	m_notice->setStyleSheet(XiveTheme::noticeCss(false));
	m_notice->setVisible(false);
	layout->addWidget(m_notice);

	m_view = new QTextBrowser(this);
	m_view->setOpenExternalLinks(false);
	m_view->setFrameShape(QFrame::NoFrame);
	m_view->document()->setDefaultStyleSheet(XiveTheme::documentStyleSheet());
	layout->addWidget(m_view, 1);

	if (XiveChannel *ch = XiveChannel::instance()) {
		connect(ch, &XiveChannel::changed, this, [this]() {
			XiveChannel *live = XiveChannel::instance();
			XiveTheme::setLiveState(m_headerDot, m_headerState, live && live->isLive());
		});
		XiveTheme::setLiveState(m_headerDot, m_headerState, ch->isLive());
	}

	m_timer = new QTimer(this);
	m_timer->setInterval(XiveConfig::instance().statusPollMs());
	connect(m_timer, &QTimer::timeout, this, &XiveAlertsDock::poll);
	m_timer->start();

	poll();
}

void XiveAlertsDock::setNotice(const QString &text)
{
	m_notice->setText(text);
	m_notice->setVisible(!text.isEmpty());
}

void XiveAlertsDock::poll()
{
	XiveApi *api = XiveApi::instance();
	XiveAuth *auth = XiveAuth::instance();
	XiveChannel *ch = XiveChannel::instance();
	if (!api || !auth || !ch)
		return;

	if (!auth->isSignedIn()) {
		setNotice(tr("Connect to Xive in the Xive dock."));
		return;
	}

	if (!isVisible() || m_inFlight)
		return;

	m_inFlight = true;

	api->walletBalance([this](bool ok, const QJsonObject &body, int, const QString &) {
		if (!ok) {
			m_walletLabel->clear();
			return;
		}
		/* credit_cost and balances are bigints, which node-postgres emitted as STRINGS —
		 * the PHP port preserves that, so a balance can arrive as either a JSON number or
		 * a quoted string. Read it as a variant rather than assuming. */
		const QJsonValue balance = body.value(QStringLiteral("balance"));
		const QString text = balance.isString() ? balance.toString()
							: QString::number(balance.toDouble());
		m_walletLabel->setText(tr("<b>Wallet</b>: %1 credits")
					       .arg(text.isEmpty() ? QStringLiteral("0") : text));
	});

	const QString streamId = ch->streamId();
	if (streamId.isEmpty()) {
		m_inFlight = false;
		setNotice(tr("No Xive channel yet."));
		return;
	}

	api->giftsLeaderboard(streamId, [this](bool ok, const QJsonObject &body, int,
					       const QString &error) {
		m_inFlight = false;

		if (!ok) {
			setNotice(error);
			return;
		}

		const QJsonValue totalVal = body.value(QStringLiteral("total_credits"));
		const QString total = totalVal.isString() ? totalVal.toString()
							  : QString::number(totalVal.toDouble());
		m_totalLabel->setText(tr("<b>This broadcast</b>: %1 credits in gifts")
					      .arg(total.isEmpty() ? QStringLiteral("0") : total));

		const QJsonArray gifters = body.value(QStringLiteral("top_gifters")).toArray();
		if (gifters.isEmpty()) {
			m_view->clear();
			setNotice(tr("No gifts yet this broadcast."));
			return;
		}

		setNotice(QString());

		QString html;
		for (const QJsonValue &v : gifters) {
			const QJsonObject g = v.toObject();

			/* display_name is null unless the sender has a profile row, so fall back
			 * through username to the email the gift was recorded against. */
			QString who = g.value(QStringLiteral("display_name")).toString();
			if (who.isEmpty())
				who = g.value(QStringLiteral("username")).toString();
			if (who.isEmpty())
				who = g.value(QStringLiteral("sender_email")).toString();
			if (who.isEmpty())
				who = tr("Someone");

			const QJsonValue creditsVal = g.value(QStringLiteral("total_credits"));
			const QString credits = creditsVal.isString()
							? creditsVal.toString()
							: QString::number(creditsVal.toDouble());

			html += QStringLiteral(
					"<div style=\"margin:3px 0;\"><b>%1.</b> %2 — "
					"<span style=\"color:#ffd34d;\">%3</span></div>")
					.arg(g.value(QStringLiteral("rank")).toInt())
					.arg(who.toHtmlEscaped(), credits.toHtmlEscaped());
		}

		m_view->setHtml(html);
	});
}
