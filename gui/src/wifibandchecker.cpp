// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "wifibandchecker.h"

#include <QNetworkInformation>

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <wlanapi.h>
#  pragma comment(lib, "wlanapi.lib")
#endif

namespace WifiBandChecker {

QString LinkInfo::summary() const
{
	if(kind == LinkKind::Ethernet)
		return QStringLiteral("Ethernet");
	if(kind == LinkKind::Cellular)
		return QStringLiteral("Cellular");
	if(kind == LinkKind::None)
		return QStringLiteral("No network");
	if(kind == LinkKind::Other)
		return transport_label.isEmpty() ? QStringLiteral("Other") : transport_label;
	if(frequency_mhz > 0)
		return QStringLiteral("%1 %2 (%3 MHz)").arg(transport_label, band_label).arg(frequency_mhz);
	return QStringLiteral("%1 %2").arg(transport_label, band_label);
}

static void applyFrequency(LinkInfo &info, int mhz)
{
	info.frequency_mhz = mhz;
	if(mhz >= 2400 && mhz < 2500) {
		info.kind = LinkKind::Wifi24;
		info.band_label = QStringLiteral("2.4 GHz");
	} else if(mhz >= 4900 && mhz < 5900) {
		info.kind = LinkKind::Wifi5;
		info.band_label = QStringLiteral("5 GHz");
	} else if(mhz >= 5900 && mhz < 7200) {
		info.kind = LinkKind::Wifi6;
		info.band_label = QStringLiteral("6 GHz");
	} else {
		info.kind = LinkKind::WifiUnknown;
		info.band_label = QStringLiteral("%1 MHz").arg(mhz);
	}
}

#if defined(Q_OS_WIN)
static LinkInfo inspectWifiWindows()
{
	LinkInfo info;
	info.kind = LinkKind::WifiUnknown;
	info.transport_label = QStringLiteral("Wi-Fi");
	info.band_label = QStringLiteral("band unknown");

	HANDLE client = nullptr;
	DWORD negotiated = 0;
	if(WlanOpenHandle(2, nullptr, &negotiated, &client) != ERROR_SUCCESS)
		return info;

	PWLAN_INTERFACE_INFO_LIST ifaces = nullptr;
	if(WlanEnumInterfaces(client, nullptr, &ifaces) != ERROR_SUCCESS || !ifaces) {
		WlanCloseHandle(client, nullptr);
		return info;
	}

	for(DWORD i = 0; i < ifaces->dwNumberOfItems; ++i) {
		const WLAN_INTERFACE_INFO &iface = ifaces->InterfaceInfo[i];
		if(iface.isState != wlan_interface_state_connected)
			continue;

		DWORD data_size = 0;
		WLAN_OPCODE_VALUE_TYPE opcode_type = wlan_opcode_value_type_invalid;
		PWLAN_CONNECTION_ATTRIBUTES attrs = nullptr;
		if(WlanQueryInterface(client, &iface.InterfaceGuid, wlan_intf_opcode_current_connection,
				nullptr, &data_size, reinterpret_cast<PVOID *>(&attrs), &opcode_type) != ERROR_SUCCESS
			|| !attrs) {
			continue;
		}

		DOT11_SSID ssid = attrs->wlanAssociationAttributes.dot11Ssid;
		WlanFreeMemory(attrs);

		PWLAN_BSS_LIST bss_list = nullptr;
		if(WlanGetNetworkBssList(client, &iface.InterfaceGuid, &ssid, dot11_BSS_type_any,
				FALSE, nullptr, &bss_list) == ERROR_SUCCESS && bss_list && bss_list->dwNumberOfItems > 0) {
			const ULONG f = bss_list->wlanBssEntries[0].ulChCenterFrequency / 1000; // kHz → MHz
			WlanFreeMemory(bss_list);
			if(f > 0)
				applyFrequency(info, static_cast<int>(f));
		} else if(bss_list) {
			WlanFreeMemory(bss_list);
		}
		break;
	}

	WlanFreeMemory(ifaces);
	WlanCloseHandle(client, nullptr);
	return info;
}
#endif

LinkInfo inspect()
{
	LinkInfo info;
	info.kind = LinkKind::None;
	info.transport_label = QStringLiteral("None");
	info.band_label = QStringLiteral("n/a");

	if(!QNetworkInformation::loadDefaultBackend()) {
		info.kind = LinkKind::Other;
		info.transport_label = QStringLiteral("Unknown");
		return info;
	}
	QNetworkInformation *ni = QNetworkInformation::instance();
	if(!ni) {
		info.kind = LinkKind::Other;
		info.transport_label = QStringLiteral("Unknown");
		return info;
	}

	using TM = QNetworkInformation::TransportMedium;
	switch(ni->transportMedium()) {
	case TM::Ethernet:
		info.kind = LinkKind::Ethernet;
		info.transport_label = QStringLiteral("Ethernet");
		info.band_label = QStringLiteral("wired");
		return info;
	case TM::Cellular:
		info.kind = LinkKind::Cellular;
		info.transport_label = QStringLiteral("Cellular");
		return info;
	case TM::WiFi:
#if defined(Q_OS_WIN)
		return inspectWifiWindows();
#else
		info.kind = LinkKind::WifiUnknown;
		info.transport_label = QStringLiteral("Wi-Fi");
		info.band_label = QStringLiteral("band unknown");
		return info;
#endif
	case TM::Bluetooth:
		info.kind = LinkKind::Other;
		info.transport_label = QStringLiteral("Bluetooth");
		return info;
	case TM::Unknown:
	default:
		info.kind = LinkKind::Other;
		info.transport_label = QStringLiteral("Unknown");
		return info;
	}
}

} // namespace WifiBandChecker
