#include <QPainter>
#include <QHash>
#include <QSharedPointer>
#include <QJsonDocument>
#include <QStyleOption>
#include <QMouseEvent>
#include <QTabletEvent>
#include <QSettings>
#include <QApplication>
#include <QTimer>
#include <QtCore/qmath.h>

#include "../../common/common.h"
#include "../../common/network/commandsocket.h"
#include "../paintingTools/brush/brushmanager.h"
#include "../paintingTools/brush/brush.h"
#include "../paintingTools/brush/sketchbrush.h"
#include "../paintingTools/brush/eraser.h"
#include "../paintingTools/brush/pencil.h"
#include "../misc/platformextend.h"
#include "../misc/singleton.h"

#include "canvas.h"


/*!
     \class Canvas

     \brief The widget that used for rendering painting results.

     Canvas uses several \l{QPixmap} to render contents. Each layer
     opaque one \l{QPixmap} and painted together when paintEvent happens.

     Canvas can be used for mouse painting, tablet painting and remote
     painting via json messages from network.

     \l{Brush} is used for painting. There're 4 kind of brush
     currently and will be more. When changing color of
     a \l{Brush} using on Canvas now,
     you can use either setBrushColor()
     or \l{Brush::} {setColor()} .

     \sa Brush
*/

/*!
    \fn Canvas::Canvas(QWidget *parent)

    Construct a canvas with width of 720px, height of 480px and a \a parent.
*/

Canvas::Canvas(QWidget *parent) :
    QWidget(parent),
    canvasSize(3240,2160),
    layers(canvasSize),
    image(canvasSize),
    layerNameCounter(0),
    historySize_(0),
    shareColor_(true),
    jitterCorrection_(true),
    jitterCorrectionLevel_(3),
    backend_(new CanvasBackend(0)),
    worker_(new QThread(this))
{
    setAttribute(Qt::WA_StaticContents);
    inPicker = false;
    drawing = false;
    disableMouse_ = false;
    brush_ = BrushPointer(new Brush);
    updateCursor();

    setMouseTracking(true);
    setFocusPolicy(Qt::WheelFocus); // necessary for IME control
    resize(canvasSize);

    BrushPointer p1(new Brush);
    BrushPointer p2(new Pencil);
    BrushPointer p3(new SketchBrush);
    BrushPointer p4(new Eraser);
    Singleton<BrushManager>::instance().addBrush(p1);
    Singleton<BrushManager>::instance().addBrush(p2);
    Singleton<BrushManager>::instance().addBrush(p3);
    Singleton<BrushManager>::instance().addBrush(p4);
    setJitterCorrectionLevel(5);

    backend_->moveToThread(worker_);
    connect(backend_, &CanvasBackend::newDataGroup,
            this, &Canvas::sendData);
    connect(backend_, &CanvasBackend::remoteDrawLine,
            this, &Canvas::remoteDrawLine);
    connect(backend_, &CanvasBackend::remoteDrawPoint,
            this, &Canvas::remoteDrawPoint);
    worker_->start();
}

/*!
    \fn Canvas::~Canvas()

    Destroys the canvas. The internal \l Brush will also be deleted.
*/

Canvas::~Canvas()
{
    delete backend_;
}

QPixmap Canvas::currentCanvas()
{
    QPixmap pmp = image;
    layers.combineLayers(&pmp);
    return pmp;
}

QPixmap Canvas::allCanvas()
{
    QPixmap exp(canvasSize);
    exp.fill();
    QPainter painter(&exp);
    int count = layers.count();
    QPixmap * im = 0;
    for(int i=0;i<count;++i){
        LayerPointer l = layers.layerFrom(i);
        im = l->imagePtr();
        painter.drawPixmap(0, 0, *im);
    }
    return exp;
}

void Canvas::setHistorySize(quint64 s)
{
    historySize_ = s;
}

int Canvas::jitterCorrectionLevel() const
{
    return jitterCorrectionLevel_;
}

bool Canvas::isJitterCorrectionEnabled() const
{
    return jitterCorrection_;
}

void Canvas::setJitterCorrectionEnabled(bool correct)
{
    jitterCorrection_ = correct;
}

void Canvas::setJitterCorrectionLevel(int value)
{
    jitterCorrectionLevel_ = qBound(0, value, 10);
    jitterCorrectionLevel_internal_ =
            qBound(0.0,
                   jitterCorrectionLevel_ / 10.0 * 0.99 + 0.85,
                   0.99);
}

