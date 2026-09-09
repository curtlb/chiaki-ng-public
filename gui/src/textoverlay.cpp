#include "textoverlay.h"
#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

TextOverlay::TextOverlay(QObject *parent)
    : QObject(parent)
    , visible_(false)
{
    // Настройка шрифта для отображения текста
    font_.setFamily("Arial");
    font_.setPixelSize(20);
    font_.setBold(true);

    // Цвета для оверлея
    backgroundColor_ = QColor(0, 0, 0, 180); // Полупрозрачный черный фон
    textColor_ = QColor(255, 255, 255, 255); // Белый текст
    borderColor_ = QColor(255, 0, 0, 200);   // Красная рамка
}

TextOverlay::~TextOverlay()
{
}

void TextOverlay::setTextBlocks(const QVector<RecognizedTextBlock> &blocks, const QSize &imageSize)
{
    QMutexLocker locker(&mutex_);
    textBlocks_ = blocks;
    originalImageSize_ = imageSize;
    visible_ = !blocks.isEmpty();
    
    // Кэшируем QVariantList для QML чтобы избежать deadlock
    cachedQmlBlocks_.clear();
    for (const RecognizedTextBlock &block : textBlocks_) {
        QVariantMap blockMap;
        blockMap["original"] = block.text;
        blockMap["translated"] = block.translated;
        blockMap["language"] = block.languageCode;
        
        QVariantMap bboxMap;
        bboxMap["x"] = block.boundingBox.x();
        bboxMap["y"] = block.boundingBox.y();
        bboxMap["width"] = block.boundingBox.width();
        bboxMap["height"] = block.boundingBox.height();
        blockMap["boundingBox"] = bboxMap;
        
        cachedQmlBlocks_.append(blockMap);
    }
    
    locker.unlock();
    
    qCInfo(chiakiGui) << "TextOverlay: Set" << blocks.size() << "text blocks, image size:" << imageSize;
    
    // Уведомляем QML об изменениях
    emit textBlocksChanged();
    emit imageSizeChanged();
    emit activeChanged();
}

void TextOverlay::clear()
{
    QMutexLocker locker(&mutex_);
    textBlocks_.clear();
    cachedQmlBlocks_.clear();
    visible_ = false;
    
    locker.unlock();
    
    qCInfo(chiakiGui) << "TextOverlay: Cleared";
    
    // Уведомляем QML об изменениях
    emit textBlocksChanged();
    emit activeChanged();
}

bool TextOverlay::isActive() const
{
    QMutexLocker locker(const_cast<QMutex*>(&mutex_));
    return !textBlocks_.isEmpty();
}

void TextOverlay::setVisible(bool visible)
{
    QMutexLocker locker(&mutex_);
    visible_ = visible;
}

bool TextOverlay::isVisible() const
{
    QMutexLocker locker(const_cast<QMutex*>(&mutex_));
    return visible_;
}

QRect TextOverlay::scaleRect(const QRect &rect, const QSize &fromSize, const QSize &toSize)
{
    if (fromSize.width() == 0 || fromSize.height() == 0) {
        return QRect();
    }

    float scaleX = static_cast<float>(toSize.width()) / fromSize.width();
    float scaleY = static_cast<float>(toSize.height()) / fromSize.height();

    int x = static_cast<int>(rect.x() * scaleX);
    int y = static_cast<int>(rect.y() * scaleY);
    int width = static_cast<int>(rect.width() * scaleX);
    int height = static_cast<int>(rect.height() * scaleY);

    return QRect(x, y, width, height);
}

void TextOverlay::drawTextBlock(QPainter &painter, const RecognizedTextBlock &block, const QRect &rect)
{
    // Рисуем рамку вокруг области текста
    QPen borderPen(borderColor_, 2);
    painter.setPen(borderPen);
    painter.drawRect(rect);

    // Рисуем полупрозрачный фон для текста
    painter.fillRect(rect, backgroundColor_);

    // Настраиваем шрифт в зависимости от размера блока
    QFont scaledFont = font_;
    int fontSize = qMax(12, qMin(rect.height() / 3, 32));
    scaledFont.setPixelSize(fontSize);
    painter.setFont(scaledFont);

    // Рисуем переведенный текст
    painter.setPen(textColor_);
    QTextOption textOption;
    textOption.setAlignment(Qt::AlignCenter);
    textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    // Добавляем небольшие отступы
    QRect textRect = rect.adjusted(5, 5, -5, -5);
    painter.drawText(textRect, block.translated, textOption);
}

void TextOverlay::render(QPainter &painter, const QSize &targetSize)
{
    QMutexLocker locker(&mutex_);

    if (!visible_ || textBlocks_.isEmpty()) {
        return;
    }

    painter.save();

    // Включаем антиалиасинг для более плавного текста
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    for (const RecognizedTextBlock &block : textBlocks_) {
        // Масштабируем координаты блока под текущий размер окна
        QRect scaledRect = scaleRect(block.boundingBox, originalImageSize_, targetSize);
        
        if (scaledRect.isValid() && !block.translated.isEmpty()) {
            drawTextBlock(painter, block, scaledRect);
        }
    }

    painter.restore();

    qCDebug(chiakiGui) << "TextOverlay: Rendered" << textBlocks_.size() << "blocks to size:" << targetSize;
}

QVariantList TextOverlay::getTextBlocksQml() const
{
    // Возвращаем закэшированную версию - БЕЗ mutex чтобы избежать deadlock!
    // Кэш обновляется в setTextBlocks() и clear()
    return cachedQmlBlocks_;
}

