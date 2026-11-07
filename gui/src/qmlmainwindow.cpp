#include "qmlmainwindow.h"
#include "qmlbackend.h"
#include "qmlsvgprovider.h"
#include "chiaki/log.h"
#include "streamsession.h"

#include <qpa/qplatformnativeinterface.h>

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

extern "C" {
#include <libswscale/swscale.h>
}

#include <QDebug>
#include <QThread>
#include <QShortcut>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QVulkanInstance>
#include <QQuickItem>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickRenderTarget>
#include <QQuickRenderControl>
#include <QQuickGraphicsDevice>
#include <QDateTime>
#include <QScreen>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#if defined(Q_OS_MACOS)
#include <objc/message.h>
#endif

Q_LOGGING_CATEGORY(chiakiGui, "chiaki.gui", QtInfoMsg);

static void placebo_log_cb(void *user, pl_log_level level, const char *msg)
{
    ChiakiLogLevel chiaki_level;
    switch (level) {
    case PL_LOG_NONE:
    case PL_LOG_TRACE:
    case PL_LOG_DEBUG:
        qCDebug(chiakiGui).noquote() << "[libplacebo]" << msg;
        break;
    case PL_LOG_INFO:
        qCInfo(chiakiGui).noquote() << "[libplacebo]" << msg;
        break;
    case PL_LOG_WARN:
        qCWarning(chiakiGui).noquote() << "[libplacebo]" << msg;
        break;
    case PL_LOG_ERR:
    case PL_LOG_FATAL:
        qCCritical(chiakiGui).noquote() << "[libplacebo]" << msg;
        break;
    }
}

static QString shader_cache_path()
{
    static QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/pl_shader.cache";
    return path;
}


static const char *render_params_path()
{
    static QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/Chiaki/pl_render_params.conf";
    return qPrintable(path);
}

class RenderControl : public QQuickRenderControl
{
public:
    explicit RenderControl(QWindow *window)
        : window(window)
    {
    }

    QWindow *renderWindow(QPoint *offset) override
    {
        Q_UNUSED(offset);
        return window;
    }

private:
    QWindow *window = {};
};

QmlMainWindow::QmlMainWindow(Settings *settings, bool exit_app_on_stream_exit)
    : QWindow()
    , settings(settings)
{
    init(settings, exit_app_on_stream_exit);
}

QmlMainWindow::QmlMainWindow(const StreamSessionConnectInfo &connect_info, bool exit_app_on_stream_exit)
    : QWindow()
    , settings(connect_info.settings)
{
    direct_stream = true;
    emit directStreamChanged();
    init(connect_info.settings, exit_app_on_stream_exit);
    backend->createSession(connect_info);

    if (connect_info.fullscreen || connect_info.zoom || connect_info.stretch)
        fullscreenTime();
    if (connect_info.zoom)
        setVideoMode(VideoMode::Zoom);
    else if (connect_info.stretch)
        setVideoMode(VideoMode::Stretch);

    if(exit_app_on_stream_exit)
        connect(session, &StreamSession::SessionQuit, qGuiApp, &QGuiApplication::quit);
}

QmlMainWindow::~QmlMainWindow()
{
    Q_ASSERT(!placebo_swapchain);

#ifndef Q_OS_MACOS
    QMetaObject::invokeMethod(quick_render, &QQuickRenderControl::invalidate);
    render_thread->quit();
    render_thread->wait();
#endif

    delete quick_item;
    delete quick_window;
    // calls invalidate here if not already called
    delete quick_render;
    delete render_thread->parent();
    delete qml_engine;
    delete qt_vk_inst;

    // Освобождаем screenshot_frame
    if (screenshot_frame) {
        av_frame_free(&screenshot_frame);
    }

    av_buffer_unref(&vulkan_hw_dev_ctx);

    pl_unmap_avframe(placebo_vulkan->gpu, &current_frame);
    pl_unmap_avframe(placebo_vulkan->gpu, &previous_frame);
    for (int i = 0; i < 3; i++) {
        pl_unmap_avframe(placebo_vulkan->gpu, &gen_pl[i]);
        av_frame_free(&gen_av[i]);
    }
    av_frame_free(&held_sw);

    pl_tex_destroy(placebo_vulkan->gpu, &quick_tex);
    pl_vulkan_sem_destroy(placebo_vulkan->gpu, &quick_sem);

    for (auto tex : placebo_tex)
        pl_tex_destroy(placebo_vulkan->gpu, &tex);

    FILE *file = fopen(qPrintable(shader_cache_path()), "wb");
    if (file) {
        pl_cache_save_file(placebo_cache, file);
        fclose(file);
    }
    pl_cache_destroy(&placebo_cache);
    pl_renderer_destroy(&placebo_renderer);
    pl_vulkan_destroy(&placebo_vulkan);
    pl_vk_inst_destroy(&placebo_vk_inst);
    pl_options_free(&renderparams_opts);
    pl_log_destroy(&placebo_log);
}

void QmlMainWindow::updateWindowType(WindowType type)
{
    switch (type) {
    case WindowType::SelectedResolution:
        setVideoMode(VideoMode::Normal);
        break;
    case WindowType::CustomResolution:
        setVideoMode(VideoMode::Normal);
        break;
    case WindowType::Fullscreen:
        fullscreenTime();
        setVideoMode(VideoMode::Normal);
        break;
    case WindowType::Zoom:
        fullscreenTime();
        setVideoMode(VideoMode::Zoom);
        break;
    case WindowType::Stretch:
        fullscreenTime();
        setVideoMode(VideoMode::Stretch);
        break;
    default:
        break;
    }
}

bool QmlMainWindow::hasVideo() const
{
    return has_video;
}

int QmlMainWindow::droppedFrames() const
{
    return dropped_frames;
}

bool QmlMainWindow::keepVideo() const
{
    return keep_video;
}

void QmlMainWindow::setKeepVideo(bool keep)
{
    keep_video = keep;
    emit keepVideoChanged();
}

void QmlMainWindow::grabInput()
{
    setCursor(Qt::ArrowCursor);
    grab_input++;
    if (session)
        session->BlockInput(grab_input);
}

void QmlMainWindow::releaseInput()
{
    if (!grab_input)
        return;
    grab_input--;
    if (!grab_input && has_video && settings->GetHideCursor())
        setCursor(Qt::BlankCursor);
    if (session)
        session->BlockInput(grab_input);
}

bool QmlMainWindow::directStream() const
{
    return direct_stream;
}

QmlMainWindow::VideoMode QmlMainWindow::videoMode() const
{
    return video_mode;
}

void QmlMainWindow::setVideoMode(VideoMode mode)
{
    video_mode = mode;
    emit videoModeChanged();
}

float QmlMainWindow::zoomFactor() const
{
    return zoom_factor;
}

void QmlMainWindow::setZoomFactor(float factor)
{
    zoom_factor = factor;
    emit zoomFactorChanged();
}

QmlMainWindow::VideoPreset QmlMainWindow::videoPreset() const
{
    return video_preset;
}

void QmlMainWindow::setVideoPreset(VideoPreset preset)
{
    video_preset = preset;
    emit videoPresetChanged();
}

void QmlMainWindow::setSettings(Settings *new_settings)
{
    settings = new_settings;
    QString profile = settings->GetCurrentProfile();
    qCCritical(chiakiGui) << "Current Profile: " << profile;
    if(profile.isEmpty())
        QGuiApplication::setApplicationDisplayName("chiaki-ng");
    else
        QGuiApplication::setApplicationDisplayName(QString("chiaki-ng:%1").arg(profile));
    this->setTitle(QGuiApplication::applicationDisplayName());
}

void QmlMainWindow::show()
{
    QQmlComponent component(qml_engine, QUrl(QStringLiteral("qrc:/Main.qml")));
    if (!component.isReady()) {
        qCCritical(chiakiGui) << "Component not ready\n" << component.errors();
        QMetaObject::invokeMethod(QGuiApplication::instance(), &QGuiApplication::quit, Qt::QueuedConnection);
        return;
    }

    QVariantMap props;
    props[QStringLiteral("parent")] = QVariant::fromValue(quick_window->contentItem());
    quick_item = qobject_cast<QQuickItem*>(component.createWithInitialProperties(props));
    if (!quick_item) {
        qCCritical(chiakiGui) << "Failed to create root item\n" << component.errors();
        QMetaObject::invokeMethod(QGuiApplication::instance(), &QGuiApplication::quit, Qt::QueuedConnection);
        return;
    }
    auto screen_size = QGuiApplication::primaryScreen()->availableSize();
    resize(screen_size);

    if (qEnvironmentVariable("XDG_CURRENT_DESKTOP") == "gamescope")
        fullscreenTime();
    else
    {
        if(!settings->GetGeometry().isEmpty())
        {
            setGeometry(settings->GetGeometry());
            showNormal();
        }
        else
            showMaximized();
        setWindowAdjustable(true);
    }
}

