#include "ResultDataset.h"

#include <QDir>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QFile>
#include <QTextStream>
#include <limits>

ResultDataset::ResultDataset(QObject* parent)
    : QObject(parent)
{
}

bool ResultDataset::scanDirectory(const QString& dirPath)
{
    QMutexLocker lock(&mutex_);

    QDir dir(dirPath);
    if (!dir.exists())
        return false;

    frameIndex_.clear();
    cache_.clear();
    cacheOrder_.clear();
    ++generation_;
    dirPath_ = dirPath;

    // Strategy 1: look for eta_timestep_N.txt
    static QRegularExpression reTimestep(R"(eta_timestep_(\d+)\.txt)");
    const auto tsEntries = dir.entryList({"eta_timestep_*.txt"}, QDir::Files, QDir::Name);
    for (const QString& name : tsEntries) {
        auto match = reTimestep.match(name);
        if (match.hasMatch()) {
            int ts = match.captured(1).toInt();
            frameIndex_[ts] = dir.filePath(name);
        }
    }

    // Strategy 2: if no timestep files, load all .txt files sorted by name
    if (frameIndex_.isEmpty()) {
        const auto allTxt = dir.entryList({"*.txt"}, QDir::Files, QDir::Name);
        int idx = 0;
        for (const QString& name : allTxt) {
            frameIndex_[idx] = dir.filePath(name);
            ++idx;
        }
    }

    int count = frameIndex_.size();
    lock.unlock();
    emit directoryScanned(count);
    return count > 0;
}

bool ResultDataset::loadSingleFile(const QString& filePath)
{
    QMutexLocker lock(&mutex_);

    frameIndex_.clear();
    cache_.clear();
    cacheOrder_.clear();
    ++generation_;
    dirPath_ = QFileInfo(filePath).absolutePath();

    frameIndex_[0] = filePath;

    int count = 1;
    lock.unlock();
    emit directoryScanned(count);
    return true;
}

std::shared_ptr<FrameData> ResultDataset::frame(int timestep)
{
    QMutexLocker lock(&mutex_);

    // Check cache
    if (cache_.contains(timestep)) {
        cacheOrder_.removeOne(timestep);
        cacheOrder_.append(timestep);
        return cache_[timestep];
    }

    // Check index
    if (!frameIndex_.contains(timestep))
        return nullptr;

    QString path = frameIndex_[timestep];

    // Snapshot the transpose dimensions here: loadFrame runs with the lock
    // released, and setExpectedGridDims can rewrite them from the GUI thread
    // while a worker is mid-read.
    const int expectedRows = expectedGridRows_;
    const int expectedCols = expectedGridCols_;
    const quint64 generation = generation_;

    // Release lock during disk I/O (loadFrame is pure)
    lock.unlock();
    auto f = loadFrame(path, timestep, expectedRows, expectedCols);
    if (!f)
        return nullptr;

    // Re-acquire to update cache
    lock.relock();

    // The interpretation may have changed while the lock was released. Caching
    // now would publish a frame decoded under the old dimensions or from a path
    // that is no longer indexed, and an invalidated cache makes that insertion
    // look valid. Hand the frame back to this caller but do not keep it.
    if (generation != generation_) {
        return f;
    }

    // Another thread may have loaded this frame while we were reading
    if (cache_.contains(timestep))
        return cache_[timestep];

    evictCache();
    cache_[timestep] = f;
    cacheOrder_.append(timestep);

    lock.unlock();
    emit frameLoaded(timestep);
    return f;
}

void ResultDataset::setExpectedGridDims(int gridRows, int gridCols)
{
    // Held across the whole update: the dimensions and the cache they govern
    // have to change together, or a frame read under the old dimensions can land
    // in the cache after it was invalidated.
    QMutexLocker lock(&mutex_);

    if (expectedGridRows_ == gridRows && expectedGridCols_ == gridCols) {
        return;
    }

    expectedGridRows_ = gridRows;
    expectedGridCols_ = gridCols;
    // Invalidate cache — frames need re-reading with new transpose logic
    cache_.clear();
    cacheOrder_.clear();
    ++generation_;
}

void ResultDataset::clear()
{
    QMutexLocker lock(&mutex_);
    frameIndex_.clear();
    cache_.clear();
    cacheOrder_.clear();
    ++generation_;
    dirPath_.clear();
    expectedGridRows_ = 0;
    expectedGridCols_ = 0;
}

std::shared_ptr<FrameData> ResultDataset::loadFrame(const QString& path, int timestep,
                                                    int expectedRows, int expectedCols)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return nullptr;

    QTextStream in(&file);
    std::vector<std::vector<double>> rows;

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        std::vector<double> row;
        row.reserve(parts.size());
        for (const QString& p : parts)
            row.push_back(p.toDouble());
        rows.push_back(std::move(row));
    }

    if (rows.empty())
        return nullptr;

    int fileRows = static_cast<int>(rows.size());
    int fileCols = static_cast<int>(rows[0].size());

    // Auto-detect if result needs transposing:
    // Simulator writes eta(i,j) where i=nx(=ASC cols), j=ny(=ASC rows)
    // So file has nx rows × ny cols, but grid expects nrows × ncols
    bool doTranspose = false;
    if (expectedRows > 0 && expectedCols > 0) {
        bool matchesDirect = (fileRows == expectedRows) &&
                             (fileCols == expectedCols || fileCols == expectedCols + 1);
        bool matchesTransposed = (fileRows == expectedCols || fileRows == expectedCols + 1) &&
                                 (fileCols == expectedRows || fileCols == expectedRows + 1);
        if (!matchesDirect && matchesTransposed)
            doTranspose = true;
    }

    auto f = std::make_shared<FrameData>();
    f->timestep = timestep;

    if (doTranspose) {
        // Transpose: swap rows↔cols
        f->rows = fileCols;
        f->cols = fileRows;
        f->values.resize(f->rows * f->cols, 0.0);
        f->minVal = std::numeric_limits<double>::max();
        f->maxVal = std::numeric_limits<double>::lowest();

        for (int r = 0; r < fileRows; ++r) {
            for (int c = 0; c < fileCols && c < static_cast<int>(rows[r].size()); ++c) {
                double v = rows[r][c];
                f->values[c * f->cols + r] = v;
                if (v < f->minVal) f->minVal = v;
                if (v > f->maxVal) f->maxVal = v;
            }
        }
    } else {
        f->rows = fileRows;
        f->cols = fileCols;
        f->values.resize(f->rows * f->cols, 0.0);
        f->minVal = std::numeric_limits<double>::max();
        f->maxVal = std::numeric_limits<double>::lowest();

        for (int r = 0; r < fileRows; ++r) {
            for (int c = 0; c < fileCols && c < static_cast<int>(rows[r].size()); ++c) {
                double v = rows[r][c];
                f->values[r * f->cols + c] = v;
                if (v < f->minVal) f->minVal = v;
                if (v > f->maxVal) f->maxVal = v;
            }
        }
    }

    if (f->minVal > f->maxVal) {
        f->minVal = 0.0;
        f->maxVal = 0.0;
    }

    return f;
}

void ResultDataset::evictCache()
{
    // Called with mutex held
    while (static_cast<int>(cache_.size()) >= cacheCapacity_ && !cacheOrder_.isEmpty()) {
        int oldest = cacheOrder_.takeFirst();
        cache_.remove(oldest);
    }
}
