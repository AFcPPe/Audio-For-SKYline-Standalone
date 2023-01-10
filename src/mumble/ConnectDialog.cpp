// Copyright 2007-2022 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ConnectDialog.h"

#ifdef USE_ZEROCONF
#	include "Zeroconf.h"
#endif

#include "Channel.h"
#include "Database.h"
#include "ServerHandler.h"
#include "ServerResolver.h"
#include "Utils.h"
#include "WebFetch.h"
#include "Global.h"

#include <QSettings>
#include <QtCore/QMimeData>
#include <QtCore/QUrlQuery>
#include <QtCore/QtEndian>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QPainter>
#include <QtNetwork/QUdpSocket>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QShortcut>
#include <QtXml/QDomDocument>

#include <boost/accumulators/statistics/extended_p_square.hpp>
#include <boost/array.hpp>

#ifdef Q_OS_WIN
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <shlobj.h>
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#	include <QRandomGenerator>
#endif

#include <algorithm>

QMap< QString, QIcon > ServerItem::qmIcons;
QList< PublicInfo > ConnectDialog::qlPublicServers;
QString ConnectDialog::qsUserCountry, ConnectDialog::qsUserCountryCode, ConnectDialog::qsUserContinentCode;
Timer ConnectDialog::tPublicServers;


PingStats::PingStats() {
	init();
}

PingStats::~PingStats() {
	delete asQuantile;
}

void PingStats::init() {
	boost::array< double, 3 > probs = { { 0.75, 0.80, 0.95 } };

	asQuantile  = new asQuantileType(boost::accumulators::tag::extended_p_square::probabilities = probs);
	dPing       = 0.0;
	uiPing      = 0;
	uiPingSort  = 0;
	uiUsers     = 0;
	uiMaxUsers  = 0;
	uiBandwidth = 0;
	uiSent      = 0;
	uiRecv      = 0;
	m_version   = Version::UNKNOWN;
}

void PingStats::reset() {
	delete asQuantile;
	init();
}

ServerViewDelegate::ServerViewDelegate(QObject *p) : QStyledItemDelegate(p) {
}

ServerViewDelegate::~ServerViewDelegate() {
}

void ServerViewDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
	// Allow a ServerItem's BackgroundRole to override the current theme's default color.
	QVariant bg = index.data(Qt::BackgroundRole);
	if (bg.isValid()) {
		painter->fillRect(option.rect, bg.value< QBrush >());
	}

	QStyledItemDelegate::paint(painter, option, index);
}

ServerView::ServerView(QWidget *p) : QTreeWidget(p) {
	siFavorite = new ServerItem(tr("Favorite"), ServerItem::FavoriteType);
	addTopLevelItem(siFavorite);
	siFavorite->setExpanded(true);
	siFavorite->setHidden(true);

#ifdef USE_ZEROCONF
	siLAN = new ServerItem(tr("LAN"), ServerItem::LANType);
	addTopLevelItem(siLAN);
	siLAN->setExpanded(true);
	siLAN->setHidden(true);
#else
	siLAN         = nullptr;
#endif

	if (!Global::get().s.bDisablePublicList) {
		siPublic = new ServerItem(tr("Public Internet"), ServerItem::PublicType);
		siPublic->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
		addTopLevelItem(siPublic);

		siPublic->setExpanded(false);
	} else {
		qWarning() << "Public list disabled";

		siPublic = nullptr;
	}
}

ServerView::~ServerView() {
	delete siFavorite;
	delete siLAN;
	delete siPublic;
}

QMimeData *ServerView::mimeData(const QList< QTreeWidgetItem * > mimeitems) const {
	if (mimeitems.isEmpty())
		return nullptr;

	ServerItem *si = static_cast< ServerItem * >(mimeitems.first());
	return si->toMimeData();
}

QStringList ServerView::mimeTypes() const {
	QStringList qsl;
	qsl << QStringList(QLatin1String("text/uri-list"));
	qsl << QStringList(QLatin1String("text/plain"));
	return qsl;
}

Qt::DropActions ServerView::supportedDropActions() const {
	return Qt::CopyAction | Qt::LinkAction;
}