void QmlMainWindow::presentFrame(AVFrame *frame, int32_t frames_lost)
{
    frame_mutex.lock();
    if (av_frame) {
        qCDebug(chiakiGui) << "Dropping rendering frame";
        dropped_frames_current++;
        av_frame_free(&av_frame);
    }
    av_frame = frame;
    
    // Сохраняем копию фрейма для скриншотов (каждый 30й фрейм чтобы не нагружать систему)
    static int frame_counter = 0;
    if (frame && (++frame_counter % 30 == 0)) {
        if (screenshot_frame) {
            av_frame_free(&screenshot_frame);
        }
        screenshot_frame = av_frame_clone(frame);
    }
    
    frame_mutex.unlock();

    dropped_frames_current += frames_lost;

    if (!has_video) {
        has_video = true;
        if (!grab_input && settings->GetHideCursor())
            setCursor(Qt::BlankCursor);
        emit hasVideoChanged();
    }
    if (QScreen *scr = screen()) {
        const float hz = scr->refreshRate();
        if (hz >= 30.f)
            display_hz.store(hz, std::memory_order_relaxed);
    }

    update();
}

AVBufferRef *QmlMainWindow::vulkanHwDeviceCtx()
{
    if (vulkan_hw_dev_ctx || vk_decode_queue_index < 0)
        return vulkan_hw_dev_ctx;

    vulkan_hw_dev_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    AVHWDeviceContext *hwctx = reinterpret_cast<AVHWDeviceContext*>(vulkan_hw_dev_ctx->data);
    hwctx->user_opaque = const_cast<void*>(reinterpret_cast<const void*>(placebo_vulkan));
    AVVulkanDeviceContext *vkctx = reinterpret_cast<AVVulkanDeviceContext*>(hwctx->hwctx);
    vkctx->get_proc_addr = placebo_vulkan->get_proc_addr;
    vkctx->inst = placebo_vulkan->instance;
    vkctx->phys_dev = placebo_vulkan->phys_device;
    vkctx->act_dev = placebo_vulkan->device;
    vkctx->device_features = *placebo_vulkan->features;
    vkctx->enabled_inst_extensions = placebo_vk_inst->extensions;
    vkctx->nb_enabled_inst_extensions = placebo_vk_inst->num_extensions;
    vkctx->enabled_dev_extensions = placebo_vulkan->extensions;
    vkctx->nb_enabled_dev_extensions = placebo_vulkan->num_extensions;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100)
    vkctx->nb_qf = 0;
    if (placebo_vulkan->queue_graphics.index >= 0 && placebo_vulkan->queue_graphics.count > 0) {
        AVVulkanDeviceQueueFamily &qf = vkctx->qf[vkctx->nb_qf++];
        qf.idx = placebo_vulkan->queue_graphics.index;
        qf.num = placebo_vulkan->queue_graphics.count;
        qf.flags = VK_QUEUE_GRAPHICS_BIT;
        qf.video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(0);
    }
    if (placebo_vulkan->queue_transfer.index >= 0 && placebo_vulkan->queue_transfer.count > 0) {
        AVVulkanDeviceQueueFamily &qf = vkctx->qf[vkctx->nb_qf++];
        qf.idx = placebo_vulkan->queue_transfer.index;
        qf.num = placebo_vulkan->queue_transfer.count;
        qf.flags = VK_QUEUE_TRANSFER_BIT;
        qf.video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(0);
    }
    if (placebo_vulkan->queue_compute.index >= 0 && placebo_vulkan->queue_compute.count > 0) {
        AVVulkanDeviceQueueFamily &qf = vkctx->qf[vkctx->nb_qf++];
        qf.idx = placebo_vulkan->queue_compute.index;
        qf.num = placebo_vulkan->queue_compute.count;
        qf.flags = VK_QUEUE_COMPUTE_BIT;
        qf.video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(0);
    }
    if (vk_decode_queue_index >= 0) {
        AVVulkanDeviceQueueFamily &qf = vkctx->qf[vkctx->nb_qf++];
        qf.idx = vk_decode_queue_index;
        qf.num = 1;
        qf.flags = VK_QUEUE_VIDEO_DECODE_BIT_KHR;
        qf.video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(
            VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR | VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR);
    }
#else
    vkctx->queue_family_index = placebo_vulkan->queue_graphics.index;
    vkctx->nb_graphics_queues = placebo_vulkan->queue_graphics.count;
    vkctx->queue_family_tx_index = placebo_vulkan->queue_transfer.index;
    vkctx->nb_tx_queues = placebo_vulkan->queue_transfer.count;
    vkctx->queue_family_comp_index = placebo_vulkan->queue_compute.index;
    vkctx->nb_comp_queues = placebo_vulkan->queue_compute.count;
    vkctx->queue_family_decode_index = vk_decode_queue_index;
    vkctx->nb_decode_queues = 1;
#endif
    vkctx->lock_queue = [](struct AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index) {
        auto vk = reinterpret_cast<pl_vulkan>(dev_ctx->user_opaque);
        vk->lock_queue(vk, queue_family, index);
    };
    vkctx->unlock_queue = [](struct AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index) {
        auto vk = reinterpret_cast<pl_vulkan>(dev_ctx->user_opaque);
        vk->unlock_queue(vk, queue_family, index);
    };
    if (av_hwdevice_ctx_init(vulkan_hw_dev_ctx) < 0) {
        qCWarning(chiakiGui) << "Failed to create Vulkan decode context";
        av_buffer_unref(&vulkan_hw_dev_ctx);
    }

    return vulkan_hw_dev_ctx;
}

