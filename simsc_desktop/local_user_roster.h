// Persistent {alias, api_key} roster for local games.
//
// Storage: ~/.config/openbw/simsc_desktop/local_users.json
// (on macOS: ~/Library/Application Support/openbw/simsc_desktop/local_users.json).
//
// The roster's sole purpose is to let the user pick from a
// remembered list of "local personas" when launching a local
// game, so the flow matches what they'll do against the remote
// server (pick a user by alias, not by typing an API key each
// time).
//
// Roster entries are not accounts. Anyone who has this JSON file
// has both the alias and the plaintext api_key. That's fine —
// this is a local dev tool, and the same keys go through the
// simsc REST layer over TLS for the remote flow.

#pragma once

#include <QtCore/QAbstractTableModel>
#include <QtCore/QString>
#include <QtCore/QVector>

namespace simsc_desktop {

struct LocalUser {
	QString alias;
	QString api_key;
};

// Qt table model: two columns (alias, api_key), one row per user.
// SettingsTab binds this to a QTableView with a "+"/"-" pair of
// buttons for add/remove. All mutation goes through the model API
// so views auto-refresh; nothing else touches the vector.
class LocalUserRoster : public QAbstractTableModel {
	Q_OBJECT
public:
	enum Column {
		Alias   = 0,
		ApiKey  = 1,
		ColumnCount
	};

	explicit LocalUserRoster(QObject* parent = nullptr);

	// Load or reload from disk. Silently starts empty on a
	// missing/malformed file; malformed file is *not* wiped
	// automatically (the user can inspect + edit by hand).
	void load();
	// Persist current state to disk. Called explicitly by the
	// settings tab's "Save" action; not on every mutation.
	bool save() const;

	// Convenience read.
	const QVector<LocalUser>& users() const { return users_; }

	// Look up a user's api_key by alias. Returns empty if absent.
	QString apiKeyFor(const QString& alias) const;
	// SHA256 hex of api_key -- used to build --user-hash for
	// openbw_server so plaintext keys never appear in `ps`.
	QString hashFor(const QString& alias) const;

	// Row-level mutation. Both signal model changes.
	void addUser(const QString& alias, const QString& api_key);
	void removeUserAt(int row);

	// Cryptographically-random api_key for a fresh local user.
	// Shape matches the remote server's `sk-<base64url>` convention
	// so an agent's key-parsing code (if any) works the same way
	// across local and remote. Not tied to the roster's contents --
	// caller uses it to build a new user, then addUser()'s it in.
	static QString generateApiKey();

	// QAbstractTableModel implementation.
	int rowCount(const QModelIndex& parent = {}) const override;
	int columnCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& idx, int role) const override;
	QVariant headerData(int section, Qt::Orientation, int) const override;
	Qt::ItemFlags flags(const QModelIndex& idx) const override;
	bool setData(const QModelIndex& idx, const QVariant& v, int role) override;

private:
	static QString rosterPath();
	static QString sha256Hex(const QString& s);

	QVector<LocalUser> users_;
};

}   // namespace simsc_desktop