/* Extract and append (2), (3) etc to the end of a servers name if it is cloned. */
void ServerView::fixupName(ServerItem *si) {
	QString name = si->qsName;

	int tag = 1;

	QRegExp tmatch(QLatin1String("(.+)\\((\\d+)\\)"));
	tmatch.setMinimal(true);
	if (tmatch.exactMatch(name)) {
		name = tmatch.capturedTexts().at(1).trimmed();
		tag  = tmatch.capturedTexts().at(2).toInt();
	}

	bool found;
	QString cmpname;
	do {
		found = false;
		if (tag > 1)
			cmpname = name + QString::fromLatin1(" (%1)").arg(tag);
		else
			cmpname = name;

		foreach (ServerItem *f, siFavorite->qlChildren)
			if (f->qsName == cmpname)
				found = true;

		++tag;
	} while (found);

	si->qsName = cmpname;
}

bool ServerView::dropMimeData(QTreeWidgetItem *, int, const QMimeData *mime, Qt::DropAction) {
	ServerItem *si = ServerItem::fromMimeData(mime);
	if (!si)
		return false;

	fixupName(si);

	qobject_cast< ConnectDialog * >(parent())->qlItems << si;
	siFavorite->addServerItem(si);

	qobject_cast< ConnectDialog * >(parent())->startDns(si);

	setCurrentItem(si);

	return true;
}

void ServerItem::init() {
	// Without this, columncount is wrong.
	setData(0, Qt::DisplayRole, QVariant());
	setData(1, Qt::DisplayRole, QVariant());
	setData(2, Qt::DisplayRole, QVariant());
	emitDataChanged();
	qsHostname = Global::get().SklineIP;
	qsCountryCode   = "cn";
	qsCountry       = "China";
}

ServerItem::ServerItem(const FavoriteServer &fs) : QTreeWidgetItem(QTreeWidgetItem::UserType) {
	siParent = nullptr;
	bParent  = false;

	itType = FavoriteType;
	qsName = fs.qsName;
	usPort = fs.usPort;

	qsUsername = fs.qsUsername;
	qsPassword = fs.qsPassword;

	qsUrl = fs.qsUrl;

	bCA = false;
#ifdef USE_ZEROCONF
	if (fs.qsHostname.startsWith(QLatin1Char('@'))) {
		zeroconfHost   = fs.qsHostname.mid(1);
		zeroconfRecord = BonjourRecord(zeroconfHost, QLatin1String("_mumble._tcp."), QLatin1String("local."));
	} else {
		qsHostname = fs.qsHostname;
	}
#else
	qsHostname    = fs.qsHostname;
#endif
	init();
}

ServerItem::ServerItem(const PublicInfo &pi) : QTreeWidgetItem(QTreeWidgetItem::UserType) {
	siParent        = nullptr;
	bParent         = false;
	itType          = PublicType;
	qsName          = pi.qsName;
	qsHostname      = pi.qsIp;
	usPort          = pi.usPort;
	qsUrl           = pi.quUrl.toString();
	qsCountry       = pi.qsCountry;
	qsCountryCode   = pi.qsCountryCode;
	qsContinentCode = pi.qsContinentCode;
	bCA             = pi.bCA;

	init();
}

ServerItem::ServerItem(const QString &name, const QString &host, unsigned short port, const QString &username,
					   const QString &password)
	: QTreeWidgetItem(QTreeWidgetItem::UserType) {
	siParent   = nullptr;
	bParent    = false;
	itType     = FavoriteType;
	qsName     = name;
	usPort     = port;
	qsUsername = username;
	qsPassword = password;

	bCA = false;
#ifdef USE_ZEROCONF
	if (host.startsWith(QLatin1Char('@'))) {
		zeroconfHost   = host.mid(1);
		zeroconfRecord = BonjourRecord(zeroconfHost, QLatin1String("_mumble._tcp."), QLatin1String("local."));
	} else {
		qsHostname = host;
	}
#else
	qsHostname    = host;
#endif
	init();
}

#ifdef USE_ZEROCONF
ServerItem::ServerItem(const BonjourRecord &br) : QTreeWidgetItem(QTreeWidgetItem::UserType) {
	siParent       = nullptr;
	bParent        = false;
	itType         = LANType;
	qsName         = br.serviceName;
	zeroconfHost   = qsName;
	zeroconfRecord = br;
	usPort         = 0;
	bCA            = false;

	init();
}
#endif

ServerItem::ServerItem(const QString &name, ItemType itype) {
	siParent = nullptr;
	bParent  = true;
	qsName   = name;
	itType   = itype;
	setFlags(flags() & ~Qt::ItemIsDragEnabled);
	bCA = false;

	init();
}