void QmlMainWindow::init(Settings *settings, bool exit_app_on_stream_exit)
{
    setSurfaceType(QWindow::VulkanSurface);

    const char *vk_exts[] = {
        nullptr,
        VK_KHR_SURFACE_EXTENSION_NAME,
    };

#if defined(Q_OS_LINUX)
    if (QGuiApplication::platformName().startsWith("wayland"))
        vk_exts[0] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
    else if (QGuiApplication::platformName().startsWith("xcb"))
        vk_exts[0] = VK_KHR_XCB_SURFACE_EXTENSION_NAME;
    else
        Q_UNREACHABLE();
#elif defined(Q_OS_MACOS)
    vk_exts[0] = VK_EXT_METAL_SURFACE_EXTENSION_NAME;
#elif defined(Q_OS_WIN32)
    vk_exts[0] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#endif

    const char *opt_extensions[] = {
        VK_EXT_HDR_METADATA_EXTENSION_NAME,
    };

    struct pl_log_params log_params = {
        .log_cb = placebo_log_cb,
        .log_level = PL_LOG_DEBUG,
    };
    placebo_log = pl_log_create(PL_API_VER, &log_params);

    struct pl_vk_inst_params vk_inst_params = {
        .extensions = vk_exts,
        .num_extensions = 2,
        .opt_extensions = opt_extensions,
        .num_opt_extensions = 1,
    };
    placebo_vk_inst = pl_vk_inst_create(placebo_log, &vk_inst_params);

#define GET_PROC(name_) { \
    vk_funcs.name_ = reinterpret_cast<decltype(vk_funcs.name_)>( \
        placebo_vk_inst->get_proc_addr(placebo_vk_inst->instance, #name_)); \
    if (!vk_funcs.name_) { \
        qCCritical(chiakiGui) << "Failed to resolve" << #name_; \
        return; \
    } }
    GET_PROC(vkGetDeviceProcAddr)
#if defined(Q_OS_LINUX)
    if (QGuiApplication::platformName().startsWith("wayland"))
        GET_PROC(vkCreateWaylandSurfaceKHR)
    else if (QGuiApplication::platformName().startsWith("xcb"))
        GET_PROC(vkCreateXcbSurfaceKHR)
#elif defined(Q_OS_MACOS)
    GET_PROC(vkCreateMetalSurfaceEXT)
#elif defined(Q_OS_WIN32)
    GET_PROC(vkCreateWin32SurfaceKHR)
#endif
    GET_PROC(vkDestroySurfaceKHR)
    GET_PROC(vkGetPhysicalDeviceQueueFamilyProperties)
    GET_PROC(vkGetPhysicalDeviceProperties)
    GET_PROC(vkGetPhysicalDeviceSurfacePresentModesKHR)
#undef GET_PROC

    const char *opt_dev_extensions[] = {
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
    };

    struct pl_vulkan_params vulkan_params = {
        .instance = placebo_vk_inst->instance,
        .get_proc_addr = placebo_vk_inst->get_proc_addr,
        .allow_software = true,
        PL_VULKAN_DEFAULTS
        .extra_queues = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
        .opt_extensions = opt_dev_extensions,
        .num_opt_extensions = 4,
    };
    placebo_vulkan = pl_vulkan_create(placebo_log, &vulkan_params);

#define GET_PROC(name_) { \
    vk_funcs.name_ = reinterpret_cast<decltype(vk_funcs.name_)>( \
        vk_funcs.vkGetDeviceProcAddr(placebo_vulkan->device, #name_)); \
    if (!vk_funcs.name_) { \
        qCCritical(chiakiGui) << "Failed to resolve" << #name_; \
        return; \
    } }
    GET_PROC(vkWaitSemaphores)
#undef GET_PROC

    uint32_t queueFamilyCount = 0;
    vk_funcs.vkGetPhysicalDeviceQueueFamilyProperties(placebo_vulkan->phys_device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilyProperties(queueFamilyCount);
    vk_funcs.vkGetPhysicalDeviceQueueFamilyProperties(placebo_vulkan->phys_device, &queueFamilyCount, queueFamilyProperties.data());
    auto queue_it = std::find_if(queueFamilyProperties.begin(), queueFamilyProperties.end(), [](VkQueueFamilyProperties prop) {
        return prop.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR;
    });
    if (queue_it != queueFamilyProperties.end())
        vk_decode_queue_index = std::distance(queueFamilyProperties.begin(), queue_it);
    VkPhysicalDeviceProperties device_props;
    vk_funcs.vkGetPhysicalDeviceProperties(placebo_vulkan->phys_device, &device_props);
    if(device_props.vendorID == 0x1002)
    {
        amd_card = true;
        qCInfo(chiakiGui) << "Using amd graphics card";
    }

    struct pl_cache_params cache_params = {
        .log = placebo_log,
        .max_total_size = 10 << 20, // 10 MB
    };
    placebo_cache = pl_cache_create(&cache_params);
    pl_gpu_set_cache(placebo_vulkan->gpu, placebo_cache);
    FILE *file = fopen(qPrintable(shader_cache_path()), "rb");
    if (file) {
        pl_cache_load_file(placebo_cache, file);
        fclose(file);
    }

    placebo_renderer = pl_renderer_create(
        placebo_log,
        placebo_vulkan->gpu
    );

    struct pl_vulkan_sem_params sem_params = {
        .type = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    quick_sem = pl_vulkan_sem_create(placebo_vulkan->gpu, &sem_params);

    qt_vk_inst = new QVulkanInstance;
    qt_vk_inst->setVkInstance(placebo_vk_inst->instance);
    if (!qt_vk_inst->create())
        qFatal("Failed to create QVulkanInstance");

    quick_render = new RenderControl(this);

    QQuickWindow::setDefaultAlphaBuffer(true);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    quick_window = new QQuickWindow(quick_render);
    quick_window->setVulkanInstance(qt_vk_inst);
    quick_window->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceObjects(placebo_vulkan->phys_device, placebo_vulkan->device, placebo_vulkan->queue_graphics.index));
    quick_window->setColor(QColor(0, 0, 0, 0));
    connect(quick_window, &QQuickWindow::focusObjectChanged, this, &QmlMainWindow::focusObjectChanged);

    qml_engine = new QQmlEngine(this);
    qml_engine->addImageProvider(QStringLiteral("svg"), new QmlSvgProvider);
    if (!qml_engine->incubationController())
        qml_engine->setIncubationController(quick_window->incubationController());
    connect(qml_engine, &QQmlEngine::quit, this, &QWindow::close);

    backend = new QmlBackend(settings, this);
    connect(backend, &QmlBackend::sessionChanged, this, [this, exit_app_on_stream_exit](StreamSession *s) {
        session = s;
        grab_input = 0;
        if (has_video) {
            has_video = false;
            setCursor(Qt::ArrowCursor);
            emit hasVideoChanged();
        }
        if (!frame_gen_readout.isEmpty()) {
            frame_gen_readout.clear();
            emit frameGenReadoutChanged();
        }
        if(session && exit_app_on_stream_exit)
        {
            connect(session, &StreamSession::SessionQuit, qGuiApp, &QGuiApplication::quit);
        }
        if(session)
        {
            connect(session, &StreamSession::PsChordFired, this, [this]() {
                emit menuRequested();
            });
            connect(session, &StreamSession::FullscreenToggleRequested, this, [this]() {
                if (windowState() != Qt::WindowFullScreen)
                    fullscreenTime();
                else
                    normalTime();
            });
        }
        if(!session)
        {
            setStreamWindowAdjustable(false);
            if(qEnvironmentVariable("XDG_CURRENT_DESKTOP") != "gamescope")
            {
                if(!this->settings->GetGeometry().isEmpty())
                {
                    setGeometry(this->settings->GetGeometry());
                    normalTime();
                }
                else
                    showMaximized();
            }
        }
    });
    connect(backend, &QmlBackend::windowTypeUpdated, this, &QmlMainWindow::updateWindowType);

    render_thread = new QThread;
    render_thread->setObjectName("render");
    render_thread->start();

    quick_render->prepareThread(render_thread);
    quick_render->moveToThread(render_thread);

    connect(quick_render, &QQuickRenderControl::sceneChanged, this, [this]() {
        quick_need_sync = true;
        scheduleUpdate();
    });
    connect(quick_render, &QQuickRenderControl::renderRequested, this, [this]() {
        quick_need_render = true;
        scheduleUpdate();
    });

    update_timer = new QTimer(this);
    update_timer->setSingleShot(true);
    connect(update_timer, &QTimer::timeout, this, &QmlMainWindow::update);

    QMetaObject::invokeMethod(quick_render, &QQuickRenderControl::initialize);

    QTimer *dropped_frames_timer = new QTimer(this);
    dropped_frames_timer->setInterval(1000);
    dropped_frames_timer->start();
    connect(dropped_frames_timer, &QTimer::timeout, this, [this]() {
        if (dropped_frames != dropped_frames_current) {
            dropped_frames = dropped_frames_current;
            emit droppedFramesChanged();
        }
        dropped_frames_current = 0;
    });

    this->renderparams_opts = pl_options_alloc(this->placebo_log);
    pl_options_reset(this->renderparams_opts, &pl_render_high_quality_params);
    this->renderparams_changed = true;


    switch (settings->GetPlaceboPreset()) {
    case PlaceboPreset::Fast:
        setVideoPreset(VideoPreset::Fast);
        break;
    case PlaceboPreset::Default:
        setVideoPreset(VideoPreset::Default);
        break;
    case PlaceboPreset::Custom:
        setVideoPreset(VideoPreset::Custom);
        break;
    case PlaceboPreset::HighQuality:
    default:
        setVideoPreset(VideoPreset::HighQuality);
        break;
    }
    setZoomFactor(settings->GetZoomFactor());

    // Initialize screenshot frame to nullptr
    screenshot_frame = nullptr;
    
    // Initialize Yandex OCR for translation overlay
    yandex_ocr = new YandexOCR(this);
    text_overlay = new TextOverlay(this);
    
    // Load credentials from settings
    QString iamToken = settings->GetYandexIamToken();
    QString folderId = settings->GetYandexFolderId();
    
    if (!iamToken.isEmpty() && !folderId.isEmpty()) {
        yandex_ocr->setIamToken(iamToken);
        yandex_ocr->setFolderId(folderId);
        qCInfo(chiakiGui) << "Translation overlay initialized with credentials from settings";
    } else {
        qCInfo(chiakiGui) << "Translation overlay initialized (no credentials configured)";
    }
    
    connect(yandex_ocr, &YandexOCR::recognitionFinished, this, &QmlMainWindow::onRecognitionFinished);
    connect(yandex_ocr, &YandexOCR::errorOccurred, this, &QmlMainWindow::onRecognitionError);
}

void QmlMainWindow::normalTime()
{
    if(windowState() == Qt::WindowFullScreen)
    {
        if(was_maximized)
        {
            showMaximized();
            was_maximized = false;
        }
        else
            showNormal();
        setMinimumSize(QSize(0, 0));
    }
    QTimer::singleShot(1000, this, [this]{
        if(session)
            setStreamWindowAdjustable(true);
        else
        {
            setWindowAdjustable(true);
        }
    });
}

void QmlMainWindow::fullscreenTime()
{
    if(windowState() == Qt::WindowFullScreen)
        return;
    if(session)
        setStreamWindowAdjustable(false);
    else
        setWindowAdjustable(false);
    setMinimumSize(size());
    if(windowState() == Qt::WindowMaximized)
        was_maximized = true;
    else
        was_maximized = false;
    showFullScreen();
}
void QmlMainWindow::update()
{
    Q_ASSERT(QThread::currentThread() == QGuiApplication::instance()->thread());

    if (render_scheduled)
        return;

    if (quick_need_sync) {
        quick_need_sync = false;
        quick_render->polishItems();
        QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::sync, this), Qt::BlockingQueuedConnection);
    }

    update_timer->stop();
    render_scheduled = true;
    QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::render, this));
}

