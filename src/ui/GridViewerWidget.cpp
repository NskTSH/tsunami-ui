#include "GridViewerWidget.h"
#include "GridScene.h"
#include "GradientEditor.h"
#include "AnimationPlayer.h"
#include "ProfileTool.h"
#include "CoastHistogramTool.h"
#include "model/GridDataset.h"
#include "model/ResultDataset.h"
#include "model/ParameterSet.h"
#include "geo/TileProvider.h"

#include <QGraphicsView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTabWidget>
#include <QToolBar>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QPainter>
#include <QFileDialog>
#include <QColorDialog>
#include <QMenu>
#include <QSettings>
#include <QMouseEvent>
#include <QtConcurrent/QtConcurrentRun>
#include <QWheelEvent>
#include <QApplication>
#include <QList>
#include <QPointer>

// Custom view with mouse tracking, wheel zoom, and rubber band finish callback
class TrackingGraphicsView : public QGraphicsView
{
private:
    bool mousePressed_ = false;
    bool mouseMoved_ = false;
    QPoint pressPos_;

public:
    using QGraphicsView::QGraphicsView;
    std::function<void(QPointF)> onMouseMove;
    std::function<void()> onRubberBandFinished;
    std::function<void(QPointF)> onMouseClick;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (mousePressed_ && event->button() != Qt::LeftButton) {
            return;
        }

        mousePressed_ = true;
        mouseMoved_ = false;
        pressPos_ = event->pos();
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (mousePressed_ && !mouseMoved_) {
            int distance = (event->pos() - pressPos_).manhattanLength();
            if (distance > QApplication::startDragDistance()) {
                mouseMoved_ = true;
            }
        }

        QGraphicsView::mouseMoveEvent(event);
        if (onMouseMove) {
            onMouseMove(mapToScene(event->pos()));
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (mousePressed_ && event->button() != Qt::LeftButton) {
            return;
        }

        QGraphicsView::mouseReleaseEvent(event);

        bool wasRubberBand = (dragMode() == QGraphicsView::RubberBandDrag &&
                              mousePressed_ &&
                              mouseMoved_);

        if (wasRubberBand && onRubberBandFinished) {
            onRubberBandFinished();
        }
        else if (onMouseClick) {
            onMouseClick(mapToScene(event->pos()));
        }
    }

    void wheelEvent(QWheelEvent* event) override {
        const double factor = (event->angleDelta().y() > 0) ? 1.25 : 0.8;
        scale(factor, factor);
        event->accept();
    }
};

GridViewerWidget::GridViewerWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
}

