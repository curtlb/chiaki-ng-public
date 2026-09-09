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

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <winnt.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")
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
	, udpSocket(nullptr)
	, serverHost("5.183.190.150")
	, serverPort(12420)
{
	udpSocket = new QUdpSocket(this);
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
	if (!udpSocket) {
		qWarning() << "UDP socket not initialized";
		return;
	}

	// Отправляем асинхронно (не блокируем)
	QHostAddress host(serverHost);
	qint64 sent = udpSocket->writeDatagram(data, host, serverPort);
	
	if (sent < 0) {
		qWarning() << "Failed to send crash report:" << udpSocket->errorString();
	} else {
		qInfo() << "Crash report sent to" << serverHost << ":" << serverPort;
	}
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
		if (SymFromAddr(process, stackFrame.AddrPC.Offset, &displacement, symbolInfo)) {
			stackLines << QString("#%1 0x%2 - %3")
				.arg(frameCount)
				.arg(stackFrame.AddrPC.Offset, 0, 16)
				.arg(symbolInfo->Name);
		} else {
			stackLines << QString("#%1 0x%2 - <unknown>")
				.arg(frameCount)
				.arg(stackFrame.AddrPC.Offset, 0, 16);
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