void QmlMainWindow::scheduleUpdate()
{
    Q_ASSERT(QThread::currentThread() == QGuiApplication::instance()->thread());

    if (!update_timer->isActive())
        update_timer->start(has_video ? 50 : 10);
}

void QmlMainWindow::updatePlacebo()
{
    renderparams_changed = true;
}

void QmlMainWindow::createSwapchain()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    if (placebo_swapchain)
        return;

    VkResult err = VK_ERROR_UNKNOWN;
#if defined(Q_OS_LINUX)
    if (QGuiApplication::platformName().startsWith("wayland")) {
        VkWaylandSurfaceCreateInfoKHR surfaceInfo = {};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.display = reinterpret_cast<struct wl_display*>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow("display", this));
        surfaceInfo.surface = reinterpret_cast<struct wl_surface*>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface", this));
        err = vk_funcs.vkCreateWaylandSurfaceKHR(placebo_vk_inst->instance, &surfaceInfo, nullptr, &surface);
    } else if (QGuiApplication::platformName().startsWith("xcb")) {
        VkXcbSurfaceCreateInfoKHR surfaceInfo = {};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.connection = reinterpret_cast<xcb_connection_t*>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow("connection", this));
        surfaceInfo.window = static_cast<xcb_window_t>(winId());
        err = vk_funcs.vkCreateXcbSurfaceKHR(placebo_vk_inst->instance, &surfaceInfo, nullptr, &surface);
    } else {
        Q_UNREACHABLE();
    }
#elif defined(Q_OS_MACOS)
    VkMetalSurfaceCreateInfoEXT surfaceInfo = {};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    surfaceInfo.pLayer = static_cast<const CAMetalLayer*>(reinterpret_cast<void*(*)(id, SEL)>(objc_msgSend)(reinterpret_cast<id>(winId()), sel_registerName("layer")));
    err = vk_funcs.vkCreateMetalSurfaceEXT(placebo_vk_inst->instance, &surfaceInfo, nullptr, &surface);
#elif defined(Q_OS_WIN32)
    VkWin32SurfaceCreateInfoKHR surfaceInfo = {};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = GetModuleHandle(nullptr);
    surfaceInfo.hwnd = reinterpret_cast<HWND>(winId());
    err = vk_funcs.vkCreateWin32SurfaceKHR(placebo_vk_inst->instance, &surfaceInfo, nullptr, &surface);
#endif

    if (err != VK_SUCCESS)
        qFatal("Failed to create VkSurfaceKHR");

    present_mode = desiredPresentMode();
    struct pl_vulkan_swapchain_params swapchain_params = {
        .surface = surface,
        .present_mode = present_mode,
        .swapchain_depth = 1,
    };
    placebo_swapchain = pl_vulkan_create_swapchain(placebo_vulkan, &swapchain_params);
}

void QmlMainWindow::destroySwapchain()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    if (!placebo_swapchain)
        return;

    pl_swapchain_destroy(&placebo_swapchain);
    vk_funcs.vkDestroySurfaceKHR(placebo_vk_inst->instance, surface, nullptr);
    swapchain_size = QSize();
}

void QmlMainWindow::resizeSwapchain()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    if (!placebo_swapchain)
        createSwapchain();

    const QSize window_size(width() * devicePixelRatio(), height() * devicePixelRatio());
    if (window_size == swapchain_size)
        return;

    swapchain_size = window_size;
    pl_swapchain_resize(placebo_swapchain, &swapchain_size.rwidth(), &swapchain_size.rheight());

    struct pl_tex_params tex_params = {
        .w = swapchain_size.width(),
        .h = swapchain_size.height(),
        .format = pl_find_fmt(placebo_vulkan->gpu, PL_FMT_UNORM, 4, 8, 8, PL_FMT_CAP_RENDERABLE),
        .sampleable = true,
        .renderable = true,
    };
    if (!pl_tex_recreate(placebo_vulkan->gpu, &quick_tex, &tex_params))
        qCCritical(chiakiGui) << "Failed to create placebo texture";

    VkFormat vk_format;
    VkImage vk_image = pl_vulkan_unwrap(placebo_vulkan->gpu, quick_tex, &vk_format, nullptr);
    quick_window->setRenderTarget(QQuickRenderTarget::fromVulkanImage(vk_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, vk_format, swapchain_size));
}

void QmlMainWindow::updateSwapchain()
{
    Q_ASSERT(QThread::currentThread() == QGuiApplication::instance()->thread());

    quick_item->setSize(size());
    quick_window->resize(size());

    QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::resizeSwapchain, this), Qt::BlockingQueuedConnection);
    quick_render->polishItems();
    QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::sync, this), Qt::BlockingQueuedConnection);
    update();
}

void QmlMainWindow::sync()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    if (!quick_tex)
        return;

    beginFrame();
    quick_need_render = quick_render->sync();
}

void QmlMainWindow::beginFrame()
{
    if (quick_frame)
        return;

    struct pl_vulkan_hold_params hold_params = {
        .tex = quick_tex,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .qf = VK_QUEUE_FAMILY_IGNORED,
        .semaphore = {
            .sem = quick_sem,
            .value = ++quick_sem_value,
        }
    };
    pl_vulkan_hold_ex(placebo_vulkan->gpu, &hold_params);

    VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &quick_sem,
        .pValues = &quick_sem_value,
    };
    vk_funcs.vkWaitSemaphores(placebo_vulkan->device, &wait_info, UINT64_MAX);

    quick_frame = true;
    quick_render->beginFrame();
}

void QmlMainWindow::endFrame()
{
    if (!quick_frame)
        return;

    quick_frame = false;
    quick_render->endFrame();

    struct pl_vulkan_release_params release_params = {
        .tex = quick_tex,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .qf = VK_QUEUE_FAMILY_IGNORED,
    };
    pl_vulkan_release_ex(placebo_vulkan->gpu, &release_params);
}

namespace {

struct FrameFmt {
    int luma_bpp;
    int chroma_div;
    int chroma_bpp;
    bool interleaved_uv;
    bool planar_uv;
};

bool frame_fmt(int fmt, FrameFmt *out)
{
    switch (fmt) {
    case AV_PIX_FMT_NV12:
        *out = { 1, 2, 2, true, false };
        return true;
    case AV_PIX_FMT_YUV420P:
        *out = { 1, 2, 1, false, true };
        return true;
    case AV_PIX_FMT_P010LE:
        *out = { 2, 2, 4, true, false };
        return true;
    default:
        return false;
    }
}

int sample_luma(const AVFrame *f, int x, int y)
{
    if (!f || !f->data[0] || f->width <= 0 || f->height <= 0)
        return -1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= f->width) x = f->width - 1;
    if (y >= f->height) y = f->height - 1;
    const uint8_t *row = f->data[0] + y * f->linesize[0];
    switch (f->format) {
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_YUV420P:
        return row[x];
    case AV_PIX_FMT_P010LE:
        return ((const uint16_t *)row)[x] >> 8;
    default:
        return -1;
    }
}

bool build_luma8(const AVFrame *f, int dw, int dh, std::vector<uint8_t> *out)
{
    if (dw < 8 || dh < 8)
        return false;
    out->resize((size_t)dw * dh);
    for (int y = 0; y < dh; y++) {
        const int sy = y * f->height / dh;
        for (int x = 0; x < dw; x++) {
            const int v = sample_luma(f, x * f->width / dw, sy);
            if (v < 0)
                return false;
            (*out)[(size_t)y * dw + x] = (uint8_t)v;
        }
    }
    return true;
}

