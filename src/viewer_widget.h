#pragma once

#include <QWidget>
#include <QPointF>
#include <QTransform>

#include "dwg_document.h"

// Minimal viewer widget: no toolbars, no editing, no undo — just
// "paint the shapes, let the user pan and zoom". This is the direct
// analogue of LibreCAD's RS_GraphicView + RS_Painter, reimplemented
// from scratch against QPainter so there's no dependency on
// lib/gui/render or the app-window/dialog-factory singletons those
// classes currently reach into.
class ViewerWidget : public QWidget {
    Q_OBJECT
public:
    explicit ViewerWidget(QWidget *parent = nullptr);

    // Takes ownership of the parsed document and fits the view to it.
    void setDocument(DwgDocument doc);

    void zoomFit();

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    DwgDocument document_;

    // Drawing-space -> screen-space transform. Rebuilt by zoomFit() and
    // adjusted in place by wheelEvent()/mouseMoveEvent() for zoom/pan.
    QTransform documentToScreen_;
    bool hasFitOnce_ = false;

    bool panning_ = false;
    QPoint lastMousePos_;
};