void Canvas::tryJitterCorrection()
{
    if(stackPoints.length() < qBound(3, jitterCorrectionLevel_, 10) )
        return;

    int amount = stackPoints.length();
    int redudent = amount;

    auto should_correct = [this, &redudent](const QPoint& p1,
            const QPoint& p2,
            const QPoint& p3) -> bool
    {
        QLine l1(p1, p2);
        QLine l2(p2, p3);
        QLine l3(p3, p1);
        // l3^2 = l1^2 + l2^2 + 2*l1*l2*cos(a), where cos(a) is our goal
        qreal len3 = l3.dx()*l3.dx() + l3.dy()*l3.dy();
        len3 = qSqrt(len3);
        qreal len2 = l2.dx()*l2.dx() + l2.dy()*l2.dy();
        len2 = qSqrt(len2);
        qreal len1 = l1.dx()*l1.dx() + l1.dy()*l1.dy();
        len1 = qSqrt(len1);

        qreal cosa = (len3*len3 - len2*len2 - len1*len1) / (2*len1*len2);

        if(cosa < jitterCorrectionLevel_internal_ ){
            redudent--;
            return true;
        }else{
            return false;
        }

    };

    for(int i=0;i<stackPoints.length()-3;++i){
        if(should_correct(stackPoints[i],
                          stackPoints[i+1],
                          stackPoints[i+2])){
            stackPoints.removeAt(i+1);
        }
    }
    // you can see the correction rate.
    qDebug()<<"correction rate: "<<qreal(amount-redudent)/amount *100<<"%";
}

/*!
    \fn QVariantMap Canvas::brushInfo()

    Returns the brush info painting now.
    \sa Brush::brushInfo()
*/

QVariantMap Canvas::brushInfo()
{
    return brush_->brushInfo();
}

void Canvas::setShareColor(bool b)
{
    shareColor_ = b;
}

/*!
    \fn void Canvas::setBrushColor(const QColor &newColor)

    Sets a \a newColor to current brush.
    \sa Brush::setColor()
*/

void Canvas::setBrushColor(const QColor &newColor)
{
    if(brush_->color() == newColor) return;
    brush_->setColor(newColor);
}

/*!
    \fn void Canvas::setBrushWidth(int newWidth)

    Sets a \a newWidth to current brush.
    \sa Brush::setWidth()
*/

void Canvas::setBrushWidth(int newWidth)
{
    if(brush_->width() != newWidth){
        brush_->setWidth(newWidth);
        updateCursor();
    }
}

void Canvas::setBrushHardness(int h)
{
    if(brush_->hardness() != h){
        brush_->setHardness(h);
    }
}

BrushPointer Canvas::brushFactory(const QString &name)
{
    return Singleton<BrushManager>::instance().makeBrush(name);
}

/*!
    \fn void Canvas::changeBrush(const QString &name)

    Changes current brush to \a name .
    At this time, we have only brushButton for Brush, pencilButton for Pencil, sketchButton for Sketch, and eraserButton for Eraser.
    \sa Brush, SketchBrush, Pencil, Eraser
*/

void Canvas::changeBrush(const QString &name)
{
    QVariantMap currentSettings;
    LayerPointer sur = brush_->surface();
    QPointF lp = brush_->lastPoint();
    QVariantMap colorMap = brush_->brushInfo()
            .value("color").toMap();

    QString brushName = name;
    if(localBrush.contains(brushName)){
        brush_ = localBrush[brushName];
        currentSettings = brush_->brushInfo();
    }else{
        brush_ = brushFactory(brushName);
        localBrush.insert(brushName, brush_);
        currentSettings = brush_->defaultInfo();
    }
    // share same color between brushes
    if(shareColor_){
        currentSettings["color"] = colorMap;
    }

    brush_->setLastPoint(lp);
    brush_->setSurface(sur);
    updateCursor();

    emit newBrushSettings(currentSettings);
}

void Canvas::onColorPicker(bool in)
{
    if(in){
        inPicker = true;
        QPixmap icon = QPixmap(":/iconset/ui/picker-cursor.png");
        setCursor(QCursor(icon, 11, 20));
    }else{
        inPicker = false;
        updateCursor();
        emit pickColorComplete();
    }
}

/*!
    \fn void Canvas::drawLineTo(const QPoint &endPoint)

    Draws a line from last point to \a endPoint .
    The last point can be either determined by drawPoint() or by function itself.
    After drawing, it emits linePainted()

    \sa drawPoint() , linePainted()
*/

