#include "MainWindow.h"
#include "ParameterEditorWidget.h"
#include "GridViewerWidget.h"
#include "GridScene.h"
#include "SimulationRunnerWidget.h"
#include "ProfileTool.h"
#include "CoastHistogramTool.h"
#include "AnimationPlayer.h"
#include "model/Project.h"
#include "model/ParameterSet.h"
#include "model/GridDataset.h"
#include "model/ResultDataset.h"
#include "geo/TileProvider.h"
#include "io/AsyncGridLoader.h"

#include <QDockWidget>
#include <QTreeWidget>
#include <QMenu>
#include <QMenuBar>
#include <QFileDialog>
#include <QStatusBar>
#include <QLabel>
#include <QSettings>
#include <QMessageBox>
#include <QCloseEvent>

// Bump when the dock set / object names change so restoreState() rejects
// incompatible saved layouts instead of restoring them wrongly.
static constexpr int kLayoutVersion = 1;

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      project_(new Project(this)),
      tileProvider_(new TileProvider(this))
{
    setWindowTitle(tr("Tsunami Simulator UI"));
    resize(1400, 900);
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks
                   | QMainWindow::AllowTabbedDocks | QMainWindow::GroupedDragging);

    setupDocks();      // creates panels + docks, map = central widget
    setupMenuBar();    // File menu
    setupViewMenu();   // View > Panels (toggle) + Reset Layout
    connectModules();

    applyDefaultLayout();
    restoreLayout();   // saved arrangement overrides defaults if present

    statusBar()->showMessage(tr("Ready"));
}

MainWindow::~MainWindow()
{
    // project_ is constructed before the map widget, so QObject tears it down
    // first -- taking the ResultDataset with it while a region scan may still be
    // reading frames. This body runs before any child is destroyed.
    if (gridViewer_) {
        gridViewer_->cancelBackgroundWork();
    }
}

void MainWindow::setupMenuBar()
{
    auto* fileMenu = menuBar()->addMenu(tr("&File"));

    auto* newProjectAction = fileMenu->addAction(tr("&New Project"));
    connect(newProjectAction, &QAction::triggered, this, [this]() {
        // The project owns the ResultDataset a region scan may still be reading.
        gridViewer_->cancelBackgroundWork();
        project_->deleteLater();
        project_ = new Project(this);
        connectModules();
        statusBar()->showMessage(tr("New project created"));
    });

    auto* openProjectAction = fileMenu->addAction(tr("Open &Project..."));
    connect(openProjectAction, &QAction::triggered, this, [this]() {
        QString path = QFileDialog::getOpenFileName(
            this, tr("Open Project"), QString(),
            tr("Tsunami Project (*.tsunamiproj);;All Files (*)"));
        if (!path.isEmpty()) {
            // Load into a candidate first. Tearing the old project down before
            // knowing the new one parses would leave the map widget pointing at
            // a freed GridDataset/ResultDataset on the failure branch, because
            // only the success branch re-points it via connectModules().
            auto* candidate = new Project(this);
            if (!candidate->loadProject(path)) {
                delete candidate;
                QMessageBox::warning(this, tr("Error"),
                    tr("Failed to load project: %1").arg(path));
                return;
            }

            gridViewer_->cancelBackgroundWork();
            project_->deleteLater();
            project_ = candidate;

            if (!project_->parFilePath().isEmpty())
                paramEditor_->loadFromFile(project_->parFilePath());
            connectModules();
            statusBar()->showMessage(tr("Project loaded: %1").arg(path));
        }
    });

    auto* saveProjectAction = fileMenu->addAction(tr("&Save Project..."));
    connect(saveProjectAction, &QAction::triggered, this, [this]() {
        QString path = QFileDialog::getSaveFileName(
            this, tr("Save Project"), QString(),
            tr("Tsunami Project (*.tsunamiproj);;All Files (*)"));
        if (!path.isEmpty())
            project_->saveProject(path);
    });

    fileMenu->addSeparator();

    auto* openParAction = fileMenu->addAction(tr("Open .&par file..."));
    connect(openParAction, &QAction::triggered, this, [this]() {
        QSettings s;
        QString path = QFileDialog::getOpenFileName(
            this, tr("Open Parameter File"), s.value("lastParDir").toString(),
            tr("Parameter Files (*.par);;All Files (*)"));
        if (!path.isEmpty() && paramEditor_) {
            s.setValue("lastParDir", QFileInfo(path).absolutePath());
            paramEditor_->loadFromFile(path);
            project_->setParFilePath(path);
            // Update layer tree and reconnect params to scene
            auto* params = paramEditor_->parameterSet();
            gridViewer_->setParameterSet(params, QFileInfo(path).fileName());
            simRunner_->setParameterSet(params);
            if (project_->gridDataset()->isLoaded())
                gridViewer_->scene()->rebuildRaster();
        }
    });

    auto* saveParAction = fileMenu->addAction(tr("Sa&ve .par file..."));
    connect(saveParAction, &QAction::triggered, this, [this]() {
        QString path = QFileDialog::getSaveFileName(
            this, tr("Save Parameter File"), QString(),
            tr("Parameter Files (*.par);;All Files (*)"));
        if (!path.isEmpty() && paramEditor_)
            paramEditor_->saveToFile(path);
    });

    fileMenu->addSeparator();

    auto* openBathAction = fileMenu->addAction(tr("Open &Bathymetry..."));
    connect(openBathAction, &QAction::triggered, this, [this]() {
        QSettings s;
        QString path = QFileDialog::getOpenFileName(
            this, tr("Open Bathymetry"), s.value("lastBathDir").toString(),
            tr("Grid Files (*.asc *.z);;ASC Files (*.asc);;Z Files (*.z);;All Files (*)"));
        if (!path.isEmpty()) {
            s.setValue("lastBathDir", QFileInfo(path).absolutePath());
            loadBathymetry(path);
        }
    });

    fileMenu->addSeparator();

    auto* exitAction = fileMenu->addAction(tr("E&xit"));
    connect(exitAction, &QAction::triggered, this, &QMainWindow::close);
}

