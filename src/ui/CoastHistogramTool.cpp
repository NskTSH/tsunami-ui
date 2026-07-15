#include "CoastHistogramTool.h"
#include "model/GridDataset.h"

#include <QPainter>
#include <QVBoxLayout>
#include <QLabel>
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_set>
#include <unordered_map>

CoastHistogramTool::CoastHistogramTool(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);

    showCoastlineCheckBox_ = new QCheckBox(tr("Show coastline"), this);
    showCoastlineCheckBox_->setChecked(true);
    layout->addWidget(showCoastlineCheckBox_);
    connect(showCoastlineCheckBox_, &QCheckBox::toggled, this, &CoastHistogramTool::onShowCoastlineToggled);

    infoLabel_ = new QLabel(tr("Select a region on the grid to see coastline wave heights."));
    layout->addWidget(infoLabel_);
    layout->addStretch();
    setMinimumHeight(200);
}

void CoastHistogramTool::setGridDataset(GridDataset* grid)
{
    grid_ = grid;
}

void CoastHistogramTool::setRegion(int rowMin, int rowMax, int colMin, int colMax)
{
    selectionId_++;
    globalMaxEta_ = 0;
    droppedComponentCount_ = 0;
    regionRowMin_ = rowMin;
    regionRowMax_ = rowMax;
    regionColMin_ = colMin;
    regionColMax_ = colMax;
    hasRegion_ = true;
    recompute();
}

void CoastHistogramTool::clearRegion() {
    globalMaxEta_ = 0;
    hasRegion_ = false;
    droppedComponentCount_ = 0;
    recompute();
}

bool CoastHistogramTool::hasRegion() const {
    return hasRegion_;
}

void CoastHistogramTool::setEtaMaxData(const std::vector<double>& etaMax, int rows, int cols)
{
    etaMaxData_ = etaMax;
    etaRows_ = rows;
    etaCols_ = cols;
}

void CoastHistogramTool::updateEtaMaxData() {
    if (etaMaxData_.empty()) {
        return;
    }

    if (coastNodes_.empty() && hasRegion_ && grid_ && grid_->isLoaded()) {
        recompute();
        return;
    }

    if (coastNodes_.empty()) {
        return;
    }

    double maxEta = 0;
    for (auto& node : coastNodes_) {
        if (node.isSeparator) {
            continue;
        }

        if (node.row < 0 || node.row >= etaRows_ ||
            node.col < 0 || node.col >= etaCols_) {
            continue;
        }

        int idx = node.row * etaCols_ + node.col;
        if (idx >= 0 && idx < static_cast<int>(etaMaxData_.size())) {
            node.etaMax = etaMaxData_[idx];

            double absEta = std::abs(node.etaMax);
            if (absEta > maxEta) {
                maxEta = absEta;
            }
        }
    }

    if (globalMaxEta_ == 0) {
        globalMaxEta_ = maxEta;
    }

    update();
}

void CoastHistogramTool::recompute()
{
    coastNodes_ = findCoastNodes();

    QVector<QPointF> points;
    for (const auto& node : coastNodes_) {
        if (!node.isSeparator) {
            points.append(QPointF(node.col, node.row));
        }
    }
    emit coastlineCellsCalculated(points);

    QMap<int, QPointF> labels;
    int currentId = -1;
    for (const auto& node : coastNodes_) {
        if (node.componentId != currentId && node.componentId > 0) {
            labels[node.componentId] = QPointF(node.col, node.row);
            currentId = node.componentId;
        }
    }
    emit coastlineLabelsReady(labels);

    int realNodeCount = static_cast<int>(
        std::count_if(coastNodes_.begin(), coastNodes_.end(), [](const CoastNode& n) {
            return !n.isSeparator;
        }));

    if (droppedComponentCount_ > 0) {
        infoLabel_->setText(tr("Coast nodes: %1 (%2 tiny components filtered)")
                                .arg(realNodeCount)
                                .arg(droppedComponentCount_));
    } else {
        infoLabel_->setText(tr("Coast nodes found: %1").arg(realNodeCount));
    }

    update();
}

/*
 * orderCoastNodes orders coastline nodes along the coastline.
 *
 * Complex cases:
 * 1. Islands / Closed loops.
 * Full perimeter traversal via traverseComponent().
 *
 * 2. Breaks (multiple components).
 * The algorithm orders all found components separately.
 * This allows for outputting data for all found components, adding separators.
 *
 * 3. Coves.
 * Traversal from the farthest endpoint via greedy walk with backtracking.
 */
