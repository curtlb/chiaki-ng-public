// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Инструмент для конвертации INI конфига chiaki-ng в JSON формат для Android
// Использует нативные библиотеки chiaki-ng для правильного парсинга @ByteArray

#include <host.h>
#include <QCoreApplication>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDebug>
#include <QCommandLineParser>

static QString targetToString(ChiakiTarget target)
{
	switch(target)
	{
		case CHIAKI_TARGET_PS4_8: return "PS4_8";
		case CHIAKI_TARGET_PS4_9: return "PS4_9";
		case CHIAKI_TARGET_PS4_10: return "PS4_10";
		case CHIAKI_TARGET_PS5_1: return "PS5_1";
		default: return "PS4_10";
	}
}

static ChiakiTarget stringToTarget(const QString &str)
{
	if(str == "PS4_8") return CHIAKI_TARGET_PS4_8;
	if(str == "PS4_9") return CHIAKI_TARGET_PS4_9;
	if(str == "PS4_10") return CHIAKI_TARGET_PS4_10;
	if(str == "PS5_1") return CHIAKI_TARGET_PS5_1;
	return CHIAKI_TARGET_PS4_10;
}

static QString macToHex(const uint8_t *mac, size_t len)
{
	QString result;
	for(size_t i = 0; i < len; i++)
	{
		if(i > 0) result += ":";
		result += QString::number(mac[i], 16).rightJustified(2, '0');
	}
	return result;
}

static QByteArray base64Encode(const uint8_t *data, size_t len)
{
	return QByteArray((const char *)data, len).toBase64();
}

static QJsonObject hostToJson(const RegisteredHost &host)
{
	QJsonObject obj;
	obj["target"] = targetToString(host.GetTarget());
	
	// Используем методы из RegisteredHost для получения значений
	// Поля приватные, но методы доступны
	
	// Для строковых полей нужно читать из QSettings
	// Но после LoadFromSettings они уже правильно распарсены в host
	// Используем QSettings напрямую, но значения уже правильно распарсены через LoadFromSettings
	
	obj["server_mac"] = macToHex(host.GetServerMAC().GetMAC(), 6);
	obj["server_nickname"] = host.GetServerNickname();
	
	// rp_regist_key как base64 (используем метод из RegisteredHost)
	QByteArray rp_regist_key_bytes = host.GetRPRegistKey();
	obj["rp_regist_key"] = QString::fromLatin1(rp_regist_key_bytes.toBase64());
	
	// rp_key как base64 (используем метод из RegisteredHost)
	QByteArray rp_key_bytes = host.GetRPKey();
	obj["rp_key"] = QString::fromLatin1(rp_key_bytes.toBase64());
	
	return obj;
}

static QJsonObject manualHostToJson(const ManualHost &host)
{
	QJsonObject obj;
	obj["host"] = host.GetHost();
	
		HostMAC mac = host.GetMAC();
		// Проверяем, что MAC валиден (не нулевой)
		uint64_t macValue = mac.GetValue();
		if(macValue != 0)
			obj["server_mac"] = macToHex(mac.GetMAC(), 6);
		else
			obj["server_mac"] = QJsonValue();
	
	return obj;
}

