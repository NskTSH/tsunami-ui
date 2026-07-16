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

// A different threshold picks out a different coast, so the memo no longer says
// anything. Rebuild here rather than leaving the invalidation for the next
// animation frame: a paused view would otherwise keep showing the old
// threshold's coastline indefinitely.
void CoastHistogramTool::setMinDepth(double d)
{
    if (d == minDepth_) {
        return;
    }

    minDepth_ = d;
    coastNodesComputed_ = false;

    if (hasRegion_ && grid_ && grid_->isLoaded() && !etaMaxData_.empty()) {
        recompute();
    }
}

void CoastHistogramTool::setRegion(int rowMin, int rowMax, int colMin, int colMax)
{
    selectionId_++;
    globalMaxEta_ = 0;
    globalMaxFromScan_ = false;
    droppedComponentCount_ = 0;
    regionRowMin_ = rowMin;
    regionRowMax_ = rowMax;
    regionColMin_ = colMin;
    regionColMax_ = colMax;
    hasRegion_ = true;
    recompute();
}

void CoastHistogramTool::clearRegion() {
    selectionId_++;
    globalMaxEta_ = 0;
    globalMaxFromScan_ = false;
    hasRegion_ = false;
    droppedComponentCount_ = 0;
    recompute();
}

// Drops the eta samples along with the result set they came from. Without this
// a later selection would silently build a histogram out of unloaded results.
void CoastHistogramTool::clearEtaMaxData() {
    etaMaxData_.clear();
    etaRows_ = 0;
    etaCols_ = 0;
    globalMaxEta_ = 0;
    globalMaxFromScan_ = false;
    recompute();
}

bool CoastHistogramTool::hasRegion() const {
    return hasRegion_;
}

void CoastHistogramTool::setEtaMaxData(const std::vector<double>& etaMax, int rows, int cols)
{
    // Which coastal cells have a sample is decided by the shape of the samples,
    // so only a change of shape can change the answer findCoastNodes gave. Frame
    // after frame of one result set carries the same shape and keeps the memo,
    // which is what stops a full region rescan on every animation tick.
    if (rows != etaRows_ || cols != etaCols_ || etaMax.size() != etaMaxData_.size()) {
        coastNodesComputed_ = false;
    }

    etaMaxData_ = etaMax;
    etaRows_ = rows;
    etaCols_ = cols;
}