ServerItem::ServerItem(const ServerItem *si) {
	siParent = nullptr;
	bParent  = false;
	itType   = FavoriteType;

	qsName          = si->qsName;
	qsHostname      = si->qsHostname;
	usPort          = si->usPort;
	qsUsername      = si->qsUsername;
	qsPassword      = si->qsPassword;
	qsCountry       = si->qsCountry;
	qsCountryCode   = si->qsCountryCode;
	qsContinentCode = si->qsContinentCode;
	qsUrl           = si->qsUrl;
#ifdef USE_ZEROCONF
	zeroconfHost   = si->zeroconfHost;
	zeroconfRecord = si->zeroconfRecord;
#endif
	qlAddresses = si->qlAddresses;
	bCA         = si->bCA;

	m_version   = si->m_version;
	uiPing      = si->uiPing;
	uiPingSort  = si->uiPing;
	uiUsers     = si->uiUsers;
	uiMaxUsers  = si->uiMaxUsers;
	uiBandwidth = si->uiBandwidth;
	uiSent      = si->uiSent;
	dPing       = si->dPing;
	*asQuantile = *si->asQuantile;
}

ServerItem::~ServerItem() {
	if (siParent) {
		siParent->qlChildren.removeAll(this);
		if (siParent->bParent && siParent->qlChildren.isEmpty())
			siParent->setHidden(true);
	}

	// This is just for cleanup when exiting the dialog, it won't stop pending DNS for the children.
	foreach (ServerItem *si, qlChildren)
		delete si;
}

ServerItem *ServerItem::fromMimeData(const QMimeData *mime, bool default_name, QWidget *p, bool convertHttpUrls) {
	if (mime->hasFormat(QLatin1String("OriginatedInMumble")))
		return nullptr;

	QUrl url;
	if (mime->hasUrls() && !mime->urls().isEmpty())
		url = mime->urls().at(0);
	else if (mime->hasText())
		url = QUrl::fromEncoded(mime->text().toUtf8());

	QString qsFile = url.toLocalFile();
	if (!qsFile.isEmpty()) {
		QFile f(qsFile);
		// Make sure we don't accidentally read something big the user
		// happened to have in his clipboard. We only want to look
		// at small link files.
		if (f.open(QIODevice::ReadOnly) && f.size() < 10240) {
			QByteArray qba = f.readAll();
			f.close();

			url = QUrl::fromEncoded(qba, QUrl::StrictMode);
			if (!url.isValid()) {
				// Windows internet shortcut files (.url) are an ini with an URL value
				QSettings qs(qsFile, QSettings::IniFormat);
				url =
					QUrl::fromEncoded(qs.value(QLatin1String("InternetShortcut/URL")).toByteArray(), QUrl::StrictMode);
			}
		}
	}

	if (default_name) {
		QUrlQuery query(url);
		if (!query.hasQueryItem(QLatin1String("title"))) {
			query.addQueryItem(QLatin1String("title"), url.host());
		}
	}

	if (!url.isValid()) {
		return nullptr;
	}

	// An URL from text without a scheme will have the hostname text
	// in the QUrl scheme and no hostname. We do not want to use that.
	if (url.host().isEmpty()) {
		return nullptr;
	}

	// Some communication programs automatically create http links from domains.
	// When a user sends another user a domain to connect to, and http is added wrongly,
	// we do our best to remove it again.
	if (convertHttpUrls && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))) {
		url.setScheme(QLatin1String("mumble"));
	}

	return fromUrl(url, p);
}

ServerItem *ServerItem::fromUrl(QUrl url, QWidget *p) {
	if (!url.isValid() || (url.scheme() != QLatin1String("mumble"))) {
		return nullptr;
	}

	QUrlQuery query(url);

	if (url.userName().isEmpty()) {
		if (Global::get().s.qsUsername.isEmpty()) {
			bool ok;
			QString defUserName = QInputDialog::getText(p, ConnectDialog::tr("Adding host %1").arg(url.host()),
														ConnectDialog::tr("Enter username"), QLineEdit::Normal,
														Global::get().s.qsUsername, &ok)
									  .trimmed();
			if (!ok)
				return nullptr;
			if (defUserName.isEmpty())
				return nullptr;
			Global::get().s.qsUsername = defUserName;
		}
		url.setUserName(Global::get().s.qsUsername);
	}

	ServerItem *si =
		new ServerItem(query.queryItemValue(QLatin1String("title")), url.host(),
					   static_cast< unsigned short >(url.port(DEFAULT_MUMBLE_PORT)), url.userName(), url.password());

	if (query.hasQueryItem(QLatin1String("url")))
		si->qsUrl = query.queryItemValue(QLatin1String("url"));

	return si;
}

