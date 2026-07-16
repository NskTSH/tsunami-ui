#pragma once

#include <QMainWindow>

class QDockWidget;
class QMenu;
class ParameterEditorWidget;
class GridViewerWidget;
class SimulationRunnerWidget;
class Project;
class TileProvider;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void setupMenuBar();
    void setupDocks();
    void setupViewMenu();
    void connectModules();

    // Dock layout helpers.
    QDockWidget* makeDock(const QString& title, const QString& objectName,
                          QWidget* content);
    void applyDefaultLayout();
    void saveLayout();
    void restoreLayout();

    void loadBathymetry(const QString& path);
    void onSimulationFinished(bool success, const QString& outputDir);

    Project* project_ = nullptr;
    TileProvider* tileProvider_ = nullptr;

    ParameterEditorWidget* paramEditor_ = nullptr;
    GridViewerWidget* gridViewer_ = nullptr;
    SimulationRunnerWidget* simRunner_ = nullptr;

    // Docks hosting the workspace panels (map is the central widget).
    QDockWidget* layersDock_ = nullptr;
    QDockWidget* parametersDock_ = nullptr;
    QDockWidget* profileDock_ = nullptr;
    QDockWidget* coastDock_ = nullptr;
    QDockWidget* timelineDock_ = nullptr;
    QDockWidget* simulationDock_ = nullptr;

    QMenu* viewMenu_ = nullptr;
};
