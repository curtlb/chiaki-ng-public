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
#include <QThread>
#include <QCoreApplication>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
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
	, udpSocket(nullptr)
	, serverHost("5.188.29.131")
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

#ifdef Q_OS_WIN
	// Список загруженных модулей (DLL) с их адресами
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

	// Дополнительная отладочная информация (упрощенно, чтобы не увеличивать размер пакета)
	QJsonObject debugInfo;
	
	// Информация о потоке, в котором произошел краш
	try {
		debugInfo["thread_id"] = static_cast<qint64>(reinterpret_cast<quintptr>(QThread::currentThreadId()));
		if (QCoreApplication::instance()) {
			debugInfo["is_main_thread"] = (QThread::currentThread() == QCoreApplication::instance()->thread());
		} else {
			debugInfo["is_main_thread"] = false;
		}
	} catch (...) {
		// Игнорируем ошибки при получении информации о потоке
	}
	
	report["debug_info"] = debugInfo;

	// Конвертируем в JSON
	QJsonDocument doc(report);
	return doc.toJson(QJsonDocument::Compact);
}

void CrashReporter::SendReport(const QString &errorType, const QString &message, const QString &stackTrace)
{
	if (!instance) {
		// В обработчике исключений qWarning может не работать
		return;
	}

	// Обернуто в try-catch на случай проблем с Qt при краше
	try {
		QByteArray data = CreateReport(errorType, message, stackTrace);
		if (!data.isEmpty()) {
			instance->sendData(data);
		}
	} catch (...) {
		// Если создание отчета или отправка не удались, просто игнорируем
		// В обработчике исключений мы не можем безопасно логировать
	}
}

void CrashReporter::SendExceptionReport(const QString &exceptionType, const QString &what)
{
	SendReport("exception", QString("%1: %2").arg(exceptionType, what));
}

void CrashReporter::sendData(const QByteArray &data)
{
	if (!udpSocket) {
		return;
	}

	// Обернуто в try-catch на случай проблем с Qt при краше
	try {
		// Проверяем размер данных (UDP пакет не должен превышать ~64KB, но на практике лучше ограничиться меньшим размером)
		const int maxUdpSize = 60000; // Оставляем запас
		QByteArray dataToSend = data;
		if (dataToSend.size() > maxUdpSize) {
			// Пытаемся обрезать stack trace
			QJsonDocument doc = QJsonDocument::fromJson(dataToSend);
			if (!doc.isNull() && doc.isObject()) {
				QJsonObject obj = doc.object();
				QString stackTrace = obj["stack_trace"].toString();
				if (stackTrace.length() > 10000) {
					stackTrace = stackTrace.left(10000) + "\n... (truncated)";
					obj["stack_trace"] = stackTrace;
					doc.setObject(obj);
					dataToSend = doc.toJson(QJsonDocument::Compact);
				}
			}
			// Если все еще слишком большой, просто обрезаем
			if (dataToSend.size() > maxUdpSize) {
				dataToSend = dataToSend.left(maxUdpSize);
			}
		}

		// Отправляем данные
		QHostAddress host(serverHost);
		qint64 sent = udpSocket->writeDatagram(dataToSend, host, serverPort);
		
		// Обрабатываем события для гарантированной отправки
		if (QCoreApplication::instance()) {
			// Обрабатываем события несколько раз для гарантированной отправки
			for (int i = 0; i < 5; i++) {
				QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
				QThread::msleep(10); // Небольшая задержка для отправки UDP пакета
			}
		} else {
			// Если нет event loop, просто ждем
			QThread::msleep(100);
		}
		
		// Не логируем в обработчике исключений, так как это может не работать
	} catch (...) {
		// Если отправка не удалась, просто игнорируем
		// В обработчике исключений мы не можем безопасно логировать
	}
}