void blit_block(uint8_t *dst, int dst_stride, int dx, int dy,
                const uint8_t *src, int src_stride, int sx, int sy,
                int bw, int bh, int bpp, int src_w, int src_h)
{
    if (sx < 0) { bw += sx; dx -= sx; sx = 0; }
    if (sy < 0) { bh += sy; dy -= sy; sy = 0; }
    if (sx + bw > src_w) bw = src_w - sx;
    if (sy + bh > src_h) bh = src_h - sy;
    if (bw <= 0 || bh <= 0 || dx < 0 || dy < 0)
        return;
    for (int y = 0; y < bh; y++) {
        memcpy(dst + (dy + y) * dst_stride + dx * bpp,
               src + (sy + y) * src_stride + sx * bpp,
               (size_t)bw * bpp);
    }
}

void paint_moved_blocks(AVFrame *dst, const AVFrame *src, int plane, int bpp,
                        int plane_w, int plane_h, int bxn, int byn,
                        const int8_t *mvx, const int8_t *mvy, const uint8_t *good,
                        float t, int dw, int dh)
{
    if (!dst->data[plane] || !src->data[plane] || plane_w <= 0 || plane_h <= 0)
        return;
    for (int by = 0; by < byn; by++) {
        for (int bx = 0; bx < bxn; bx++) {
            const int i = by * bxn + bx;
            if (!good[i])
                continue;
            const int x = bx * plane_w / bxn;
            const int y = by * plane_h / byn;
            const int w = (bx + 1) * plane_w / bxn - x;
            const int h = (by + 1) * plane_h / byn - y;
            const int dx = (int)std::lround(mvx[i] * (plane_w / (double)dw) * t);
            const int dy = (int)std::lround(mvy[i] * (plane_h / (double)dh) * t);
            blit_block(dst->data[plane], dst->linesize[plane], x, y,
                       src->data[plane], src->linesize[plane], x + dx, y + dy,
                       w, h, bpp, plane_w, plane_h);
        }
    }
}

// One whole picture between two different decoded frames. Blocks that have a
// confident motion match are taken from the previous frame, shifted. The rest
// stay on the nearer original, so the two pictures are never drawn on top of
// each other.
AVFrame *synthesize_frame(const AVFrame *prev, const AVFrame *next, float t)
{
    FrameFmt fmt;
    if (!frame_fmt(prev->format, &fmt) || prev->format != next->format
            || prev->width != next->width || prev->height != next->height
            || prev->width < 64 || prev->height < 64)
        return nullptr;

    const int dw = std::max(32, prev->width / (prev->width >= 2560 ? 8 : 4));
    const int dh = std::max(32, prev->height / (prev->height >= 2560 ? 8 : 4));
    std::vector<uint8_t> prev_luma, next_luma;
    if (!build_luma8(prev, dw, dh, &prev_luma) || !build_luma8(next, dw, dh, &next_luma))
        return nullptr;

    const int block = 4;
    const int bxn = dw / block;
    const int byn = dh / block;
    const int radius = 4;
    std::vector<int8_t> mvx((size_t)bxn * byn), mvy((size_t)bxn * byn);
    std::vector<uint8_t> good((size_t)bxn * byn);
    for (int by = 0; by < byn; by++) {
        for (int bx = 0; bx < bxn; bx++) {
            const int x0 = bx * block;
            const int y0 = by * block;
            int best = 0;
            int bdx = 0, bdy = 0;
            bool have = false;
            auto sad_at = [&](int dx, int dy) {
                int sad = 0;
                for (int y = 0; y < block; y++) {
                    const uint8_t *a = prev_luma.data() + (size_t)(y0 + dy + y) * dw + (x0 + dx);
                    const uint8_t *b = next_luma.data() + (size_t)(y0 + y) * dw + x0;
                    for (int x = 0; x < block; x++)
                        sad += std::abs(int(a[x]) - int(b[x]));
                }
                return sad;
            };
            if (x0 + block <= dw && y0 + block <= dh) {
                best = sad_at(0, 0);
                have = true;
            }
            if (!have || best > block * block * 3) {
                for (int dy = -radius; dy <= radius; dy++) {
                    if (y0 + dy < 0 || y0 + dy + block > dh)
                        continue;
                    for (int dx = -radius; dx <= radius; dx++) {
                        if (x0 + dx < 0 || x0 + dx + block > dw)
                            continue;
                        const int sad = sad_at(dx, dy);
                        if (!have || sad < best) {
                            best = sad;
                            bdx = dx;
                            bdy = dy;
                            have = true;
                        }
                    }
                }
            }
            const size_t i = (size_t)by * bxn + bx;
            mvx[i] = (int8_t)bdx;
            mvy[i] = (int8_t)bdy;
            good[i] = have && best <= block * block * 10;
        }
    }

    AVFrame *out = av_frame_alloc();
    if (!out)
        return nullptr;
    out->format = next->format;
    out->width = next->width;
    out->height = next->height;
    const AVFrame *base = t < 0.5f ? prev : next;
    if (av_frame_get_buffer(out, 32) < 0 || av_frame_copy(out, base) < 0) {
        av_frame_free(&out);
        return nullptr;
    }
    av_frame_copy_props(out, next);

    paint_moved_blocks(out, prev, 0, fmt.luma_bpp, out->width, out->height, bxn, byn,
                       mvx.data(), mvy.data(), good.data(), t, dw, dh);
    const int cw = out->width / fmt.chroma_div;
    const int ch = out->height / fmt.chroma_div;
    if (fmt.interleaved_uv) {
        paint_moved_blocks(out, prev, 1, fmt.chroma_bpp, cw, ch, bxn, byn,
                           mvx.data(), mvy.data(), good.data(), t, dw, dh);
    } else if (fmt.planar_uv) {
        paint_moved_blocks(out, prev, 1, fmt.chroma_bpp, cw, ch, bxn, byn,
                           mvx.data(), mvy.data(), good.data(), t, dw, dh);
        paint_moved_blocks(out, prev, 2, fmt.chroma_bpp, cw, ch, bxn, byn,
                           mvx.data(), mvy.data(), good.data(), t, dw, dh);
    }
    return out;
}

} // namespace

VkPresentModeKHR QmlMainWindow::desiredPresentMode() const
{
    // Frame generation has to scan out every inserted frame, so it stays on FIFO.
    // Reflex alone switches to mailbox: the latest frame is shown without waiting
    // out a full vsync queue. Depth stays 1 either way.
    if (!settings || !settings->GetNvidiaReflex() || settings->GetDlssFrameGen() >= 2)
        return VK_PRESENT_MODE_FIFO_KHR;
    if (!placebo_vulkan || surface == VK_NULL_HANDLE || !vk_funcs.vkGetPhysicalDeviceSurfacePresentModesKHR)
        return VK_PRESENT_MODE_FIFO_KHR;

    uint32_t count = 0;
    if (vk_funcs.vkGetPhysicalDeviceSurfacePresentModesKHR(placebo_vulkan->phys_device, surface, &count, nullptr) != VK_SUCCESS || count == 0)
        return VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkPresentModeKHR> modes(count);
    if (vk_funcs.vkGetPhysicalDeviceSurfacePresentModesKHR(placebo_vulkan->phys_device, surface, &count, modes.data()) != VK_SUCCESS)
        return VK_PRESENT_MODE_FIFO_KHR;
    if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end())
        return VK_PRESENT_MODE_MAILBOX_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}

void QmlMainWindow::notePresentedFrame(bool generated)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (frame_stat_window_start == 0)
        frame_stat_window_start = now;
    if (generated)
        frame_stat_generated++;
    else
        frame_stat_native++;
    if (now - frame_stat_window_start < 500)
        return;

    const double seconds = (now - frame_stat_window_start) / 1000.0;
    const int native = seconds > 0.0 ? qRound(frame_stat_native / seconds) : 0;
    const int generated_fps = seconds > 0.0 ? qRound(frame_stat_generated / seconds) : 0;
    frame_stat_native = 0;
    frame_stat_generated = 0;
    frame_stat_window_start = now;

    QString text;
    if (settings && settings->GetShowFrameGenFps()) {
        if (settings->GetDlssFrameGen() >= 2)
            text = QStringLiteral("%1(+%2DLSS)").arg(native).arg(generated_fps);
        else
            text = QString::number(native);
    }
    QMetaObject::invokeMethod(this, [this, text]() {
        if (frame_gen_readout == text)
            return;
        frame_gen_readout = text;
        emit frameGenReadoutChanged();
    }, Qt::QueuedConnection);
}

