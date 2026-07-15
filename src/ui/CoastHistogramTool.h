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
    void setMinDepth(double d) { minDepth_ = d; }

    // Set the rectangular region (in grid coordinates) and eta_max data
    void setRegion(int rowMin, int rowMax, int colMin, int colMax);
    void setEtaMaxData(const std::vector<double>& etaMax, int rows, int cols);
    void updateEtaMaxData();

    void setGlobalMaxEta(double maxEta, int selectionId);
    int currentSelectionId() const;

    void clearRegion();
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
