#include "GridScene.h"
#include "GradientEditor.h"
#include "model/GridDataset.h"
#include "model/ParameterSet.h"
#include "geo/TileProvider.h"
#include "geo/Reprojection.h"

#include <QPainter>
#include <QGraphicsPixmapItem>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsEllipseItem>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsLineItem>
#include <QPen>
#include <QFont>
#include <QString>
#include <cmath>

GridScene::GridScene(QObject* parent)
    : QGraphicsScene(parent)
{
    setBackgroundBrush(QColor(200, 220, 240)); // light blue while tiles load
}

void GridScene::setGridDataset(GridDataset* grid)
{
    grid_ = grid;
    if (grid_ && grid_->isLoaded())
        rebuildRaster();
}

void GridScene::setGradient(GradientEditor* gradient)
{
    gradient_ = gradient;
}

void GridScene::setTileProvider(TileProvider* provider)
{
    if (tileProvider_)
        disconnect(tileProvider_, nullptr, this, nullptr);
    tileProvider_ = provider;
    if (tileProvider_) {
        connect(tileProvider_, &TileProvider::tileReady,
                this, &GridScene::onTileReady);
    }
}

void GridScene::setParameterSet(ParameterSet* params)
{
    params_ = params;
}

void GridScene::rebuildRaster()
{
    QRectF savedSelectionRect;
    if (selectionRectItem_) {
        savedSelectionRect = selectionRectItem_->rect();
    }

    for (auto* item : coastlineCells_) {
        removeItem(item);
    }

    for (auto* item : coastlineLabelItems_) {
        removeItem(item);
    }

    clear();
    mapTileItems_.clear();
    selectionRectItem_ = nullptr;

    for (auto* item : coastlineCells_) {
        addItem(item);
    }

    for (auto* item : coastlineLabelItems_) {
        addItem(item);
    }

    if (!gradient_) return;

    bool hasGrid = grid_ && grid_->isLoaded();

    // Determine scene rect from grid or overlay
    if (hasGrid) {
        setSceneRect(0, 0, grid_->cols(), grid_->rows());
    } else if (hasOverlay_ && overlayRows_ > 0 && overlayCols_ > 0) {
        setSceneRect(0, 0, overlayCols_, overlayRows_);
    } else {
        return;
    }

    // Layer 0: Request map tiles (non-blocking — tiles arrive via onTileReady)
    if (hasGrid && grid_->isGeoreferenced() && tileProvider_)
        requestMapTiles();

    // Layer 1: Bathymetry — only water cells (toggleable)
    if (hasGrid && showBathymetry_)
        renderBathymetryTiles();

    // Layer 5: Overlay (eta animation frame)
    if (hasOverlay_)
        renderOverlay();

    // Layer 10: Computation mask
    if (hasGrid && showMask_)
        renderComputationMask();

    // Layer 20: Subgrid decomposition lines
    if (hasGrid && showSubgrids_ && params_)
        renderSubgridLines();

    // Layer 25: Bathymetry isolines
    if (showBathIso_ && hasGrid)
        renderIsolines(bathIsoLevels_, bathIsoColor_, false, 25);

    // Layer 26: Overlay isolines
    if (showOverlayIso_ && hasOverlay_)
        renderIsolines(overlayIsoLevels_, overlayIsoColor_, true, 26);

    // Layer 30: Source ellipses
    if (showSources_ && params_)
        renderSourceEllipses();

    if (!savedSelectionRect.isNull() &&
        savedSelectionRect.width() > 0 &&
        savedSelectionRect.height() > 0)
    {
        setSelectedRegion(savedSelectionRect);
    }
}

