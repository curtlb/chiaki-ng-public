
// ugly workaround because Windows does weird things and ENOTIME
int real_main(int argc, char *argv[]);
int main(int argc, char *argv[]) { return real_main(argc, argv); }

#include <streamsession.h>
#include <settings.h>
#include <host.h>
#include <controllermanager.h>
#include <discoverymanager.h>
#include <qmlmainwindow.h>
#include <crashreporter.h>
#include <cloudlog.h>
#include <QApplication>
#include <QtTypes>

#ifdef CHIAKI_ENABLE_CLI
#include <chiaki-cli.h>
#endif

#include <chiaki/session.h>
#include <chiaki/regist.h>
#include <chiaki/base64.h>

#include <stdio.h>
#include <string.h>

#ifdef CHIAKI_HAVE_WEBENGINE
#include <QtWebEngineQuick>
#endif

#include <QCommandLineParser>
#include <QMap>
#include <QSurfaceFormat>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QVector>

Q_DECLARE_METATYPE(ChiakiLogLevel)
Q_DECLARE_METATYPE(ChiakiRegistEventType)

#if defined(CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE) && defined(Q_OS_LINUX)
#include <QtPlugin>
Q_IMPORT_PLUGIN(SDInputContextPlugin)
#endif

#ifdef CHIAKI_ENABLE_CLI
struct CLICommand
{
	int (*cmd)(ChiakiLog *log, int argc, char *argv[]);
};

static const QMap<QString, CLICommand> cli_commands = {
	{ "discover", { chiaki_cli_cmd_discover } },
	{ "wakeup", { chiaki_cli_cmd_wakeup } }
};
#endif

int RunStream(QGuiApplication &app, const StreamSessionConnectInfo &connect_info);
int RunMain(QGuiApplication &app, Settings *settings, bool exit_app_on_stream_exit);

int real_main(int argc, char *argv[])
{
	qRegisterMetaType<DiscoveryHost>();
	qRegisterMetaType<RegisteredHost>();
	qRegisterMetaType<HostMAC>();
	qRegisterMetaType<ChiakiQuitReason>();
	qRegisterMetaType<ChiakiRegistEventType>();
	qRegisterMetaType<ChiakiLogLevel>();

	QGuiApplication::setOrganizationName("Chiaki");
	QGuiApplication::setApplicationName("Chiaki");
	QGuiApplication::setApplicationVersion(CHIAKI_VERSION);
	QGuiApplication::setApplicationDisplayName("chiaki-ng");
#if defined(Q_OS_LINUX)
	if(qEnvironmentVariableIsSet("FLATPAK_ID"))
		QGuiApplication::setDesktopFileName(qEnvironmentVariable("FLATPAK_ID"));
	else
#endif
		QGuiApplication::setDesktopFileName("chiaki-ng");

	qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu");
#if defined(Q_OS_WIN)
	const size_t cSize = strlen(argv[0])+1;
	wchar_t wc[cSize];
	mbstowcs (wc, argv[0], cSize);
	QString import_path = QFileInfo(QString::fromWCharArray(wc)).dir().absolutePath() + "/qml";
	qputenv("QML_IMPORT_PATH", import_path.toUtf8());
#endif
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
	qputenv("ANV_VIDEO_DECODE", "1");
	qputenv("RADV_PERFTEST", "video_decode");
#endif
#ifdef CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	if (qEnvironmentVariableIsSet("SteamDeck"))
		qputenv("QT_IM_MODULE", "sdinput");
#endif

	ChiakiErrorCode err = chiaki_lib_init();
	if(err != CHIAKI_ERR_SUCCESS)
	{
		// Note: on Windows GUI builds stdout/stderr might be invisible, so show a dialog too.
		const QString msg = QString("Chiaki lib init failed: %1").arg(chiaki_error_string(err));
		fprintf(stderr, "%s\n", qPrintable(msg));
		QMessageBox::critical(nullptr, "chiaki-ng", msg);
		return 1;
	}

    SDL_SetHint(SDL_HINT_APP_NAME, "chiaki-ng");

	if(SDL_Init(SDL_INIT_AUDIO) < 0)
	{
		const QString msg = QString("SDL Audio init failed: %1").arg(SDL_GetError());
		fprintf(stderr, "%s\n", qPrintable(msg));
		QMessageBox::critical(nullptr, "chiaki-ng", msg);
		return 1;
	}

	QGuiApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
#ifdef CHIAKI_HAVE_WEBENGINE
	QtWebEngineQuick::initialize();
#endif

	// Инициализируем систему отчетов об ошибках
	CrashReporter::Initialize();

#ifdef Q_OS_MACOS
	QGuiApplication::setWindowIcon(QIcon(":/icons/chiaking_macos.svg"));
#else
	QGuiApplication::setWindowIcon(QIcon(":/icons/chiaking.svg"));
#endif

	QCommandLineParser parser;
	parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);
	parser.addHelpOption();
	
	QStringList cmds;
	cmds.append("stream");
	cmds.append("list");
	cmds.append("farm");
