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
        // Non-left presses are dropped only while a left drag is in progress, so
        // a stray right-click cannot abort a rubber band.
        if (mousePressed_ && event->button() != Qt::LeftButton) {
            return;
        }

        // Tracks the LEFT button only. Setting it for any button would latch it
        // on a bare right-click, whose release the guard above then drops.
        if (event->button() == Qt::LeftButton) {
            mousePressed_ = true;
            mouseMoved_ = false;
            pressPos_ = event->pos();
        }

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
        // Non-left buttons are ignored only while the left button is actually
        // down, so that a stray right-click cannot abort a rubber-band drag.
        // Outside a drag they must reach QGraphicsView like any other event.
        if (mousePressed_ && event->button() != Qt::LeftButton) {
            return;
        }

        QGraphicsView::mouseReleaseEvent(event);

        const bool wasRubberBand = (dragMode() == QGraphicsView::RubberBandDrag &&
                                    mousePressed_ &&
                                    mouseMoved_);

        if (event->button() == Qt::LeftButton) {
            mousePressed_ = false;
        }

        if (wasRubberBand && onRubberBandFinished) {
            onRubberBandFinished();
        }
        else if (onMouseClick && event->button() == Qt::LeftButton) {
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

GridViewerWidget::~GridViewerWidget()
{
    // The scan reads the ResultDataset from a pool thread. Nothing here owns
    // that dataset, so the scan has to be unwound before this widget goes.
    cancelBackgroundWork();
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

    // Track rubber band selection coordinates. Qt reports null endpoints when it
    // tears the band down, which is the only case worth ignoring here; a real
    // endpoint may legitimately be (0,0), the grid origin.
    connect(trackView, &QGraphicsView::rubberBandChanged, this,
            [this](QRect viewportRect, QPointF fromScene, QPointF toScene) {
        if (viewportRect.isNull()) return;
        hasRubberBand_ = true;
        lastRubberFrom_ = fromScene;
        lastRubberTo_ = toScene;
    });

    // When rubber band finished, send region to coast tool
    trackView->onRubberBandFinished = [this]() {
        if (!grid_ || !grid_->isLoaded()) {
            return;
        }

        if (!hasRubberBand_) {
            return;
        }
        hasRubberBand_ = false;

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

        // A drag too thin to cover a cell in either axis selects nothing. Drop
        // whatever was selected before rather than leaving it on screen looking
        // like the result of this drag.
        if (cMin == cMax || rMin == rMax) {
            clearSelection();
            return;
        }

        QRectF rect(cMin, rMin, cMax - cMin + 1, rMax - rMin + 1);
        scene_->setSelectedRegion(rect);

        // Remembered so the scan can be re-armed if the result set underneath
        // this selection is later replaced.
        hasSelection_ = true;
        selRowMin_ = rMin; selRowMax_ = rMax;
        selColMin_ = cMin; selColMax_ = cMax;

        // setRegion first: it bumps the selection id, so the scan is tagged with
        // the id the tool actually holds rather than a predicted one.
        coastTool_->setRegion(rMin, rMax, cMin, cMax);
        startRegionScan(rMin, rMax, cMin, cMax);
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
            // A scan may still be reading the dataset that is about to be
            // re-indexed underneath it.
            cancelBackgroundWork();
            invalidateResultEpoch();
            syncGridDims();
            coordLabel_->setText(tr("Scanning %1...").arg(dir));
            results_->scanDirectory(dir);
            animPlayer_->setResultDataset(results_);
            addResultLayer(QFileInfo(dir).fileName() + tr(" (%1 frames)").arg(results_->frameCount()));
            coordLabel_->setText(tr("Loaded: %1 frames from %2")
                .arg(results_->frameCount()).arg(QFileInfo(dir).fileName()));

            // Loading a directory does not move the animation player off frame
            // 0, so nothing emits frameChanged and nothing would repaint: the
            // map would keep showing the previous result set while the tools
            // showed this one.
            showFirstResultFrame();

            // These are different frames, so any scale measured over the
            // previous result set no longer describes them.
            onResultSourceChanged();
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
            // A scan or an earlier load may still be reading the dataset that is
            // about to be re-indexed underneath it.
            cancelBackgroundWork();
            invalidateResultEpoch();
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
            // Tracked, not fire-and-forget: this reads the dataset off the GUI
            // thread, and nothing else keeps it alive for the parse.
            const QList<int> loadedSteps = r->timesteps();
            const int firstStep = loadedSteps.isEmpty() ? 0 : loadedSteps.first();
            frameLoad_ = QtConcurrent::run([r, firstStep]() {
                return r->frame(firstStep);
            });
            const quint64 epoch = resultEpoch_;
            frameLoad_.then(this, [this, epoch, sc, ct, pt, lbl, grad, fname](std::shared_ptr<FrameData> frame) {
                // The result set moved on while this was parsing, so this frame
                // no longer describes what is loaded.
                if (epoch != resultEpoch_) {
                    return;
                }

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

                    // These are different frames, so any scale measured over the
                    // previous result set no longer describes them.
                    onResultSourceChanged();
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
            // New dimensions re-interpret every frame's orientation. A frame load
            // queued before this point decoded under the old ones, so its
            // continuation must not be allowed to paint over what follows.
            invalidateResultEpoch();
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
    // Any scan in flight is still reading the outgoing dataset.
    cancelBackgroundWork();
    invalidateResultEpoch();

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
    invalidateResultEpoch();
    if (results_)
        results_->clear();
    // The tool keeps its own copy of the frame; without this a later selection
    // would plot a histogram of results that are no longer loaded.
    coastTool_->clearEtaMaxData();
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
    cancelBackgroundWork();
    hasSelection_ = false;

    if (scene_) {
        scene_->clearSelectionRegion();
    }

    if (coastTool_) {
        coastTool_->clearRegion();
    }
}

// Scans every frame over the selected region for the largest |eta|, so the
// histogram keeps one scale across the whole animation instead of rescaling per
// frame. Runs off the GUI thread: it parses each frame from disk.
void GridViewerWidget::startRegionScan(int rowMin, int rowMax, int colMin, int colMax)
{
    cancelBackgroundWork();

    if (!results_ || results_->frameCount() == 0 || !coastTool_) {
        return;
    }

    const QList<int> timesteps = results_->timesteps();
    const int selectionId = coastTool_->currentSelectionId();

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    regionScanCancelled_ = cancelled;

    // Deliberately a bare pointer, not a QPointer: a QPointer carries no
    // cross-thread lifetime guarantee, so testing it here would only narrow the
    // race, not close it. The dataset is instead held alive by
    // cancelBackgroundWork(), which every path that can drop it calls first.
    ResultDataset* results = results_;

    regionScan_ = QtConcurrent::run(
        [results, timesteps, cancelled, rowMin, rowMax, colMin, colMax]() {
            double globalMax = 0;

            for (int t : timesteps) {
                // Polled per frame so a cancel unwinds within one frame load
                // rather than waiting out the whole directory.
                if (cancelled->load()) {
                    return 0.0;
                }

                auto frame = results->frame(t);
                if (!frame) continue;
                // An all-zero frame cannot hold the largest |eta|.
                if (frame->maxVal == 0 && frame->minVal == 0) continue;

                for (int r = rowMin; r <= rowMax; ++r) {
                    for (int c = colMin; c <= colMax; ++c) {
                        if (r >= frame->rows || c >= frame->cols) {
                            continue;
                        }

                        const size_t idx = static_cast<size_t>(r) * frame->cols + c;
                        if (idx >= frame->values.size()) {
                            continue;
                        }

                        globalMax = std::max(globalMax, std::abs(frame->values[idx]));
                    }
                }
            }

            return globalMax;
        });

    regionScan_.then(this, [this, selectionId, cancelled](double globalMax) {
        if (cancelled->load()) {
            return;
        }
        coastTool_->setGlobalMaxEta(globalMax, selectionId);
    });
}

void GridViewerWidget::cancelBackgroundWork()
{
    if (regionScanCancelled_) {
        regionScanCancelled_->store(true);
    }

    // Both of these hold a bare ResultDataset* and read it off the GUI thread,
    // so both must be joined, not merely abandoned. The region scan polls the
    // flag once per frame; the single-file load is one frame, so the wait is
    // bounded by a single parse in either case.
    if (regionScan_.isRunning()) {
        regionScan_.waitForFinished();
    }

    if (frameLoad_.isRunning()) {
        frameLoad_.waitForFinished();
    }

    regionScanCancelled_.reset();
    regionScan_ = QFuture<double>();
    frameLoad_ = QFuture<std::shared_ptr<FrameData>>();
}

// Joining a frame load waits for its parse but cannot un-queue the GUI
// continuation Qt has already scheduled. Moving the epoch is what makes that
// continuation drop its frame. Called only where the result set itself changes
// -- not from cancelBackgroundWork(), which also runs when merely re-arming a
// scan, where a frame load in flight is still describing the current source.
void GridViewerWidget::invalidateResultEpoch()
{
    ++resultEpoch_;
}

bool GridViewerWidget::showFirstResultFrame()
{
    if (!results_) {
        return false;
    }

    // Frames are keyed by the timestep in the file name, so the first key is not
    // generally 0.
    const QList<int> timesteps = results_->timesteps();
    if (timesteps.isEmpty()) {
        return false;
    }

    // Routed through the frame-display path rather than pushing samples at the
    // tools directly: the map overlay and the gradient legend have to move to the
    // new result set too, or they keep showing the previous one.
    onFrameChanged(timesteps.first());
    return true;
}

// A new result set invalidates the scale the histogram was drawn against: the
// scan that produced it measured frames that are no longer loaded.
void GridViewerWidget::onResultSourceChanged()
{
    cancelBackgroundWork();

    if (!coastTool_) {
        return;
    }

    coastTool_->resetScaleAuthority();

    // Re-arm against the frames now loaded, so the selection keeps a scale
    // measured over the whole animation rather than one frame.
    if (hasSelection_) {
        startRegionScan(selRowMin_, selRowMax_, selColMin_, selColMax_);
    }
}