void QmlMainWindow::clearGenerated()
{
    for (int i = 0; i < 3; i++) {
        if (placebo_vulkan && gen_pl[i].num_planes)
            pl_unmap_avframe(placebo_vulkan->gpu, &gen_pl[i]);
        gen_pl[i] = {};
        av_frame_free(&gen_av[i]);
    }
    gen_count = 0;
}

bool QmlMainWindow::isRepeatFrame(const AVFrame *frame)
{
    std::vector<uint8_t> now;
    if (!build_luma8(frame, 48, 27, &now))
        return false;
    bool repeat = false;
    if (frame_thumb.size() == now.size()) {
        int sum = 0;
        int big = 0;
        for (size_t i = 0; i < now.size(); i++) {
            const int d = std::abs(int(now[i]) - int(frame_thumb[i]));
            sum += d;
            if (d > 12)
                big++;
        }
        // A 30 fps game inside a 60 fps stream repeats the same picture.
        // Those copies are not new frames and must not be counted or displayed as generated.
        repeat = sum <= 4 * (int)now.size() && big * 50 < (int)now.size();
    }
    if (!repeat)
        frame_thumb.swap(now);
    return repeat;
}

void QmlMainWindow::buildGenerated(const AVFrame *prev, const AVFrame *next, int slots)
{
    clearGenerated();
    if (!prev || !next || slots < 1)
        return;
    if (slots > 3)
        slots = 3;
    for (int i = 0; i < slots; i++) {
        const float t = float(i + 1) / float(slots + 1);
        gen_av[i] = synthesize_frame(prev, next, t);
        if (!gen_av[i])
            break;
        struct pl_avframe_params avparams = {
            .frame = gen_av[i],
            .tex = &placebo_tex[1 + i],
        };
        if (!pl_map_avframe_ex(placebo_vulkan->gpu, &gen_pl[i], &avparams)) {
            av_frame_free(&gen_av[i]);
            gen_pl[i] = {};
            break;
        }
        gen_count++;
    }
}

void QmlMainWindow::render()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    render_scheduled = false;

    if (placebo_swapchain && desiredPresentMode() != present_mode) {
        destroySwapchain();
        resizeSwapchain();
    }

    if (!placebo_swapchain)
        return;

    AVFrame *frame = nullptr;
    pl_tex *tex = &placebo_tex[0];

    frame_mutex.lock();
    if (av_frame || (!has_video && !keep_video))
        std::swap(frame, av_frame);
    frame_mutex.unlock();

    const int requested = settings ? settings->GetDlssFrameGen() : 1;
    bool unique_frame = false;
    if (frame && requested >= 2 && isRepeatFrame(frame)) {
        av_frame_free(&frame);
        if (current_frame.num_planes)
            return;
    }
    if (!frame && !has_video && !keep_video) {
        pl_unmap_avframe(placebo_vulkan->gpu, &previous_frame);
        pl_unmap_avframe(placebo_vulkan->gpu, &current_frame);
        clearGenerated();
        av_frame_free(&held_sw);
        frame_thumb.clear();
    }
    if (frame) {
        unique_frame = true;
        if (session)
            session->ApplyDisplayCrop(frame);
        if (current_frame.num_planes && current_frame.planes[0].texture == *tex)
            tex = &placebo_tex[4];

        int slots = 0;
        if (requested >= 2 && held_sw) {
            const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
            double base = decoded_fps_ema;
            if (last_decoded_ms > 0) {
                const double dt = double(now_ms - last_decoded_ms);
                if (dt >= 5.0 && dt <= 100.0)
                    base = 1000.0 / dt;
            }
            if (base < 1.0)
                base = 60.0;
            float hz = display_hz.load(std::memory_order_relaxed);
            if (hz < 30.f)
                hz = 60.f;
            int fit = (int)std::floor(hz / base + 1e-3);
            if (fit < 1)
                fit = 1;
            slots = std::min(requested, fit) - 1;
        }
        if (slots > 0)
            buildGenerated(held_sw, frame, slots);
        else
            clearGenerated();

        struct pl_avframe_params avparams = {
            .frame = frame,
            .tex = tex,
        };
        pl_unmap_avframe(placebo_vulkan->gpu, &previous_frame);
        previous_frame = current_frame;
        current_frame = {};
        if (!pl_map_avframe_ex(placebo_vulkan->gpu, &current_frame, &avparams)) {
            qCWarning(chiakiGui) << "Failed to map AVFrame to Placebo frame!";
            current_frame = previous_frame;
            previous_frame = {};
            if (backend && backend->zeroCopy()) {
                qCInfo(chiakiGui) << "Mapping frame failed, trying without zero copy!";
                backend->disableZeroCopy();
            }
        }
        av_frame_free(&held_sw);
        held_sw = av_frame_clone(frame);
        av_frame_free(&frame);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (last_decoded_ms > 0) {
            const double dt = double(now - last_decoded_ms);
            if (dt >= 5.0 && dt <= 100.0) {
                const double instant = 1000.0 / dt;
                decoded_fps_ema = decoded_fps_ema <= 0.0 ? instant : decoded_fps_ema * 0.85 + instant * 0.15;
            }
        }
        last_decoded_ms = now;
    }

    const int passes = (unique_frame && gen_count > 0) ? gen_count + 1 : 1;

    for (int pass = 0; pass < passes; pass++) {
    struct pl_swapchain_frame sw_frame = {};
    if (!pl_swapchain_start_frame(placebo_swapchain, &sw_frame)) {
        qCWarning(chiakiGui) << "Failed to start Placebo frame!";
        return;
    }

    struct pl_color_space target_csp = sw_frame.color_space;
    if(!pl_color_transfer_is_hdr(target_csp.transfer))
    {
        target_csp.hdr.max_luma = 0;
        target_csp.hdr.min_luma = 0;
    }
    float target_peak = settings->GetDisplayTargetPeak() && session ? settings->GetDisplayTargetPeak() : 0;
    int target_contrast = settings->GetDisplayTargetContrast() && session ? settings->GetDisplayTargetContrast(): 0;
    int target_prim = settings->GetDisplayTargetPrim() && session ? settings->GetDisplayTargetPrim(): 0;
    int target_trc = settings->GetDisplayTargetTrc() && session ? settings->GetDisplayTargetTrc(): 0;
    struct pl_color_space hint = current_frame.color;

    if(target_trc)
        hint.transfer = static_cast<pl_color_transfer>(target_trc);

    if(target_prim)
    {
        hint.primaries = static_cast<pl_color_primaries>(target_prim);
        hint.hdr.prim = *pl_raw_primaries_get(hint.primaries);
    }

    if(target_peak)
        hint.hdr.max_luma = target_peak;

    switch(target_contrast)
    {
        case -1:
            hint.hdr.min_luma = 1e-7;
            break;
        case 0:
            hint.hdr.min_luma = target_csp.hdr.min_luma;
            break;
        default:
            // Infer max_luma for current pl_color_space
            struct pl_nominal_luma_params hint_params = {};
            hint_params.color = &hint;
            hint_params.metadata = PL_HDR_METADATA_HDR10;
            hint_params.scaling = PL_HDR_NITS;
            hint_params.out_max = &hint.hdr.max_luma;
            pl_color_space_nominal_luma_ex(&hint_params);
            hint.hdr.min_luma = hint.hdr.max_luma / (float)target_contrast;
            break;
    }
    pl_swapchain_colorspace_hint(placebo_swapchain, &hint);

    struct pl_frame target_frame = {};
    pl_frame_from_swapchain(&target_frame, &sw_frame);
    if(target_prim)
    {
        target_frame.color.primaries = static_cast<pl_color_primaries>(target_prim);
        target_frame.color.hdr.prim = *pl_raw_primaries_get(target_frame.color.primaries);
    }

    if(target_trc)
        target_frame.color.transfer = static_cast<pl_color_transfer>(target_trc);

    if(target_peak && !target_frame.color.hdr.max_luma)
    {
        target_frame.color.hdr.max_luma = target_peak;
    }
    if(!target_frame.color.hdr.min_luma)
    {
        switch(target_contrast)
        {
            case -1:
                target_frame.color.hdr.min_luma = 1e-7;
                break;
            case 0:
                target_frame.color.hdr.min_luma = target_csp.hdr.min_luma;
                break;
            default:
                // Infer max_luma for current pl_color_space
                struct pl_nominal_luma_params target_params = {};
                target_params.color = &target_frame.color;
                target_params.metadata = PL_HDR_METADATA_HDR10;
                target_params.scaling = PL_HDR_NITS;
                target_params.out_max = &target_frame.color.hdr.max_luma;
                pl_color_space_nominal_luma_ex(&target_params);
                target_frame.color.hdr.min_luma = target_frame.color.hdr.max_luma / (float)target_contrast;
                break;
        }
    }
    if (pass == 0) {
        if (quick_need_render) {
            quick_need_render = false;
            beginFrame();
            quick_render->render();
        }
        endFrame();
    }

    struct pl_overlay_part overlay_part = {
        .src = {0, 0, static_cast<float>(swapchain_size.width()), static_cast<float>(swapchain_size.height())},
        .dst = {0, 0, static_cast<float>(swapchain_size.width()), static_cast<float>(swapchain_size.height())},
    };
    struct pl_overlay overlay = {
        .tex = quick_tex,
        .repr = pl_color_repr_rgb,
        .color = pl_color_space_srgb,
        .parts = &overlay_part,
        .num_parts = 1,
    };
    target_frame.overlays = &overlay;
    target_frame.num_overlays = 1;

    const struct pl_render_params *render_params;
    switch (video_preset) {
    case VideoPreset::Fast:
        render_params = &pl_render_fast_params;
        break;
    case VideoPreset::Default:
        render_params = &pl_render_default_params;
        break;
    case VideoPreset::HighQuality:
        render_params = &pl_render_high_quality_params;
        break;
    case VideoPreset::Custom:
        if (renderparams_changed) {
            renderparams_changed = false;
            QMap<QString, QString> paramsData = settings->GetPlaceboValues();
            QMapIterator<QString, QString> i(paramsData);
            bool invalid_render_params = false;
            while (i.hasNext()) {
                i.next();
                if(!pl_options_set_str(this->renderparams_opts, i.key().toUtf8().constData(), i.value().toUtf8().constData()))
                {
                    invalid_render_params = true;
                    qCCritical(chiakiGui) << "Failed to load custom render param: " << i.key() << " with value: " << i.value();
                }
            }
            if (invalid_render_params)
                qCInfo(chiakiGui) << "Updated custom render parameters with one or more invalid parameters.";
            else {
                qCInfo(chiakiGui) << "Updated custom render parameters successfully.";
            }
        }
        render_params = &(this->renderparams_opts->params);
        break;
    }

    if (current_frame.num_planes) {
        pl_rect2df crop = current_frame.crop;
        switch (video_mode) {
        case VideoMode::Normal:
            pl_rect2df_aspect_copy(&target_frame.crop, &crop, 0.0);
            break;
        case VideoMode::Stretch:
            // Nothing to do, target.crop already covers the full image
            break;
        case VideoMode::Zoom:
            if(zoom_factor == -1)
                pl_rect2df_aspect_copy(&target_frame.crop, &crop, 1.0);
            else
            {
                const float z = powf(2.0f, zoom_factor);
                const float sx = z * fabsf(pl_rect_w(crop)) / pl_rect_w(target_frame.crop);
                const float sy = z * fabsf(pl_rect_h(crop)) / pl_rect_h(target_frame.crop);
                pl_rect2df_stretch(&target_frame.crop, sx, sy);
            }
            break;
        }
    }

    const bool generated_pass = pass < gen_count && unique_frame;
    const struct pl_frame *source = generated_pass
            ? &gen_pl[pass]
            : (current_frame.num_planes ? &current_frame : nullptr);
    if (!pl_render_image(placebo_renderer, source, &target_frame, render_params))
        qCWarning(chiakiGui) << "Failed to render Placebo frame!";

    const bool submitted = pl_swapchain_submit_frame(placebo_swapchain);
    if (!submitted)
        qCWarning(chiakiGui) << "Failed to submit Placebo frame!";

    pl_swapchain_swap_buffers(placebo_swapchain);
    if (submitted && unique_frame)
        notePresentedFrame(generated_pass);
    }
}

