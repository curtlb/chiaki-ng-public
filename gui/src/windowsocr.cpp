// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "windowsocr.h"
#include <QDebug>

// Windows OCR requires MSVC compiler with C++/WinRT support
// For MSYS2/MinGW builds, OCR will be disabled
#if defined(_WIN32) && defined(_MSC_VER) && !defined(__MINGW32__)
#define CHIAKI_WINDOWS_OCR_AVAILABLE
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <robuffer.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media::Ocr;
using namespace Windows::Storage::Streams;
using namespace Windows::Globalization;
#endif

WindowsOCR::WindowsOCR(QObject *parent)
	: QObject(parent)
{
#ifdef CHIAKI_WINDOWS_OCR_AVAILABLE
	try {
		winrt::init_apartment();
	} catch (...) {
		// Apartment already initialized
	}
#endif
}

WindowsOCR::~WindowsOCR()
{
}

bool WindowsOCR::isAvailable()
{
#ifdef CHIAKI_WINDOWS_OCR_AVAILABLE
	return true;
#else
	return false;
#endif
}

void WindowsOCR::recognizeText(const QImage &image, const QString &language)
{
#ifdef CHIAKI_WINDOWS_OCR_AVAILABLE
	processImage(image, language);
#else
	emit ocrError("Windows OCR not available on this platform (requires MSVC compiler)");
#endif
}

#ifdef CHIAKI_WINDOWS_OCR_AVAILABLE
void WindowsOCR::processImage(const QImage &image, const QString &language)
{
	try {
		// Convert QImage to BGRA32 format for Windows
		QImage bgra_image = image.convertToFormat(QImage::Format_RGBA8888);
		
		// Create SoftwareBitmap from QImage data
		auto bitmap = SoftwareBitmap(
			BitmapPixelFormat::Rgba8,
			bgra_image.width(),
			bgra_image.height()
		);

		// Copy image data
		auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
		auto reference = buffer.CreateReference();
		
		byte* data;
		uint32_t capacity;
		auto interop = reference.as<Windows::Storage::Streams::IBufferByteAccess>();
		interop->Buffer(&data);
		
		memcpy(data, bgra_image.bits(), bgra_image.sizeInBytes());

		// Create OCR engine
		Language lang(language.toStdWString().c_str());
		auto engine = OcrEngine::TryCreateFromLanguage(lang);
		
		if(!engine)
		{
			// Fallback to English if requested language is not available
			lang = Language(L"en");
			engine = OcrEngine::TryCreateFromLanguage(lang);
		}

		if(!engine)
		{
			emit ocrError("Failed to create OCR engine");
			return;
		}

		// Recognize text
		auto result = engine.RecognizeAsync(bitmap).get();

		// Extract text
		QString recognized_text = QString::fromStdWString(result.Text().c_str());

		emit textRecognized(recognized_text);

	} catch (const std::exception &e) {
		emit ocrError(QString("OCR error: %1").arg(e.what()));
	} catch (...) {
		emit ocrError("Unknown OCR error");
	}
}
#endif

