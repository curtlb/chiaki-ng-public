// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "crashreporter.h"
#include <QUdpSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDateTime>
#include <QSysInfo>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QMessageLogContext>
#include <QFileInfo>
#include <QJsonArray>
#ifdef Q_OS_WIN
#include <tlhelp32.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
// Note: Libraries are linked via CMakeLists.txt for MinGW compatibility
// #pragma comment(lib, ...) only works with MSVC
#else
#include <signal.h>
#include <execinfo.h>
#include <cxxabi.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <exception>
#include <cstdlib>
#include <csignal>
#include <sstream>
#include <iomanip>

CrashReporter *CrashReporter::instance = nullptr;

CrashReporter::CrashReporter(QObject *parent)
	: QObject(parent)
	, serverHost("5.188.29.131")
	, serverPort(12420)
{
	// Не создаем QUdpSocket здесь, так как он может использоваться из других потоков
	// Создаем его при необходимости в sendData()
}

CrashReporter::~CrashReporter()
{
}

void CrashReporter::Initialize()
{
	if (instance)
		return;

	instance = new CrashReporter();

	// Устанавливаем обработчик std::terminate
	std::set_terminate(TerminateHandler);

#ifdef Q_OS_WIN
	// Windows: SetUnhandledExceptionFilter
	SetUnhandledExceptionFilter(ExceptionHandler);
#else
	// Linux/Mac: Signal handlers
	signal(SIGSEGV, SignalHandler);  // Segmentation fault
	signal(SIGABRT, SignalHandler); // Abort
	signal(SIGFPE, SignalHandler);  // Floating point exception
	signal(SIGILL, SignalHandler);  // Illegal instruction
	signal(SIGBUS, SignalHandler);  // Bus error
#endif

	// Примечание: Qt сообщения обрабатываются через существующий msg_handler в qmlbackend.cpp
	// Критические исключения будут перехвачены через std::terminate и signal handlers
}

QJsonObject CrashReporter::CollectSystemInfo()
{
	QJsonObject info;

	// OS информация
	info["os_name"] = QSysInfo::prettyProductName();
	info["os_type"] = QSysInfo::productType();
	info["os_version"] = QSysInfo::productVersion();
	info["kernel_type"] = QSysInfo::kernelType();
	info["kernel_version"] = QSysInfo::kernelVersion();
	info["architecture"] = QSysInfo::currentCpuArchitecture();
	info["machine_hostname"] = QSysInfo::machineHostName();

	// Qt информация
	info["qt_version"] = QT_VERSION_STR;
	info["qt_runtime_version"] = qVersion();

	// Приложение
	info["app_name"] = QGuiApplication::applicationName();
	info["app_version"] = GetAppVersion();
	info["app_display_name"] = QGuiApplication::applicationDisplayName();
	info["organization_name"] = QGuiApplication::organizationName();

	// Пути
	info["app_data_path"] = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	info["temp_path"] = QStandardPaths::writableLocation(QStandardPaths::TempLocation);

#ifdef Q_OS_WIN
	// Список загруженных модулей (DLL)
	QJsonArray modules;
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
	if (hSnapshot != INVALID_HANDLE_VALUE) {
		MODULEENTRY32W modEntry;
		modEntry.dwSize = sizeof(MODULEENTRY32W);
		if (Module32FirstW(hSnapshot, &modEntry)) {
			do {
				QJsonObject module;
				module["name"] = QString::fromWCharArray(modEntry.szModule);
				module["path"] = QString::fromWCharArray(modEntry.szExePath);
				module["base_address"] = QString::number((quintptr)modEntry.modBaseAddr, 16);
				module["size"] = static_cast<qint64>(modEntry.modBaseSize);
				modules.append(module);
			} while (Module32NextW(hSnapshot, &modEntry));
		}
		CloseHandle(hSnapshot);
	}
	info["loaded_modules"] = modules;
#endif

	return info;
}