QVariant ServerItem::data(int column, int role) const {
	if (bParent) {
		if (column == 0) {
			switch (role) {
				case Qt::DisplayRole:
					return qsName;
				case Qt::DecorationRole:
					if (itType == FavoriteType)
						return loadIcon(QLatin1String("skin:emblems/emblem-favorite.svg"));
					else if (itType == LANType)
						return loadIcon(QLatin1String("skin:places/network-workgroup.svg"));
					else
						return loadIcon(QLatin1String("skin:categories/applications-internet.svg"));
			}
		}
	} else {
		if (role == Qt::DecorationRole && column == 0) {
			QString flag;
			if (!qsCountryCode.isEmpty()) {
				flag = QString::fromLatin1(":/flags/%1.svg").arg(qsCountryCode);
				if (!QFileInfo(flag).exists()) {
					flag = QLatin1String("skin:categories/applications-internet.svg");
				}
			} else {
				flag = QLatin1String("skin:categories/applications-internet.svg");
			}
			return loadIcon(flag);
		}
		if (role == Qt::DisplayRole) {
			switch (column) {
				case 0:
					return qsName;
				case 1:
					return (dPing > 0.0) ? QString::number(uiPing) : QVariant();
				case 2:
					return uiUsers ? QString::fromLatin1("%1/%2 ").arg(uiUsers).arg(uiMaxUsers) : QVariant();
			}
		} else if (role == Qt::ToolTipRole) {
			QStringList ipv4List;
			QStringList ipv6List;
			foreach (const ServerAddress &addr, qlAddresses) {
				const QString address = addr.host.toString(false).toHtmlEscaped();
				if (addr.host.isV6()) {
					ipv6List << address;
				} else {
					ipv4List << address;
				}
			}
			QString ipv4 = "-";
			QString ipv6 = "-";
			if (!ipv4List.isEmpty()) {
				ipv4 = ipv4List.join(QLatin1String(", "));
			}
			if (!ipv6List.isEmpty()) {
				ipv6 = ipv6List.join(QLatin1String(", "));
			}

			double ploss = 100.0;

			if (uiSent > 0)
				ploss = (uiSent - std::min(uiRecv, uiSent)) * 100. / uiSent;

			QString qs;
			qs += QLatin1String("<table>")
				  + QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
						.arg(ConnectDialog::tr("Servername"), qsName.toHtmlEscaped())
				  + QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
						.arg(ConnectDialog::tr("Hostname"), qsHostname.toHtmlEscaped());
#ifdef USE_ZEROCONF
			if (!zeroconfHost.isEmpty())
				qs += QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
						  .arg(ConnectDialog::tr("Bonjour name"), zeroconfHost.toHtmlEscaped());
#endif
			qs += QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
					  .arg(ConnectDialog::tr("Port"))
					  .arg(usPort)
				  + QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
						.arg(ConnectDialog::tr("IPv4 address"), ipv4)
				  + QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
						.arg(ConnectDialog::tr("IPv6 address"), ipv6);

			if (!qsUrl.isEmpty())
				qs += QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
						  .arg(ConnectDialog::tr("Website"), qsUrl.toHtmlEscaped());

			if (uiSent > 0) {
				qs += QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
						  .arg(ConnectDialog::tr("Packet loss"), QString::fromLatin1("%1% (%2/%3)")
																	 .arg(ploss, 0, 'f', 1)
																	 .arg(uiSent - std::min(uiRecv, uiSent))
																	 .arg(uiSent));
				if (uiRecv > 0) {
					qs += QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
							  .arg(ConnectDialog::tr("Ping (80%)"),
								   ConnectDialog::tr("%1 ms").arg(
									   boost::accumulators::extended_p_square(*asQuantile)[1] / 1000., 0, 'f', 2))
						  + QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
								.arg(ConnectDialog::tr("Ping (95%)"),
									 ConnectDialog::tr("%1 ms").arg(
										 boost::accumulators::extended_p_square(*asQuantile)[2] / 1000., 0, 'f', 2))
						  + QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
								.arg(ConnectDialog::tr("Bandwidth"),
									 ConnectDialog::tr("%1 kbit/s").arg(uiBandwidth / 1000))
						  + QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
								.arg(ConnectDialog::tr("Users"),
									 QString::fromLatin1("%1/%2").arg(uiUsers).arg(uiMaxUsers))
						  + QString::fromLatin1("<tr><th align=left>%1</th><td>%2</td></tr>")
								.arg(ConnectDialog::tr("Version"))
								.arg(Version::toString(m_version));
				}
			}
			qs += QLatin1String("</table>");
			return qs;
		} else if (role == Qt::BackgroundRole) {
			if (bCA) {
				QColor qc(Qt::green);
				qc.setAlpha(32);
				return qc;
			}
		}
	}
	return QTreeWidgetItem::data(column, role);
}

