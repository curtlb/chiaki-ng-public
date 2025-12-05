#include "yandexocr.h"
#include <QBuffer>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

YandexOCR::YandexOCR(QObject *parent)
    : QObject(parent)
    , networkManager_(new QNetworkAccessManager(this))
    , translationsCompleted_(0)
    , translationsTotal_(0)
{
}

YandexOCR::~YandexOCR()
{
    // Отменяем все ожидающие запросы
    for (auto *reply : pendingTranslations_) {
        if (reply && reply->isRunning()) {
            reply->abort();
        }
    }
}

void YandexOCR::setIamToken(const QString &token)
{
    iamToken_ = token;
}

void YandexOCR::setFolderId(const QString &folderId)
{
    folderId_ = folderId;
}

bool YandexOCR::isConfigured() const
{
    return !iamToken_.isEmpty() && !folderId_.isEmpty();
}

QString YandexOCR::encodeImageToBase64(const QImage &image)
{
    qCInfo(chiakiGui) << "  encodeImageToBase64: input image size:" << image.size() << "isNull:" << image.isNull();
    qCInfo(chiakiGui) << "  Image format:" << image.format();
    qCInfo(chiakiGui) << "  Image depth:" << image.depth();
    qCInfo(chiakiGui) << "  Image bytesPerLine:" << image.bytesPerLine();
    
    if (image.isNull() || image.size().isEmpty()) {
        qCWarning(chiakiGui) << "⚠️ Cannot encode null or empty image";
        return QString();
    }
    
    // Конвертируем в RGB888 формат если нужно (для надежности)
    QImage rgbImage = image;
    if (image.format() != QImage::Format_RGB888 && image.format() != QImage::Format_RGB32) {
        qCInfo(chiakiGui) << "  Converting image format from" << image.format() << "to RGB888";
        rgbImage = image.convertToFormat(QImage::Format_RGB888);
        if (rgbImage.isNull()) {
            qCWarning(chiakiGui) << "⚠️ Failed to convert image format";
            return QString();
        }
    }
    
    QByteArray byteArray;
    QBuffer buffer(&byteArray);
    
    if (!buffer.open(QIODevice::WriteOnly)) {
        qCWarning(chiakiGui) << "⚠️ Failed to open buffer for writing";
        return QString();
    }
    
    // Пробуем сначала JPEG
    bool saveResult = rgbImage.save(&buffer, "JPEG", 85);
    buffer.close();
    
    qCInfo(chiakiGui) << "  JPEG save result:" << saveResult;
    qCInfo(chiakiGui) << "  JPEG size:" << byteArray.size() << "bytes";
    
    // Если JPEG не сработал, пробуем PNG
    if (!saveResult || byteArray.isEmpty()) {
        qCWarning(chiakiGui) << "  JPEG failed, trying PNG...";
        byteArray.clear();
        buffer.setBuffer(&byteArray);
        if (!buffer.open(QIODevice::WriteOnly)) {
            qCWarning(chiakiGui) << "⚠️ Failed to reopen buffer";
            return QString();
        }
        saveResult = rgbImage.save(&buffer, "PNG");
        buffer.close();
        
        qCInfo(chiakiGui) << "  PNG save result:" << saveResult;
        qCInfo(chiakiGui) << "  PNG size:" << byteArray.size() << "bytes";
        
        if (!saveResult || byteArray.isEmpty()) {
            qCWarning(chiakiGui) << "⚠️ Failed to save image in any format";
            return QString();
        }
    }
    
    QString base64 = QString::fromLatin1(byteArray.toBase64());
    qCInfo(chiakiGui) << "  ✓ Base64 encoded, length:" << base64.length();
    
    return base64;
}