#ifdef Q_OS_WIN
LONG WINAPI CrashReporter::ExceptionHandler(EXCEPTION_POINTERS *exceptionInfo)
{
	QString errorType = "crash";
	QString message;
	QString stackTrace;

	// Определяем тип исключения и добавляем детали для Access Violation
	QString exceptionDetails;
	switch (exceptionInfo->ExceptionRecord->ExceptionCode) {
		case EXCEPTION_ACCESS_VIOLATION:
			message = "Access Violation";
			// Добавляем информацию о типе доступа и адресе
			if (exceptionInfo->ExceptionRecord->NumberParameters >= 2) {
				bool isWrite = exceptionInfo->ExceptionRecord->ExceptionInformation[0] != 0;
				ULONG_PTR address = exceptionInfo->ExceptionRecord->ExceptionInformation[1];
				exceptionDetails = QString(" (%1 at address 0x%2)")
					.arg(isWrite ? "Write" : "Read")
					.arg(address, 0, 16);
			}
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
	
	// Добавляем детали к сообщению
	if (!exceptionDetails.isEmpty()) {
		message += exceptionDetails;
	}

	// Пытаемся получить stack trace (обернуто в try-catch для безопасности)
	// Используем минимальные Qt операции, так как куча может быть повреждена
	try {
		HANDLE process = GetCurrentProcess();
		if (!SymInitialize(process, NULL, TRUE)) {
			stackTrace = QString("Failed to initialize symbol handler");
		} else {
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

				// Определяем модуль по адресу (безопасно, без Qt классов)
				HMODULE hModule = NULL;
				char moduleName[MAX_PATH] = "<unknown>";
				DWORD64 moduleBase = 0;
				
				// Используем проверки возвращаемых значений вместо __try/__except (для MinGW совместимости)
				if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					(LPCTSTR)stackFrame.AddrPC.Offset, &hModule)) {
					wchar_t modulePath[MAX_PATH];
					if (GetModuleFileNameW(hModule, modulePath, MAX_PATH)) {
						// Извлекаем только имя файла без Qt классов
						const wchar_t* fileName = wcsrchr(modulePath, L'\\');
						if (fileName) {
							fileName++; // Пропускаем обратный слэш
						} else {
							fileName = modulePath;
						}
						// Конвертируем в char (упрощенно, только ASCII)
						int i = 0;
						while (fileName[i] && i < MAX_PATH - 1) {
							moduleName[i] = (char)fileName[i];
							i++;
						}
						moduleName[i] = '\0';
						moduleBase = SymGetModuleBase64(process, stackFrame.AddrPC.Offset);
					}
				}
				
				// Получаем информацию о символе (безопасно)
				char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)];
				PSYMBOL_INFO symbolInfo = (PSYMBOL_INFO)symbolBuffer;
				symbolInfo->SizeOfStruct = sizeof(SYMBOL_INFO);
				symbolInfo->MaxNameLen = MAX_SYM_NAME;

				DWORD64 displacement = 0;
				QString symbolName = "<unknown symbol>";
				// Используем проверку возвращаемого значения вместо __try/__except
				if (SymFromAddr(process, stackFrame.AddrPC.Offset, &displacement, symbolInfo)) {
					symbolName = QString::fromLocal8Bit(symbolInfo->Name);
				}
				
				// Вычисляем смещение внутри модуля
				DWORD64 offsetInModule = stackFrame.AddrPC.Offset - moduleBase;
				
				// Формируем строку stack trace с полной информацией
				QString frameInfo = QString("#%1 0x%2 [%3+0x%4] %5")
					.arg(frameCount)
					.arg(stackFrame.AddrPC.Offset, 0, 16)
					.arg(QString::fromLocal8Bit(moduleName))
					.arg(offsetInModule, 0, 16)
					.arg(symbolName);
				
				stackLines << frameInfo;
				frameCount++;
			}

			SymCleanup(process);
			stackTrace = stackLines.join("\n");
		}
	} catch (...) {
		// Если что-то пошло не так, используем простой stack trace
		stackTrace = QString("Failed to generate detailed stack trace");
	}

	// Отправляем отчет (обернуто в try-catch на случай проблем с Qt)
	try {
		SendReport(errorType, message, stackTrace);
	} catch (...) {
		// Если даже отправка не удалась, хотя бы логируем
		// (но в обработчике исключений логирование может не работать)
	}

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

