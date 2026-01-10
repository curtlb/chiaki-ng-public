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
	parser.setApplicationDescription("Конвертирует INI конфиг chiaki-ng в JSON формат для Android");
	parser.addHelpOption();
	parser.addVersionOption();
	
	QCommandLineOption inputOption(QStringList() << "i" << "input",
		"Входной INI файл", "file");
	parser.addOption(inputOption);
	
	QCommandLineOption outputOption(QStringList() << "o" << "output",
		"Выходной JSON файл", "file");
	parser.addOption(outputOption);
	
	parser.process(app);
	
	QString inputFile = parser.value(inputOption);
	QString outputFile = parser.value(outputOption);
	
	if(inputFile.isEmpty())
	{
		qCritical() << "Ошибка: не указан входной файл";
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
	
	// Загружаем registered_hosts
	int count = iniSettings.beginReadArray("registered_hosts");
	
	// Определяем активный хост (из manual_hosts с registered=true)
	QString activeMac;
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
				activeMac = macToHex((const uint8_t *)registeredMacBytes.constData(), 6);
				break;
			}
		}
	}
	iniSettings.endArray();
	
	// Если не нашли активный хост в manual_hosts, берем первый из registered_hosts
	if(activeMac.isEmpty() && count > 0)
	{
		iniSettings.setArrayIndex(0);
		QByteArray firstMacBytes = iniSettings.value("server_mac").toByteArray();
		if(firstMacBytes.size() == 6)
		{
			activeMac = macToHex((const uint8_t *)firstMacBytes.constData(), 6);
		}
	}
	
	for(int i = 0; i < count; i++)
	{
		iniSettings.setArrayIndex(i);
		
		// Загружаем хост для правильного парсинга @ByteArray
		RegisteredHost host = RegisteredHost::LoadFromSettings(&iniSettings);
		
		// Проверяем, соответствует ли этот хост активному
		HostMAC hostMAC = host.GetServerMAC();
		QString hostMac = macToHex(hostMAC.GetMAC(), 6);
		if(activeMac.isEmpty() || hostMac == activeMac)
		{
			// Используем текущий контекст QSettings для чтения значений
			QJsonObject hostJson = hostToJson(host);
			
			// Добавляем строковые поля из QSettings (они правильно распарсены)
			// Но после LoadFromSettings значения уже в host
			// Читаем из QSettings для получения оригинальных строковых значений
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
			if(!activeMac.isEmpty())
				break;
		}
	}
	iniSettings.endArray();
	
	settings["registered_hosts"] = registeredHosts;
	
	// Загружаем manual_hosts (только те, что соответствуют активному хосту)
	QJsonArray manualHosts;
	if(registeredHosts.size() > 0)
	{
		// Берем MAC первого зарегистрированного хоста
		QString activeMac;
		if(registeredHosts[0].isObject())
		{
			QJsonObject firstHost = registeredHosts[0].toObject();
			if(firstHost.contains("server_mac"))
				activeMac = firstHost["server_mac"].toString();
		}
		
		// Ищем manual_host с registered=true и соответствующим MAC
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
					QString registeredMac = macToHex((const uint8_t *)registeredMacBytes.constData(), 6);
					if(registeredMac == activeMac)
					{
						QString host = iniSettings.value("host").toString();
						if(!host.isEmpty())
						{
							QJsonObject manualHost;
							manualHost["host"] = host;
							manualHost["server_mac"] = registeredMac;
							manualHosts.append(manualHost);
							break; // Берем только первый подходящий
						}
					}
				}
			}
		}
		iniSettings.endArray();
	}
	
	settings["manual_hosts"] = manualHosts;
	root["settings"] = settings;
	
	// Сохраняем JSON
	QJsonDocument doc(root);
	QFile outFile(outputFile);
	if(!outFile.open(QIODevice::WriteOnly))
	{
		qCritical() << "Ошибка: не удалось открыть файл для записи:" << outputFile;
		return 1;
	}
	
	outFile.write(doc.toJson(QJsonDocument::Indented));
	outFile.close();
	
	qInfo() << "Конвертация завершена:";
	qInfo() << "  Входной файл:" << inputFile;
	qInfo() << "  Выходной файл:" << outputFile;
	qInfo() << "  Зарегистрированных хостов:" << registeredHosts.size();
	qInfo() << "  Ручных хостов:" << manualHosts.size();
	
	return 0;
}
