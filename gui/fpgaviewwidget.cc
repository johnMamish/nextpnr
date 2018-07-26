/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Serge Bazanski <q3k@symbioticeda.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <cstdio>
#include <math.h>

#include <QApplication>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QWidget>

#include "fpgaviewwidget.h"
#include "log.h"
#include "mainwindow.h"

NEXTPNR_NAMESPACE_BEGIN

void PolyLine::buildPoint(LineShaderData *building, const QVector2D *prev, const QVector2D *cur,
                          const QVector2D *next) const
{
    // buildPoint emits two vertices per line point, along with normals to move
    // them the right directio when rendering and miter to compensate for
    // bends.

    if (cur == nullptr) {
        // BUG
        return;
    }

    if (prev == nullptr && next == nullptr) {
        // BUG
        return;
    }

    // TODO(q3k): fast path for vertical/horizontal lines?

    // TODO(q3k): consider moving some of the linear algebra to the GPU,
    // they're better at this than poor old CPUs.

    // Build two unit vectors pointing in the direction of the two segments
    // defined by (prev, cur) and (cur, next)
    QVector2D dprev, dnext;
    if (prev == nullptr) {
        dnext = *next - *cur;
        dprev = dnext;
    } else if (next == nullptr) {
        dprev = *cur - *prev;
        dnext = dprev;
    } else {
        dprev = *cur - *prev;
        dnext = *next - *cur;
    }
    dprev.normalize();
    dnext.normalize();

    // Calculate tangent unit vector.
    QVector2D tangent(dprev + dnext);
    tangent.normalize();

    // Calculate normal to tangent - this is the line on which the vectors need
    // to be pushed to build a thickened line.
    const QVector2D tangent_normal = QVector2D(-tangent.y(), tangent.x());

    // Calculate normal to one of the lines.
    const QVector2D dprev_normal = QVector2D(-dprev.y(), dprev.x());
    // https://people.eecs.berkeley.edu/~sequin/CS184/IMGS/Sweep_PolyLine.jpg
    // (the ^-1 is performed in the shader)
    const float miter = QVector2D::dotProduct(tangent_normal, dprev_normal);

    const float x = cur->x();
    const float y = cur->y();
    const float mx = tangent_normal.x();
    const float my = tangent_normal.y();

    // Push back 'left' vertex.
    building->vertices.push_back(Vertex2DPOD(x, y));
    building->normals.push_back(Vertex2DPOD(mx, my));
    building->miters.push_back(miter);

    // Push back 'right' vertex.
    building->vertices.push_back(Vertex2DPOD(x, y));
    building->normals.push_back(Vertex2DPOD(mx, my));
    building->miters.push_back(-miter);
}

void PolyLine::build(LineShaderData &target) const
{
    if (points_.size() < 2) {
        return;
    }
    const QVector2D *first = &points_.front();
    const QVector2D *last = &points_.back();

    // Index number of vertices, used to build the index buffer.
    unsigned int startIndex = target.vertices.size();
    unsigned int index = startIndex;

    // For every point on the line, call buildPoint with (prev, point, next).
    // If we're building a closed line, prev/next wrap around. Otherwise
    // they are passed as nullptr and buildPoint interprets that accordinglu.
    const QVector2D *prev = nullptr;

    // Loop iterator used to ensure next is valid.
    unsigned int i = 0;
    for (const QVector2D &point : points_) {
        const QVector2D *next = nullptr;
        if (++i < points_.size()) {
            next = (&point + 1);
        }

        // If the line is closed, wrap around. Otherwise, pass nullptr.
        if (prev == nullptr && closed_) {
            buildPoint(&target, last, &point, next);
        } else if (next == nullptr && closed_) {
            buildPoint(&target, prev, &point, first);
        } else {
            buildPoint(&target, prev, &point, next);
        }

        // If we have a prev point relative to cur, build a pair of triangles
        // to render vertices into lines.
        if (prev != nullptr) {
            target.indices.push_back(index);
            target.indices.push_back(index + 1);
            target.indices.push_back(index + 2);

            target.indices.push_back(index + 2);
            target.indices.push_back(index + 1);
            target.indices.push_back(index + 3);

            index += 2;
        }
        prev = &point;
    }

    // If we're closed, build two more vertices that loop the line around.
    if (closed_) {
        target.indices.push_back(index);
        target.indices.push_back(index + 1);
        target.indices.push_back(startIndex);

        target.indices.push_back(startIndex);
        target.indices.push_back(index + 1);
        target.indices.push_back(startIndex + 1);
    }
}

