#pragma once

#include "yandexocr.h"
#include <QObject>
#include <QVector>
#include <QVariantList>
#include <QVariantMap>
#include <QPainter>
#include <QFont>
#include <QColor>
#include <QMutex>

/**
 * Класс для отображения переведенного текста поверх видео
 */
class TextOverlay : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    Q_PROPERTY(QVariantList textBlocks READ getTextBlocksQml NOTIFY textBlocksChanged)
    Q_PROPERTY(int imageWidth READ getImageWidth NOTIFY imageSizeChanged)
    Q_PROPERTY(int imageHeight READ getImageHeight NOTIFY imageSizeChanged)

public:
    explicit TextOverlay(QObject *parent = nullptr);
    ~TextOverlay();

    /**
     * Установить блоки текста для отображения
     * @param blocks - список распознанных и переведенных блоков
     * @param imageSize - размер изображения, на котором были распознаны блоки
     */
    void setTextBlocks(const QVector<RecognizedTextBlock> &blocks, const QSize &imageSize);

    /**
     * Очистить оверлей
     */
    void clear();

    /**
     * Отрисовать оверлей на QPainter
     * @param painter - painter для рисования
     * @param targetSize - размер области рисования
     */
    void render(QPainter &painter, const QSize &targetSize);

    /**
     * Проверить, есть ли активный оверлей
     */
    bool isActive() const;

    /**
     * Установить видимость оверлея
     */
    void setVisible(bool visible);

    /**
     * Получить видимость оверлея
     */
    bool isVisible() const;

    /**
     * Получить список блоков для QML
     */
    QVariantList getTextBlocksQml() const;
    
    /**
     * Получить ширину изображения
     */
    int getImageWidth() const { return originalImageSize_.width(); }
    
    /**
     * Получить высоту изображения
     */
    int getImageHeight() const { return originalImageSize_.height(); }

signals:
    void activeChanged();
    void textBlocksChanged();
    void imageSizeChanged();

private:
    QRect scaleRect(const QRect &rect, const QSize &fromSize, const QSize &toSize);
    void drawTextBlock(QPainter &painter, const RecognizedTextBlock &block, const QRect &rect);

    QVector<RecognizedTextBlock> textBlocks_;
    QVariantList cachedQmlBlocks_;  // Закэшированная версия для QML (без mutex!)
    QSize originalImageSize_;
    QMutex mutex_;
    bool visible_;
    QFont font_;
    QColor backgroundColor_;
    QColor textColor_;
    QColor borderColor_;
};