QDockWidget* MainWindow::makeDock(const QString& title, const QString& objectName,
                                  QWidget* content)
{
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(objectName);   // required for saveState/restoreState
    dock->setWidget(content);          // re-parents content into the dock
    dock->setFeatures(QDockWidget::DockWidgetMovable
                      | QDockWidget::DockWidgetFloatable
                      | QDockWidget::DockWidgetClosable);
    return dock;
}

void MainWindow::setupDocks()
{
    paramEditor_ = new ParameterEditorWidget(this);
    gridViewer_  = new GridViewerWidget(this);
    simRunner_   = new SimulationRunnerWidget(this);

    // The map canvas is the central working area; everything else is a dock.
    setCentralWidget(gridViewer_);

    layersDock_     = makeDock(tr("Layers"), "layersDock",
                               gridViewer_->layerTreeWidget());
    parametersDock_ = makeDock(tr("Parameters"), "parametersDock", paramEditor_);
    profileDock_    = makeDock(tr("Profile"), "profileDock",
                               gridViewer_->profileTool());
    coastDock_      = makeDock(tr("Coast Histogram"), "coastDock",
                               gridViewer_->coastTool());
    timelineDock_   = makeDock(tr("Timeline"), "timelineDock",
                               gridViewer_->animationPlayer());
    simulationDock_ = makeDock(tr("Simulation Console"), "simulationDock",
                               simRunner_);

    addDockWidget(Qt::LeftDockWidgetArea, layersDock_);
    addDockWidget(Qt::RightDockWidgetArea, parametersDock_);
    addDockWidget(Qt::RightDockWidgetArea, profileDock_);
    addDockWidget(Qt::RightDockWidgetArea, coastDock_);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock_);
    addDockWidget(Qt::BottomDockWidgetArea, simulationDock_);

    // Side docks keep full height; the bottom row serves the central map.
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
}

void MainWindow::setupViewMenu()
{
    viewMenu_ = menuBar()->addMenu(tr("&View"));

    auto* panels = viewMenu_->addMenu(tr("&Panels"));
    for (QDockWidget* dock : {layersDock_, parametersDock_, profileDock_,
                              coastDock_, timelineDock_, simulationDock_}) {
        panels->addAction(dock->toggleViewAction());
    }

    viewMenu_->addSeparator();
    auto* resetAction = viewMenu_->addAction(tr("&Reset Layout"));
    connect(resetAction, &QAction::triggered, this, [this]() {
        for (QDockWidget* dock : {layersDock_, parametersDock_, profileDock_,
                                  coastDock_, timelineDock_, simulationDock_}) {
            dock->setFloating(false);
            dock->show();
        }
        addDockWidget(Qt::LeftDockWidgetArea, layersDock_);
        addDockWidget(Qt::RightDockWidgetArea, parametersDock_);
        addDockWidget(Qt::RightDockWidgetArea, profileDock_);
        addDockWidget(Qt::RightDockWidgetArea, coastDock_);
        addDockWidget(Qt::BottomDockWidgetArea, timelineDock_);
        addDockWidget(Qt::BottomDockWidgetArea, simulationDock_);
        setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
        setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
        applyDefaultLayout();
    });
}