void ServerItem::addServerItem(ServerItem *childitem) {
	Q_ASSERT(!childitem->siParent);

	childitem->siParent = this;
	qlChildren.append(childitem);
	addChild(childitem);
	// Public servers must initially be hidden for the search to work properly
	// They will be set to visible later on
	if (childitem->itType == PublicType) {
		childitem->setHidden(true);
	}

	if (bParent && (itType != PublicType) && isHidden()) {
		setHidden(false);
	}
}

void ServerItem::setDatas(double elapsed, quint32 users, quint32 maxusers) {
	if (elapsed == 0.0) {
		emitDataChanged();
		return;
	}

	(*asQuantile)(static_cast< double >(elapsed));
	dPing = boost::accumulators::extended_p_square(*asQuantile)[0];
	if (dPing == 0.0)
		dPing = elapsed;

	quint32 ping = static_cast< quint32 >(lround(dPing / 1000.));
	uiRecv       = static_cast< quint32 >(boost::accumulators::count(*asQuantile));

	bool changed = (ping != uiPing) || (users != uiUsers) || (maxusers != uiMaxUsers);

	uiUsers    = users;
	uiMaxUsers = maxusers;
	uiPing     = ping;

	double grace = qMax(5000., 50. * uiPingSort);
	double diff  = fabs(1000. * uiPingSort - dPing);

	if ((uiPingSort == 0) || ((uiSent >= 10) && (diff >= grace)))
		uiPingSort = ping;

	if (changed)
		emitDataChanged();
}

FavoriteServer ServerItem::toFavoriteServer() const {
	FavoriteServer fs;
	fs.qsName = qsName;
#ifdef USE_ZEROCONF
	if (!zeroconfHost.isEmpty())
		fs.qsHostname = QLatin1Char('@') + zeroconfHost;
	else
		fs.qsHostname = qsHostname;
#else
	fs.qsHostname = qsHostname;
#endif
	fs.usPort     = usPort;
	fs.qsUsername = qsUsername;
	fs.qsPassword = qsPassword;
	fs.qsUrl      = qsUrl;
	return fs;
}

/**
 * This function turns a ServerItem object into a QMimeData object holding a URL to the server.
 */
QMimeData *ServerItem::toMimeData() const {
	QMimeData *mime = ServerItem::toMimeData(qsName, qsHostname, usPort);

	if (itType == FavoriteType)
		mime->setData(QLatin1String("OriginatedInMumble"), QByteArray());

	return mime;
}

/**
 * This function creates a QMimeData object containing a URL to the server at host and port. name is passed in the
 * query string as "title", which is used for adding a server to favorites. channel may be omitted, but if specified it
 * should be in the format of "/path/to/channel".
 */