void Canvas::drawLineTo(const QPoint &endPoint, qreal pressure)
{
    LayerPointer l = layers.selectedLayer();
    if(l.isNull() || l->isLocked() || l->isHided()){
        setCursor(Qt::ForbiddenCursor);
        return;
    }
    updateCursor();
    brush_->setSurface(l);
    brush_->lineTo(endPoint, pressure);

    update();

    QVariantMap start_j;
    start_j.insert("x", this->lastPoint.x());
    start_j.insert("y", this->lastPoint.y());
    QVariantMap end_j;
    end_j.insert("x", endPoint.x());
    end_j.insert("y", endPoint.y());

    QVariantMap map;
    map.insert("brush", QVariant(brushInfo()));
    map.insert("start", QVariant(start_j));
    map.insert("end", QVariant(end_j));
    // guarantee that pressure is double
    map.insert("pressure", QVariant(double(pressure)));
    map.insert("layer", QVariant(currentLayer()));
    map.insert("clientid",
               Singleton<CommandSocket>::instance().clientId());

    QVariantMap bigMap;
    bigMap.insert("info", map);
    bigMap.insert("action", "drawline");

    backend_->onDataBlock(bigMap);
}

/*!
    \fn void Canvas::drawPoint(const QPoint &point)

    Draws a point at \a endPoint.
    After drawing, it emits pointPainted()
    \sa drawPoint() , pointPainted()
*/

void Canvas::drawPoint(const QPoint &point, qreal pressure)
{
    LayerPointer l = layers.selectedLayer();
    if(l.isNull() || l->isLocked() || l->isHided()){
        setCursor(Qt::ForbiddenCursor);
        return;
    }
    updateCursor();
    brush_->setSurface(l);
    brush_->start(point, pressure);

    int rad = (brush_->width() / 2) + 2;
    update(QRect(lastPoint, point).normalized()
           .adjusted(-rad, -rad, +rad, +rad));

    QVariantMap point_j;
    point_j.insert("x", point.x());
    point_j.insert("y", point.y());

    QVariantMap map;
    map.insert("brush", QVariant(brushInfo()));
    // guarantee that pressure is double
    map.insert("pressure", QVariant(double(pressure)));
    map.insert("layer", QVariant(currentLayer()));
    map.insert("point", QVariant(point_j));
    map.insert("clientid",
               QVariant(Singleton<CommandSocket>::instance().clientId()));

    QVariantMap bigMap;
    bigMap.insert("info", map);
    bigMap.insert("action", "drawpoint");

    backend_->onDataBlock(bigMap);
}

void Canvas::pickColor(const QPoint &point)
{
    brush_->setColor(image.toImage().pixel(point));
    newBrushSettings(brush_->brushInfo());
}

void Canvas::updateCursor()
{
    this->setCursor(brush_->cursor());
}

/*!
    \fn void Canvas::remoteDrawPoint(const QPoint &point, const QVariantMap &brushInfo,
                             const QString &layer,
                             const quint64 userid)

    Draws a remote point at \a point at \a layer with \a brushInfo.
    To identical user, \a userid must provided.
    \sa Canvas::remoteDrawLine()
*/

void Canvas::remoteDrawPoint(const QPoint &point,
                             const QVariantMap &brushInfo,
                             const QString &layer,
                             const QString clientid,
                             const qreal pressure)
{
    if(!layers.exists(layer)) return;
    LayerPointer l = layers.layerFrom(layer);

    QString brushName = brushInfo["name"].toString();
    int width = brushInfo["width"].toInt();
    int hardness = brushInfo["hardness"].toInt();
    QVariantMap colorMap = brushInfo["color"].toMap();
    QColor color(colorMap["red"].toInt(),
            colorMap["green"].toInt(),
            colorMap["blue"].toInt());

    if(remoteBrush.contains(clientid)){
        BrushPointer t = remoteBrush[clientid];
        if(brushInfo != t->brushInfo()){
            BrushPointer newOne = brushFactory(brushName);
            newOne->setSurface(l);
            newOne->setWidth(width);
            newOne->setHardness(hardness);
            newOne->setColor(color);
            newOne->start(point, pressure);
            remoteBrush[clientid] = newOne;
            t.clear();
        }else{
            BrushPointer original = remoteBrush[clientid];
            original->setSurface(l);
            original->setWidth(width);
            original->setHardness(hardness);
            original->setColor(color);
            original->start(point, pressure);
        }
    }else{
        BrushPointer newOne = brushFactory(brushName);
        newOne->setSurface(l);
        newOne->setWidth(width);
        newOne->setHardness(hardness);
        newOne->setColor(color);
        newOne->start(point, pressure);
        remoteBrush[clientid] = newOne;
    }

    update();
}