std::vector<CoastHistogramTool::CoastNode> CoastHistogramTool::orderCoastNodes(const std::vector<CoastNode>& nodes)
{
    droppedComponentCount_ = 0;

    std::vector<std::vector<int>> adj(nodes.size());

    std::unordered_map<int, std::unordered_map<int, int>> spatialIndex;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        spatialIndex[nodes[i].row][nodes[i].col] = i;
    }

    const int dr8[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    const int dc8[] = {-1, 0, 1, -1, 1, -1, 0, 1};

    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        int r = nodes[i].row;
        int c = nodes[i].col;

        for (int d = 0; d < 8; ++d) {
            int nr = r + dr8[d];
            int nc = c + dc8[d];

            auto rowIt = spatialIndex.find(nr);
            if (rowIt != spatialIndex.end()) {
                auto colIt = rowIt->second.find(nc);

                if (colIt != rowIt->second.end()) {
                    int j = colIt->second;

                    if (j > i) {
                        adj[i].push_back(j);
                        adj[j].push_back(i);
                    }
                }
            }
        }
    }

    // Finding all connected components
    std::vector<bool> visited(nodes.size(), false);
    std::vector<std::vector<int>> components;

    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        if (!visited[i]) {
            std::vector<int> comp;
            std::queue<int> q;
            q.push(i);
            visited[i] = true;
            while (!q.empty()) {
                int v = q.front(); q.pop();
                comp.push_back(v);
                for (int nb : adj[v]) {
                    if (!visited[nb]) {
                        visited[nb] = true;
                        q.push(nb);
                    }
                }
            }
            components.push_back(comp);
        }
    }

    std::vector<CoastNode> finalOrderedNodes;

    // Finding the farthest node
    auto findFarthestNode = [&](int startIdx, const std::unordered_set<int> &compSet, std::vector<int> &parent) -> int {
        std::vector<bool> was_visited(nodes.size(), false);
        std::queue<std::pair<int, int>> q; // Node - Distance from the start

        // Start of traversal
        was_visited[startIdx] = true;
        parent[startIdx] = -1;
        q.push({startIdx, 0});

        // Traversal
        int farthestNode = startIdx;
        int maxDist = 0;
        while (!q.empty()) {
            auto [node, dist] = q.front();
            q.pop();

            if (dist > maxDist) {
                maxDist = dist;
                farthestNode = node;
            }

            for (int neighbour : adj[node]) {
                if (was_visited[neighbour]) {
                    continue;
                }

                if (!compSet.count(neighbour)) {
                    continue;
                }

                was_visited[neighbour] = true;
                parent[neighbour] = node;
                q.push({neighbour, dist + 1});
            }
        }

        return farthestNode;
    };

    auto buildPathFromParent = [&](int startNode, int endNode, const std::vector<int>& parent) -> std::vector<int> {
        std::vector<int> path;
        int cur = endNode;

        while (cur != -1) {
            path.push_back(cur);
            if (cur == startNode) {
                break;
            }
            cur = parent[cur];
        }

        std::reverse(path.begin(), path.end());
        return path;
    };

    // Build main path + backtracking when reach a dead end
    auto traverseComponent = [&](const std::vector<int>& comp, int startNode, int endNode, const std::vector<int>& parent) -> std::vector<int> {
        std::vector<int> result;
        if (comp.empty()) return result;

        std::unordered_set<int> compSet(comp.begin(), comp.end());
        std::unordered_set<int> mainPathSet;

        std::vector<int> mainPath = buildPathFromParent(startNode, endNode, parent);
        for (int v : mainPath) {
            mainPathSet.insert(v);
        }

        std::vector<bool> used(nodes.size(), false);

        std::function<void(int, int)> dfsBranch = [&](int v, int parentNode) {
            used[v] = true;
            result.push_back(v);

            for (int nb : adj[v]) {
                if (compSet.count(nb) && nb != parentNode && !used[nb] && !mainPathSet.count(nb)) {
                    dfsBranch(nb, v);
                    result.push_back(v);
                }
            }
        };

        for (size_t i = 0; i < mainPath.size(); ++i) {
            int v = mainPath[i];

            result.push_back(v);
            used[v] = true;

            for (int nb : adj[v]) {
                if (compSet.count(nb) && !used[nb] && !mainPathSet.count(nb)) {
                    dfsBranch(nb, v);
                    result.push_back(v);
                }
            }
        }

        return result;
    };

    // Ordering of nodes in each component
    int componentCounter = 0;
    int droppedComponents = 0;
    for (const auto& comp : components) {
        // If the component is too small
        const int MINIMUM_COMPONENT_SIZE = 5;
        if (comp.size() < MINIMUM_COMPONENT_SIZE) {
            droppedComponents++;
            continue;
        }

        componentCounter++;

        std::unordered_set<int> compSet(comp.begin(), comp.end());
        std::vector<int> parent(nodes.size(), -1);
        int startNode = findFarthestNode(comp[0], compSet, parent);
        std::vector<int> parent2(nodes.size(), -1);
        int endNode = findFarthestNode(startNode, compSet, parent2);

        std::vector<int> path = traverseComponent(comp, startNode, endNode, parent2);

        // Add separator
        if (!finalOrderedNodes.empty()) {
            CoastNode separator;
            separator.row = -1;
            separator.col = -1;
            separator.etaMax = -1.0;
            separator.isSeparator = true;
            finalOrderedNodes.push_back(separator);
        }

        for (size_t i = 0; i < path.size(); ++i) {
            int idx = path[i];
            CoastNode node = nodes[idx];
            node.componentId = componentCounter;
            finalOrderedNodes.push_back(node);
        }
    }

    droppedComponentCount_ = droppedComponents;
    return finalOrderedNodes;
}