void GridScene::renderBathymetryTiles()
{
    if (!grid_ || !gradient_) return;

    int rows = grid_->rows();
    int cols = grid_->cols();
    double minV = grid_->minValue();
    double maxV = grid_->maxValue();
    double nodata = grid_->nodataValue();
    bool isGeo = grid_->isGeoreferenced();

    for (int ty = 0; ty < rows; ty += TILE_SIZE) {
        for (int tx = 0; tx < cols; tx += TILE_SIZE) {
            int tw = std::min(TILE_SIZE, cols - tx);
            int th = std::min(TILE_SIZE, rows - ty);

            QImage tile(tw, th, QImage::Format_ARGB32);
            tile.fill(Qt::transparent);

            for (int r = 0; r < th; ++r) {
                auto* line = reinterpret_cast<QRgb*>(tile.scanLine(r));
                for (int c = 0; c < tw; ++c) {
                    double v = grid_->value(ty + r, tx + c);
                    if (isGeo && v == nodata) continue;
                    if (v <= 0.0) continue;

                    QColor color = gradient_->mapValue(v, minV, maxV);
                    line[c] = color.rgba();
                }
            }

            auto* item = addPixmap(QPixmap::fromImage(tile));
            item->setPos(tx, ty);
            item->setZValue(1);
        }
    }
}

void GridScene::requestMapTiles()
{
    if (!tileProvider_ || !grid_ || !grid_->isGeoreferenced())
        return;

    double lonMin = grid_->worldX(0);
    double lonMax = grid_->worldX(grid_->cols() - 1);
    double latMin = grid_->worldY(grid_->rows() - 1);
    double latMax = grid_->worldY(0);

    double degreesPerPixel = grid_->cellSize();
    int zoom = static_cast<int>(std::log2(360.0 / (degreesPerPixel * 256.0)));
    zoom = std::clamp(zoom, 1, 18);
    currentZoom_ = zoom;

    auto [txMin, tyMin] = Reprojection::lonLatToTile(lonMin, latMax, zoom);
    auto [txMax, tyMax] = Reprojection::lonLatToTile(lonMax, latMin, zoom);

    for (int tileY = tyMin; tileY <= tyMax; ++tileY) {
        for (int tileX = txMin; tileX <= txMax; ++tileX) {
            QPixmap pix = tileProvider_->requestTile(zoom, tileX, tileY);
            if (!pix.isNull()) {
                // Tile was cached — place immediately
                placeMapTile(zoom, tileX, tileY, pix);
            }
            // Otherwise: tile download started, onTileReady will be called later
        }
    }
}

void GridScene::placeMapTile(int zoom, int tx, int ty, const QPixmap& pixmap)
{
    if (!grid_ || !grid_->isGeoreferenced()) return;

    TileKey key{zoom, tx, ty};
    if (mapTileItems_.contains(key)) return;

    double lonMin = grid_->worldX(0);
    double latMax = grid_->worldY(0);
    double cs = grid_->cellSize();

    auto [tLon, tLat] = Reprojection::tileToLonLat(tx, ty, zoom);
    auto [tLon2, tLat2] = Reprojection::tileToLonLat(tx + 1, ty + 1, zoom);

    double gxMin = (tLon - lonMin) / cs;
    double gyMin = (latMax - tLat) / cs;
    double gxMax = (tLon2 - lonMin) / cs;
    double gyMax = (latMax - tLat2) / cs;

    // +1 pixel overlap to eliminate seam lines between tiles
    int w = std::max(1, static_cast<int>(std::ceil(gxMax - gxMin))) + 1;
    int h = std::max(1, static_cast<int>(std::ceil(gyMax - gyMin))) + 1;

    QPixmap scaled = pixmap.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    auto* item = addPixmap(scaled);
    item->setPos(gxMin, gyMin);
    item->setZValue(0);

    mapTileItems_[key] = item;
}

void GridScene::onTileReady(int zoom, int tx, int ty, const QPixmap& pixmap)
{
    // A tile just downloaded — add it incrementally, no full rebuild
    if (zoom == currentZoom_)
        placeMapTile(zoom, tx, ty, pixmap);
}

void GridScene::renderComputationMask()
{
    if (!grid_ || !params_) return;

    int rows = grid_->rows();
    int cols = grid_->cols();
    double minDepth = params_->minimumDepthToCalculate();

    QImage mask(cols, rows, QImage::Format_ARGB32);
    mask.fill(Qt::transparent);

    for (int r = 0; r < rows; ++r) {
        auto* line = reinterpret_cast<QRgb*>(mask.scanLine(r));
        for (int c = 0; c < cols; ++c) {
            double d = grid_->value(r, c);
            if (d <= 0.0) continue;
            if (d >= minDepth)
                line[c] = qRgba(0, 200, 0, 80);
            else
                line[c] = qRgba(200, 0, 0, 80);
        }
    }

    auto* item = addPixmap(QPixmap::fromImage(mask));
    item->setPos(0, 0);
    item->setZValue(10);
}