bool LineShader::compile(void)
{
    program_ = new QOpenGLShaderProgram(parent_);
    program_->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource_);
    program_->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSource_);
    if (!program_->link()) {
        printf("could not link program: %s\n", program_->log().toStdString().c_str());
        return false;
    }

    if (!vao_.create())
        log_abort();
    vao_.bind();

    if (!buffers_.position.create())
        log_abort();
    if (!buffers_.normal.create())
        log_abort();
    if (!buffers_.miter.create())
        log_abort();
    if (!buffers_.index.create())
        log_abort();

    attributes_.position = program_->attributeLocation("position");
    attributes_.normal = program_->attributeLocation("normal");
    attributes_.miter = program_->attributeLocation("miter");
    uniforms_.thickness = program_->uniformLocation("thickness");
    uniforms_.projection = program_->uniformLocation("projection");
    uniforms_.color = program_->uniformLocation("color");

    vao_.release();
    return true;
}

void LineShader::draw(const LineShaderData &line, const QColor &color, float thickness, const QMatrix4x4 &projection)
{
    auto gl = QOpenGLContext::currentContext()->functions();
    if (line.vertices.size() == 0)
        return;
    vao_.bind();
    program_->bind();

    buffers_.position.bind();
    buffers_.position.allocate(&line.vertices[0], sizeof(Vertex2DPOD) * line.vertices.size());

    buffers_.normal.bind();
    buffers_.normal.allocate(&line.normals[0], sizeof(Vertex2DPOD) * line.normals.size());

    buffers_.miter.bind();
    buffers_.miter.allocate(&line.miters[0], sizeof(GLfloat) * line.miters.size());

    buffers_.index.bind();
    buffers_.index.allocate(&line.indices[0], sizeof(GLuint) * line.indices.size());

    program_->setUniformValue(uniforms_.projection, projection);
    program_->setUniformValue(uniforms_.thickness, thickness);
    program_->setUniformValue(uniforms_.color, color.redF(), color.greenF(), color.blueF(), color.alphaF());

    buffers_.position.bind();
    program_->enableAttributeArray("position");
    gl->glVertexAttribPointer(attributes_.position, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);

    buffers_.normal.bind();
    program_->enableAttributeArray("normal");
    gl->glVertexAttribPointer(attributes_.normal, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);

    buffers_.miter.bind();
    program_->enableAttributeArray("miter");
    gl->glVertexAttribPointer(attributes_.miter, 1, GL_FLOAT, GL_FALSE, 0, (void *)0);

    buffers_.index.bind();
    gl->glDrawElements(GL_TRIANGLES, line.indices.size(), GL_UNSIGNED_INT, (void *)0);

    program_->disableAttributeArray("miter");
    program_->disableAttributeArray("normal");
    program_->disableAttributeArray("position");

    program_->release();
    vao_.release();
}

FPGAViewWidget::FPGAViewWidget(QWidget *parent) :
        QOpenGLWidget(parent), ctx_(nullptr), paintTimer_(this),
        lineShader_(this), zoom_(500.0f),
        rendererData_(new FPGAViewWidget::RendererData),
        rendererArgs_(new FPGAViewWidget::RendererArgs)
{
    colors_.background = QColor("#000000");
    colors_.grid = QColor("#333");
    colors_.frame = QColor("#808080");
    colors_.hidden = QColor("#606060");
    colors_.inactive = QColor("#303030");
    colors_.active = QColor("#f0f0f0");
    colors_.selected = QColor("#ff6600");
    colors_.highlight[0] = QColor("#6495ed");
    colors_.highlight[1] = QColor("#7fffd4");
    colors_.highlight[2] = QColor("#98fb98");
    colors_.highlight[3] = QColor("#ffd700");
    colors_.highlight[4] = QColor("#cd5c5c");
    colors_.highlight[5] = QColor("#fa8072");
    colors_.highlight[6] = QColor("#ff69b4");
    colors_.highlight[7] = QColor("#da70d6");

    rendererArgs_->highlightedOrSelectedChanged = false;

    auto fmt = format();
    fmt.setMajorVersion(3);
    fmt.setMinorVersion(1);
    setFormat(fmt);

    fmt = format();
    if (fmt.majorVersion() < 3) {
        printf("Could not get OpenGL 3.0 context. Aborting.\n");
        log_abort();
    }
    if (fmt.minorVersion() < 1) {
        printf("Could not get OpenGL 3.1 context - trying anyway...\n ");
    }

    connect(&paintTimer_, SIGNAL(timeout()), this, SLOT(update()));
    paintTimer_.start(1000 / 20); // paint GL 20 times per second

    renderRunner_ = std::unique_ptr<PeriodicRunner>(new PeriodicRunner(this, [this] { renderLines(); }));
    renderRunner_->start();
    renderRunner_->startTimer(1000 / 2); // render lines 2 times per second
}