/*!
    \fn void Canvas::remoteDrawLine(const QPoint &start, const QPoint &end,
                            const QVariantMap &brushInfo,
                            const QString &layer,
                            const quint64 userid)

    Draws a remote line from \a start to \a end at \a layer with \a brushInfo .
    To identical user, \a userid must provided.
    \sa Canvas::remoteDrawLine()
*/

void Canvas::remoteDrawLine(const QPoint &, const QPoint &end,
                            const QVariantMap &brushInfo,
                            const QString &layer,
                            const QString clientid,
                            const qreal pressure)
{
    if(!layers.exists(layer)){
        return;
    }
    LayerPointer l = layers.layerFrom(layer);

    QString brushName = brushInfo["name"].toString();
    int width = brushInfo["width"].toInt();
    int hardness = brushInfo["hardness"].toInt();
    QVariantMap colorMap = brushInfo["color"].toMap();
    QColor color(colorMap["red"].toInt(),
            colorMap["green"].toInt(),
            colorMap["blue"].toInt());

    if(remoteBrush.contains(clientid)){
        BrushPointer t = remoteBrush[clientid];
        if(brushName != t->brushInfo()["name"].toString()){
            BrushPointer newOne = brushFactory(brushName);
            newOne->setSurface(l);
            newOne->setWidth(width);
            newOne->setHardness(hardness);
            newOne->setColor(color);
            newOne->lineTo(end, pressure);
            remoteBrush[clientid] = newOne;
            t.clear();
        }else{
            BrushPointer original = remoteBrush[clientid];
            original->setSurface(l);
            original->setWidth(width);
            original->setHardness(hardness);
            original->setColor(color);
            original->lineTo(end, pressure);
        }
    }else{
        BrushPointer newOne = brushFactory(brushName);
        newOne->setSurface(l);
        newOne->setWidth(width);
        newOne->setHardness(hardness);
        newOne->setColor(color);
        newOne->lineTo(end, pressure);
        remoteBrush[clientid] = newOne;
    }

    update();
}

void Canvas::onNewData(const QByteArray & array)
{
    static quint64 h_size = 0;
    if(historySize_) {
        h_size += array.size();
        if(h_size < historySize_){
            this->setDisabled(true);
            //            qDebug()<<"History: "<<historySize_
            //                   <<"Loaded: "<<h_size;
        }else{
            qDebug()<<"History"<<historySize_<<"bytes loaded!";
            historySize_ = 0;
            h_size = 0;
            this->setEnabled(true);
            emit historyComplete();
        }
    }
    backend_->onIncomingData(array);
}

/* Layer */

/*!
    \fn QString Canvas::currentLayer()

    Returns current drawing layer.
    In detail, it always returns the layer selected by user, even it is hided or locked.
*/

QString Canvas::currentLayer()
{
    return layers.selectedLayer()->name();
}

/*!
    \fn void Canvas::addLayer(const QString &name)

    Adds a new layer named \a name. The layer will append on the top.
    \sa deleteLayer()
*/

void Canvas::addLayer(const QString &name)
{
    layers.appendLayer(name);
    layerNameCounter++;
}

/*!
    \fn bool Canvas::deleteLayer(const QString &name)

    Deletes the layer named \a name. Returns false if failed.
    A locked layer cannot be deleted, while a hided one can.
    \sa addLayer()
*/

bool Canvas::deleteLayer(const QString &name)
{
    if(layers.layerFrom(name)->isLocked())
        return false;

    layers.removeLayer(name);
    update();
    return true;
}

void Canvas::clearLayer(const QString &name)
{
    layers.clearLayer(name);
    update();
}

void Canvas::clearAllLayer()
{
    layers.clearAllLayer();
    update();
}

/*!
    \fn void Canvas::lockLayer(const QString &name)

    Locks one layer named \a name.
    \sa unlockLayer()
*/

void Canvas::lockLayer(const QString &name)
{
    layers.layerFrom(name)->lock();
}

/*!
    \fn void Canvas::unlockLayer(const QString &name)

    Unlocks one layer named \a name.
    \sa lockLayer()
*/