void YandexOCR::recognizeText(const QImage &image)
{
    if (!isConfigured()) {
        qCWarning(chiakiGui) << "YandexOCR: Not configured";
        emit errorOccurred("YandexOCR not configured");
        return;
    }

    recognizedBlocks_.clear();
    pendingTranslations_.clear();
    translationsCompleted_ = 0;
    translationsTotal_ = 0;

    QString base64Image = encodeImageToBase64(image);
    if (base64Image.isEmpty()) {
        qCWarning(chiakiGui) << "YandexOCR: Failed to encode image";
        emit errorOccurred("Failed to encode image");
        return;
    }

    QJsonObject requestBody;
    QString mimeType = (base64Image.length() > 500000) ? "PNG" : "JPEG";
    requestBody["mimeType"] = mimeType;
    requestBody["languageCodes"] = QJsonArray{"en"};
    requestBody["model"] = "page";
    requestBody["content"] = base64Image;

    QJsonDocument doc(requestBody);
    QByteArray jsonData = doc.toJson();

    QNetworkRequest request(QUrl("https://ocr.api.cloud.yandex.net/ocr/v1/recognizeText"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(iamToken_).toUtf8());
    request.setRawHeader("x-folder-id", folderId_.toUtf8());
    request.setRawHeader("x-data-logging-enabled", "true");

    QNetworkReply *reply = networkManager_->post(request, jsonData);
    connect(reply, &QNetworkReply::finished, this, &YandexOCR::onRecognitionReplyFinished);
}

QRect YandexOCR::parseVertices(const QJsonArray &vertices)
{
    if (vertices.size() < 4) {
        return QRect();
    }

    // Берем первую вершину (левый верхний угол) и третью (правый нижний угол)
    QJsonObject topLeft = vertices[0].toObject();
    QJsonObject bottomRight = vertices[2].toObject();

    int x = topLeft["x"].toString().toInt();
    int y = topLeft["y"].toString().toInt();
    int width = bottomRight["x"].toString().toInt() - x;
    int height = bottomRight["y"].toString().toInt() - y;

    return QRect(x, y, width, height);
}

void YandexOCR::parseRecognitionResponse(const QJsonDocument &doc)
{
    QJsonObject root = doc.object();
    QJsonObject result = root["result"].toObject();
    QJsonObject textAnnotation = result["textAnnotation"].toObject();
    QJsonArray blocks = textAnnotation["blocks"].toArray();

    int imageWidth = textAnnotation["width"].toString().toInt();
    int imageHeight = textAnnotation["height"].toString().toInt();

    qCInfo(chiakiGui) << "YandexOCR: Recognized" << blocks.size() << "text blocks"
                      << "Image size:" << imageWidth << "x" << imageHeight;

    for (const QJsonValue &blockValue : blocks) {
        QJsonObject blockObj = blockValue.toObject();
        
        // Получаем bounding box
        QJsonObject boundingBox = blockObj["boundingBox"].toObject();
        QJsonArray vertices = boundingBox["vertices"].toArray();
        QRect rect = parseVertices(vertices);

        // Получаем язык
        QJsonArray languages = blockObj["languages"].toArray();
        QString languageCode;
        if (!languages.isEmpty()) {
            languageCode = languages[0].toObject()["languageCode"].toString();
        }

        // Собираем текст из всех строк
        QString fullText;
        QJsonArray lines = blockObj["lines"].toArray();
        for (const QJsonValue &lineValue : lines) {
            QJsonObject lineObj = lineValue.toObject();
            QString lineText = lineObj["text"].toString();
            if (!fullText.isEmpty()) {
                fullText += "\n";
            }
            fullText += lineText;
        }

        if (!fullText.isEmpty()) {
            RecognizedTextBlock block;
            block.text = fullText;
            block.boundingBox = rect;
            block.languageCode = languageCode;
            recognizedBlocks_.append(block);

        }
    }

    // Теперь переводим каждый блок
    if (recognizedBlocks_.isEmpty()) {
        emit recognitionFinished(true);
        return;
    }

    translationsTotal_ = recognizedBlocks_.size();
    
    // Отправляем запросы перевода с небольшой задержкой чтобы избежать rate limit (20 req/sec)
    for (int i = 0; i < recognizedBlocks_.size(); ++i) {
        // Задержка 60ms между запросами = макс 16 req/sec (безопасно для лимита 20 req/sec)
        QTimer::singleShot(i * 60, this, [this, i]() {
            if (i < recognizedBlocks_.size()) {
                translateBlock(i);
            }
        });
    }
}

void YandexOCR::translateBlock(int blockIndex)
{
    if (blockIndex < 0 || blockIndex >= recognizedBlocks_.size()) {
        return;
    }
    
    RecognizedTextBlock &block = recognizedBlocks_[blockIndex];
    
    // Только английский язык переводим на русский
    if (block.languageCode != "en") {
        block.translated = block.text;
        ++translationsCompleted_;
        if (translationsCompleted_ >= translationsTotal_) {
            emit recognitionFinished(true);
        }
        return;
    }

    QJsonObject requestBody;
    requestBody["targetLanguageCode"] = "ru";
    requestBody["texts"] = QJsonArray{block.text};
    requestBody["folderId"] = folderId_;

    QJsonDocument doc(requestBody);
    QByteArray jsonData = doc.toJson();

    QNetworkRequest request(QUrl("https://translate.api.cloud.yandex.net/translate/v2/translate"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(iamToken_).toUtf8());


    QNetworkReply *reply = networkManager_->post(request, jsonData);
    
    // Сохраняем индекс блока в reply
    reply->setProperty("blockIndex", blockIndex);
    
    connect(reply, &QNetworkReply::finished, this, &YandexOCR::onTranslationReplyFinished);
    pendingTranslations_.append(reply);
}

void YandexOCR::onRecognitionReplyFinished()
{
    qCInfo(chiakiGui) << "=== onRecognitionReplyFinished() START ===";
    
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        qCWarning(chiakiGui) << "⚠️ Reply is null!";
        return;
    }

    qCInfo(chiakiGui) << "";
    qCInfo(chiakiGui) << "=== RESPONSE DETAILS ===";
    qCInfo(chiakiGui) << "HTTP Status:" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    qCInfo(chiakiGui) << "Error code:" << reply->error();
    qCInfo(chiakiGui) << "";
    qCInfo(chiakiGui) << "RESPONSE HEADERS:";
    for (const auto &header : reply->rawHeaderPairs()) {
        qCInfo(chiakiGui) << "  " << header.first << ":" << header.second;
    }

    if (reply->error() != QNetworkReply::NoError) {
        QString errorMsg = QString("Recognition error: %1").arg(reply->errorString());
        qCWarning(chiakiGui) << "";
        qCWarning(chiakiGui) << "⚠️ ERROR OCCURRED:";
        qCWarning(chiakiGui) << "  Message:" << errorMsg;
        
        QByteArray errorData = reply->readAll();
        if (!errorData.isEmpty()) {
            qCWarning(chiakiGui) << "";
            qCWarning(chiakiGui) << "ERROR RESPONSE BODY:";
            
            // Пытаемся распарсить как JSON для красивого вывода
            QJsonDocument errorDoc = QJsonDocument::fromJson(errorData);
            if (!errorDoc.isNull()) {
                qCWarning(chiakiGui) << errorDoc.toJson(QJsonDocument::Indented);
            } else {
                qCWarning(chiakiGui) << errorData;
            }
        }
        qCWarning(chiakiGui) << "=== END RESPONSE DETAILS ===";
        
        emit errorOccurred(errorMsg);
        emit recognitionFinished(false);
        reply->deleteLater();
        return;
    }

    QByteArray responseData = reply->readAll();
    qCInfo(chiakiGui) << "  Response size:" << responseData.size() << "bytes";

    QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (doc.isNull()) {
        qCWarning(chiakiGui) << "⚠️ Failed to parse recognition response";
        qCWarning(chiakiGui) << "  Raw response:" << responseData.left(500);
        emit errorOccurred("Failed to parse recognition response");
        emit recognitionFinished(false);
        reply->deleteLater();
        return;
    }

    qCInfo(chiakiGui) << "✓ Response parsed successfully, processing...";
    parseRecognitionResponse(doc);
    reply->deleteLater();
    qCInfo(chiakiGui) << "=== onRecognitionReplyFinished() END ===";
}

void YandexOCR::onTranslationReplyFinished()
{
    qCInfo(chiakiGui) << "=== onTranslationReplyFinished() ===";
    
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        qCWarning(chiakiGui) << "⚠️ Translation reply is null!";
        return;
    }

    int blockIndex = reply->property("blockIndex").toInt();
    qCInfo(chiakiGui) << "  Block index:" << blockIndex;
    qCInfo(chiakiGui) << "  HTTP Status:" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError) {
        qCWarning(chiakiGui) << "⚠️ YandexOCR: Translation error:" << reply->errorString();
        QByteArray errorData = reply->readAll();
        if (!errorData.isEmpty()) {
            qCWarning(chiakiGui) << "  Error response:" << errorData;
        }
        // Используем оригинальный текст, если перевод не удался
        if (blockIndex >= 0 && blockIndex < recognizedBlocks_.size()) {
            recognizedBlocks_[blockIndex].translated = recognizedBlocks_[blockIndex].text;
        }
    } else {
        QByteArray responseData = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(responseData);

        if (!doc.isNull()) {
            QJsonObject root = doc.object();
            QJsonArray translations = root["translations"].toArray();
            if (!translations.isEmpty()) {
                QString translatedText = translations[0].toObject()["text"].toString();
                if (blockIndex >= 0 && blockIndex < recognizedBlocks_.size()) {
                    recognizedBlocks_[blockIndex].translated = translatedText;
                    qCInfo(chiakiGui) << "YandexOCR: Translated block" << blockIndex 
                                      << ":" << translatedText;
                }
            }
        }
    }

    ++translationsCompleted_;
    pendingTranslations_.removeOne(reply);
    reply->deleteLater();

    // Если все переводы завершены, отправляем сигнал
    if (translationsCompleted_ >= translationsTotal_) {
        qCInfo(chiakiGui) << "YandexOCR: All translations completed";
        emit recognitionFinished(true);
    }
}

QVector<RecognizedTextBlock> YandexOCR::getRecognizedBlocks() const
{
    return recognizedBlocks_;
}

