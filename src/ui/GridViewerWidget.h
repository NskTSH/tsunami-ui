#pragma once

#include <QWidget>
#include <QFuture>
#include <atomic>
#include <memory>
#include <vector>

#include "model/ResultDataset.h"

class QGraphicsView;
class QToolBar;
class QLabel;
class QCheckBox;
class QComboBox;
class QSplitter;
class QTreeWidget;
class QTreeWidgetItem;
class GridScene;
class GradientEditor;
class GridDataset;
class ParameterSet;
class TileProvider;
class AnimationPlayer;
class ProfileTool;
class CoastHistogramTool;

class GridViewerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit GridViewerWidget(QWidget* parent = nullptr);
    ~GridViewerWidget() override;

    // Stops every background task that reads the ResultDataset and waits for
    // them to unwind. Must be called before whatever owns that dataset goes
    // away, otherwise a worker keeps reading a destroyed object.
    void cancelBackgroundWork();

    // Drops the histogram's scale authority and re-arms the scan against the
    // result set now loaded. Call after replacing what results_ points at, or
    // after loading new frames into it.
    void onResultSourceChanged();

    // Pushes the first indexed frame into the overlay and the analysis tools, so
    // they show the result set that is actually loaded rather than the previous
    // one. Returns false when nothing is indexed.
    bool showFirstResultFrame();

    void setGridDataset(GridDataset* grid, const QString& filename = {});
    void setResultDataset(ResultDataset* results);
    void setParameterSet(ParameterSet* params, const QString& filename = {});
    void setTileProvider(TileProvider* provider);

    GridScene* scene() const { return scene_; }
    GradientEditor* gradientEditor() const { return gradient_; }
    AnimationPlayer* animationPlayer() const { return animPlayer_; }
    ProfileTool* profileTool() const { return profileTool_; }
    CoastHistogramTool* coastTool() const { return coastTool_; }
    // Layer tree is hosted by MainWindow in a dock; expose it for re-parenting.
    QTreeWidget* layerTreeWidget() const { return layerTree_; }

    // Show overlay frame (from animation)
    void setOverlayData(const std::vector<double>& data, int rows, int cols,
                        double minVal, double maxVal);
    void clearOverlay();
    void clearResults();

    // Dynamic layer tree management
    void addBathymetryLayer(const QString& filename);
    void removeBathymetryLayer();
    void addResultLayer(const QString& filename);
    void removeResultLayer();
    void addParameterLayer(const QString& filename);
    void removeParameterLayer();

signals:
    void cellHovered(int row, int col, double value);
    void gridClearRequested();
    void resultsClearRequested();

private:
    void setupUI();
    void setupToolbar();
    void setupLayerTree();
    void updateStatusLabel(QPointF scenePos);
    void onFrameChanged(int timestep);
    void onLayerTreeContextMenu(QPoint pos);
    void onLayerItemChanged(QTreeWidgetItem* item, int column);
    void onLayerItemDoubleClicked(QTreeWidgetItem* item, int column);

    void clearSelection();
    void startRegionScan(int rowMin, int rowMax, int colMin, int colMax);
    void invalidateResultEpoch();

    QIcon colorIcon(const QColor& c) const;

    QGraphicsView* view_ = nullptr;
    GridScene* scene_ = nullptr;
    GradientEditor* gradient_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QLabel* coordLabel_ = nullptr;
    QComboBox* presetCombo_ = nullptr;
    QTreeWidget* layerTree_ = nullptr;

    // Category items (always present)
    QTreeWidgetItem* catBathymetry_ = nullptr;
    QTreeWidgetItem* catResults_ = nullptr;
    QTreeWidgetItem* catParameters_ = nullptr;

    // Dynamic items (created on load, removed on clear)
    QTreeWidgetItem* bathItem_ = nullptr;      // e.g. "JapanSea.asc"
    QTreeWidgetItem* bathIsoItem_ = nullptr;
    QTreeWidgetItem* bathIsoAutoItem_ = nullptr;

    QTreeWidgetItem* resultItem_ = nullptr;    // e.g. "eta_timestep_100/"
    QTreeWidgetItem* resultIsoItem_ = nullptr;
    QTreeWidgetItem* resultIsoAutoItem_ = nullptr;

    QTreeWidgetItem* paramItem_ = nullptr;     // e.g. "S7.par"
    QTreeWidgetItem* sourcesItem_ = nullptr;
    QTreeWidgetItem* maskItem_ = nullptr;
    QTreeWidgetItem* subgridItem_ = nullptr;

    // Embedded results tools
    AnimationPlayer* animPlayer_ = nullptr;
    ProfileTool* profileTool_ = nullptr;
    CoastHistogramTool* coastTool_ = nullptr;
    ResultDataset* results_ = nullptr;
    GridDataset* grid_ = nullptr;

    // Rubber-band endpoints in scene coordinates. The flag is the only valid
    // "unset" marker: (0,0) is the grid origin, a perfectly legal endpoint.
    bool hasRubberBand_ = false;
    QPointF lastRubberFrom_;
    QPointF lastRubberTo_;

    // Last selected region, kept so the scan can be re-armed when the result
    // set underneath it is replaced.
    bool hasSelection_ = false;
    int selRowMin_ = 0, selRowMax_ = 0, selColMin_ = 0, selColMax_ = 0;

    // Background scan of every frame over the selected region. The flag is
    // polled by the worker so a cancel does not have to wait out the whole scan.
    QFuture<double> regionScan_;
    std::shared_ptr<std::atomic_bool> regionScanCancelled_;

    // The single-file load also reads the dataset off the GUI thread, so it is
    // tracked here too; nothing else keeps that dataset alive for it.
    QFuture<std::shared_ptr<FrameData>> frameLoad_;

    // Joining frameLoad_ waits for the parse but cannot un-queue the GUI
    // continuation Qt has already scheduled. The continuation compares this
    // epoch instead, so a frame from a result set we have since moved off is
    // dropped rather than painted over the current one.
    quint64 resultEpoch_ = 0;
};