void GridViewerWidget::setupUI()
{
    // This widget is now the central map workspace. The layer tree, analysis
    // tools and animation player are created here but hosted by MainWindow in
    // separate QDockWidgets (re-parented via QDockWidget::setWidget). Only the
    // map view, its gradient legend and the coordinate label live here.
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(2);

    // Create gradient and scene BEFORE toolbar (toolbar connects to them)
    gradient_ = new GradientEditor(this);
    scene_ = new GridScene(this);

    setupToolbar();
    mainLayout->addWidget(toolbar_);

    // Map + gradient legend side by side (the central working area)
    auto* mapRow = new QHBoxLayout;
    mapRow->setSpacing(2);

    auto* trackView = new TrackingGraphicsView(scene_, this);
    view_ = trackView;
    view_->setRenderHint(QPainter::Antialiasing, false);
    view_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    view_->setDragMode(QGraphicsView::ScrollHandDrag);
    view_->setMouseTracking(true);
    trackView->onMouseMove = [this](QPointF pos) { updateStatusLabel(pos); };

    // Track rubber band selection coordinates
    connect(trackView, &QGraphicsView::rubberBandChanged, this,
            [this](QRect, QPointF fromScene, QPointF toScene) {
        if (fromScene.isNull() || toScene.isNull()) return;
        lastRubberFrom_ = fromScene;
        lastRubberTo_ = toScene;
    });

    // When rubber band finished, send region to coast tool
    trackView->onRubberBandFinished = [this]() {
        if (grid_ && grid_->isLoaded()) {
            if (lastRubberFrom_.isNull() && lastRubberTo_.isNull()) {
                return;
            }
            int rMin = static_cast<int>(std::min(lastRubberFrom_.y(), lastRubberTo_.y()));
            int rMax = static_cast<int>(std::max(lastRubberFrom_.y(), lastRubberTo_.y()));
            int cMin = static_cast<int>(std::min(lastRubberFrom_.x(), lastRubberTo_.x()));
            int cMax = static_cast<int>(std::max(lastRubberFrom_.x(), lastRubberTo_.x()));

            int rows = grid_->rows();
            int cols = grid_->cols();
            rMin = std::max(0, std::min(rMin, rows - 1));
            rMax = std::max(0, std::min(rMax, rows - 1));
            cMin = std::max(0, std::min(cMin, cols - 1));
            cMax = std::max(0, std::min(cMax, cols - 1));

            if (cMin == cMax || rMin == rMax) {
                return;
            }

            QPointer<ResultDataset> resultsPtr(results_);
            if (resultsPtr && resultsPtr->frameCount() > 0) {
                QList<int> timesteps = resultsPtr->timesteps();
                int rMinCopy = rMin, rMaxCopy = rMax, cMinCopy = cMin, cMaxCopy = cMax;
                int selectionId = coastTool_->currentSelectionId() + 1;

                QtConcurrent::run([resultsPtr, timesteps, rMinCopy, rMaxCopy, cMinCopy, cMaxCopy]() {
                    double globalMax = 0;
                    if (!resultsPtr) {
                        return 0.0;
                    }

                    for (int t : timesteps) {
                        auto frame = resultsPtr->frame(t);
                        if (!frame) continue;
                        if (frame->maxVal == 0 && frame->minVal == 0) continue;
                        for (int r = rMinCopy; r <= rMaxCopy; ++r) {
                            for (int c = cMinCopy; c <= cMaxCopy; ++c) {
                                if (r >= frame->rows || c >= frame->cols) {
                                    continue;
                                }
                                int idx = r * frame->cols + c;
                                double val = std::abs(frame->values[idx]);
                                if (val > globalMax) {
                                    globalMax = val;
                                }
                            }
                        }
                    }
                    return globalMax;
                }).then(this, [this, selectionId](double globalMax) {
                        coastTool_->setGlobalMaxEta(globalMax, selectionId);
                    });
            }

            QRectF rect(cMin, rMin, cMax - cMin + 1, rMax - rMin + 1);
            scene_->setSelectedRegion(rect);
            coastTool_->setRegion(rMin, rMax, cMin, cMax);
        }
    };

    // Click outside the selected area to deselect
    trackView->onMouseClick = [this, trackView](QPointF pos) {
        if (trackView->dragMode() != QGraphicsView::RubberBandDrag) {
            return;
        }

        if (!scene_->hasSelectedRegion()) {
            return;
        }

        QRectF selectionRect = scene_->selectionRegion();
        if (selectionRect.contains(pos)) {
            return;
        }

        clearSelection();
    };

    mapRow->addWidget(view_, 1);
    mapRow->addWidget(gradient_);  // gradient bar right of map
    mainLayout->addLayout(mapRow, 1);

    coordLabel_ = new QLabel(this);
    mainLayout->addWidget(coordLabel_);

    // Dock-hosted panels: created here, placed into docks by MainWindow.
    setupLayerTree();

    animPlayer_ = new AnimationPlayer(this);
    connect(animPlayer_, &AnimationPlayer::frameChanged,
            this, &GridViewerWidget::onFrameChanged);

    profileTool_ = new ProfileTool(this);
    coastTool_ = new CoastHistogramTool(this);

    connect(coastTool_, &CoastHistogramTool::coastlineCellsCalculated, this, [this](const QVector<QPointF>& points) {
        if (scene_) {
            scene_->setCoastlineCells(points);
        }
    });

    connect(coastTool_, &CoastHistogramTool::showCoastlineChanged, this, [this](bool visible) {
        if (scene_) {
            scene_->setCoastlineVisible(visible);
        }
    });

    connect(coastTool_, &CoastHistogramTool::coastlineLabelsReady, this, [this](const QMap<int, QPointF>& labels) {
        if (scene_) {
            scene_->setCoastlineLabels(labels);
        }
    });

    // Connect gradient changes to scene rebuild
    connect(gradient_, &GradientEditor::gradientChanged, this, [this]() {
        scene_->setGradient(gradient_);
        scene_->rebuildRaster();
    });

    scene_->setGradient(gradient_);
}