void MainWindow::applyDefaultLayout()
{
    // Right column: Parameters / Profile / Coast tabbed, Parameters in front.
    tabifyDockWidget(parametersDock_, profileDock_);
    tabifyDockWidget(profileDock_, coastDock_);
    parametersDock_->raise();

    // Bottom row: Timeline / Simulation Console tabbed, Timeline in front.
    tabifyDockWidget(timelineDock_, simulationDock_);
    timelineDock_->raise();

    resizeDocks({layersDock_, parametersDock_}, {240, 480}, Qt::Horizontal);
    resizeDocks({timelineDock_}, {130}, Qt::Vertical);
}

void MainWindow::saveLayout()
{
    QSettings s;
    s.setValue("mainWindow/geometry", saveGeometry());
    s.setValue("mainWindow/state", saveState(kLayoutVersion));
}

void MainWindow::restoreLayout()
{
    QSettings s;
    const QByteArray geom = s.value("mainWindow/geometry").toByteArray();
    const QByteArray state = s.value("mainWindow/state").toByteArray();
    if (!geom.isEmpty())
        restoreGeometry(geom);
    if (!state.isEmpty())
        restoreState(state, kLayoutVersion);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::connectModules()
{
    disconnect(simRunner_, &SimulationRunnerWidget::simulationFinished,
               this, &MainWindow::onSimulationFinished);

    auto* params = paramEditor_->parameterSet();
    gridViewer_->setParameterSet(params); // no filename = no tree item yet
    simRunner_->setParameterSet(params);

    gridViewer_->setGridDataset(project_->gridDataset()); // no filename = no tree item yet
    gridViewer_->setResultDataset(project_->resultDataset());
    gridViewer_->setTileProvider(tileProvider_);

    connect(simRunner_, &SimulationRunnerWidget::simulationFinished,
            this, &MainWindow::onSimulationFinished);

    connect(params, &ParameterSet::parameterChanged, this, [this]() {
        if (project_->gridDataset()->isLoaded())
            gridViewer_->scene()->rebuildRaster();
    }, static_cast<Qt::ConnectionType>(Qt::AutoConnection | Qt::UniqueConnection));
}

void MainWindow::loadBathymetry(const QString& path)
{
    project_->setBathymetryPath(path);

    auto* grid = project_->gridDataset();
    bool isASC = path.endsWith(".asc", Qt::CaseInsensitive);

    auto* loader = new AsyncGridLoader(
        grid, path,
        isASC ? AsyncGridLoader::ASC : AsyncGridLoader::Z,
        this);

    statusBar()->showMessage(tr("Loading bathymetry: %1...").arg(path));

    auto* currentProject = project_;
    connect(loader, &AsyncGridLoader::loadFinished, this,
            [this, loader, path, currentProject](bool ok) {
        loader->deleteLater();
        if (project_ != currentProject) return;

        if (ok) {
            gridViewer_->setGridDataset(project_->gridDataset(),
                                        QFileInfo(path).fileName());
            statusBar()->showMessage(tr("Loaded: %1 (%2x%3)")
                .arg(path)
                .arg(project_->gridDataset()->rows())
                .arg(project_->gridDataset()->cols()));
        } else {
            statusBar()->showMessage(tr("Failed to load: %1").arg(path));
            QMessageBox::warning(this, tr("Error"),
                                 tr("Failed to load bathymetry file: %1").arg(path));
        }
    });

    loader->start();
}

void MainWindow::onSimulationFinished(bool success, const QString& outputDir)
{
    if (success) {
        auto reply = QMessageBox::question(
            this, tr("Simulation Finished"),
            tr("Simulation completed successfully.\nLoad results from %1?").arg(outputDir));

        if (reply == QMessageBox::Yes) {
            project_->setResultDirectory(outputDir);
            // Before scanDirectory: it re-indexes the dataset a region scan may
            // still be reading.
            gridViewer_->cancelBackgroundWork();
            project_->resultDataset()->scanDirectory(outputDir);
            gridViewer_->setResultDataset(project_->resultDataset());
            gridViewer_->addResultLayer(QFileInfo(outputDir).fileName());
            // Without this the map and the histogram keep showing the previous
            // run's frame while being scaled against this one.
            gridViewer_->showFirstResultFrame();
            // New frames: the histogram's old scale no longer describes them.
            gridViewer_->onResultSourceChanged();
            timelineDock_->show();   // un-hide if the user had closed it
            timelineDock_->raise();
        }
    }
}