QMimeData *ServerItem::toMimeData(const QString &name, const QString &host, unsigned short port,
								  const QString &channel) {
	QUrl url;
	url.setScheme(QLatin1String("mumble"));
	url.setHost(host);
	if (port != DEFAULT_MUMBLE_PORT)
		url.setPort(port);
	url.setPath(channel);

	QUrlQuery query;
	query.addQueryItem(QLatin1String("title"), name);
	query.addQueryItem(QLatin1String("version"), QLatin1String("1.2.0"));
	url.setQuery(query);

	QString qs = QLatin1String(url.toEncoded());

	QMimeData *mime = new QMimeData;

#ifdef Q_OS_WIN
	QString contents = QString::fromLatin1("[InternetShortcut]\r\nURL=%1\r\n").arg(qs);
	QString urlname  = QString::fromLatin1("%1.url").arg(name);

	FILEGROUPDESCRIPTORA fgda;
	ZeroMemory(&fgda, sizeof(fgda));
	fgda.cItems              = 1;
	fgda.fgd[0].dwFlags      = FD_LINKUI | FD_FILESIZE;
	fgda.fgd[0].nFileSizeLow = contents.length();
	strcpy_s(fgda.fgd[0].cFileName, MAX_PATH, urlname.toLocal8Bit().constData());
	mime->setData(QLatin1String("FileGroupDescriptor"),
				  QByteArray(reinterpret_cast< const char * >(&fgda), sizeof(fgda)));

	FILEGROUPDESCRIPTORW fgdw;
	ZeroMemory(&fgdw, sizeof(fgdw));
	fgdw.cItems              = 1;
	fgdw.fgd[0].dwFlags      = FD_LINKUI | FD_FILESIZE;
	fgdw.fgd[0].nFileSizeLow = contents.length();
	wcscpy_s(fgdw.fgd[0].cFileName, MAX_PATH, urlname.toStdWString().c_str());
	mime->setData(QLatin1String("FileGroupDescriptorW"),
				  QByteArray(reinterpret_cast< const char * >(&fgdw), sizeof(fgdw)));

	mime->setData(QString::fromWCharArray(CFSTR_FILECONTENTS), contents.toLocal8Bit());

	DWORD context[4];
	context[0] = 0;
	context[1] = 1;
	context[2] = 0;
	context[3] = 0;
	mime->setData(QLatin1String("DragContext"),
				  QByteArray(reinterpret_cast< const char * >(&context[0]), sizeof(context)));

	DWORD dropaction;
	dropaction = DROPEFFECT_LINK;
	mime->setData(QString::fromWCharArray(CFSTR_PREFERREDDROPEFFECT),
				  QByteArray(reinterpret_cast< const char * >(&dropaction), sizeof(dropaction)));
#endif
	QList< QUrl > urls;
	urls << url;
	mime->setUrls(urls);

	mime->setText(qs);
	mime->setHtml(QString::fromLatin1("<a href=\"%1\">%2</a>").arg(qs).arg(name.toHtmlEscaped()));

	return mime;
}

bool ServerItem::operator<(const QTreeWidgetItem &o) const {
	const ServerItem &other = static_cast< const ServerItem & >(o);
	const QTreeWidget *w    = treeWidget();

	const int column = w ? w->sortColumn() : 0;

	if (itType != other.itType) {
		const bool inverse = w ? (w->header()->sortIndicatorOrder() == Qt::DescendingOrder) : false;
		bool less;

		if (itType == FavoriteType)
			less = true;
		else if ((itType == LANType) && (other.itType == PublicType))
			less = true;
		else
			less = false;
		return less ^ inverse;
	}

	if (bParent) {
		const bool inverse = w ? (w->header()->sortIndicatorOrder() == Qt::DescendingOrder) : false;
		return (qsName < other.qsName) ^ inverse;
	}

	if (column == 0) {
		QString a = qsName.toLower();
		QString b = other.qsName.toLower();

		QRegExp re(QLatin1String("[^0-9a-z]"));
		a.remove(re);
		b.remove(re);
		return a < b;
	} else if (column == 1) {
		quint32 a = uiPingSort ? uiPingSort : UINT_MAX;
		quint32 b = other.uiPingSort ? other.uiPingSort : UINT_MAX;
		return a < b;
	} else if (column == 2) {
		return uiUsers < other.uiUsers;
	}
	return false;
}

QIcon ServerItem::loadIcon(const QString &name) {
	if (!qmIcons.contains(name))
		qmIcons.insert(name, QIcon(name));
	return qmIcons.value(name);
}

ConnectDialogEdit::ConnectDialogEdit(QWidget *p, const QString &name, const QString &host, const QString &user,
									 unsigned short port, const QString &password)
	: QDialog(p) {
	setupUi(this);
	init();

	bCustomLabel = !name.simplified().isEmpty();

	qleName->setText(name);
	qleServer->setText(host);
	qleUsername->setText(user);
	qlePort->setText(QString::number(port));
	qlePassword->setText(password);

	validate();
}

ConnectDialogEdit::ConnectDialogEdit(QWidget *parent) : QDialog(parent) {
	setupUi(this);
	setWindowTitle(tr("Add Server"));
	init();

	if (!updateFromClipboard()) {
		// If connected to a server assume the user wants to add it
		if (Global::get().sh && Global::get().sh->isRunning()) {
			QString host, name, user, pw;
			unsigned short port = DEFAULT_MUMBLE_PORT;

			Global::get().sh->getConnectionInfo(host, port, user, pw);
			Channel *c = Channel::get(Channel::ROOT_ID);
			if (c && c->qsName != QLatin1String("Root")) {
				name = c->qsName;
			}

			showNotice(tr("You are currently connected to a server.\nDo you want to fill the dialog with the "
						  "connection data of this server?\nHost: %1 Port: %2")
						   .arg(host)
						   .arg(port));
			m_si = new ServerItem(name, host, port, user, pw);
		}
	}
	qleUsername->setText(Global::get().s.qsUsername);
}