void GridScene::renderSubgridLines()
{
    if (!params_ || !grid_) return;

    int numSub = params_->numSubgrids();
    if (numSub <= 0) return;

    int rows = grid_->rows();
    int cols = grid_->cols();
    QPen pen(subgridColor_, 0);
    pen.setStyle(Qt::DashLine);

    int bandHeight = rows / numSub;
    for (int i = 1; i < numSub; ++i) {
        int y = i * bandHeight;
        auto* line = addLine(0, y, cols, y, pen);
        line->setZValue(20);
    }
}

void GridScene::renderSourceEllipses()
{
    if (!params_) return;

    const auto& sources = params_->sources();
    QPen pen(Qt::red, 0);
    pen.setWidth(2);

    bool geo = grid_ && grid_->isGeoreferenced();

    for (const auto& s : sources) {
        if (s.type == 0) {
            double cx = s.centerX;
            double cy = s.centerY;
            double rx = s.radius1;
            double ry = s.radius2;

            if (geo) {
                // Source coords are in world (lon/lat), convert to grid pixels
                double cs = grid_->cellSize();
                cx = (s.centerX - grid_->xllCorner()) / cs;
                cy = (grid_->yllCorner() + grid_->rows() * cs - s.centerY) / cs;
                rx = s.radius1 / cs;
                ry = s.radius2 / cs;
            }

            auto* ellipse = addEllipse(
                cx - rx, cy - ry,
                rx * 2, ry * 2,
                pen, QBrush(QColor(255, 0, 0, 40)));
            ellipse->setTransformOriginPoint(cx, cy);
            ellipse->setRotation(-s.angle);
            ellipse->setZValue(30);
        }
    }
}

void GridScene::setOverlayData(const std::vector<double>& data, int rows, int cols,
                               double minVal, double maxVal)
{
    if (data.empty()) return;

    overlayData_ = data;
    overlayRows_ = rows;
    overlayCols_ = cols;
    overlayMin_ = minVal;
    overlayMax_ = maxVal;
    hasOverlay_ = true;
}

void GridScene::clearOverlay()
{
    hasOverlay_ = false;
    overlayImage_ = QImage();
    overlayData_.clear();
    overlayRows_ = 0;
    overlayCols_ = 0;
}

double GridScene::overlayValueAt(int row, int col) const
{
    if (!hasOverlay_ || row < 0 || row >= overlayRows_ || col < 0 || col >= overlayCols_)
        return 0.0;
    return overlayData_[row * overlayCols_ + col];
}

void GridScene::renderOverlay()
{
    if (!hasOverlay_ || overlayData_.empty() || !gradient_) return;

    // Rebuild overlay image from raw data using current gradient
    int alpha = showBathymetry_ ? 200 : 255;
    overlayImage_ = QImage(overlayCols_, overlayRows_, QImage::Format_ARGB32);
    overlayImage_.fill(Qt::transparent);

    for (int r = 0; r < overlayRows_; ++r) {
        auto* line = reinterpret_cast<QRgb*>(overlayImage_.scanLine(r));
        for (int c = 0; c < overlayCols_; ++c) {
            double v = overlayData_[r * overlayCols_ + c];
            if (std::abs(v) < 1e-6) continue;
            QColor color = gradient_->mapValue(v, overlayMin_, overlayMax_);
            color.setAlpha(alpha);
            line[c] = color.rgba();
        }
    }

    auto* item = addPixmap(QPixmap::fromImage(overlayImage_));
    item->setPos(0, 0);
    item->setZValue(5);
}

double GridScene::valueAt(int row, int col) const
{
    if (grid_)
        return grid_->value(row, col);
    return 0.0;
}

