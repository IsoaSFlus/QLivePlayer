#include "mpvwidget.h"
#include <stdexcept>
#include <QtGui/QOpenGLContext>
#include <QtCore/QMetaObject>
#include <QFontMetrics>
#include <QPainterPath>

static void wakeup(void *ctx)
{
    QMetaObject::invokeMethod((MpvWidget*)ctx, "on_mpv_events", Qt::QueuedConnection);
}

static void *get_proc_address(void *ctx, const char *name) {
    Q_UNUSED(ctx);
    QOpenGLContext *glctx = QOpenGLContext::currentContext();
    if (!glctx)
        return NULL;
    return (void *)glctx->getProcAddress(QByteArray(name));
}

MpvWidget::MpvWidget(QWidget *parent, Qt::WindowFlags f, bool cli)
    : QOpenGLWidget(parent, f)
{
    if(cli == true)
    {
        mpv = mpv::qt::Handle::FromRawHandle(mpv_create());
        if (!mpv)
            throw std::runtime_error("could not create mpv context");

        mpv_set_option_string(mpv, "terminal", "yes");
        if (mpv_initialize(mpv) < 0)
            throw std::runtime_error("could not initialize mpv context");

        mpv::qt::set_option_variant(mpv, "vo", "null");

        mpv::qt::set_option_variant(mpv, "ao", "null");

        mpv_observe_property(mpv, 0, "duration", MPV_FORMAT_DOUBLE);
        mpv_observe_property(mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
        mpv_set_wakeup_callback(mpv, wakeup, this);
    }
    else
    {
        mpv = mpv::qt::Handle::FromRawHandle(mpv_create());
        if (!mpv)
            throw std::runtime_error("could not create mpv context");

        mpv_set_option_string(mpv, "terminal", "yes");
        //    mpv_set_option_string(mpv, "msg-level", "all=v");
        if (mpv_initialize(mpv) < 0)
            throw std::runtime_error("could not initialize mpv context");

        // Make use of the MPV_SUB_API_OPENGL_CB API.
        mpv::qt::set_option_variant(mpv, "vo", "opengl-cb");

        // Request hw decoding, just for testing.
        mpv::qt::set_option_variant(mpv, "hwdec", "vaapi");

        mpv_gl = (mpv_opengl_cb_context *)mpv_get_sub_api(mpv, MPV_SUB_API_OPENGL_CB);
        if (!mpv_gl)
            throw std::runtime_error("OpenGL not compiled in");
        mpv_opengl_cb_set_update_callback(mpv_gl, MpvWidget::on_update, (void *)this);
        connect(this, SIGNAL(frameSwapped()), SLOT(swapped()));

        mpv_observe_property(mpv, 0, "duration", MPV_FORMAT_DOUBLE);
        mpv_observe_property(mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
        mpv_set_wakeup_callback(mpv, wakeup, this);

    }
}



MpvWidget::~MpvWidget()
{
    makeCurrent();
    if (mpv_gl)
        mpv_opengl_cb_set_update_callback(mpv_gl, NULL, NULL);
    // Until this call is done, we need to make sure the player remains
    // alive. This is done implicitly with the mpv::qt::Handle instance
    // in this class.
    mpv_opengl_cb_uninit_gl(mpv_gl);
}

void MpvWidget::command(const QVariant& params)
{
    mpv::qt::command_variant(mpv, params);
}

void MpvWidget::setProperty(const QString& name, const QVariant& value)
{
    mpv::qt::set_property_variant(mpv, name, value);
}

QVariant MpvWidget::getProperty(const QString &name) const
{
    return mpv::qt::get_property_variant(mpv, name);
}


void MpvWidget::initializeGL()
{
    int r = mpv_opengl_cb_init_gl(mpv_gl, NULL, get_proc_address, NULL);
    if (r < 0)
        throw std::runtime_error("could not initialize OpenGL");
}

void MpvWidget::paintGL()
{
    mpv_opengl_cb_draw(mpv_gl, defaultFramebufferObject(), width(), -height());
}

void MpvWidget::swapped()
{
    mpv_opengl_cb_report_flip(mpv_gl, 0);
}

void MpvWidget::on_mpv_events()
{
    // Process all events, until the event queue is empty.
    while (mpv) {
        mpv_event *event = mpv_wait_event(mpv, 0);
        if (event->event_id == MPV_EVENT_NONE) {
            break;
        }
        handle_mpv_event(event);
    }
}

void MpvWidget::handle_mpv_event(mpv_event *event)
{
    switch (event->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property *prop = (mpv_event_property *)event->data;
        if (strcmp(prop->name, "time-pos") == 0) {
            if (prop->format == MPV_FORMAT_DOUBLE) {
                double time = *(double *)prop->data;
                Q_EMIT positionChanged(time);
            }
        } else if (strcmp(prop->name, "duration") == 0) {
            if (prop->format == MPV_FORMAT_DOUBLE) {
                double time = *(double *)prop->data;
                Q_EMIT durationChanged(time);
            }
        }
        break;
    }
    default: ;
        // Ignore uninteresting or unknown events.
    }
}

// Make Qt invoke mpv_opengl_cb_draw() to draw a new/updated video frame.
void MpvWidget::maybeUpdate()
{
    // If the Qt window is not visible, Qt's update() will just skip rendering.
    // This confuses mpv's opengl-cb API, and may lead to small occasional
    // freezes due to video rendering timing out.
    // Handle this by manually redrawing.
    // Note: Qt doesn't seem to provide a way to query whether update() will
    //       be skipped, and the following code still fails when e.g. switching
    //       to a different workspace with a reparenting window manager.
    if (window()->isMinimized()) {
        makeCurrent();
        paintGL();
        context()->swapBuffers(context()->surface());
        swapped();
        doneCurrent();
    } else {
        update();
    }
}

void MpvWidget::on_update(void *ctx)
{
    QMetaObject::invokeMethod((MpvWidget*)ctx, "maybeUpdate");
}

/*************************DanmakuPlayer BEGIN*************************/


DanmakuPlayer::DanmakuPlayer(QWidget *parent, Qt::WindowFlags f) : MpvWidget(parent, f)
{
    setFocusPolicy(Qt::StrongFocus);
    checkVideoResolutionTimer = new QTimer(this);
    checkVideoResolutionTimer->start(500);
    time.start();
    initDanmaku();

    connect(checkVideoResolutionTimer, &QTimer::timeout, this, &DanmakuPlayer::checkVideoResolution);
}

DanmakuPlayer::~DanmakuPlayer()
{

}

void DanmakuPlayer::initDanmaku()
{

}

bool DanmakuPlayer::isDanmakuVisible()
{
    return danmakuShowFlag;
}

void DanmakuPlayer::launchDanmaku(QString danmakuText)
{
    int availDChannel = getAvailDanmakuChannel();
    if(checkVideoResolutionTimer == nullptr && (QCoreApplication::arguments().at(3) != "false"))
        danmakuRecorder->danmaku2ASS(danmakuText, 13000, 24, availDChannel);
    int danmakuPos = availDChannel * (this->height() / 24);
    int danmakuSpeed = (this->width()+500) / 0.17;//0.17 pixel per second

    QLabel* danmaku;
    danmaku = new QLabel(this);
    danmaku->setText(danmakuText);
    danmaku->setStyleSheet("color: #FFFFFF; font-size: 18px; font-weight: bold");

    QGraphicsDropShadowEffect *danmakuTextShadowEffect = new QGraphicsDropShadowEffect(this);
    danmakuTextShadowEffect->setColor(QColor("#000000"));
    danmakuTextShadowEffect->setBlurRadius(4);
    danmakuTextShadowEffect->setOffset(1,1);
    danmaku->setGraphicsEffect(danmakuTextShadowEffect);

    QPropertyAnimation* mAnimation=new QPropertyAnimation(danmaku, "pos");
    mAnimation->setStartValue(QPoint(this->width(), danmakuPos));
    mAnimation->setEndValue(QPoint(-500, danmakuPos));
    mAnimation->setDuration(danmakuSpeed);
    mAnimation->setEasingCurve(QEasingCurve::Linear);
    danmaku->show();
    mAnimation->start();
    danmakuTimeNodeSeq[availDChannel] = time.elapsed();
    danmakuTimeLengthSeq[availDChannel] = (danmaku->width() / 0.17) + 100;


    connect(this, &DanmakuPlayer::closeDanmaku, danmaku, &QLabel::close);
    connect(mAnimation, &QPropertyAnimation::finished, danmaku, &QLabel::deleteLater);
}

int DanmakuPlayer::getAvailDanmakuChannel()
{
    int currentTime = time.elapsed();
    int i;
    for(i = 0; i < 24; i++)
    {
        if((currentTime - danmakuTimeNodeSeq[i]) > danmakuTimeLengthSeq[i])
            return i;
    }
    i = qrand()%24;
    return i;
}

void DanmakuPlayer::checkVideoResolution()
{
    if(getProperty("video-params/w").toString() != QString(""))
    {
        checkVideoResolutionTimer->stop();
        delete checkVideoResolutionTimer;
        checkVideoResolutionTimer = nullptr;
        if(QCoreApplication::arguments().at(3) != "false")
//            danmakuRecorder = new DanmakuRecorder(getProperty("video-params/w").toInt(), getProperty("video-params/h").toInt(), QCoreApplication::arguments().at(3));
            danmakuRecorder = new DanmakuRecorder(1280, 720, QCoreApplication::arguments().at(3));
    }
}

void DanmakuPlayer::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_D:
        danmakuShowFlag = !danmakuShowFlag;
        if(danmakuShowFlag == false) {
            Q_EMIT closeDanmaku();
        }else {
            initDanmaku();
        }
        break;
    case Qt::Key_F:
        if(!QApplication::activeWindow()->isFullScreen()) {
            QApplication::activeWindow()->showFullScreen();
        }else {
            QApplication::activeWindow()->showNormal();
        }
        break;
    case Qt::Key_Q:
        QApplication::exit();
        break;
    case Qt::Key_Space:
    {
        const bool paused = getProperty("pause").toBool();
        setProperty("pause", !paused);
        break;
    }
    case Qt::Key_M:
    {
        const bool muted = getProperty("ao-mute").toBool();
        setProperty("ao-mute", !muted);
        break;
    }
    case Qt::Key_Minus:
    {
        int volume = getProperty("ao-volume").toInt();
        if(volume > 0)
            volume -= 5;
        setProperty("ao-volume", QString::number(volume));
        break;
    }
    case Qt::Key_Equal:
    {
        int volume = getProperty("ao-volume").toInt();
        if(volume < 100)
            volume += 5;
        setProperty("ao-volume", QString::number(volume));
        break;
    }
    default:
        break;
    }
    MpvWidget::keyPressEvent(event);
}