void GridViewerWidget::setupToolbar()
{
    toolbar_ = new QToolBar(this);

    // Gradient preset selector
    toolbar_->addWidget(new QLabel(tr(" Palette: ")));
    presetCombo_ = new QComboBox;
    presetCombo_->addItems({"ocean", "terrain", "diverging", "hot"});
    connect(presetCombo_, &QComboBox::currentTextChanged,
            gradient_, &GradientEditor::loadPreset);
    toolbar_->addWidget(presetCombo_);

    toolbar_->addSeparator();

    // Select Region button for coast histogram
    auto* selectRegionBtn = new QPushButton(tr("Select Region"));
    selectRegionBtn->setCheckable(true);
    connect(selectRegionBtn, &QPushButton::toggled, this, [this](bool on) {
        if (on)
            view_->setDragMode(QGraphicsView::RubberBandDrag);
        else
            view_->setDragMode(QGraphicsView::ScrollHandDrag);
    });
    toolbar_->addWidget(selectRegionBtn);

    toolbar_->addSeparator();

    // Helper: tell ResultDataset about grid dimensions for auto-transpose
    auto syncGridDims = [this]() {
        if (results_ && grid_ && grid_->isLoaded())
            results_->setExpectedGridDims(grid_->rows(), grid_->cols());
    };

    // Load results — directory
    auto* loadResultsDirBtn = new QPushButton(tr("Load Dir..."));
    connect(loadResultsDirBtn, &QPushButton::clicked, this, [this, syncGridDims]() {
        QSettings s;
        QString last = s.value("lastResultDir").toString();
        QString dir = QFileDialog::getExistingDirectory(this, tr("Select Result Directory"), last);
        if (!dir.isEmpty() && results_) {
            s.setValue("lastResultDir", dir);
            syncGridDims();
            coordLabel_->setText(tr("Scanning %1...").arg(dir));
            results_->scanDirectory(dir);
            animPlayer_->setResultDataset(results_);
            addResultLayer(QFileInfo(dir).fileName() + tr(" (%1 frames)").arg(results_->frameCount()));
            coordLabel_->setText(tr("Loaded: %1 frames from %2")
                .arg(results_->frameCount()).arg(QFileInfo(dir).fileName()));

            if (results_->frameCount() > 0) {
                auto frame = results_->frame(0);
                if (frame) {
                    coastTool_->setEtaMaxData(frame->values, frame->rows, frame->cols);
                    if (coastTool_->hasRegion()) {
                        coastTool_->updateEtaMaxData();
                    }
                }
            }
        }
    });
    toolbar_->addWidget(loadResultsDirBtn);

    // Load results — single file
    auto* loadResultFileBtn = new QPushButton(tr("Load File..."));
    connect(loadResultFileBtn, &QPushButton::clicked, this, [this, syncGridDims]() {
        QSettings s;
        QString last = s.value("lastResultFile").toString();
        QString path = QFileDialog::getOpenFileName(
            this, tr("Open Result File"), last,
            tr("Text Files (*.txt);;All Files (*)"));
        if (!path.isEmpty() && results_) {
            s.setValue("lastResultFile", QFileInfo(path).absolutePath());
            syncGridDims();
            results_->loadSingleFile(path);
            animPlayer_->setResultDataset(results_);
            addResultLayer(QFileInfo(path).fileName());
            coordLabel_->setText(tr("Loading %1...").arg(QFileInfo(path).fileName()));
            // Load frame asynchronously
            auto* r = results_;
            auto* sc = scene_;
            auto* ct = coastTool_;
            auto* pt = profileTool_;
            auto* lbl = coordLabel_;
            auto* grad = gradient_;
            QString fname = QFileInfo(path).fileName();
            QtConcurrent::run([r]() {
                return r->frame(0);
            }).then(this, [sc, ct, pt, lbl, grad, fname](std::shared_ptr<FrameData> frame) {
                if (frame) {
                    grad->setRange(frame->minVal, frame->maxVal);
                    grad->update();
                    sc->setOverlayData(frame->values, frame->rows, frame->cols,
                                       frame->minVal, frame->maxVal);
                    sc->rebuildRaster();
                    ct->setEtaMaxData(frame->values, frame->rows, frame->cols);
                    pt->setFrameData(frame->values, frame->rows, frame->cols);
                    lbl->setText(tr("Loaded: %1 (%2x%3)")
                        .arg(fname).arg(frame->rows).arg(frame->cols));

                    if (ct->hasRegion()) {
                        ct->updateEtaMaxData();
                    }
                } else {
                    lbl->setText(tr("Failed to load: %1").arg(fname));
                }
            });
        }
    });
    toolbar_->addWidget(loadResultFileBtn);

    toolbar_->addSeparator();

    // Zoom controls
    auto* zoomInAct = toolbar_->addAction(tr("Zoom +"));
    connect(zoomInAct, &QAction::triggered, this, [this]() {
        view_->scale(1.25, 1.25);
    });
    auto* zoomOutAct = toolbar_->addAction(tr("Zoom -"));
    connect(zoomOutAct, &QAction::triggered, this, [this]() {
        view_->scale(0.8, 0.8);
    });
    auto* fitAct = toolbar_->addAction(tr("Fit"));
    connect(fitAct, &QAction::triggered, this, [this]() {
        view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
    });
}

