#pragma once

#include <QObject>
#include <QImage>
#include <QString>
#include <QVector>
#include <QRect>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QTimer>

/**
 * Структура для хранения распознанного текстового блока
 */
struct RecognizedTextBlock
{
    QString text;           // Распознанный текст
    QString translated;     // Переведенный текст
    QRect boundingBox;      // Координаты блока на изображении
    QString languageCode;   // Код языка (например, "en")
};

/**
 * Класс для работы с Yandex Cloud Vision OCR API
 */
class YandexOCR : public QObject
{
    Q_OBJECT

public:
    explicit YandexOCR(QObject *parent = nullptr);
    ~YandexOCR();

    /**
     * Установить IAM токен для Yandex Cloud
     */
    void setIamToken(const QString &token);

    /**
     * Установить идентификатор каталога Yandex Cloud
     */
    void setFolderId(const QString &folderId);

    /**
     * Распознать текст на изображении
     * @param image - изображение для распознавания
     */
    void recognizeText(const QImage &image);

    /**
     * Получить список распознанных текстовых блоков
     */
    QVector<RecognizedTextBlock> getRecognizedBlocks() const;

    /**
     * Проверить, настроены ли учетные данные
     */
    bool isConfigured() const;

signals:
    /**
     * Сигнал о завершении распознавания текста
     * @param success - true, если распознавание прошло успешно
     */
    void recognitionFinished(bool success);

    /**
     * Сигнал об ошибке
     * @param errorMessage - текст ошибки
     */
    void errorOccurred(const QString &errorMessage);

private slots:
    void onRecognitionReplyFinished();
    void onTranslationReplyFinished();

private:
    QString encodeImageToBase64(const QImage &image);
    void parseRecognitionResponse(const QJsonDocument &doc);
    void translateBlock(int blockIndex);
    QRect parseVertices(const QJsonArray &vertices);

    QString iamToken_;
    QString folderId_;
    QNetworkAccessManager *networkManager_;
    QVector<RecognizedTextBlock> recognizedBlocks_;
    QVector<QNetworkReply*> pendingTranslations_;
    int translationsCompleted_;
    int translationsTotal_;
};