void CoastHistogramTool::updateEtaMaxData() {
    if (etaMaxData_.empty()) {
        return;
    }

    // Driven by the memo rather than by whether the coast came out empty: gating
    // on emptiness would freeze a partial coast found against a smaller frame,
    // and would equally defeat the invalidation done by setMinDepth().
    if (!coastNodesComputed_ && hasRegion_ && grid_ && grid_->isLoaded()) {
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

    // Until the background scan reports the max across every frame, track the
    // frames seen so far and only ever grow. Latching the first frame's max
    // instead would leave every later, larger wave drawn off the chart.
    if (!globalMaxFromScan_ && maxEta > globalMaxEta_) {
        globalMaxEta_ = maxEta;
    }

    update();
}

void CoastHistogramTool::recompute()
{
    coastNodes_ = findCoastNodes();

    // A pass ran against the samples in hand, so memoize it whatever it found.
    // A pass that reached only some of the coast is not worth repeating against
    // the same shape -- it would give the same answer -- and setEtaMaxData drops
    // the memo when the shape changes, which is the only way the answer can
    // improve.
    coastNodesComputed_ = hasRegion_ && grid_ && grid_->isLoaded() && !etaMaxData_.empty();

    // The walk retraces out of dead ends, so one cell can appear several times
    // in the series. The map overlay and the cell count describe the coast
    // itself, so they take each cell once.
    QVector<QPointF> points;
    std::unordered_set<long long> uniqueCells;
    for (const auto& node : coastNodes_) {
        if (node.isSeparator) {
            continue;
        }

        const long long cellKey =
            (static_cast<long long>(node.row) << 32) | static_cast<unsigned int>(node.col);
        if (!uniqueCells.insert(cellKey).second) {
            continue;
        }

        points.append(QPointF(node.col, node.row));
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

    const int realNodeCount = static_cast<int>(uniqueCells.size());

    if (droppedComponentCount_ > 0) {
        infoLabel_->setText(tr("Coast nodes: %1 (%2 tiny components filtered)")
                                .arg(realNodeCount)
                                .arg(droppedComponentCount_));
    } else {
        infoLabel_->setText(tr("Coast nodes found: %1").arg(realNodeCount));
    }

    // Seed the scale from the frame in hand so the first paint of a selection is
    // drawn to size rather than against the 1.0 m fallback while the background
    // scan is still running.
    if (!globalMaxFromScan_) {
        double maxEta = 0;
        for (const auto& node : coastNodes_) {
            if (node.isSeparator) {
                continue;
            }
            maxEta = std::max(maxEta, std::abs(node.etaMax));
        }
        if (maxEta > globalMaxEta_) {
            globalMaxEta_ = maxEta;
        }
    }

    update();
}

/*
 * orderCoastNodes orders coastline nodes along the coastline.
 *
 * Complex cases:
 * 1. Islands / Closed loops.
 * A smooth closed contour runs out of unvisited cells before the walk can
 * dead-end, so its perimeter is emitted once. A ragged one can still dead-end
 * and retrace; the walk is a greedy heuristic, not a guarantee.
 *
 * 2. Breaks (multiple components).
 * The algorithm orders all found components separately.
 * This allows for outputting data for all found components, adding separators.
 *
 * 3. Coves.
 * Traversal starts at an extremal cell; a dead end is left by retracing the
 * walked path, re-emitting those cells so the series stays spatially continuous.
 */
std::vector<CoastHistogramTool::CoastNode> CoastHistogramTool::orderCoastNodes(const std::vector<CoastNode>& nodes)
{
    droppedComponentCount_ = 0;

    const int nodeCount = static_cast<int>(nodes.size());
    if (nodeCount == 0) {
        return {};
    }

    std::vector<std::vector<int>> adj(nodes.size());

    std::unordered_map<int, std::unordered_map<int, int>> spatialIndex;
    for (int i = 0; i < nodeCount; ++i) {
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

    // Scratch buffers shared by every component. Each pass stamps them with its
    // component index rather than reallocating, so ordering stays O(nodes)
    // instead of O(components x nodes).
    std::vector<int> bfsStamp(nodes.size(), -1);
    std::vector<int> walkStamp(nodes.size(), -1);
    std::vector<int> walkStack;

    // Far end of a BFS from an arbitrary member of the component. On an open
    // coast that is a tip, so the walk starts at one end rather than the middle.
    // Edges never leave a component, so the BFS cannot escape it.
    auto findEndpoint = [&](int startIdx, int stamp) -> int {
        std::queue<std::pair<int, int>> q; // Node - Distance from the start

        bfsStamp[startIdx] = stamp;
        q.push({startIdx, 0});

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
                if (bfsStamp[neighbour] == stamp) {
                    continue;
                }

                bfsStamp[neighbour] = stamp;
                q.push({neighbour, dist + 1});
            }
        }

        return farthestNode;
    };

    // Walk the coast, always stepping onto the most constrained unvisited
    // neighbour: fewest unvisited neighbours of its own, orthogonal before
    // diagonal. Eight-connected contours carry diagonal chords, so a walk that
    // just takes the first unvisited neighbour cuts corners and has to come back
    // for the cell it skipped; taking the most constrained cell first holds a
    // smooth contour to a single pass. It is a greedy heuristic and a ragged
    // contour can still box it in, which costs retrace, never correctness.
    //
    // On a dead end the walk retraces its own path, re-emitting those cells,
    // until it reaches one with an unvisited neighbour. That retrace is what
    // keeps a cove continuous: the series walks in and back out, as a survey
    // along the shore would.
    //
    // Iterative by necessity: the recursive form descended about half the
    // component and overflowed the stack on large islands.
    auto traverseComponent = [&](int startNode, int stamp, int compSize) -> std::vector<int> {
        std::vector<int> result;
        result.reserve(compSize);
        walkStack.clear();

        auto unvisitedDegree = [&](int node) {
            int degree = 0;
            for (int neighbour : adj[node]) {
                if (walkStamp[neighbour] != stamp) {
                    ++degree;
                }
            }
            return degree;
        };

        auto isDiagonalStep = [&](int from, int to) {
            return nodes[from].row != nodes[to].row && nodes[from].col != nodes[to].col;
        };

        int current = startNode;
        int visitedCount = 1;
        walkStamp[current] = stamp;
        result.push_back(current);
        walkStack.push_back(current);

        while (visitedCount < compSize) {
            int next = -1;
            int bestDegree = 0;
            bool bestIsDiagonal = false;

            for (int neighbour : adj[current]) {
                if (walkStamp[neighbour] == stamp) {
                    continue;
                }

                const int degree = unvisitedDegree(neighbour);
                const bool diagonal = isDiagonalStep(current, neighbour);

                if (next == -1 || degree < bestDegree ||
                    (degree == bestDegree && bestIsDiagonal && !diagonal)) {
                    next = neighbour;
                    bestDegree = degree;
                    bestIsDiagonal = diagonal;
                }
            }

            if (next != -1) {
                walkStamp[next] = stamp;
                ++visitedCount;
                result.push_back(next);
                walkStack.push_back(next);
                current = next;
                continue;
            }

            walkStack.pop_back();
            if (walkStack.empty()) {
                break;
            }

            current = walkStack.back();
            result.push_back(current);
        }

        return result;
    };

    // Ordering of nodes in each component
    int componentCounter = 0;
    int droppedComponents = 0;
    for (int compIdx = 0; compIdx < static_cast<int>(components.size()); ++compIdx) {
        const auto& comp = components[compIdx];

        // Components this small are coastal noise (stray cells, single rocks);
        // they are reported as a filtered count instead of being plotted.
        const int MINIMUM_COMPONENT_SIZE = 5;
        if (static_cast<int>(comp.size()) < MINIMUM_COMPONENT_SIZE) {
            droppedComponents++;
            continue;
        }

        componentCounter++;

        int startNode = findEndpoint(comp[0], compIdx);
        std::vector<int> path = traverseComponent(startNode, compIdx, static_cast<int>(comp.size()));

        // Add separator
        if (!finalOrderedNodes.empty()) {
            CoastNode separator;
            separator.row = -1;
            separator.col = -1;
            separator.etaMax = -1.0;
            separator.isSeparator = true;
            finalOrderedNodes.push_back(separator);
        }

        for (int idx : path) {
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

            if (!adjacentToLand) {
                continue;
            }

            // The eta grid is sized independently of the bathymetry, so the
            // index is checked against the samples actually held, not just
            // against the declared dimensions. A coastal cell the samples do not
            // reach carries no wave height and is left out.
            if (r >= etaRows_ || c >= etaCols_) {
                continue;
            }

            const size_t idx = static_cast<size_t>(r) * etaCols_ + c;
            if (idx >= etaMaxData_.size()) {
                continue;
            }

            nodes.push_back({r, c, etaMaxData_[idx]});
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

    // A result file may carry an overflowing token (QString::toDouble yields
    // infinity), which would turn every bar height into NaN and hand QRectF
    // non-finite geometry.
    double scaleMax = (globalMaxEta_ > 0 && std::isfinite(globalMaxEta_)) ? globalMaxEta_ : 1.0;

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

        // Held to the plot area: a sample above the current scale would
        // otherwise be painted across the axes and over the rest of the widget.
        // Reachable while the background scan of every frame is still running.
        // std::clamp cannot filter a NaN, so non-finite samples are dropped to
        // zero first rather than reaching QRectF.
        const double eta = coastNodes_[i].etaMax;
        double h = std::isfinite(eta) ? (std::abs(eta) / scaleMax) * chartRect.height() : 0.0;
        h = std::clamp(h, 0.0, static_cast<double>(chartRect.height()));

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
    globalMaxFromScan_ = true;
    update();
}

int CoastHistogramTool::currentSelectionId() const {
    return selectionId_;
}

void CoastHistogramTool::resetScaleAuthority() {
    // Bumping the id makes any scan still in flight report against a selection
    // that no longer exists, so its answer is discarded rather than applied to
    // the new result set.
    selectionId_++;
    globalMaxEta_ = 0;
    globalMaxFromScan_ = false;
    update();
}