void GridViewerWidget::setupLayerTree()
{
    layerTree_ = new QTreeWidget(this);
    layerTree_->setHeaderLabel(tr("Layers"));
    layerTree_->setMinimumWidth(140);
    layerTree_->setIndentation(16);

    // Category items (always present, non-checkable headers)
    auto makeCategory = [this](const QString& name) {
        auto* item = new QTreeWidgetItem(layerTree_);
        item->setText(0, name);
        item->setFlags(Qt::ItemIsEnabled);
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
        return item;
    };

    catBathymetry_  = makeCategory(tr("Bathymetry"));
    catResults_     = makeCategory(tr("Results"));
    catParameters_  = makeCategory(tr("Parameters"));

    // Connections
    connect(layerTree_, &QTreeWidget::itemChanged,
            this, &GridViewerWidget::onLayerItemChanged);
    connect(layerTree_, &QTreeWidget::itemDoubleClicked,
            this, &GridViewerWidget::onLayerItemDoubleClicked);

    layerTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(layerTree_, &QTreeWidget::customContextMenuRequested,
            this, &GridViewerWidget::onLayerTreeContextMenu);

    layerTree_->expandAll();
}

QIcon GridViewerWidget::colorIcon(const QColor& c) const
{
    QPixmap px(14, 14);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setPen(Qt::darkGray);
    p.setBrush(c);
    p.drawRect(1, 1, 12, 12);
    return QIcon(px);
}

void GridViewerWidget::addBathymetryLayer(const QString& filename)
{
    removeBathymetryLayer();

    bathItem_ = new QTreeWidgetItem(catBathymetry_);
    bathItem_->setText(0, filename);
    bathItem_->setCheckState(0, Qt::Checked);
    bathItem_->setIcon(0, colorIcon(QColor(0, 100, 200)));
    bathItem_->setFlags(bathItem_->flags() | Qt::ItemIsUserCheckable);

    bathIsoItem_ = new QTreeWidgetItem(bathItem_);
    bathIsoItem_->setText(0, tr("Isolines"));
    bathIsoItem_->setCheckState(0, Qt::Unchecked);
    bathIsoItem_->setIcon(0, colorIcon(QColor(40, 40, 40)));
    bathIsoItem_->setFlags(bathIsoItem_->flags() | Qt::ItemIsUserCheckable);
    bathIsoItem_->setToolTip(0, tr("Double-click to change color"));

    bathIsoAutoItem_ = new QTreeWidgetItem(bathIsoItem_);
    bathIsoAutoItem_->setText(0, tr("Auto 10 levels [dbl-click]"));
    bathIsoAutoItem_->setFlags(bathIsoAutoItem_->flags() & ~Qt::ItemIsUserCheckable);

    catBathymetry_->setExpanded(true);
    bathItem_->setExpanded(true);
}