int main(int argc, char *argv[])
{
	QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName("chiaki-config-converter");
	QCoreApplication::setApplicationVersion("1.0");
	
	QCommandLineParser parser;
	parser.setApplicationDescription("Converts Chiaki-ng INI config to JSON format for Android");
	parser.addHelpOption();
	parser.addVersionOption();
	
	QCommandLineOption inputOption(QStringList() << "i" << "input",
		"Input INI file", "file");
	parser.addOption(inputOption);
	
	QCommandLineOption outputOption(QStringList() << "o" << "output",
		"Output JSON file", "file");
	parser.addOption(outputOption);
	
	parser.process(app);
	
	QString inputFile = parser.value(inputOption);
	QString outputFile = parser.value(outputOption);
	
	if(inputFile.isEmpty())
	{
		qCritical() << "Error: input file not specified";
		parser.showHelp(1);
	}
	
	if(outputFile.isEmpty())
	{
		outputFile = inputFile;
		if(outputFile.endsWith(".ini"))
			outputFile.chop(4);
		outputFile += ".json";
	}
	
	// Загружаем INI файл через QSettings (правильный парсинг @ByteArray)
	QSettings iniSettings(inputFile, QSettings::IniFormat);
	
	QJsonObject root;
	root["format"] = "chiaki-settings";
	root["version"] = 2;
	
	QJsonObject settings;
	QJsonArray registeredHosts;
	QJsonArray manualHosts;
	
	// Сначала определяем активный хост из manual_hosts (с registered=true)
	QString activeMac;
	QString activeManualHost;
	int manualCount = iniSettings.beginReadArray("manual_hosts");
	for(int i = 0; i < manualCount; i++)
	{
		iniSettings.setArrayIndex(i);
		bool registered = iniSettings.value("registered").toBool();
		if(registered)
		{
			QByteArray registeredMacBytes = iniSettings.value("registered_mac").toByteArray();
			if(registeredMacBytes.size() == 6)
			{
				QString mac = macToHex((const uint8_t *)registeredMacBytes.constData(), 6);
				QString host = iniSettings.value("host").toString();
				if(!host.isEmpty())
				{
					activeMac = mac;
					activeManualHost = host;
					break;
				}
			}
		}
	}
	iniSettings.endArray();
	
	// Загружаем registered_hosts
	int count = iniSettings.beginReadArray("registered_hosts");
	
	// Если не нашли активный хост в manual_hosts, берем первый из registered_hosts
	if(activeMac.isEmpty() && count > 0)
	{
		iniSettings.setArrayIndex(0);
		RegisteredHost firstHost = RegisteredHost::LoadFromSettings(&iniSettings);
		HostMAC firstHostMAC = firstHost.GetServerMAC();
		activeMac = macToHex(firstHostMAC.GetMAC(), 6);
	}
	
	// Читаем все registered_hosts и оставляем только активный
	for(int i = 0; i < count; i++)
	{
		iniSettings.setArrayIndex(i);
		
		// Загружаем хост для правильного парсинга @ByteArray
		RegisteredHost host = RegisteredHost::LoadFromSettings(&iniSettings);
		
		// Проверяем, соответствует ли этот хост активному
		HostMAC hostMAC = host.GetServerMAC();
		QString hostMac = macToHex(hostMAC.GetMAC(), 6);
		
		// Если activeMac пуст, берем первый; иначе берем только соответствующий
		if(activeMac.isEmpty() || hostMac == activeMac)
		{
			// Обновляем activeMac, если он был пуст
			if(activeMac.isEmpty())
				activeMac = hostMac;
			
			QJsonObject hostJson = hostToJson(host);
			
			// Читаем строковые поля из QSettings (они правильно распарсены)
			QString ap_ssid = iniSettings.value("ap_ssid").toString();
			hostJson["ap_ssid"] = ap_ssid.isEmpty() ? "" : ap_ssid;
			
			QString ap_bssid = iniSettings.value("ap_bssid").toString();
			hostJson["ap_bssid"] = ap_bssid.isEmpty() ? "" : ap_bssid;
			
			QString ap_key = iniSettings.value("ap_key").toString();
			hostJson["ap_key"] = ap_key.isEmpty() ? "" : ap_key;
			
			QString ap_name = iniSettings.value("ap_name").toString();
			hostJson["ap_name"] = ap_name.isEmpty() ? "" : ap_name;
			
			hostJson["rp_key_type"] = iniSettings.value("rp_key_type").toInt();
			
			registeredHosts.append(hostJson);
			
			// Если нашли активный хост, останавливаемся
			if(!activeMac.isEmpty() && hostMac == activeMac)
				break;
		}
	}
	iniSettings.endArray();
	
	settings["registered_hosts"] = registeredHosts;
	
	// Добавляем manual_host для активного хоста (если есть)
	if(!activeMac.isEmpty() && !activeManualHost.isEmpty())
	{
		QJsonObject manualHost;
		manualHost["host"] = activeManualHost;
		manualHost["server_mac"] = activeMac;
		manualHosts.append(manualHost);
	}
	
	settings["manual_hosts"] = manualHosts;
	root["settings"] = settings;
	
	// Сохраняем JSON
	QJsonDocument doc(root);
	QFile outFile(outputFile);
	if(!outFile.open(QIODevice::WriteOnly))
	{
		qCritical() << "Error: failed to open output file:" << outputFile;
		return 1;
	}
	
	outFile.write(doc.toJson(QJsonDocument::Indented));
	outFile.close();
	
	qInfo() << "Conversion completed:";
	qInfo() << "  Input file:" << inputFile;
	qInfo() << "  Output file:" << outputFile;
	qInfo() << "  Registered hosts:" << registeredHosts.size();
	qInfo() << "  Manual hosts:" << manualHosts.size();
	
	return 0;
}
