#pragma once

#include <QWidget>
#include <vector>

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
class ResultDataset;
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
    QPointF lastRubberFrom_;
    QPointF lastRubberTo_;
};