void Canvas::unlockLayer(const QString &name)
{
    layers.layerFrom(name)->unlock();
}

/*!
    \fn void Canvas::hideLayer(const QString &name)

    Hides one layer named \a name.
    \sa showLayer()
*/

void Canvas::hideLayer(const QString &name)
{
    layers.layerFrom(name)->hide();
    update();
}

/*!
    \fn void Canvas::showLayer(const QString &name)

    Show one layer named \a name.
    \sa hideLayer()
*/

void Canvas::showLayer(const QString &name)
{
    layers.layerFrom(name)->show();
    update();
}

void Canvas::moveLayerUp(const QString &name)
{
    layers.moveUp(name);
    update();
}

void Canvas::moveLayerDown(const QString &name)
{
    layers.moveDown(name);
    update();
}

/*!
    \fn void Canvas::layerSelected(const QString &name)

    Selects the layer named \a name.

    This function is used when render on the screen.
*/

void Canvas::layerSelected(const QString &name)
{
    layers.select(name);
}

/* Event control */

void Canvas::tabletEvent(QTabletEvent *ev)
{
    //TODO: fully support tablet
    qreal pressure = ev->pressure();
    QPoint pos = ev->pos();

    switch(ev->type()){
    case QEvent::TabletPress:
        if(!drawing){
            lastPoint = pos;
            drawPoint(pos, pressure);
            drawing = true;
        }
        break;
    case QEvent::TabletMove:
        if(drawing && lastPoint != pos){
            drawLineTo(pos, pressure);
            lastPoint = pos;
        }
        break;
    case QEvent::TabletRelease:
        if(drawing){
            drawing = false;
            updateCursor();
        }
        break;
    default:
        break;
    }
    ev->accept();
}

void Canvas::focusInEvent(QFocusEvent *)
{
    QSettings settings(GlobalDef::SETTINGS_NAME,
                       QSettings::defaultFormat(),
                       qApp);
    bool disable_ime = settings.value("canvas/auto_disable_ime").toBool();
    if(disable_ime)
        PlatformExtend::setIMEState(this, false);
}

void Canvas::focusOutEvent(QFocusEvent *)
{
    QSettings settings(GlobalDef::SETTINGS_NAME,
                       QSettings::defaultFormat(),
                       qApp);
    bool disable_ime = settings.value("canvas/auto_disable_ime").toBool();
    if(disable_ime)
        PlatformExtend::setIMEState(this, true);
}

void Canvas::mousePressEvent(QMouseEvent *event)
{
    if(disableMouse_){
        return;
    }
    if (event->button() == Qt::LeftButton) {
        lastPoint = event->pos();
        if(inPicker){
        }else{
            drawing = true;
            stackPoints.push_back(lastPoint);
            drawPoint(lastPoint);
        }
    }
}

void Canvas::mouseMoveEvent(QMouseEvent *event)
{
    if(disableMouse_){
        return;
    }
    if ((event->buttons() & Qt::LeftButton)){
        if(inPicker){
            pickColor(event->pos());
        }else{
            if(drawing){
                if(jitterCorrection_){
                    if(stackPoints.length() < qBound(3, jitterCorrectionLevel_, 10)){
                        stackPoints.push_back(event->pos());
                    }else{
                        tryJitterCorrection();
                        for(auto &p: stackPoints){
                            drawLineTo(p);
                            lastPoint = p;
                        }
                        stackPoints.clear();
                    }
                }else{
                    drawLineTo(event->pos());
                    lastPoint = event->pos();
                }
            }
        }
    }
}

void Canvas::mouseReleaseEvent(QMouseEvent *event)
{
    if(disableMouse_){
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if(inPicker){
            pickColor(event->pos());
            onColorPicker(false);
        }else{
            if(drawing){
                drawing = false;
                stackPoints.clear();
                updateCursor();
                backend_->commit();
            }
        }
    }
}

void Canvas::paintEvent(QPaintEvent *event)
{

    QPainter painter(this);
    QRect dirtyRect = event->rect();
    if(dirtyRect.isEmpty())return;
    layers.combineLayers(&image, dirtyRect);

    painter.drawPixmap(dirtyRect, image, dirtyRect);

    if(!isEnabled()){
        QBrush brush;
        brush.setStyle(Qt::BDiagPattern);
        brush.setColor(Qt::lightGray);
        painter.setBrush(brush);
        QRect rect = this->rect();
        rect.setWidth(rect.width());
        rect.setHeight(rect.height());
        painter.drawRect(rect);
    }

    QStyleOption opt;
    opt.init(this);
    style()->drawPrimitive(QStyle::PE_Widget,
                           &opt, &painter, this);

}