void ConnectDialogEdit::init() {
	//qleServer->setText("SKYline服务器");
	qleServer->setEnabled(false);
	m_si         = nullptr;
	usPort       = 0;
	bOk          = true;
	bCustomLabel = false;

	qwInlineNotice->hide();

	qlePort->setValidator(new QIntValidator(1, 65535, qlePort));
	qlePort->setText(QString::number(DEFAULT_MUMBLE_PORT));
	qlePassword->setEchoMode(QLineEdit::Password);

	connect(qleName, SIGNAL(textChanged(const QString &)), this, SLOT(validate()));
	connect(qleServer, SIGNAL(textChanged(const QString &)), this, SLOT(validate()));
	connect(qlePort, SIGNAL(textChanged(const QString &)), this, SLOT(validate()));
	connect(qleUsername, SIGNAL(textChanged(const QString &)), this, SLOT(validate()));
	connect(qlePassword, SIGNAL(textChanged(const QString &)), this, SLOT(validate()));

	validate();
}

ConnectDialogEdit::~ConnectDialogEdit() {
	delete m_si;
}

void ConnectDialogEdit::showNotice(const QString &text) {
	QLabel *label = qwInlineNotice->findChild< QLabel * >(QLatin1String("qlPasteNotice"));
	Q_ASSERT(label);
	label->setText(text);
	qwInlineNotice->show();
	adjustSize();
}

bool ConnectDialogEdit::updateFromClipboard() {
	delete m_si;
	m_si = ServerItem::fromMimeData(QApplication::clipboard()->mimeData(), false, nullptr, true);
	if (m_si) {
		showNotice(
			tr("You have an URL in your clipboard.\nDo you want to fill the dialog with this data?\nHost: %1 Port: %2")
				.arg(m_si->qsHostname)
				.arg(m_si->usPort));
		return true;
	} else {
		qwInlineNotice->hide();
		adjustSize();
		return false;
	}
}

void ConnectDialogEdit::on_qbFill_clicked() {
	Q_ASSERT(m_si);

	qwInlineNotice->hide();
	adjustSize();

	qleName->setText(m_si->qsName);
	qleServer->setText(m_si->qsHostname);
	qleUsername->setText(m_si->qsUsername);
	qlePort->setText(QString::number(m_si->usPort));
	qlePassword->setText(m_si->qsPassword);

	delete m_si;
	m_si = nullptr;
}

void ConnectDialogEdit::on_qbDiscard_clicked() {
	qwInlineNotice->hide();
	adjustSize();
}

void ConnectDialogEdit::on_qleName_textEdited(const QString &name) {
	if (bCustomLabel) {
		// If empty, then reset to automatic label.
		// NOTE(nik@jnstw.us): You may be tempted to set qleName to qleServer, but that results in the odd
		// UI behavior that clearing the field doesn't clear it; it'll immediately equal qleServer. Instead,
		// leave it empty and let it update the next time qleServer updates. Code in accept will default it
		// to qleServer if it isn't updated beforehand.
		if (name.simplified().isEmpty()) {
			bCustomLabel = false;
		}
	} else {
		// If manually edited, set to Custom
		bCustomLabel = true;
	}
}

void ConnectDialogEdit::on_qleServer_textEdited(const QString &server) {
	// If using automatic label, update it
	//qleServer->setText("SKYline服务器");
	if (!bCustomLabel) {
		qleName->setText(server);
	}
}

void ConnectDialogEdit::validate() {
	qsName     = qleName->text().simplified();
	qsHostname = qleServer->text().simplified();
	usPort     = qlePort->text().toUShort();
	qsUsername = qleUsername->text().simplified();
	qsPassword = qlePassword->text();

	// For bonjour hosts disable the port field as it's auto-detected
	qlePort->setDisabled(!qsHostname.isEmpty() && qsHostname.startsWith(QLatin1Char('@')));

	// For SuperUser show password edit
	if (qsUsername.toLower() == QLatin1String("superuser")) {
		qliPassword->setVisible(true);
		qlePassword->setVisible(true);
		qcbShowPassword->setVisible(true);
		adjustSize();
	} else if (qsPassword.isEmpty()) {
		qliPassword->setVisible(false);
		qlePassword->setVisible(false);
		qcbShowPassword->setVisible(false);
		adjustSize();
	}

	bOk = !qsHostname.isEmpty() && !qsUsername.isEmpty() && usPort;
	qdbbButtonBox->button(QDialogButtonBox::Ok)->setEnabled(bOk);
}