std::vector<CoastHistogramTool::CoastNode> CoastHistogramTool::findCoastNodes()
{
    std::vector<CoastNode> nodes;
    if (!hasRegion_ || !grid_ || !grid_->isLoaded() || etaMaxData_.empty())
        return nodes;

    int rows = grid_->rows();
    int cols = grid_->cols();

    // Clamp region
    int rMin = std::max(0, regionRowMin_);
    int rMax = std::min(rows - 1, regionRowMax_);
    int cMin = std::max(0, regionColMin_);
    int cMax = std::min(cols - 1, regionColMax_);

    static const int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    static const int dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};

    for (int r = rMin; r <= rMax; ++r) {
        for (int c = cMin; c <= cMax; ++c) {
            double depth = grid_->value(r, c);
            // Coastal water node: depth >= minDepth (water) adjacent to land
            if (depth < minDepth_) continue;

            bool adjacentToLand = false;
            for (int d = 0; d < 8; ++d) {
                int nr = r + dr[d];
                int nc = c + dc[d];
                if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) {
                    adjacentToLand = true; // edge of grid = land
                    break;
                }
                if (grid_->value(nr, nc) < minDepth_) {
                    adjacentToLand = true;
                    break;
                }
            }

            if (adjacentToLand && r < etaRows_ && c < etaCols_) {
                double eta = etaMaxData_[r * etaCols_ + c];
                nodes.push_back({r, c, eta});
            }
        }
    }

    return orderCoastNodes(nodes);
}