QString CrashReporter::GetOSInfo()
{
	QString osInfo = QString("%1 %2 (%3)")
		.arg(QSysInfo::prettyProductName())
		.arg(QSysInfo::productVersion())
		.arg(QSysInfo::currentCpuArchitecture());
	return osInfo;
}

QString CrashReporter::GetAppVersion()
{
	return QGuiApplication::applicationVersion();
}

QByteArray CrashReporter::CreateReport(const QString &errorType, const QString &message, const QString &stackTrace)
{
	QJsonObject report;

	// Основная информация
	report["error_type"] = errorType;
	report["message"] = message;
	report["timestamp"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
	report["app_version"] = GetAppVersion();
	report["os_info"] = GetOSInfo();

	// Системная информация
	report["system_info"] = CollectSystemInfo();

	// Stack trace (если есть)
	if (!stackTrace.isEmpty()) {
		report["stack_trace"] = stackTrace;
	}

	// Конвертируем в JSON
	QJsonDocument doc(report);
	return doc.toJson(QJsonDocument::Compact);
}

void CrashReporter::SendReport(const QString &errorType, const QString &message, const QString &stackTrace)
{
	if (!instance) {
		qWarning() << "CrashReporter not initialized";
		return;
	}

	QByteArray data = CreateReport(errorType, message, stackTrace);
	instance->sendData(data);
}

void CrashReporter::SendExceptionReport(const QString &exceptionType, const QString &what)
{
	SendReport("exception", QString("%1: %2").arg(exceptionType, what));
}

void CrashReporter::sendData(const QByteArray &data)
{
#ifdef Q_OS_WIN
	// Используем WinSock API напрямую для гарантированной отправки без зависимости от Qt event loop
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		qWarning() << "WSAStartup failed";
		return;
	}
	
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET) {
		qWarning() << "Failed to create socket:" << WSAGetLastError();
		WSACleanup();
		return;
	}
	
	// Разрешаем broadcast
	BOOL broadcast = TRUE;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));
	
	// Настраиваем адрес получателя
	sockaddr_in serverAddr;
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(serverPort);
	
	// Преобразуем IP адрес
	QString hostStr = serverHost;
	QByteArray hostBytes = hostStr.toLocal8Bit();
	const char* hostCStr = hostBytes.constData();
	
	qInfo() << "Resolving host:" << serverHost << "(" << hostCStr << ")";
	
	if (hostStr == "localhost" || hostStr == "127.0.0.1") {
		serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
		qInfo() << "Using localhost address: 127.0.0.1";
	} else {
		// Используем inet_pton для более надежного преобразования IP адреса
		int result = InetPtonA(AF_INET, hostCStr, &serverAddr.sin_addr);
		if (result != 1) {
			// Fallback на inet_addr
			serverAddr.sin_addr.s_addr = inet_addr(hostCStr);
			if (serverAddr.sin_addr.s_addr == INADDR_NONE) {
				// Попытка резолва через DNS (может не работать при краше)
				qInfo() << "Attempting DNS resolution for:" << hostCStr;
				hostent* host = gethostbyname(hostCStr);
				if (host && host->h_addr_list[0]) {
					serverAddr.sin_addr = *(in_addr*)host->h_addr_list[0];
					qInfo() << "DNS resolution successful";
				} else {
					qWarning() << "Failed to resolve host:" << serverHost << "Error:" << WSAGetLastError();
					closesocket(sock);
					WSACleanup();
					return;
				}
			} else {
				qInfo() << "inet_addr conversion successful";
			}
		} else {
			qInfo() << "InetPtonA conversion successful";
		}
	}
	
	// Логируем финальный адрес
	char addrStr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &serverAddr.sin_addr, addrStr, INET_ADDRSTRLEN);
	qInfo() << "Target address:" << addrStr << "Port:" << ntohs(serverAddr.sin_port);
	
	// Отправляем данные
	qInfo() << "Sending" << data.size() << "bytes to" << addrStr << ":" << serverPort;
	int sent = sendto(sock, data.constData(), data.size(), 0, (sockaddr*)&serverAddr, sizeof(serverAddr));
	
	if (sent == SOCKET_ERROR) {
		int error = WSAGetLastError();
		qWarning() << "Failed to send crash report. Error code:" << error;
		qWarning() << "Socket error details:";
		switch (error) {
			case WSAENOTSOCK: qWarning() << "  Socket descriptor is not valid"; break;
			case WSAEADDRNOTAVAIL: qWarning() << "  Address not available"; break;
			case WSAENETUNREACH: qWarning() << "  Network unreachable"; break;
			case WSAEHOSTUNREACH: qWarning() << "  Host unreachable"; break;
			case WSAECONNREFUSED: qWarning() << "  Connection refused"; break;
			case WSAEWOULDBLOCK: qWarning() << "  Operation would block"; break;
			default: qWarning() << "  Unknown error:" << error; break;
		}
	} else {
		qInfo() << "Crash report sent successfully:" << sent << "bytes sent to" << addrStr << ":" << serverPort;
		// Дополнительная проверка - получаем локальный адрес сокета
		sockaddr_in localAddr;
		int addrLen = sizeof(localAddr);
		if (getsockname(sock, (sockaddr*)&localAddr, &addrLen) == 0) {
			char localAddrStr[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &localAddr.sin_addr, localAddrStr, INET_ADDRSTRLEN);
			qInfo() << "Sent from local address:" << localAddrStr << ":" << ntohs(localAddr.sin_port);
		}
	}
	
	closesocket(sock);
	WSACleanup();
