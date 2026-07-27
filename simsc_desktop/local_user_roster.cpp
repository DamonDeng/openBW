// See local_user_roster.h.

#include "local_user_roster.h"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QStandardPaths>

namespace simsc_desktop {

LocalUserRoster::LocalUserRoster(QObject* parent)
	: QAbstractTableModel(parent) {}

QString LocalUserRoster::rosterPath() {
	// AppLocalDataLocation resolves to:
	//   macOS   ~/Library/Application Support/openbw/simsc_desktop
	//   Linux   ~/.local/share/openbw/simsc_desktop
	// Both are fine for this data class. We deliberately do NOT
	// use QStandardPaths::ConfigLocation because on macOS that
	// puts us in ~/Library/Preferences which is really meant for
	// QSettings plists.
	const auto dir = QStandardPaths::writableLocation(
		QStandardPaths::AppLocalDataLocation);
	QDir().mkpath(dir);
	return dir + "/local_users.json";
}

QString LocalUserRoster::generateApiKey() {
	// 24 random bytes -> ~32-char base64url string, matching the
	// remote server's `sk-<base64url>` convention. QRandomGenerator's
	// system() generator is CSPRNG-backed on every platform Qt6
	// supports, so this is safe for use as a shared secret between
	// the local server and the local agent process. (Not that a
	// local-machine-only key needs a huge amount of entropy, but
	// the shape parity matters more than the raw strength.)
	QByteArray bytes(24, 0);
	QRandomGenerator::system()->fillRange(
		reinterpret_cast<quint32*>(bytes.data()),
		bytes.size() / sizeof(quint32));
	const auto b64 = bytes.toBase64(
		QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
	return QStringLiteral("sk-") + QString::fromLatin1(b64);
}

QString LocalUserRoster::sha256Hex(const QString& s) {
	QCryptographicHash h(QCryptographicHash::Sha256);
	h.addData(s.toUtf8());
	return QString::fromLatin1(h.result().toHex());
}

void LocalUserRoster::load() {
	beginResetModel();
	users_.clear();

	QFile f(rosterPath());
	if (f.open(QIODevice::ReadOnly)) {
		const auto doc = QJsonDocument::fromJson(f.readAll());
		if (doc.isArray()) {
			for (const auto& v : doc.array()) {
				const auto o = v.toObject();
				const auto alias = o.value("alias").toString();
				const auto key   = o.value("api_key").toString();
				if (!alias.isEmpty()) {
					users_.push_back({alias, key});
				}
			}
		}
	}

	endResetModel();
}

bool LocalUserRoster::save() const {
	QJsonArray arr;
	for (const auto& u : users_) {
		QJsonObject o;
		o.insert("alias",   u.alias);
		o.insert("api_key", u.api_key);
		arr.push_back(o);
	}
	QFile f(rosterPath());
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
	f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
	return true;
}

QString LocalUserRoster::apiKeyFor(const QString& alias) const {
	for (const auto& u : users_) if (u.alias == alias) return u.api_key;
	return {};
}

QString LocalUserRoster::hashFor(const QString& alias) const {
	const auto k = apiKeyFor(alias);
	return k.isEmpty() ? QString() : sha256Hex(k);
}

void LocalUserRoster::addUser(const QString& alias, const QString& api_key) {
	beginInsertRows({}, users_.size(), users_.size());
	users_.push_back({alias, api_key});
	endInsertRows();
	save();
}

void LocalUserRoster::removeUserAt(int row) {
	if (row < 0 || row >= users_.size()) return;
	beginRemoveRows({}, row, row);
	users_.remove(row);
	endRemoveRows();
	save();
}

int LocalUserRoster::rowCount(const QModelIndex&) const {
	return users_.size();
}

int LocalUserRoster::columnCount(const QModelIndex&) const {
	return ColumnCount;
}

QVariant LocalUserRoster::data(const QModelIndex& idx, int role) const {
	if (!idx.isValid()) return {};
	if (role != Qt::DisplayRole && role != Qt::EditRole) return {};
	const auto& u = users_.at(idx.row());
	switch (idx.column()) {
		case Alias:  return u.alias;
		case ApiKey: return u.api_key;
	}
	return {};
}

QVariant LocalUserRoster::headerData(int section, Qt::Orientation o, int role) const {
	if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
	switch (section) {
		case Alias:  return QStringLiteral("Alias");
		case ApiKey: return QStringLiteral("API key");
	}
	return {};
}

Qt::ItemFlags LocalUserRoster::flags(const QModelIndex& idx) const {
	if (!idx.isValid()) return Qt::NoItemFlags;
	auto base = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
	// Alias remains user-editable (rename). API key is auto-generated
	// and MUST NOT be edited by hand -- editing it silently would
	// break any running agent using the previous value.
	if (idx.column() == Alias) base |= Qt::ItemIsEditable;
	return base;
}

bool LocalUserRoster::setData(const QModelIndex& idx, const QVariant& v, int role) {
	if (!idx.isValid() || role != Qt::EditRole) return false;
	auto& u = users_[idx.row()];
	switch (idx.column()) {
		case Alias:  u.alias   = v.toString(); break;
		case ApiKey: u.api_key = v.toString(); break;
		default: return false;
	}
	emit dataChanged(idx, idx, {Qt::DisplayRole, Qt::EditRole});
	save();
	return true;
}

}   // namespace simsc_desktop