void GridScene::renderIsolines(const std::vector<double>& levels, const QColor& color,
                               bool useOverlay, double zValue)
{
    if (levels.empty()) return;
    if (useOverlay && overlayData_.empty()) return;
    if (!useOverlay && !grid_) return;

    int rows = useOverlay ? overlayRows_ : grid_->rows();
    int cols = useOverlay ? overlayCols_ : grid_->cols();

    QPen pen(color, 0);
    pen.setCosmetic(true);
    pen.setWidthF(1.0);

    QFont labelFont;
    labelFont.setPixelSize(8);

    auto val = [&](int r, int c) -> double {
        if (useOverlay)
            return overlayData_[r * cols + c];
        return grid_->value(r, c);
    };

    for (double level : levels) {
        int segCount = 0;
        for (int r = 0; r < rows - 1; ++r) {
            for (int c = 0; c < cols - 1; ++c) {
                double v00 = val(r, c);
                double v10 = val(r, c + 1);
                double v01 = val(r + 1, c);
                double v11 = val(r + 1, c + 1);

                int code = 0;
                if (v00 >= level) code |= 1;
                if (v10 >= level) code |= 2;
                if (v11 >= level) code |= 4;
                if (v01 >= level) code |= 8;

                if (code == 0 || code == 15) continue;

                auto lerp = [](double a, double b, double lev) -> double {
                    if (std::abs(b - a) < 1e-12) return 0.5;
                    return (lev - a) / (b - a);
                };

                double tTop = lerp(v00, v10, level);
                double tRight = lerp(v10, v11, level);
                double tBottom = lerp(v01, v11, level);
                double tLeft = lerp(v00, v01, level);

                QPointF pTop(c + tTop, r);
                QPointF pRight(c + 1, r + tRight);
                QPointF pBottom(c + tBottom, r + 1);
                QPointF pLeft(c, r + tLeft);

                auto drawSeg = [&](QPointF a, QPointF b) {
                    auto* line = addLine(a.x(), a.y(), b.x(), b.y(), pen);
                    line->setZValue(zValue);
                    ++segCount;

                    if (isolineLabelInterval_ > 0
                        && segCount % isolineLabelInterval_ == 0) {
                        QPointF mid = (a + b) / 2.0;
                        double dx = b.x() - a.x();
                        double dy = b.y() - a.y();
                        double angleDeg = std::atan2(dy, dx) * 180.0 / M_PI;
                        // Keep label readable (not upside-down)
                        if (angleDeg > 90) angleDeg -= 180;
                        if (angleDeg < -90) angleDeg += 180;

                        auto* label = addSimpleText(
                            QString::number(level, 'g', 4), labelFont);
                        label->setPos(mid);
                        label->setRotation(angleDeg);
                        label->setBrush(color);
                        label->setZValue(zValue + 0.5);
                    }
                };

                switch (code) {
                    case 1: case 14: drawSeg(pTop, pLeft); break;
                    case 2: case 13: drawSeg(pTop, pRight); break;
                    case 3: case 12: drawSeg(pLeft, pRight); break;
                    case 4: case 11: drawSeg(pRight, pBottom); break;
                    case 5: drawSeg(pTop, pRight); drawSeg(pLeft, pBottom); break;
                    case 6: case 9: drawSeg(pTop, pBottom); break;
                    case 7: case 8: drawSeg(pLeft, pBottom); break;
                    case 10: drawSeg(pTop, pLeft); drawSeg(pRight, pBottom); break;
                }
            }
        }
    }
}

void GridScene::setBathIsolineLevels(const std::vector<double>& levels)
{
    bathIsoLevels_ = levels;
    rebuildRaster();
}

void GridScene::setOverlayIsolineLevels(const std::vector<double>& levels)
{
    overlayIsoLevels_ = levels;
    rebuildRaster();
}

void GridScene::autoComputeBathIsolineLevels(int count)
{
    if (count <= 0 || !grid_ || !grid_->isLoaded()) return;

    double lo = grid_->minValue();
    double hi = grid_->maxValue();
    if (lo < 0) lo = 0;
    if (hi <= lo) return;

    bathIsoLevels_.clear();
    double step = (hi - lo) / (count + 1);
    for (int i = 1; i <= count; ++i)
        bathIsoLevels_.push_back(lo + i * step);

    rebuildRaster();
}

void GridScene::autoComputeOverlayIsolineLevels(int count)
{
    if (count <= 0 || !hasOverlay_ || overlayData_.empty()) return;

    double lo = overlayMin_;
    double hi = overlayMax_;
    if (hi <= lo) return;

    overlayIsoLevels_.clear();
    double step = (hi - lo) / (count + 1);
    for (int i = 1; i <= count; ++i)
        overlayIsoLevels_.push_back(lo + i * step);

    rebuildRaster();
}