#else
	// Для Linux/Mac используем QUdpSocket
	QUdpSocket socket;
	QHostAddress host(serverHost);
	
	qint64 sent = socket.writeDatagram(data, host, serverPort);
	
	if (sent < 0) {
		qWarning() << "Failed to send crash report:" << socket.errorString();
	} else {
		qInfo() << "Crash report sent to" << serverHost << ":" << serverPort << "(" << sent << "bytes)";
	}
#endif
}

#ifdef Q_OS_WIN
LONG WINAPI CrashReporter::ExceptionHandler(EXCEPTION_POINTERS *exceptionInfo)
{
	QString errorType = "crash";
	QString message;
	QString stackTrace;

	// Определяем тип исключения
	switch (exceptionInfo->ExceptionRecord->ExceptionCode) {
		case EXCEPTION_ACCESS_VIOLATION:
			message = "Access Violation";
			break;
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			message = "Array Bounds Exceeded";
			break;
		case EXCEPTION_BREAKPOINT:
			message = "Breakpoint";
			break;
		case EXCEPTION_DATATYPE_MISALIGNMENT:
			message = "Data Type Misalignment";
			break;
		case EXCEPTION_FLT_DENORMAL_OPERAND:
			message = "Float Denormal Operand";
			break;
		case EXCEPTION_FLT_DIVIDE_BY_ZERO:
			message = "Float Divide By Zero";
			break;
		case EXCEPTION_FLT_INEXACT_RESULT:
			message = "Float Inexact Result";
			break;
		case EXCEPTION_FLT_INVALID_OPERATION:
			message = "Float Invalid Operation";
			break;
		case EXCEPTION_FLT_OVERFLOW:
			message = "Float Overflow";
			break;
		case EXCEPTION_FLT_STACK_CHECK:
			message = "Float Stack Check";
			break;
		case EXCEPTION_FLT_UNDERFLOW:
			message = "Float Underflow";
			break;
		case EXCEPTION_ILLEGAL_INSTRUCTION:
			message = "Illegal Instruction";
			break;
		case EXCEPTION_IN_PAGE_ERROR:
			message = "In Page Error";
			break;
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
			message = "Integer Divide By Zero";
			break;
		case EXCEPTION_INT_OVERFLOW:
			message = "Integer Overflow";
			break;
		case EXCEPTION_INVALID_DISPOSITION:
			message = "Invalid Disposition";
			break;
		case EXCEPTION_NONCONTINUABLE_EXCEPTION:
			message = "Noncontinuable Exception";
			break;
		case EXCEPTION_PRIV_INSTRUCTION:
			message = "Privileged Instruction";
			break;
		case EXCEPTION_SINGLE_STEP:
			message = "Single Step";
			break;
		case EXCEPTION_STACK_OVERFLOW:
			message = "Stack Overflow";
			break;
		default:
			message = QString("Unknown Exception (0x%1)").arg(exceptionInfo->ExceptionRecord->ExceptionCode, 8, 16, QChar('0'));
			break;
	}

	// Пытаемся получить stack trace
	HANDLE process = GetCurrentProcess();
	SymInitialize(process, NULL, TRUE);
	SymSetOptions(SYMOPT_LOAD_LINES);

	CONTEXT *context = exceptionInfo->ContextRecord;
	STACKFRAME64 stackFrame = {};
	stackFrame.AddrPC.Mode = AddrModeFlat;
	stackFrame.AddrFrame.Mode = AddrModeFlat;
	stackFrame.AddrStack.Mode = AddrModeFlat;

#ifdef _WIN64
	stackFrame.AddrPC.Offset = context->Rip;
	stackFrame.AddrFrame.Offset = context->Rbp;
	stackFrame.AddrStack.Offset = context->Rsp;
#else
	stackFrame.AddrPC.Offset = context->Eip;
	stackFrame.AddrFrame.Offset = context->Ebp;
	stackFrame.AddrStack.Offset = context->Esp;
#endif

	QStringList stackLines;
	int frameCount = 0;
	const int maxFrames = 50;

	while (StackWalk64(
#ifdef _WIN64
		IMAGE_FILE_MACHINE_AMD64,
#else
		IMAGE_FILE_MACHINE_I386,
#endif
		process,
		GetCurrentThread(),
		&stackFrame,
		context,
		NULL,
		SymFunctionTableAccess64,
		SymGetModuleBase64,
		NULL) && frameCount < maxFrames) {

		char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)];
		PSYMBOL_INFO symbolInfo = (PSYMBOL_INFO)symbolBuffer;
		symbolInfo->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbolInfo->MaxNameLen = MAX_SYM_NAME;

		DWORD64 displacement = 0;
		QString moduleName = "<unknown module>";
		
		// Пытаемся определить модуль по адресу
		HMODULE hModule = NULL;
		if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCTSTR)stackFrame.AddrPC.Offset, &hModule)) {
			wchar_t modulePath[MAX_PATH];
			if (GetModuleFileNameW(hModule, modulePath, MAX_PATH)) {
				QString fullPath = QString::fromWCharArray(modulePath);
				QFileInfo fileInfo(fullPath);
				moduleName = fileInfo.fileName();
			}
		}
		
		if (SymFromAddr(process, stackFrame.AddrPC.Offset, &displacement, symbolInfo)) {
			stackLines << QString("#%1 0x%2 [%3+0x%4] - %5")
				.arg(frameCount)
				.arg(stackFrame.AddrPC.Offset, 0, 16)
				.arg(moduleName)
				.arg(displacement, 0, 16)
				.arg(symbolInfo->Name);
		} else {
			stackLines << QString("#%1 0x%2 [%3] - <unknown symbol>")
				.arg(frameCount)
				.arg(stackFrame.AddrPC.Offset, 0, 16)
				.arg(moduleName);
		}

		frameCount++;
	}

	SymCleanup(process);
	stackTrace = stackLines.join("\n");

	// Отправляем отчет
	SendReport(errorType, message, stackTrace);

	// Возвращаем EXCEPTION_EXECUTE_HANDLER чтобы показать стандартный диалог
	return EXCEPTION_EXECUTE_HANDLER;
}