#ifdef CHIAKI_ENABLE_CLI
	cmds.append(cli_commands.keys());
#endif

	parser.addPositionalArgument("command", cmds.join(", "));
	parser.addPositionalArgument("nickname", "Needed for stream command to get credentials for connecting. "
			"Use 'list' to get the nickname.");
	parser.addPositionalArgument("host", "Address to connect to (when using the stream command).");
	parser.addPositionalArgument("farm.json", "Path to farm config (when using the farm command).");

	QCommandLineOption profile_option("profile", "", "profile", "Configuration profile");
	parser.addOption(profile_option);

	QCommandLineOption stream_exit_option("exit-app-on-stream-exit", "Exit the GUI application when the stream session ends.");
	parser.addOption(stream_exit_option);

	QCommandLineOption regist_key_option("registkey", "", "registkey");
	parser.addOption(regist_key_option);

	QCommandLineOption morning_option("morning", "", "morning");
	parser.addOption(morning_option);

	QCommandLineOption fullscreen_option("fullscreen", "Start window in fullscreen mode [maintains aspect ratio, adds black bars to fill unsused parts of screen if applicable] (only for use with stream command).");
	parser.addOption(fullscreen_option);

	QCommandLineOption dualsense_option("dualsense", "Enable DualSense haptics and adaptive triggers (PS5 and DualSense connected via USB only).");
	parser.addOption(dualsense_option);

	QCommandLineOption zoom_option("zoom", "Start window in fullscreen zoomed in to fit screen [maintains aspect ratio, cutting off edges of image to fill screen] (only for use with stream command)");
	parser.addOption(zoom_option);

	QCommandLineOption stretch_option("stretch", "Start window in fullscreen stretched to fit screen [distorts aspect ratio to fill screen] (only for use with stream command).");
	parser.addOption(stretch_option);

	QCommandLineOption passcode_option("passcode", "Automatically send your PlayStation login passcode (only affects users with a login passcode set on their PlayStation console).", "passcode");
	parser.addOption(passcode_option);

	parser.process(app);
	QStringList args = parser.positionalArguments();

	Settings settings(parser.isSet(profile_option) ? parser.value(profile_option) : QString());
	bool exit_app_on_stream_exit = parser.isSet(stream_exit_option);
	if(parser.isSet(profile_option))
		settings.SetCurrentProfile(parser.value(profile_option));
	Settings alt_settings(parser.isSet(profile_option) ? "" : settings.GetCurrentProfile());
	if(!settings.GetCurrentProfile().isEmpty())
		QGuiApplication::setApplicationDisplayName(QString("chiaki-ng:%1").arg(settings.GetCurrentProfile()));
	bool use_alt_settings = false;
	if(!parser.isSet(profile_option))
		use_alt_settings = true;

	if(args.length() == 0)
		return RunMain(app, use_alt_settings ? &alt_settings : &settings, exit_app_on_stream_exit);

	if(args[0] == "list")
	{
		for(const auto &host : settings.GetRegisteredHosts())
			printf("Host: %s \n", host.GetServerNickname().toLocal8Bit().constData());
		return 0;
	}
	if(args[0] == "farm")
	{
		if(args.length() < 2)
			parser.showHelp(1);

		const QString farm_path = args[1];
		QFile farm_file(farm_path);
		if(!farm_file.open(QIODevice::ReadOnly))
		{
			const QString msg = QString("Failed to open farm config: %1").arg(farm_path);
			fprintf(stderr, "%s\n", qPrintable(msg));
			QMessageBox::critical(nullptr, "chiaki-ng farm", msg);
			return 1;
		}
		const QByteArray farm_bytes = farm_file.readAll();
		QJsonParseError json_err;
		const QJsonDocument doc = QJsonDocument::fromJson(farm_bytes, &json_err);
		if(json_err.error != QJsonParseError::NoError || !doc.isObject())
		{
			const QString msg = QString("Invalid farm json: %1 (offset %2)")
				.arg(json_err.errorString())
				.arg((int)json_err.offset);
			fprintf(stderr, "%s\n", qPrintable(msg));
			QMessageBox::critical(nullptr, "chiaki-ng farm", msg);
			return 1;
		}
		const QJsonObject root = doc.object();
		const QString farm_hw_decoder_global = root.value("hwDecoder").toString();
		const QJsonArray consoles = root.value("consoles").toArray();
		const QFileInfo farm_json_info(farm_path);
		if(consoles.isEmpty())
		{
			const QString msg = "Farm config has no consoles. Expected: {\"consoles\":[...]}";
			fprintf(stderr, "%s\n", qPrintable(msg));
			QMessageBox::critical(nullptr, "chiaki-ng farm", msg);
			return 1;
		}

		QVector<Settings*> farm_settings;
		farm_settings.reserve(consoles.size());
		QVector<QmlMainWindow*> farm_windows;
		farm_windows.reserve(consoles.size());

		for(int i = 0; i < consoles.size(); i++)
		{
			if(!consoles[i].isObject())
				continue;
			const QJsonObject o = consoles[i].toObject();
			const QString name = o.value("name").toString(QString("console-%1").arg(i));
			QString ini = o.value("ini").toString();
			const QString nickname = o.value("nickname").toString();
			const QString host = o.value("host").toString();
			const int custom_port_base = o.value("customPortBase").toInt(0);
			const QString resolution = o.value("resolution").toString();
			const int fps = o.value("fps").toInt(0);

			if(host.isEmpty())
			{
				const QString msg = QString("[%1] Missing required field \"host\".").arg(i);
				fprintf(stderr, "%s\n", qPrintable(msg));
				QMessageBox::critical(nullptr, "chiaki-ng farm", msg);
				continue;
			}
			if(ini.isEmpty())
			{
				const QString msg = QString("[%1] Missing required field \"ini\".").arg(i);
				fprintf(stderr, "%s\n", qPrintable(msg));
				QMessageBox::critical(nullptr, "chiaki-ng farm", msg);
				continue;
			}
			if(nickname.isEmpty())
			{
				const QString msg = QString("[%1] Missing required field \"nickname\".").arg(i);
				fprintf(stderr, "%s\n", qPrintable(msg));
				QMessageBox::critical(nullptr, "chiaki-ng farm", msg);
				continue;
			}

			if(!QFileInfo(ini).isAbsolute())
				ini = farm_json_info.absolutePath() + QLatin1Char('/') + ini;

			// Isolated settings store per node to avoid collisions.
			auto *node_settings = new Settings(QStringLiteral("farm-%1").arg(i), &app);
			node_settings->ImportSettings(ini);

			QByteArray morning;
			QByteArray regist_key;
			ChiakiTarget target = CHIAKI_TARGET_PS4_10;

			bool found = false;
			for(const auto &temphost : node_settings->GetRegisteredHosts())
			{
				if(temphost.GetServerNickname() == nickname)
				{
					found = true;
					morning = temphost.GetRPKey();
					regist_key = temphost.GetRPRegistKey();
					target = temphost.GetTarget();
					break;
				}
			}
			if(!found)
			{
				const QString msg = QString("[%1] Could not find registered host for nickname \"%2\" in ini \"%3\".")
					.arg(i).arg(nickname).arg(ini);
				fprintf(stderr, "%s\n", qPrintable(msg));
				QMessageBox::critical(nullptr, "chiaki-ng farm", msg);
				return 1;
			}

			StreamSessionConnectInfo connect_info(
				node_settings,
				target,
				host,
				name,
				regist_key,
				morning,
				QString(), // initial passcode
				QString(), // duid (PSN) empty => direct
				false,     // auto_regist
				false, false, false);

			// Farm mode is primarily for "do the same actions everywhere".
			// Force video+audio enabled unless the user later adds an explicit override.
			connect_info.audio_video_disabled = CHIAKI_NONE_DISABLED;

			if(custom_port_base > 0 && custom_port_base <= 65535)
				connect_info.custom_port_base = (uint16_t)custom_port_base;

			if(!resolution.isEmpty() && (fps == 30 || fps == 60))
			{
				ChiakiVideoResolutionPreset res_preset;
				if(resolution == "360p")
					res_preset = CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
				else if(resolution == "540p")
					res_preset = CHIAKI_VIDEO_RESOLUTION_PRESET_540p;
				else if(resolution == "720p")
					res_preset = CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
				else if(resolution == "1080p")
					res_preset = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
				else
					res_preset = CHIAKI_VIDEO_RESOLUTION_PRESET_720p;

				chiaki_connect_video_profile_preset(&connect_info.video_profile, res_preset,
					fps == 60 ? CHIAKI_VIDEO_FPS_PRESET_60 : CHIAKI_VIDEO_FPS_PRESET_30);
			}

			// Multiple parallel Vulkan hw-decode sessions often fail on some GPUs/drivers
			// ("Failed to push frame: Invalid data..."). Override via farm.json hwDecoder.
			QString farm_hw = o.value("hwDecoder").toString();
			if(farm_hw.isEmpty())
				farm_hw = farm_hw_decoder_global;
			if(!farm_hw.isEmpty())
			{
				if(farm_hw == QLatin1String("software"))
					connect_info.hw_decoder.clear();
				else if(farm_hw != QLatin1String("auto"))
					connect_info.hw_decoder = farm_hw;
			}

			auto *w = new QmlMainWindow(connect_info, false /* exit app on stream exit */);
			w->setTitle(QString("chiaki-ng farm: %1").arg(name));
			w->show();

			farm_settings.push_back(node_settings);
			farm_windows.push_back(w);
		}

		if(farm_windows.isEmpty())
		{
			const QString msg = "Farm command: no windows created.";
			fprintf(stderr, "%s\n", qPrintable(msg));
			QMessageBox::critical(nullptr, "chiaki-ng farm", msg);
			return 1;
		}
		return app.exec();
	}
	if(args[0] == "stream")
	{
		if(args.length() < 2)
			parser.showHelp(1);

		//QString host = args[sizeof(args) -1]; //the ip is always the last param for stream
		QString host = args[args.size()-1];
		QByteArray morning;
		QByteArray regist_key;
		QString initial_login_passcode;
		ChiakiTarget target = CHIAKI_TARGET_PS4_10;

		if(parser.value(regist_key_option).isEmpty() && parser.value(morning_option).isEmpty())
		{
			if(args.length() < 3)
				parser.showHelp(1);

			bool found = false;
			for(const auto &temphost : settings.GetRegisteredHosts())
			{
				if(temphost.GetServerNickname() == args[1])
				{
					found = true;
					morning = temphost.GetRPKey();
					regist_key = temphost.GetRPRegistKey();
					target = temphost.GetTarget();
					break;
				}
			}
			if(!found)
			{
				printf("No configuration found for '%s'\n", args[1].toLocal8Bit().constData());
				return 1;
			}
		}
		else
		{
			// TODO: explicit option for target
			regist_key = parser.value(regist_key_option).toUtf8();
			if(regist_key.length() > sizeof(ChiakiConnectInfo::regist_key))
			{
				printf("Given regist key is too long (expected size <=%llu, got %" PRIdQSIZETYPE")\n",
					(unsigned long long)sizeof(ChiakiConnectInfo::regist_key),
					regist_key.length());
				return 1;
			}
			regist_key += QByteArray(sizeof(ChiakiConnectInfo::regist_key) - regist_key.length(), 0);
			morning = QByteArray::fromBase64(parser.value(morning_option).toUtf8());
			if(morning.length() != sizeof(ChiakiConnectInfo::morning))
			{
				printf("Given morning has invalid size (expected %llu, got %" PRIdQSIZETYPE")\n",
					(unsigned long long)sizeof(ChiakiConnectInfo::morning),
					morning.length());
				printf("Given morning has invalid size (expected %llu)", (unsigned long long)sizeof(ChiakiConnectInfo::morning));
				return 1;
			}
		}
		if ((parser.isSet(stretch_option) && (parser.isSet(zoom_option) || parser.isSet(fullscreen_option))) || (parser.isSet(zoom_option) && parser.isSet(fullscreen_option)))
		{
			printf("Must choose between fullscreen, zoom or stretch option.");
			return 1;
		}
		if(parser.value(passcode_option).isEmpty())
		{
			//Set to empty if it wasn't given by user.
			initial_login_passcode = QString("");
		}
		else
		{
			initial_login_passcode = parser.value(passcode_option);
			if(initial_login_passcode.length() != 4)
			{
				printf("Login passcode must be 4 digits. You entered %" PRIdQSIZETYPE "digits)\n", initial_login_passcode.length());
				return 1;
			}
		}
		
		StreamSessionConnectInfo connect_info(
				use_alt_settings ? &alt_settings : &settings,
				target,
				std::move(host),
				QString(),
				std::move(regist_key),
				std::move(morning),
				std::move(initial_login_passcode),
				QString(),
				false,
				parser.isSet(fullscreen_option),
				parser.isSet(zoom_option),
				parser.isSet(stretch_option));

		return RunStream(app, connect_info);
	}
#ifdef CHIAKI_ENABLE_CLI
	else if(cli_commands.contains(args[0]))
	{
		ChiakiLog log;
		// TODO: add verbose arg
		chiaki_log_init(&log, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, chiaki_log_cb_print, nullptr);

		const auto &cmd = cli_commands[args[0]];
		int sub_argc = args.count();
		QVector<QByteArray> sub_argv_b(sub_argc);
		QVector<char *> sub_argv(sub_argc);
		for(size_t i=0; i<sub_argc; i++)
		{
			sub_argv_b[i] = args[i].toLocal8Bit();
			sub_argv[i] = sub_argv_b[i].data();
		}
		return cmd.cmd(&log, sub_argc, sub_argv.data());
	}
#endif
	else
	{
		parser.showHelp(1);
	}
}

int RunMain(QGuiApplication &app, Settings *settings, bool exit_app_on_stream_exit)
{
	CloudLogInit();
	QmlMainWindow main_window(settings, exit_app_on_stream_exit);
	main_window.show();
	return app.exec();
}

int RunStream(QGuiApplication &app, const StreamSessionConnectInfo &connect_info)
{
	QmlMainWindow main_window(connect_info, true);
	main_window.show();
	return app.exec();
}
