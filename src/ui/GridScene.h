#pragma once

#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QImage>
#include <QPixmap>
#include <QMap>
#include <vector>

class GridDataset;
class GradientEditor;
class TileProvider;
class ParameterSet;

// QGraphicsScene with tiled raster rendering of bathymetry
class GridScene : public QGraphicsScene
{
    Q_OBJECT

public:
    explicit GridScene(QObject* parent = nullptr);

    void setGridDataset(GridDataset* grid);
    void setGradient(GradientEditor* gradient);
    void setTileProvider(TileProvider* provider);
    void setParameterSet(ParameterSet* params);

    // Selection region
    void setSelectedRegion(const QRectF &rect);
    void clearSelectionRegion();
    bool hasSelectedRegion() const;
    QRectF selectionRegion() const;
    void setCoastlineCells(const QVector<QPointF>& cells);
    void setCoastlineVisible(bool visible);
    void setCoastlineLabels(const QMap<int, QPointF>& labels);

    // Rebuild the raster image from grid data + gradient
    void rebuildRaster();

    // Overlay toggles
    void setShowBathymetry(bool show);
    void setShowComputationMask(bool show);
    void setShowSubgridLines(bool show);
    void setShowSources(bool show);
    void setShowBathIsolines(bool show);
    void setShowOverlayIsolines(bool show);

    // Isoline levels — separate sets for bathymetry and overlay
    void setBathIsolineLevels(const std::vector<double>& levels);
    void setOverlayIsolineLevels(const std::vector<double>& levels);
    void setBathIsolineColor(const QColor& color) { bathIsoColor_ = color; rebuildRaster(); }
    void setOverlayIsolineColor(const QColor& color) { overlayIsoColor_ = color; rebuildRaster(); }

    void autoComputeBathIsolineLevels(int count);
    void autoComputeOverlayIsolineLevels(int count);

    // Subgrid line color
    void setSubgridColor(const QColor& color) { subgridColor_ = color; if (showSubgrids_) rebuildRaster(); }

    // Set overlay on grid (e.g., eta frame from animation)
    void setOverlayData(const std::vector<double>& data, int rows, int cols,
                        double minVal, double maxVal);
    void clearOverlay();

    // Get value at grid position (bathymetry)
    double valueAt(int row, int col) const;
    // Get overlay value at grid position (wave height etc.)
    double overlayValueAt(int row, int col) const;
    bool hasOverlayData() const { return hasOverlay_; }

public slots:
    void onTileReady(int zoom, int tx, int ty, const QPixmap& pixmap);

private:
    void renderBathymetryTiles();
    void requestMapTiles();
    void placeMapTile(int zoom, int tx, int ty, const QPixmap& pixmap);
    void renderComputationMask();
    void renderSubgridLines();
    void renderSourceEllipses();
    void renderOverlay();
    void renderIsolines(const std::vector<double>& levels, const QColor& color,
                        bool useOverlay, double zValue);

    // Tile key for map tile tracking
    struct TileKey {
        int zoom, tx, ty;
        bool operator<(const TileKey& o) const {
            if (zoom != o.zoom) return zoom < o.zoom;
            if (tx != o.tx) return tx < o.tx;
            return ty < o.ty;
        }
    };

    GridDataset* grid_ = nullptr;
    GradientEditor* gradient_ = nullptr;
    TileProvider* tileProvider_ = nullptr;
    ParameterSet* params_ = nullptr;
    QGraphicsRectItem* selectionRectItem_ = nullptr;

    QVector<QGraphicsRectItem*> coastlineCells_;
    bool coastlineVisible_ = true;
    QMap<int, QPointF> coastlineLabels_;
    QVector<QGraphicsSimpleTextItem*> coastlineLabelItems_;

    QImage overlayImage_;
    bool hasOverlay_ = false;
    std::vector<double> overlayData_;
    int overlayRows_ = 0;
    int overlayCols_ = 0;
    double overlayMin_ = 0, overlayMax_ = 0;

    bool showBathymetry_ = true;
    bool showMask_ = false;
    bool showSubgrids_ = false;
    bool showSources_ = true;
    bool showBathIso_ = false;
    bool showOverlayIso_ = false;
    std::vector<double> bathIsoLevels_;
    std::vector<double> overlayIsoLevels_;
    QColor bathIsoColor_ = QColor(40, 40, 40);
    QColor overlayIsoColor_ = QColor(200, 0, 0);
    QColor subgridColor_ = QColor(255, 255, 0);
    int isolineLabelInterval_ = 50;

    // Map tile items already placed on scene (to avoid duplicates)
    QMap<TileKey, QGraphicsPixmapItem*> mapTileItems_;
    int currentZoom_ = 0;

    static constexpr int TILE_SIZE = 256;
};