void GridViewerWidget::removeBathymetryLayer()
{
    if (!bathItem_) return;
    bathIsoAutoItem_ = nullptr;
    bathIsoItem_ = nullptr;
    delete bathItem_;
    bathItem_ = nullptr;

    clearSelection();

    if (grid_) {
        grid_->clear();
        scene_->setGridDataset(grid_);
        scene_->rebuildRaster();
    }
    coordLabel_->setText(tr("Bathymetry cleared"));
    emit gridClearRequested();
}

void GridViewerWidget::addResultLayer(const QString& filename)
{
    removeResultLayer();

    resultItem_ = new QTreeWidgetItem(catResults_);
    resultItem_->setText(0, filename);
    resultItem_->setCheckState(0, Qt::Checked);
    resultItem_->setIcon(0, colorIcon(QColor(200, 80, 80)));
    resultItem_->setFlags(resultItem_->flags() | Qt::ItemIsUserCheckable);

    resultIsoItem_ = new QTreeWidgetItem(resultItem_);
    resultIsoItem_->setText(0, tr("Isolines"));
    resultIsoItem_->setCheckState(0, Qt::Unchecked);
    resultIsoItem_->setIcon(0, colorIcon(QColor(200, 0, 0)));
    resultIsoItem_->setFlags(resultIsoItem_->flags() | Qt::ItemIsUserCheckable);
    resultIsoItem_->setToolTip(0, tr("Double-click to change color"));

    resultIsoAutoItem_ = new QTreeWidgetItem(resultIsoItem_);
    resultIsoAutoItem_->setText(0, tr("Auto 10 levels [dbl-click]"));
    resultIsoAutoItem_->setFlags(resultIsoAutoItem_->flags() & ~Qt::ItemIsUserCheckable);

    catResults_->setExpanded(true);
    resultItem_->setExpanded(true);
}

void GridViewerWidget::removeResultLayer()
{
    if (!resultItem_) return;
    resultIsoAutoItem_ = nullptr;
    resultIsoItem_ = nullptr;
    delete resultItem_;
    resultItem_ = nullptr;

    clearResults();
    emit resultsClearRequested();
}

void GridViewerWidget::addParameterLayer(const QString& filename)
{
    removeParameterLayer();

    paramItem_ = new QTreeWidgetItem(catParameters_);
    paramItem_->setText(0, filename);
    paramItem_->setFlags(paramItem_->flags() | Qt::ItemIsEnabled);

    sourcesItem_ = new QTreeWidgetItem(paramItem_);
    sourcesItem_->setText(0, tr("Sources"));
    sourcesItem_->setCheckState(0, Qt::Checked);
    sourcesItem_->setIcon(0, colorIcon(Qt::red));
    sourcesItem_->setFlags(sourcesItem_->flags() | Qt::ItemIsUserCheckable);

    maskItem_ = new QTreeWidgetItem(paramItem_);
    maskItem_->setText(0, tr("Computation Mask"));
    maskItem_->setCheckState(0, Qt::Unchecked);
    maskItem_->setIcon(0, colorIcon(QColor(0, 200, 0)));
    maskItem_->setFlags(maskItem_->flags() | Qt::ItemIsUserCheckable);

    subgridItem_ = new QTreeWidgetItem(paramItem_);
    subgridItem_->setText(0, tr("Subgrid Lines"));
    subgridItem_->setCheckState(0, Qt::Unchecked);
    subgridItem_->setIcon(0, colorIcon(Qt::yellow));
    subgridItem_->setFlags(subgridItem_->flags() | Qt::ItemIsUserCheckable);
    subgridItem_->setToolTip(0, tr("Double-click to change color"));

    catParameters_->setExpanded(true);
    paramItem_->setExpanded(true);
}