#else

void CrashReporter::SignalHandler(int signal)
{
	QString errorType = "crash";
	QString message;
	QString stackTrace;

	// Определяем тип сигнала
	switch (signal) {
		case SIGSEGV:
			message = "Segmentation Fault (SIGSEGV)";
			break;
		case SIGABRT:
			message = "Abort (SIGABRT)";
			break;
		case SIGFPE:
			message = "Floating Point Exception (SIGFPE)";
			break;
		case SIGILL:
			message = "Illegal Instruction (SIGILL)";
			break;
		case SIGBUS:
			message = "Bus Error (SIGBUS)";
			break;
		default:
			message = QString("Signal %1").arg(signal);
			break;
	}

	// Получаем stack trace
	void *array[50];
	int size = backtrace(array, 50);
	char **symbols = backtrace_symbols(array, size);

	if (symbols) {
		QStringList stackLines;
		for (int i = 0; i < size; i++) {
			// Пытаемся деманглить символы
			QString line = QString(symbols[i]);
			
			// Парсим адрес и символ
			QStringList parts = line.split(' ');
			if (parts.size() >= 3) {
				QString addr = parts[0];
				QString symbol = parts[2];
				
				// Деманглинг
				int status;
				char *demangled = abi::__cxa_demangle(symbol.toLocal8Bit().constData(), 0, 0, &status);
				if (status == 0 && demangled) {
					symbol = QString(demangled);
					free(demangled);
				}
				
				stackLines << QString("#%1 %2 - %3").arg(i).arg(addr).arg(symbol);
			} else {
				stackLines << QString("#%1 %2").arg(i).arg(line);
			}
		}
		stackTrace = stackLines.join("\n");
		free(symbols);
	}

	// Отправляем отчет
	SendReport(errorType, message, stackTrace);

	// Восстанавливаем обработчик по умолчанию и ре-вызываем сигнал
	signal(signal, SIG_DFL);
	raise(signal);
}

