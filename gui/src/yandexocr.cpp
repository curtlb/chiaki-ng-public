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
    QByteArray byteArray;
    QBuffer buffer(&byteArray);
    buffer.open(QIODevice::WriteOnly);
    
    // Конвертируем в JPEG для уменьшения размера
    image.save(&buffer, "JPEG", 85);
    
    return byteArray.toBase64();
}

void YandexOCR::recognizeText(const QImage &image)
{
    qCInfo(chiakiGui) << "=== YandexOCR::recognizeText() START ===";
    qCInfo(chiakiGui) << "  Image size:" << image.size();
    qCInfo(chiakiGui) << "  Image isNull:" << image.isNull();
    qCInfo(chiakiGui) << "  isConfigured:" << isConfigured();
    
    if (!isConfigured()) {
        qCWarning(chiakiGui) << "⚠️ YandexOCR not configured. Set IAM token and folder ID first.";
        emit errorOccurred("YandexOCR not configured. Set IAM token and folder ID first.");
        return;
    }

    recognizedBlocks_.clear();
    pendingTranslations_.clear();
    translationsCompleted_ = 0;
    translationsTotal_ = 0;

    qCInfo(chiakiGui) << "Encoding image to base64...";
    QString base64Image = encodeImageToBase64(image);
    qCInfo(chiakiGui) << "  Base64 image length:" << base64Image.length();

    QJsonObject requestBody;
    requestBody["mimeType"] = "JPEG";
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

    qCInfo(chiakiGui) << "Sending OCR request to Yandex Cloud...";
    qCInfo(chiakiGui) << "  URL:" << request.url();
    qCInfo(chiakiGui) << "  Folder ID:" << folderId_;
    qCInfo(chiakiGui) << "  Request size:" << jsonData.size() << "bytes";

    QNetworkReply *reply = networkManager_->post(request, jsonData);
    connect(reply, &QNetworkReply::finished, this, &YandexOCR::onRecognitionReplyFinished);
    
    qCInfo(chiakiGui) << "OCR request sent, waiting for response...";
    qCInfo(chiakiGui) << "=== YandexOCR::recognizeText() END ===";
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

            qCInfo(chiakiGui) << "YandexOCR: Block" << recognizedBlocks_.size() 
                              << "- Text:" << fullText
                              << "Lang:" << languageCode
                              << "Rect:" << rect;
        }
    }

    // Теперь переводим каждый блок
    if (recognizedBlocks_.isEmpty()) {
        emit recognitionFinished(true);
        return;
    }

    translationsTotal_ = recognizedBlocks_.size();
    for (int i = 0; i < recognizedBlocks_.size(); ++i) {
        translateBlock(i);
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

    qCInfo(chiakiGui) << "  HTTP Status:" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    qCInfo(chiakiGui) << "  Error code:" << reply->error();

    if (reply->error() != QNetworkReply::NoError) {
        QString errorMsg = QString("Recognition error: %1").arg(reply->errorString());
        qCWarning(chiakiGui) << "⚠️ YandexOCR:" << errorMsg;
        
        QByteArray errorData = reply->readAll();
        if (!errorData.isEmpty()) {
            qCWarning(chiakiGui) << "  Error response:" << errorData;
        }
        
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