FPGAViewWidget::~FPGAViewWidget() {}

void FPGAViewWidget::newContext(Context *ctx)
{
    ctx_ = ctx;
    onSelectedArchItem(std::vector<DecalXY>());
    for (int i = 0; i < 8; i++)
        onHighlightGroupChanged(std::vector<DecalXY>(), i);
    pokeRenderer();
}

QSize FPGAViewWidget::minimumSizeHint() const { return QSize(640, 480); }

QSize FPGAViewWidget::sizeHint() const { return QSize(640, 480); }

void FPGAViewWidget::initializeGL()
{
    if (!lineShader_.compile()) {
        log_error("Could not compile shader.\n");
    }
    initializeOpenGLFunctions();
    glClearColor(colors_.background.red() / 255, colors_.background.green() / 255, colors_.background.blue() / 255,
                 0.0);
}

void FPGAViewWidget::drawGraphicElement(LineShaderData &out, const GraphicElement &el, float x, float y)
{
    const float scale = 1.0;

    if (el.type == GraphicElement::TYPE_BOX) {
        auto line = PolyLine(true);
        line.point(x + scale * el.x1, y + scale * el.y1);
        line.point(x + scale * el.x2, y + scale * el.y1);
        line.point(x + scale * el.x2, y + scale * el.y2);
        line.point(x + scale * el.x1, y + scale * el.y2);
        line.build(out);
    }

    if (el.type == GraphicElement::TYPE_LINE || el.type == GraphicElement::TYPE_ARROW) {
        PolyLine(x + scale * el.x1, y + scale * el.y1, x + scale * el.x2, y + scale * el.y2)
            .build(out);
    }
}

void FPGAViewWidget::drawDecal(LineShaderData &out, const DecalXY &decal)
{
    float offsetX = decal.x;
    float offsetY = decal.y;

    for (auto &el : ctx_->getDecalGraphics(decal.decal)) {
        drawGraphicElement(out, el, offsetX, offsetY);
    }
}

void FPGAViewWidget::drawArchDecal(LineShaderData out[GraphicElement::STYLE_MAX], const DecalXY &decal)
{
    float offsetX = decal.x;
    float offsetY = decal.y;

    for (auto &el : ctx_->getDecalGraphics(decal.decal)) {
        switch (el.style) {
        case GraphicElement::STYLE_FRAME:
        case GraphicElement::STYLE_INACTIVE:
        case GraphicElement::STYLE_ACTIVE:
            drawGraphicElement(out[el.style], el, offsetX, offsetY);
            break;
        default:
            break;
        }
    }
}

QMatrix4x4 FPGAViewWidget::getProjection(void)
{
    QMatrix4x4 matrix;

    const float aspect = float(width()) / float(height());
    matrix.perspective(3.14 / 2, aspect, zoomNear_, zoomFar_);
    matrix.translate(0.0f, 0.0f, -zoom_);
    return matrix;
}

