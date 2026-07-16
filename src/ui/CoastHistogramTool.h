#pragma once

#include <QWidget>
#include <vector>
#include <QCheckBox>

class QLabel;
class GridDataset;

// Displays a bar chart of max wave heights along a coastline region
class CoastHistogramTool : public QWidget
{
    Q_OBJECT

public:
    explicit CoastHistogramTool(QWidget* parent = nullptr);

    void setGridDataset(GridDataset* grid);
    void setMinDepth(double d);

    // Set the rectangular region (in grid coordinates) and eta_max data
    void setRegion(int rowMin, int rowMax, int colMin, int colMax);
    void setEtaMaxData(const std::vector<double>& etaMax, int rows, int cols);
    void updateEtaMaxData();

    void setGlobalMaxEta(double maxEta, int selectionId);
    int currentSelectionId() const;

    // Forgets a scale measured over a result set that is no longer loaded, and
    // invalidates any scan still reporting against it.
    void resetScaleAuthority();

    void clearRegion();
    void clearEtaMaxData();
    bool hasRegion() const;

    void recompute();

signals:
    void regionSelected(int rowMin, int rowMax, int colMin, int colMax);
    void coastlineCellsCalculated(const QVector<QPointF>& cells);
    void showCoastlineChanged(bool visible);
    void coastlineLabelsReady(const QMap<int, QPointF>& labels);

private slots:
    void onShowCoastlineToggled(bool state);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct CoastNode {
        int row, col;
        double etaMax;
        int componentId = -1;
        bool isSeparator = false;
    };

    std::vector<CoastNode> findCoastNodes();
    std::vector<CoastNode> orderCoastNodes(const std::vector<CoastNode> &nodes);

    int droppedComponentCount_ = 0;
    double globalMaxEta_ = 0;
    int selectionId_ = 0;

    // Set once the background scan over every frame has reported a max for the
    // current selection. Until then the scale tracks the frames seen so far and
    // only ever grows, so bars never rescale downwards mid-animation.
    bool globalMaxFromScan_ = false;

    // True once a coast pass has run against the samples currently held. Dropped
    // by setEtaMaxData when the sample shape changes and by setMinDepth, the two
    // things that can change what the pass would find. Without it the region is
    // rescanned on every animation frame.
    bool coastNodesComputed_ = false;

    GridDataset* grid_ = nullptr;
    std::vector<double> etaMaxData_;
    int etaRows_ = 0;
    int etaCols_ = 0;
    double minDepth_ = 7.0;

    bool hasRegion_ = false;
    int regionRowMin_ = 0, regionRowMax_ = 0;
    int regionColMin_ = 0, regionColMax_ = 0;

    std::vector<CoastNode> coastNodes_;
    QLabel* infoLabel_ = nullptr;

    QCheckBox* showCoastlineCheckBox_ = nullptr;
};
