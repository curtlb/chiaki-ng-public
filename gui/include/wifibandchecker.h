// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <QString>

/** Detect link type / Wi‑Fi band before streaming (desktop counterpart of Android WifiBandChecker). */
namespace WifiBandChecker {

enum class LinkKind {
	Ethernet,
	Wifi24,
	Wifi5,
	Wifi6,
	WifiUnknown,
	Cellular,
	Other,
	None
};

struct LinkInfo {
	LinkKind kind = LinkKind::None;
	int frequency_mhz = 0;
	QString transport_label;
	QString band_label;

	QString summary() const;
	bool isWifi24() const { return kind == LinkKind::Wifi24; }
};

LinkInfo inspect();

} // namespace WifiBandChecker
