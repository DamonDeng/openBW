// See settings.h.
//
// One implementation note: we intentionally construct a fresh
// QSettings inside every getter and setter rather than keeping a
// member instance. QSettings caches lazily but does an fsync-worthy
// flush on QSettings::~ or ::sync(); constructing on-the-fly and
// letting the temporary destruct at end-of-scope keeps the on-disk
// file up-to-date without needing an explicit sync from callers.
// Cost is negligible (~microseconds).

#include "settings.h"

#include <QtCore/QSettings>

namespace simsc_desktop {

namespace {

// Key names collected in one place so a rename shows up in code
// review, not scattered string literals.
const auto kSc1DataPath        = QStringLiteral("sc1_data_path");
const auto kSimscApiKey        = QStringLiteral("simsc_api_key");
const auto kSimscBaseUrl       = QStringLiteral("simsc_base_url");
const auto kDefaultLocalPort   = QStringLiteral("default_local_port");
const auto kServerBinOverride  = QStringLiteral("server_binary_override");
const auto kFirstRunSeen       = QStringLiteral("first_run_seen");

}   // namespace

Settings::Settings(QObject* parent) : QObject(parent) {}

QString Settings::sc1_data_path() const {
	QSettings s;
	return s.value(kSc1DataPath).toString();
}

void Settings::set_sc1_data_path(const QString& v) {
	QSettings s;
	s.setValue(kSc1DataPath, v);
	emit changed();
}

QString Settings::simsc_api_key() const {
	QSettings s;
	return s.value(kSimscApiKey).toString();
}

void Settings::set_simsc_api_key(const QString& v) {
	QSettings s;
	s.setValue(kSimscApiKey, v);
	emit changed();
}

QString Settings::simsc_base_url() const {
	QSettings s;
	return s.value(kSimscBaseUrl,
		QStringLiteral("https://simsc.agentnumber47.com")).toString();
}

void Settings::set_simsc_base_url(const QString& v) {
	QSettings s;
	s.setValue(kSimscBaseUrl, v);
	emit changed();
}

int Settings::default_local_port() const {
	QSettings s;
	return s.value(kDefaultLocalPort, 6040).toInt();
}

void Settings::set_default_local_port(int v) {
	QSettings s;
	s.setValue(kDefaultLocalPort, v);
	emit changed();
}

QString Settings::server_binary_override() const {
	QSettings s;
	return s.value(kServerBinOverride).toString();
}

void Settings::set_server_binary_override(const QString& v) {
	QSettings s;
	s.setValue(kServerBinOverride, v);
	emit changed();
}

bool Settings::has_seen_first_run() const {
	QSettings s;
	return s.value(kFirstRunSeen, false).toBool();
}

void Settings::mark_first_run_seen() {
	QSettings s;
	s.setValue(kFirstRunSeen, true);
	// no `emit changed()` -- this is a one-shot flag, not a
	// user-visible field.
}

}   // namespace simsc_desktop
