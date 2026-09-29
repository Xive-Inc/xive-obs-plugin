#pragma once

#include <QByteArray>
#include <QString>

/*
 * The release manifest — obs/latest.json, written by scripts/publish-release.sh and served from
 * media.xivecdn.com — and the arithmetic for deciding whether it describes something newer than
 * this build.
 *
 * Deliberately free of Qt Network and of libobs: everything here is a pure function over bytes,
 * so scripts/test-update.cpp can compile it against Qt Core alone (see scripts/run-tests.sh).
 * The fetching, the timer and the preference live in xive-update.h.
 */

/** One platform's entry out of the manifest, plus the manifest-wide fields. */
struct XiveRelease {
	/** Manifest-wide. Empty means the document could not be read at all. */
	QString version;
	/** Publication date, "YYYY-MM-DD". Display only. */
	QString released;
	/** OBS major the release is built against; 0 when the manifest omits it. */
	int minObsMajor = 0;

	/** The key this entry was matched under — see xivePlatformKey(). */
	QString platform;
	/** False when the manifest carries no entry for this platform: a release exists, but
	 * nothing is published that this machine could install. */
	bool hasPlatform = false;

	QString file;
	QString url;
	QString sha256;
	qint64 size = 0;
	/** The manifest's own opinion. The Linux native build is published `supported: false`
	 * because it needs a Qt 6.11-era distro; saying so beats a download that will not load. */
	bool supported = true;
	/** The manifest's `requires` note, when it has one. Named around the C++20 keyword. */
	QString requiresNote;

	bool isValid() const { return !version.isEmpty(); }
};

/**
 * Order two version strings: -1 if `a` is older than `b`, 0 if they are the same release, +1 if
 * newer. Missing trailing segments count as zero, so "0.3" and "0.3.0" are the same release.
 *
 * Any pre-release or build suffix ("0.4.0-rc1", "0.4.0+3") is CUT before comparing rather than
 * ordered, which makes a release candidate compare equal to the release. That is deliberate:
 * publish-release.sh derives the version from CMakeLists.txt, which has only ever carried plain
 * numbers, and a wrong ordering of a suffix nobody publishes would be worse than ignoring it.
 */
int xiveCompareVersions(const QString &a, const QString &b);

/**
 * Which manifest key describes THIS build.
 *
 * Linux is decided at runtime, not at compile time: one .so is built inside the OBS flatpak's SDK
 * and one on Fedora, they are not interchangeable, and only the process that is running can say
 * which OBS it is inside. /.flatpak-info exists in every flatpak sandbox and nowhere else.
 */
QString xivePlatformKey();

/**
 * Decode a manifest. A document that is not an object, or carries no `version`, comes back
 * invalid (`isValid() == false`) — there is nothing to compare against and nothing to show.
 *
 * A manifest that is fine but has no entry for `platformKey` comes back VALID with
 * `hasPlatform == false`: that is the Windows case today, and the creator is better told a newer
 * version exists than told nothing at all.
 */
XiveRelease xiveParseManifest(const QByteArray &json, const QString &platformKey);