void GridScene::setShowBathymetry(bool show)
{
    showBathymetry_ = show;
    rebuildRaster();
}
void GridScene::setShowComputationMask(bool show) { showMask_ = show; rebuildRaster(); }
void GridScene::setShowSubgridLines(bool show) { showSubgrids_ = show; rebuildRaster(); }
void GridScene::setShowSources(bool show) { showSources_ = show; rebuildRaster(); }
void GridScene::setShowBathIsolines(bool show) { showBathIso_ = show; rebuildRaster(); }
void GridScene::setShowOverlayIsolines(bool show) { showOverlayIso_ = show; rebuildRaster(); }

void GridScene::setSelectedRegion(const QRectF &rect) {
    if (selectionRectItem_) {
        removeItem(selectionRectItem_);
        delete selectionRectItem_;
        selectionRectItem_ = nullptr;
    }

    QPen pen(Qt::red, 2, Qt::SolidLine);
    selectionRectItem_ = addRect(rect, pen);

    const int Z_VALUE_FOR_SELECTION = 100;
    selectionRectItem_->setZValue(Z_VALUE_FOR_SELECTION);

    update();
}

void GridScene::clearSelectionRegion() {
    if (selectionRectItem_) {
        removeItem(selectionRectItem_);
    }
    delete selectionRectItem_;
    selectionRectItem_ = nullptr;
    update();
}

bool GridScene::hasSelectedRegion() const { return selectionRectItem_ != nullptr; }

QRectF GridScene::selectionRegion() const {
    if (selectionRectItem_) {
        return selectionRectItem_->rect();
    }
    return QRectF();
}

void GridScene::setCoastlineCells(const QVector<QPointF> &cells) {
    for (auto* item : coastlineCells_) {
        removeItem(item);
        delete item;
    }
    coastlineCells_.clear();

    if (cells.isEmpty()) {
        return;
    }

    QBrush brush(Qt::red);
    QPen pen(Qt::red, 1, Qt::SolidLine);
    pen.setCosmetic(true);

    for (const QPointF& cell : cells) {
        const qreal CELL_SIDE_SIZE = 1.0;
        QGraphicsRectItem* rect = addRect(cell.x(), cell.y(), CELL_SIDE_SIZE, CELL_SIDE_SIZE, pen, brush);

        const int Z_VALUE_FOR_COASTLINE = 99;
        rect->setZValue(Z_VALUE_FOR_COASTLINE);

        rect->setVisible(coastlineVisible_);

        coastlineCells_.append(rect);
    }

    update();
}

void GridScene::setCoastlineVisible(bool visible) {
    if (visible == coastlineVisible_) {
        return;
    }

    coastlineVisible_ = visible;

    for (auto* item : coastlineCells_) {
        item->setVisible(coastlineVisible_);
    }

    for (auto* item : coastlineLabelItems_) {
        item->setVisible(coastlineVisible_);
    }
}

void GridScene::setCoastlineLabels(const QMap<int, QPointF>& labels)
{
    for (auto* item : coastlineLabelItems_) {
        removeItem(item);
        delete item;
    }
    coastlineLabelItems_.clear();
    coastlineLabels_ = labels;

    for (auto it = labels.begin(); it != labels.end(); ++it) {
        int id = it.key();
        QPointF pos = it.value();

        QGraphicsSimpleTextItem* textItem = new QGraphicsSimpleTextItem(QString::number(id));

        textItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        textItem->setPos(pos.x(), pos.y());
        textItem->setBrush(Qt::white);
        textItem->setFont(QFont("Arial", 10, QFont::Bold));

        QGraphicsDropShadowEffect* shadow = new QGraphicsDropShadowEffect;
        shadow->setBlurRadius(3);
        shadow->setColor(Qt::black);
        shadow->setOffset(1, 1);

        textItem->setGraphicsEffect(shadow);
        textItem->setVisible(coastlineVisible_);

        const int Z_VALUE_FOR_COASTLINE_LABELS = 101;
        textItem->setZValue(Z_VALUE_FOR_COASTLINE_LABELS);

        addItem(textItem);
        coastlineLabelItems_.append(textItem);
    }
}