void GridViewerWidget::removeParameterLayer()
{
    if (!paramItem_) return;
    sourcesItem_ = nullptr;
    maskItem_ = nullptr;
    subgridItem_ = nullptr;
    delete paramItem_;
    paramItem_ = nullptr;

    scene_->setParameterSet(nullptr);
    scene_->rebuildRaster();
}

void GridViewerWidget::onLayerItemChanged(QTreeWidgetItem* item, int)
{
    bool on = (item->checkState(0) == Qt::Checked);

    if (item == bathItem_)           scene_->setShowBathymetry(on);
    else if (item == resultItem_)    { /* overlay visibility controlled by presence */ }
    else if (item == bathIsoItem_)   scene_->setShowBathIsolines(on);
    else if (item == resultIsoItem_) scene_->setShowOverlayIsolines(on);
    else if (item == sourcesItem_)   scene_->setShowSources(on);
    else if (item == maskItem_)      scene_->setShowComputationMask(on);
    else if (item == subgridItem_)   scene_->setShowSubgridLines(on);
}

void GridViewerWidget::onLayerItemDoubleClicked(QTreeWidgetItem* item, int)
{
    if (item == bathIsoItem_) {
        QColor c = QColorDialog::getColor(QColor(40,40,40), this, tr("Bath Isoline Color"));
        if (c.isValid()) {
            scene_->setBathIsolineColor(c);
            item->setIcon(0, colorIcon(c));
        }
    } else if (item == resultIsoItem_) {
        QColor c = QColorDialog::getColor(QColor(200,0,0), this, tr("Result Isoline Color"));
        if (c.isValid()) {
            scene_->setOverlayIsolineColor(c);
            item->setIcon(0, colorIcon(c));
        }
    } else if (item == subgridItem_) {
        QColor c = QColorDialog::getColor(Qt::yellow, this, tr("Subgrid Line Color"));
        if (c.isValid()) {
            scene_->setSubgridColor(c);
            item->setIcon(0, colorIcon(c));
        }
    } else if (item == bathIsoAutoItem_) {
        scene_->autoComputeBathIsolineLevels(10);
    } else if (item == resultIsoAutoItem_) {
        scene_->autoComputeOverlayIsolineLevels(10);
    }
}

void GridViewerWidget::onLayerTreeContextMenu(QPoint pos)
{
    auto* item = layerTree_->itemAt(pos);
    if (!item) return;

    QMenu menu;

    if (item == bathItem_ || item == catBathymetry_) {
        if (bathItem_) {
            menu.addAction(tr("Remove"), this, [this]() {
                removeBathymetryLayer();
            });
        }
    } else if (item == resultItem_ || item == catResults_) {
        if (resultItem_) {
            menu.addAction(tr("Remove"), this, [this]() {
                removeResultLayer();
            });
        }
    } else if (item == paramItem_ || item == catParameters_) {
        if (paramItem_) {
            menu.addAction(tr("Remove"), this, [this]() {
                removeParameterLayer();
            });
        }
    }

    if (!menu.isEmpty())
        menu.exec(layerTree_->viewport()->mapToGlobal(pos));
}

void GridViewerWidget::setGridDataset(GridDataset* grid, const QString& filename)
{
    clearSelection();
    grid_ = grid;
    scene_->setGridDataset(grid);
    profileTool_->setGridDataset(grid);
    coastTool_->setGridDataset(grid);
    if (grid && grid->isLoaded()) {
        if (!filename.isEmpty())
            addBathymetryLayer(filename);

        gradient_->setRange(grid->minValue(), grid->maxValue());

        // If results were loaded before bathymetry, update transpose dims
        // and reload the current frame with correct orientation
        if (results_ && results_->isLoaded()) {
            results_->setExpectedGridDims(grid->rows(), grid->cols());
            int curFrame = animPlayer_->currentTimestep();
            auto frame = results_->frame(curFrame);
            if (frame) {
                scene_->setOverlayData(frame->values, frame->rows, frame->cols,
                                       frame->minVal, frame->maxVal);
                gradient_->setRange(frame->minVal, frame->maxVal);
                coastTool_->setEtaMaxData(frame->values, frame->rows, frame->cols);
                if (coastTool_->hasRegion()) {
                    coastTool_->updateEtaMaxData();
                }
            }
        }

        scene_->rebuildRaster();
        view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
    }
}