void Canvas::resizeEvent(QResizeEvent *event)
{
    QSize newSize = event->size();
    canvasSize = newSize;
    layers.resizeLayers(newSize);
    QPixmap newImage(newSize);
    newImage.fill(Qt::transparent);
    QPainter painter(&newImage);
    painter.drawPixmap(QPoint(0, 0), image);
    image = newImage;

    update();
    QWidget::resizeEvent(event);
}

/*!
    \fn QSize Canvas::sizeHint() const

    Returns a pre-defined size for canvas.

    \sa QWidget::minimumSizeHint()
*/

QSize Canvas::sizeHint() const
{
    return canvasSize;
}

/*!
    \fn QSize Canvas::minimumSizeHint() const

    Returns a pre-defined minimal size for canvas.

    \sa sizeHint()
*/

QSize Canvas::minimumSizeHint() const
{
    return canvasSize;
}

void Canvas::foundTablet()
{
    disableMouse_ = true;
}

CanvasBackend::CanvasBackend(QObject *parent)
    :QObject(parent)
{
    QTimer *sendTimer = new QTimer(this);
    sendTimer->setInterval(1000*10);
}

void CanvasBackend::commit()
{
    if(tempStore.length()){
        QVariantMap doc;
        doc.insert("action", "block");
        doc.insert("block", tempStore);
        auto data = toJson(QVariant(doc));
        emit newDataGroup(data);
        tempStore.clear();
    }
}

void CanvasBackend::onDataBlock(const QVariantMap& d)
{
    tempStore.append(d);
    if(tempStore.length() >=10 ){
        commit();
    }
}

void CanvasBackend::onIncomingData(const QByteArray& data)
{
    QVariantMap m = fromJson(data).toMap();
    QString action = m["action"].toString().toLower();

    auto drawPoint = [this](const QVariantMap& m){
        QPoint point;
        QString layerName;

        QVariantMap map = m["info"].toMap();
        QVariantMap point_j = map["point"].toMap();
        point.setX(point_j["x"].toInt());
        point.setY(point_j["y"].toInt());
        layerName = map["layer"].toString();
        QVariantMap brushInfo = map["brush"].toMap();
        qreal pressure = 1.0;
        if(map.contains("pressure")){
            pressure = map["pressure"].toDouble();
        }
        QString clientid = map["clientid"].toString();

        emit remoteDrawPoint(point, brushInfo,
                             layerName, clientid,
                             pressure);
    };

    auto drawLine = [this](const QVariantMap& m){
        QPoint start;
        QPoint end;
        QString layerName;

        QVariantMap map = m["info"].toMap();
        QVariantMap start_j = map["start"].toMap();
        start.setX(start_j["x"].toInt());
        start.setY(start_j["y"].toInt());
        QVariantMap end_j = map["end"].toMap();
        end.setX(end_j["x"].toInt());
        end.setY(end_j["y"].toInt());
        layerName = map["layer"].toString();
        QVariantMap brushInfo = map["brush"].toMap();
        qreal pressure = 1.0;
        if(map.contains("pressure")){
            pressure = map["pressure"].toDouble();
        }
        QString clientid = map["clientid"].toString();

        emit remoteDrawLine(start, end,
                            brushInfo, layerName,
                            clientid, pressure);
    };

    auto dataBlock = [&drawPoint,
            &drawLine](const QVariantMap& m){
        QVariantList list = m["block"].toList();
        for(auto &item: list){
            QVariantMap singleData = item.toMap();
            QString action = singleData["action"].toString().toLower();
            if(action == "drawpoint"){
                drawPoint(singleData);
            }else if(action == "drawline"){
                drawLine(singleData);
            }
        }
    };

    if(action == "drawpoint"){
        drawPoint(m);
    }else if(action == "drawline"){
        drawLine(m);
    }else if(action == "block"){
        dataBlock(m);
    }
}

QByteArray CanvasBackend::toJson(const QVariant &m)
{
    return QJsonDocument::fromVariant(m).toJson();
}

QVariant CanvasBackend::fromJson(const QByteArray &d)
{
    return QJsonDocument::fromJson(d).toVariant();
}