void ConnectDialogEdit::accept() {
	qleServer->setText(Global::get().SklineIP);
	validate();
	if (bOk) {
		QString server = qleServer->text().simplified();

		// If the user accidentally added a schema or path part, drop it now.
		// We can't do so during editing as that is quite jarring.
		const int schemaPos = server.indexOf(QLatin1String("://"));
		if (schemaPos != -1) {
			server.remove(0, schemaPos + 3);
		}

		const int pathPos = server.indexOf(QLatin1Char('/'));
		if (pathPos != -1) {
			server.resize(pathPos);
		}

		qleServer->setText(server);

		if (qleName->text().simplified().isEmpty() || !bCustomLabel) {
			qleName->setText(server);
		}

		QDialog::accept();
	}
}

void ConnectDialogEdit::on_qcbShowPassword_toggled(bool checked) {
	qlePassword->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
}

ConnectDialog::ConnectDialog(QWidget *p, bool autoconnect) : QDialog(p), bAutoConnect(autoconnect) {
	setupUi(this);
	qlbUsername->setText(tr("Username:"));
	
	
}

ConnectDialog::~ConnectDialog() {
}

void ConnectDialog::accept() {
	
	if (this->qleUsername->text() == "")
		return;
	this->qsUsername = this->qleUsername->text();
	QDialog::accept();
}

void ConnectDialog::OnSortChanged(int logicalIndex, Qt::SortOrder) {
	
}

void ConnectDialog::on_qaFavoriteAdd_triggered() {
	
}

void ConnectDialog::on_qaFavoriteAddNew_triggered() {
	
}

void ConnectDialog::on_qaFavoriteEdit_triggered() {
	
}

void ConnectDialog::on_qaFavoriteRemove_triggered() {
	
}

void ConnectDialog::on_qaFavoriteCopy_triggered() {
	
}

void ConnectDialog::on_qaFavoritePaste_triggered() {
	
}

void ConnectDialog::on_qaUrl_triggered() {
	
}

void ConnectDialog::on_qtwServers_customContextMenuRequested(const QPoint &mpos) {
	
}

void ConnectDialog::on_qtwServers_itemDoubleClicked(QTreeWidgetItem *item, int) {
	
}

void ConnectDialog::on_qtwServers_currentItemChanged(QTreeWidgetItem *item, QTreeWidgetItem *) {
	
}

void ConnectDialog::on_qtwServers_itemExpanded(QTreeWidgetItem *item) {
	
}

void ConnectDialog::on_qtwServers_itemCollapsed(QTreeWidgetItem *item) {
	
}

void ConnectDialog::initList() {
	
}

void ConnectDialog::fillList() {
	
}

void ConnectDialog::timeTick() {
	
}

void ConnectDialog::filterPublicServerList() const {
	
}

void ConnectDialog::filterServer(ServerItem *const si) const {
	
}

void ConnectDialog::addCountriesToSearchLocation() const {
	
}

void ConnectDialog::startDns(ServerItem *si) {
	
}

void ConnectDialog::stopDns(ServerItem *si) {
	
}

void ConnectDialog::lookedUp() {
	
}

void ConnectDialog::sendPing(const QHostAddress &host, unsigned short port, Version::full_t protocolVersion) {
	
}

bool ConnectDialog::writePing(const QHostAddress &host, unsigned short port, Version::full_t protocolVersion,
							  const Mumble::Protocol::PingData &pingData) {
	
}

void ConnectDialog::udpReply() {
	
}

void ConnectDialog::fetched(QByteArray xmlData, QUrl, QMap< QString, QString > headers) {
	
}

void ConnectDialog::on_qleSearchServername_textChanged(const QString &searchServername) {
	
}

void ConnectDialog::on_qcbSearchLocation_currentIndexChanged(int searchLocationIndex) {

}

void ConnectDialog::on_qcbFilter_currentIndexChanged(int filterIndex) {

}