void GridViewerWidget::setResultDataset(ResultDataset* results)
{
    results_ = results;
    animPlayer_->setResultDataset(results);
}

void GridViewerWidget::setParameterSet(ParameterSet* params, const QString& filename)
{
    scene_->setParameterSet(params);
    if (params) {
        coastTool_->setMinDepth(params->minimumDepthToCalculate());
        if (!filename.isEmpty())
            addParameterLayer(filename);
    }
}

void GridViewerWidget::setTileProvider(TileProvider* provider)
{
    scene_->setTileProvider(provider);
}

void GridViewerWidget::setOverlayData(const std::vector<double>& data, int rows, int cols,
                                       double minVal, double maxVal)
{
    scene_->setOverlayData(data, rows, cols, minVal, maxVal);
    scene_->rebuildRaster();
}

void GridViewerWidget::clearOverlay()
{
    scene_->clearOverlay();
    scene_->rebuildRaster();
}

void GridViewerWidget::clearResults()
{
    clearSelection();
    if (results_)
        results_->clear();
    scene_->clearOverlay();
    animPlayer_->setResultDataset(nullptr);
    scene_->rebuildRaster();
    coordLabel_->setText(tr("Results cleared"));
}

void GridViewerWidget::onFrameChanged(int timestep)
{
    if (!results_) return;

    auto frame = results_->frame(timestep);
    if (!frame) return;

    // Update gradient range to match overlay data
    gradient_->setRange(frame->minVal, frame->maxVal);
    gradient_->update();

    // Update map overlay
    scene_->setOverlayData(frame->values, frame->rows, frame->cols,
                           frame->minVal, frame->maxVal);
    scene_->rebuildRaster();

    // Update profile tool
    profileTool_->setFrameData(frame->values, frame->rows, frame->cols);

    // Update coast histogram tool with current frame as eta data
    coastTool_->setEtaMaxData(frame->values, frame->rows, frame->cols);

    if (coastTool_ && coastTool_->hasRegion()) {
        coastTool_->updateEtaMaxData();
    }
}

void GridViewerWidget::updateStatusLabel(QPointF scenePos)
{
    int col = static_cast<int>(scenePos.x());
    int row = static_cast<int>(scenePos.y());

    if (!scene_ || !scene_->sceneRect().contains(scenePos)) {
        coordLabel_->setText(QString());
        return;
    }

    double depth = scene_->valueAt(row, col);
    QString text = tr("(%1, %2)").arg(row).arg(col);

    // Depth from bathymetry
    if (grid_ && grid_->isLoaded())
        text += tr("  D: %1 m").arg(depth, 0, 'f', 2);

    // Overlay value (wave height)
    if (scene_->hasOverlayData()) {
        double wave = scene_->overlayValueAt(row, col);
        text += tr("  Eta: %1 m").arg(wave, 0, 'f', 4);
    }

    // World coords if georeferenced
    if (grid_ && grid_->isGeoreferenced()) {
        double lon = grid_->worldX(col);
        double lat = grid_->worldY(row);
        text += tr("  |  %1, %2").arg(lon, 0, 'f', 4).arg(lat, 0, 'f', 4);
    }

    emit cellHovered(row, col, depth);
    coordLabel_->setText(text);
}

void GridViewerWidget::clearSelection() {
    if (scene_) {
        scene_->clearSelectionRegion();
    }

    if (coastTool_) {
        coastTool_->clearRegion();
    }
}