#endif

void CrashReporter::TerminateHandler()
{
	// Получаем текущее исключение
	std::exception_ptr ex = std::current_exception();
	
	QString errorType = "terminate";
	QString message = "std::terminate called";
	QString stackTrace;

	if (ex) {
		try {
			std::rethrow_exception(ex);
		} catch (const std::exception &e) {
			message = QString("std::terminate: %1").arg(e.what());
		} catch (...) {
			message = "std::terminate: unknown exception";
		}
	}

#ifdef Q_OS_WIN
	// Windows: для terminate handler stack trace может быть недоступен
	// Отправляем отчет без детального stack trace
	stackTrace = "Stack trace unavailable in terminate handler";
#else
	// Linux/Mac: backtrace
	void *array[50];
	int size = backtrace(array, 50);
	char **symbols = backtrace_symbols(array, size);

	if (symbols) {
		QStringList stackLines;
		for (int i = 0; i < size; i++) {
			QString line = QString(symbols[i]);
			QStringList parts = line.split(' ');
			if (parts.size() >= 3) {
				QString addr = parts[0];
				QString symbol = parts[2];
				
				int status;
				char *demangled = abi::__cxa_demangle(symbol.toLocal8Bit().constData(), 0, 0, &status);
				if (status == 0 && demangled) {
					symbol = QString(demangled);
					free(demangled);
				}
				
				stackLines << QString("#%1 %2 - %3").arg(i).arg(addr).arg(symbol);
			} else {
				stackLines << QString("#%1 %2").arg(i).arg(line);
			}
		}
		stackTrace = stackLines.join("\n");
		free(symbols);
	}
#endif

	// Отправляем отчет
	SendReport(errorType, message, stackTrace);

	// Вызываем стандартный обработчик
	std::abort();
}