bool QmlMainWindow::handleShortcut(QKeyEvent *event)
{
    if (event->modifiers() == Qt::NoModifier) {
        switch (event->key()) {
        case Qt::Key_F11:
            if (windowState() != Qt::WindowFullScreen)
                fullscreenTime();
            else
                normalTime();
            return true;
        default:
            break;
        }
    }

    // Handle Alt modifier shortcuts
    if (event->modifiers() == Qt::AltModifier) {
        qCInfo(chiakiGui) << "Alt key pressed with key:" << Qt::Key(event->key());
        switch (event->key()) {
        case Qt::Key_T:
            qCInfo(chiakiGui) << "Alt+T pressed - triggering translation toggle";
            toggleTranslation();
            return true;
        case Qt::Key_Y:
            // Alt+Y для скрытия перевода
            qCInfo(chiakiGui) << "Alt+Y pressed - hiding translation overlay";
            if (text_overlay) {
                text_overlay->clear();
                scheduleUpdate();
                qCInfo(chiakiGui) << "Translation overlay cleared";
            }
            return true;
        default:
            break;
        }
    }

    if (!event->modifiers().testFlag(Qt::ControlModifier))
        return false;

    switch (event->key()) {
    case Qt::Key_S:
        if (has_video)
            setVideoMode(videoMode() == VideoMode::Stretch ? VideoMode::Normal : VideoMode::Stretch);
        return true;
    case Qt::Key_Z:
        if (has_video)
            setVideoMode(videoMode() == VideoMode::Zoom ? VideoMode::Normal : VideoMode::Zoom);
        return true;
    case Qt::Key_M:
        if (session)
            session->ToggleMute();
        return true;
    case Qt::Key_O:
        emit menuRequested();
        return true;
    case Qt::Key_Q:
#ifndef Q_OS_MACOS
        close();
#endif
        return true;
    default:
        return false;
    }
}

bool QmlMainWindow::amdCard() const
{
    return amd_card;
}

bool QmlMainWindow::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
        if (static_cast<QMouseEvent*>(event)->source() != Qt::MouseEventNotSynthesized)
            return true;
        if (session && !grab_input) {
            if (event->type() == QEvent::MouseMove)
                session->HandleMouseMoveEvent(static_cast<QMouseEvent*>(event), width(), height());
            else if (event->type() == QEvent::MouseButtonPress)
                session->HandleMousePressEvent(static_cast<QMouseEvent*>(event));
            else if (event->type() == QEvent::MouseButtonRelease)
                session->HandleMouseReleaseEvent(static_cast<QMouseEvent*>(event));
            return true;
        }
        QGuiApplication::sendEvent(quick_window, event);
        break;
    case QEvent::MouseButtonDblClick:
        if(!settings->GetFullscreenDoubleClickEnabled())
            break;
        if (session && !grab_input) {
            if (windowState() != Qt::WindowFullScreen)
                fullscreenTime();
            else
                normalTime();
        }
        break;
    case QEvent::KeyPress:
        if (handleShortcut(static_cast<QKeyEvent*>(event)))
            return true;
    case QEvent::KeyRelease:
        if (session && !grab_input) {
            QKeyEvent *e = static_cast<QKeyEvent*>(event);
            if (e->timestamp()) // Don't send controller emulated key events
                session->HandleKeyboardEvent(e);
            return true;
        }
        QGuiApplication::sendEvent(quick_window, event);
        break;
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
        if (session && !grab_input) {
            session->HandleTouchEvent(static_cast<QTouchEvent*>(event), width(), height());
            return true;
        }
        QGuiApplication::sendEvent(quick_window, event);
        break;
    case QEvent::Close:
        if (!backend->closeRequested()) {
            event->ignore();
            return true;
        }
        QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::destroySwapchain, this), Qt::BlockingQueuedConnection);
        break;
    default:
        break;
    }

    bool ret = QWindow::event(event);

    switch (event->type()) {
    case QEvent::Expose:
        if (isExposed())
            updateSwapchain();
        else
            QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::destroySwapchain, this), Qt::BlockingQueuedConnection);
        break;
    case QEvent::Move:
        if(!session && isWindowAdjustable())
            settings->SetGeometry(geometry());
        else if(session && settings->GetWindowType() == WindowType::AdjustableResolution && isStreamWindowAdjustable())
            settings->SetStreamGeometry(geometry());
        break;
    case QEvent::Resize:
        if(!session && isWindowAdjustable())
            settings->SetGeometry(geometry());
        else if(session && settings->GetWindowType() == WindowType::AdjustableResolution && isStreamWindowAdjustable())
            settings->SetStreamGeometry(geometry());
        if (isExposed())
            updateSwapchain();
        break;
    default:
        break;
    }

    return ret;
}