void CoastHistogramTool::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::white);

    if (coastNodes_.empty()) {
        p.drawText(rect(), Qt::AlignCenter, tr("No coast data"));
        return;
    }

    // Draw bar chart
    int topOffset = 50;
    QRect chartArea = rect().adjusted(0, topOffset, 0, 0);

    const int tickLen    = 5;
    const int labelGap   = 2;
    const int titleGap   = 8;
    const int edgePad    = 5;

    const int axisReserve = tickLen + labelGap + 16 + titleGap + 20 + edgePad;

    QRect chartRect = chartArea.adjusted(axisReserve, 30, -30, -axisReserve);
    if (chartRect.width() < 10 || chartRect.height() < 10) return;

    double scaleMax = globalMaxEta_ > 0 ? globalMaxEta_ : 1.0;

    int barCount = static_cast<int>(coastNodes_.size());
    double barWidth = static_cast<double>(chartRect.width()) / barCount;

    p.setPen(Qt::black);
    p.drawLine(chartRect.bottomLeft(), chartRect.bottomRight());
    p.drawLine(chartRect.bottomLeft(), chartRect.topLeft());

    int currentComponentId = -1;
    int visualIdx = 0;
    for (int i = 0; i < barCount; ++i) {
        const auto& node = coastNodes_[i];
        double x = chartRect.left() + visualIdx * barWidth;

        // Draw separator
        if (node.isSeparator) {
            p.save();
            QPen separatorPen(Qt::red, 1, Qt::DashLine);
            p.setPen(separatorPen);
            double lineX = x + barWidth / 2.0;
            p.drawLine(QPointF(lineX, chartRect.top()), QPointF(lineX, chartRect.bottom()));
            p.restore();
            currentComponentId = -1;
            visualIdx++;
            continue;
        }

        if (node.componentId != currentComponentId && node.componentId > 0) {
            currentComponentId = node.componentId;

            QFont font = p.font();
            font.setPointSize(10);
            font.setBold(true);
            p.setFont(font);
            p.setPen(Qt::black);
            p.drawText(QPointF(x + barWidth / 2 - 5, chartRect.top() - 5), QString::number(currentComponentId));
        }

        double h = (std::abs(coastNodes_[i].etaMax) / scaleMax) * chartRect.height();
        QRectF bar(x, chartRect.bottom() - h,
                   barWidth * 0.8, h);
        p.fillRect(bar, QColor(0, 100, 200));
        p.drawRect(bar);

        visualIdx++;
    }

    // Axis labels and tick marks
    QFont axisFont = p.font();
    axisFont.setPointSize(10);
    p.setFont(axisFont);
    p.setPen(Qt::black);

    // Y-axis label
    p.save();
    p.translate(edgePad + 10, chartRect.center().y());;
    p.rotate(-90);
    p.drawText(QRect(-80, -10, 160, 20), Qt::AlignCenter, tr("Wave height (m)"));
    p.restore();

    // Y-axis tick marks
    const int yLabelRight = chartRect.left() - tickLen - labelGap;
    const int yLabelLeft  = yLabelRight - 40;
    const int Y_TICKS = 5;
    for (int i = 0; i <= Y_TICKS; ++i) {
        double value = (i / static_cast<double>(Y_TICKS)) * scaleMax;
        int y = chartRect.bottom() - (i / static_cast<double>(Y_TICKS)) * chartRect.height();

        p.drawLine(chartRect.left() - tickLen, y, chartRect.left(), y);

        QString label = QString::number(value, 'f', 1);
        p.drawText(QRect(yLabelLeft, y - 8, 40, 16),
                   Qt::AlignRight | Qt::AlignVCenter,
                   label);
    }

    // X-axis label
    p.drawText(QRect(chartRect.center().x() - 100,
                     chartRect.bottom() + tickLen + labelGap + 16 + titleGap,
                     200, 20),
               Qt::AlignCenter,
               tr("Point index (per component)"));

    // X-axis tick marks
    const int maxTicks = std::max(1, chartRect.width() / 50);

    auto computeTickStep = [&](int nodeCount, double barW) -> int {
        int minStepByWidth = static_cast<int>(std::ceil(30.0 / barW));
        if (minStepByWidth < 1) minStepByWidth = 1;

        int step = minStepByWidth;
        if (nodeCount > 50)  step = std::max(step, 5);
        if (nodeCount > 200) step = std::max(step, 10);
        if (nodeCount > 500) step = std::max(step, 25);
        if (nodeCount > 1000) step = std::max(step, 50);

        if (step * maxTicks < nodeCount)
            step = static_cast<int>(std::ceil(nodeCount / static_cast<double>(maxTicks)));

        return std::max(1, step);
    };

    int tickStep = 1;
    int localIndex = 0;
    int xAxisVisualIdx = 0;

    for (int i = 0; i < barCount; ++i) {
        const auto& node = coastNodes_[i];

        if (node.isSeparator) {
            localIndex = 0;
            xAxisVisualIdx++;
            continue;
        }

        if (localIndex == 0) {
            int compNodeCount = 0;
            for (int j = i; j < barCount; ++j) {
                if (coastNodes_[j].isSeparator) {
                    break;
                }
                compNodeCount++;
            }
            tickStep = computeTickStep(compNodeCount, barWidth);
        }

        if (localIndex % tickStep == 0) {
            double x = chartRect.left() + xAxisVisualIdx * barWidth + barWidth * 0.4;
            p.drawLine(QPointF(x, chartRect.bottom()),
                       QPointF(x, chartRect.bottom() + tickLen));
            p.drawText(QRectF(x - 15, chartRect.bottom() + tickLen + labelGap, 30, 16),
                       Qt::AlignHCenter | Qt::AlignTop,
                       QString::number(localIndex));
        }

        localIndex++;
        xAxisVisualIdx++;
    }
}

void CoastHistogramTool::onShowCoastlineToggled(bool state) {
    emit showCoastlineChanged(state);
}

void CoastHistogramTool::setGlobalMaxEta(double maxEta, int selectionId) {
    if (selectionId != selectionId_) {
        return;
    }
    globalMaxEta_ = maxEta;
    update();
}

int CoastHistogramTool::currentSelectionId() const {
    return selectionId_;
}