void FPGAViewWidget::paintGL()
{
    auto gl = QOpenGLContext::currentContext()->functions();
    const qreal retinaScale = devicePixelRatio();
    gl->glViewport(0, 0, width() * retinaScale, height() * retinaScale);
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    QMatrix4x4 matrix = getProjection();

    matrix *= viewMove_;

    // Calculate world thickness to achieve a screen 1px/1.1px line.
    float thick1Px = mouseToWorldCoordinates(1, 0).x();
    float thick11Px = mouseToWorldCoordinates(1.1, 0).x();

    // Render grid.
    auto grid = LineShaderData();
    for (float i = -100.0f; i < 100.0f; i += 1.0f) {
        PolyLine(-100.0f, i, 100.0f, i).build(grid);
        PolyLine(i, -100.0f, i, 100.0f).build(grid);
    }
    // Draw grid.
    lineShader_.draw(grid, colors_.grid, thick1Px, matrix);

    rendererDataLock_.lock();

    // Render Arch graphics.
    lineShader_.draw(rendererData_->gfxByStyle[GraphicElement::STYLE_FRAME], colors_.frame, thick11Px, matrix);
    lineShader_.draw(rendererData_->gfxByStyle[GraphicElement::STYLE_HIDDEN], colors_.hidden, thick11Px, matrix);
    lineShader_.draw(rendererData_->gfxByStyle[GraphicElement::STYLE_INACTIVE], colors_.inactive, thick11Px, matrix);
    lineShader_.draw(rendererData_->gfxByStyle[GraphicElement::STYLE_ACTIVE], colors_.active, thick11Px, matrix);

    // Draw highlighted items.
    for (int i = 0; i < 8; i++)
        lineShader_.draw(rendererData_->gfxHighlighted[i], colors_.highlight[i], thick11Px, matrix);

    lineShader_.draw(rendererData_->gfxSelected, colors_.selected, thick11Px, matrix);
    rendererDataLock_.unlock();
}

void FPGAViewWidget::pokeRenderer(void) { renderRunner_->poke(); }

void FPGAViewWidget::renderLines(void)
{
    if (ctx_ == nullptr)
        return;

    // Data from Context needed to render all decals.
    std::vector<DecalXY> belDecals;
    std::vector<DecalXY> wireDecals;
    std::vector<DecalXY> pipDecals;
    std::vector<DecalXY> groupDecals;
    bool decalsChanged = false;
    {
        // Take the UI/Normal mutex on the Context, copy over all we need as
        // fast as we can.
        std::lock_guard<std::mutex> lock_ui(ctx_->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx_->mutex);

        // For now, collapse any decal changes into change of all decals.
        // TODO(q3k): fix this
        if (ctx_->allUiReload) {
            ctx_->allUiReload = false;
            decalsChanged = true;
        }
        if (ctx_->frameUiReload) {
            ctx_->frameUiReload = false;
            decalsChanged = true;
        }
        if (ctx_->belUiReload.size() > 0) {
            ctx_->belUiReload.clear();
            decalsChanged = true;
        }
        if (ctx_->wireUiReload.size() > 0) {
            ctx_->wireUiReload.clear();
            decalsChanged = true;
        }
        if (ctx_->pipUiReload.size() > 0) {
            ctx_->pipUiReload.clear();
            decalsChanged = true;
        }
        if (ctx_->groupUiReload.size() > 0) {
            ctx_->groupUiReload.clear();
            decalsChanged = true;
        }

        // Local copy of decals, taken as fast as possible to not block the P&R.
        if (decalsChanged) {
            for (auto bel : ctx_->getBels()) {
                belDecals.push_back(ctx_->getBelDecal(bel));
            }
            for (auto wire : ctx_->getWires()) {
                wireDecals.push_back(ctx_->getWireDecal(wire));
            }
            for (auto pip : ctx_->getPips()) {
                pipDecals.push_back(ctx_->getPipDecal(pip));
            }
            for (auto group : ctx_->getGroups()) {
                groupDecals.push_back(ctx_->getGroupDecal(group));
            }
        }
    }

    // Arguments from the main UI thread on what we should render.
    std::vector<DecalXY> selectedDecals;
    std::vector<DecalXY> highlightedDecals[8];
    bool highlightedOrSelectedChanged;
    {
        // Take the renderer arguments lock, copy over all we need.
        QMutexLocker lock(&rendererArgsLock_);
        selectedDecals = rendererArgs_->selectedDecals;
        for (int i = 0; i < 8; i++)
            highlightedDecals[i] = rendererArgs_->highlightedDecals[i];
        highlightedOrSelectedChanged = rendererArgs_->highlightedOrSelectedChanged;
        rendererArgs_->highlightedOrSelectedChanged = false;
    }

    // Render decals if necessary.
    if (decalsChanged) {
        auto data = std::unique_ptr<FPGAViewWidget::RendererData>(new FPGAViewWidget::RendererData);
        // Draw Bels.
        for (auto const &decal : belDecals) {
            drawArchDecal(data->gfxByStyle, decal);
        }
        // Draw Wires.
        for (auto const &decal : wireDecals) {
            drawArchDecal(data->gfxByStyle, decal);
        }
        // Draw Pips.
        for (auto const &decal : pipDecals) {
            drawArchDecal(data->gfxByStyle, decal);
        }
        // Draw Groups.
        for (auto const &decal : groupDecals) {
            drawArchDecal(data->gfxByStyle, decal);
        }

        // Swap over.
        {
            QMutexLocker lock(&rendererDataLock_);

            // If we're not re-rendering any highlights/selections, let's
            // copy them over from teh current object.
            if (!highlightedOrSelectedChanged) {
                data->gfxSelected = rendererData_->gfxSelected;
                for (int i = 0; i < 8; i++)
                    data->gfxHighlighted[i] = rendererData_->gfxHighlighted[i];
            }

            rendererData_ = std::move(data);
        }
    }

    if (highlightedOrSelectedChanged) {
        QMutexLocker locker(&rendererDataLock_);

        // Render selected.
        rendererData_->gfxSelected.clear();
        for (auto &decal : selectedDecals) {
            drawDecal(rendererData_->gfxSelected, decal);
        }

        // Render highlighted.
        for (int i = 0; i < 8; i++) {
            rendererData_->gfxHighlighted[i].clear();
            for (auto &decal : highlightedDecals[i]) {
                drawDecal(rendererData_->gfxHighlighted[i], decal);
            }
        }
    }
}