QObject *QmlMainWindow::focusObject() const
{
    return quick_window->focusObject();
}

// ========== Translation Overlay Implementation ==========

void QmlMainWindow::triggerTranslation()
{
    // Проверяем ограничение по времени (5 секунд между переводами)
    qint64 current_time = QDateTime::currentMSecsSinceEpoch();
    qint64 time_since_last = current_time - last_translation_time;
    const qint64 MIN_INTERVAL_MS = 5000;
    
    if (last_translation_time > 0 && time_since_last < MIN_INTERVAL_MS) {
        qint64 wait_seconds = (MIN_INTERVAL_MS - time_since_last + 999) / 1000;
        qCWarning(chiakiGui) << "Translation cooldown:" << wait_seconds << "sec";
        return;
    }
    
    if (translation_in_progress) {
        return;
    }

    if (!has_video) {
        qCWarning(chiakiGui) << "No video for translation";
        return;
    }
    
    last_translation_time = current_time;

    QString iamToken = settings->GetYandexIamToken();
    QString folderId = settings->GetYandexFolderId();
    
    if (iamToken.isEmpty() || folderId.isEmpty()) {
        qCWarning(chiakiGui) << "Translation: Not configured";
        return;
    }
    
    yandex_ocr->setIamToken(iamToken);
    yandex_ocr->setFolderId(folderId);

    translation_in_progress = true;

    QImage screenshot = captureCurrentFrame();
    
    if (screenshot.isNull()) {
        qCWarning(chiakiGui) << "Translation: Failed to capture frame";
        translation_in_progress = false;
        return;
    }
    
    yandex_ocr->recognizeText(screenshot);
}

void QmlMainWindow::clearTranslation()
{
    if (text_overlay) {
        text_overlay->clear();
        scheduleUpdate();
    }
}

void QmlMainWindow::toggleTranslation()
{
    if (translation_in_progress) {
        return;
    }
    
    if (text_overlay && text_overlay->isActive()) {
        text_overlay->clear();
        scheduleUpdate();
        QTimer::singleShot(100, this, [this]() {
            triggerTranslation();
        });
    } else {
        triggerTranslation();
    }
}

QImage QmlMainWindow::captureCurrentFrame()
{
    qCInfo(chiakiGui) << "=== captureCurrentFrame() START ===";
    QMutexLocker locker(&frame_mutex);
    
    qCInfo(chiakiGui) << "  screenshot_frame exists:" << (screenshot_frame != nullptr);
    
    if (!screenshot_frame) {
        qCWarning(chiakiGui) << "⚠️ No screenshot frame available";
        qCWarning(chiakiGui) << "  This is normal during first few seconds of streaming";
        qCWarning(chiakiGui) << "  Try again in a moment when frames are being rendered";
        return QImage();
    }

    // Получаем параметры фрейма
    int width = screenshot_frame->width;
    int height = screenshot_frame->height;
    AVPixelFormat format = static_cast<AVPixelFormat>(screenshot_frame->format);

    qCInfo(chiakiGui) << "  Frame info:" << width << "x" << height << "format:" << format;
    qCInfo(chiakiGui) << "  hw_frames_ctx:" << (screenshot_frame->hw_frames_ctx != nullptr);

    // Создаем временный фрейм для конвертации в RGB
    AVFrame *rgb_frame = av_frame_alloc();
    if (!rgb_frame) {
        qCWarning(chiakiGui) << "⚠️ Failed to allocate RGB frame";
        return QImage();
    }

    rgb_frame->width = width;
    rgb_frame->height = height;
    rgb_frame->format = AV_PIX_FMT_RGB24;

    int ret = av_frame_get_buffer(rgb_frame, 0);
    if (ret < 0) {
        qCWarning(chiakiGui) << "⚠️ Failed to allocate RGB frame buffer";
        av_frame_free(&rgb_frame);
        return QImage();
    }

    // Если это hardware фрейм, сначала переносим в системную память
    AVFrame *sw_frame = screenshot_frame;
    AVFrame *temp_sw_frame = nullptr;
    
    if (screenshot_frame->hw_frames_ctx) {
        qCInfo(chiakiGui) << "  Transferring from hardware...";
        temp_sw_frame = av_frame_alloc();
        if (av_hwframe_transfer_data(temp_sw_frame, screenshot_frame, 0) < 0) {
            qCWarning(chiakiGui) << "⚠️ Failed to transfer frame from hardware";
            av_frame_free(&rgb_frame);
            av_frame_free(&temp_sw_frame);
            return QImage();
        }
        av_frame_copy_props(temp_sw_frame, screenshot_frame);
        sw_frame = temp_sw_frame;
        qCInfo(chiakiGui) << "  ✓ Hardware transfer successful";
    }

    qCInfo(chiakiGui) << "  Converting to RGB24...";
    // Конвертируем в RGB24
    struct SwsContext *sws_ctx = sws_getContext(
        width, height, static_cast<AVPixelFormat>(sw_frame->format),
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws_ctx) {
        qCWarning(chiakiGui) << "⚠️ Failed to create SwsContext";
        av_frame_free(&rgb_frame);
        if (temp_sw_frame)
            av_frame_free(&temp_sw_frame);
        return QImage();
    }

    int scaleResult = sws_scale(sws_ctx, sw_frame->data, sw_frame->linesize, 0, height,
                                rgb_frame->data, rgb_frame->linesize);

    qCInfo(chiakiGui) << "  ✓ Conversion successful, lines processed:" << scaleResult;
    qCInfo(chiakiGui) << "  Creating QImage...";
    qCInfo(chiakiGui) << "    RGB frame data[0]:" << (rgb_frame->data[0] != nullptr);
    qCInfo(chiakiGui) << "    RGB frame linesize[0]:" << rgb_frame->linesize[0];

    // Создаем QImage из RGB данных
    QImage image(rgb_frame->data[0], width, height, rgb_frame->linesize[0], 
                 QImage::Format_RGB888);
    
    qCInfo(chiakiGui) << "    Temp QImage created - size:" << image.size() << "isNull:" << image.isNull();
    qCInfo(chiakiGui) << "    Temp QImage format:" << image.format();
    
    // Делаем глубокую копию, так как данные из AVFrame не будут постоянными
    QImage result = image.copy();
    
    qCInfo(chiakiGui) << "    Deep copy created - size:" << result.size() << "isNull:" << result.isNull();
    qCInfo(chiakiGui) << "    Deep copy format:" << result.format();

    // Освобождаем ресурсы
    sws_freeContext(sws_ctx);
    av_frame_free(&rgb_frame);
    if (temp_sw_frame)
        av_frame_free(&temp_sw_frame);

    qCInfo(chiakiGui) << "✓ Frame captured successfully!";
    qCInfo(chiakiGui) << "  Result image size:" << result.size();
    qCInfo(chiakiGui) << "  Result isNull:" << result.isNull();
    qCInfo(chiakiGui) << "=== captureCurrentFrame() END ===";
    return result;
}

void QmlMainWindow::onRecognitionFinished(bool success)
{
    translation_in_progress = false;

    if (!success) {
        qCWarning(chiakiGui) << "Text recognition failed";
        return;
    }

    QVector<RecognizedTextBlock> blocks = yandex_ocr->getRecognizedBlocks();
    qCInfo(chiakiGui) << "Recognition finished successfully, found" << blocks.size() << "text blocks";

    if (blocks.isEmpty()) {
        qCInfo(chiakiGui) << "No text found in the image";
        return;
    }

    // Устанавливаем блоки в оверлей
    // Размер берем из screenshot_frame (который использовался для захвата)
    QMutexLocker locker(&frame_mutex);
    QSize imageSize(1920, 1080); // Размер по умолчанию
    
    if (screenshot_frame) {
        imageSize = QSize(screenshot_frame->width, screenshot_frame->height);
        qCInfo(chiakiGui) << "Image size from screenshot_frame:" << imageSize;
    } else {
        qCInfo(chiakiGui) << "Using default image size:" << imageSize;
    }
    
    locker.unlock();
    
    text_overlay->setTextBlocks(blocks, imageSize);
    scheduleUpdate(); // Перерисовываем экран с оверлеем
    
    qCInfo(chiakiGui) << "✓ Translation overlay activated with" << blocks.size() << "blocks";
}

void QmlMainWindow::onRecognitionError(const QString &errorMessage)
{
    translation_in_progress = false;
    qCWarning(chiakiGui) << "Recognition error:" << errorMessage;
}