void FPGAViewWidget::onSelectedArchItem(std::vector<DecalXY> decals)
{
    {
        QMutexLocker locker(&rendererArgsLock_);
        rendererArgs_->selectedDecals = decals;
        rendererArgs_->highlightedOrSelectedChanged = true;
    }
    pokeRenderer();
}

void FPGAViewWidget::onHighlightGroupChanged(std::vector<DecalXY> decals, int group)
{
    {
        QMutexLocker locker(&rendererArgsLock_);
        rendererArgs_->highlightedDecals[group] = decals;
        rendererArgs_->highlightedOrSelectedChanged = true;
    }
    pokeRenderer();
}

void FPGAViewWidget::resizeGL(int width, int height) {}

void FPGAViewWidget::mousePressEvent(QMouseEvent *event) { lastDragPos_ = event->pos(); }

// Invert the projection matrix to calculate screen/mouse to world/grid
// coordinates.
QVector4D FPGAViewWidget::mouseToWorldCoordinates(int x, int y)
{
    QMatrix4x4 p = getProjection();
    QVector2D unit = p.map(QVector4D(1, 1, 0, 1)).toVector2DAffine();

    float sx = (((float)x) / (width() / 2));
    float sy = (((float)y) / (height() / 2));
    return QVector4D(sx / unit.x(), sy / unit.y(), 0, 1);
}

void FPGAViewWidget::mouseMoveEvent(QMouseEvent *event)
{
    const int dx = event->x() - lastDragPos_.x();
    const int dy = event->y() - lastDragPos_.y();
    lastDragPos_ = event->pos();

    auto world = mouseToWorldCoordinates(dx, dy);
    viewMove_.translate(world.x(), -world.y());

    update();
}

void FPGAViewWidget::wheelEvent(QWheelEvent *event)
{
    QPoint degree = event->angleDelta() / 8;

    if (!degree.isNull())
        zoom(degree.y());
}

void FPGAViewWidget::zoom(int level)
{
    if (zoom_ < zoomNear_) {
        zoom_ = zoomNear_;
    } else if (zoom_ < zoomLvl1_) {
        zoom_ -= level / 10.0;
    } else if (zoom_ < zoomLvl2_) {
        zoom_ -= level / 5.0;
    } else if (zoom_ < zoomFar_) {
        zoom_ -= level;
    } else {
        zoom_ = zoomFar_;
    }
    update();
}

void FPGAViewWidget::zoomIn() { zoom(10); }

void FPGAViewWidget::zoomOut() { zoom(-10); }

void FPGAViewWidget::zoomSelected() {}

void FPGAViewWidget::zoomOutbound() {}

NEXTPNR_NAMESPACE_END
