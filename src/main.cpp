#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QImage>
#include <QKeySequence>
#include <QMainWindow>
#include <QMessageBox>
#include <QPainter>
#include <QStyle>
#include <QToolBar>
#include <clocale>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "dwg_document.h"
#include "viewer_widget.h"

namespace {
// Shared by both the initial argv[1] load and the toolbar's Open action.
// Reports failure via a message box rather than stderr since this is the
// interactive (non-headless) path.
bool loadIntoViewer(QMainWindow &window, ViewerWidget &viewer, const QString &path) {
    DwgDocument doc;
    if (!doc.loadFile(path.toStdString())) {
        QMessageBox::warning(&window, "Failed to open file",
                              QString::fromStdString(doc.errorMessage()));
        return false;
    }
    std::fprintf(stderr, "Loaded %zu shapes from %s\n", doc.shapes().size(),
                 path.toLocal8Bit().constData());
    viewer.setDocument(std::move(doc));
    window.setWindowTitle("DWG Viewer — " + path);
    return true;
}
} // namespace

int main(int argc, char *argv[]) {
#ifdef _WIN32
    // The exe is built with WIN32_EXECUTABLE (see CMakeLists.txt) so no
    // console window pops up when launched from Explorer/double-click.
    // That also means stdout/stderr have nowhere to go by default -- if we
    // were launched from an existing console (cmd/PowerShell) instead,
    // reattach to it so the --png path and the stderr diagnostics below
    // still show up there. AttachConsole() simply fails (no-op) when
    // there's no parent console, i.e. the double-click case.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *unused;
        freopen_s(&unused, "CONOUT$", "w", stdout);
        freopen_s(&unused, "CONOUT$", "w", stderr);
    }
#endif
    QApplication app(argc, argv);
    // QApplication adopts the system locale, but libdxfrw's DXF number
    // parser uses locale-sensitive strtod() (third_party/libdxfrw/src/intern/
    // dxfreader.cpp) and DXF files always use '.' as the decimal separator.
    // On any system locale with a different separator (e.g. pt_BR, most of
    // Europe) this silently breaks parsing of every coordinate/radius/angle,
    // making loadFile() fail. Force the numeric locale back to "C" for
    // parsing, independent of the UI locale.
    std::setlocale(LC_NUMERIC, "C");

    // Headless verification path: render straight to a PNG instead of
    // opening a window. Useful for CI, and for confirming the pipeline
    // works in an environment with no display attached. Still requires a
    // file argument, since there's no way to drive a file dialog headlessly.
    if (argc >= 3 && std::strcmp(argv[2], "--png") == 0) {
        const std::string path = argv[1];
        DwgDocument doc;
        if (!doc.loadFile(path)) {
            std::fprintf(stderr, "Error: %s\n", doc.errorMessage().c_str());
            return 1;
        }
        std::fprintf(stderr, "Loaded %zu shapes from %s\n", doc.shapes().size(), path.c_str());

        const std::string outPath = argc >= 4 ? argv[3] : "out.png";
        int w = 800, h = 600;
        if (argc >= 5) std::sscanf(argv[4], "%dx%d", &w, &h);

        ViewerWidget widget;
        widget.resize(w, h);
        widget.setDocument(std::move(doc));

        QImage image(w, h, QImage::Format_ARGB32);
        widget.render(&image);
        image.save(QString::fromStdString(outPath));
        std::fprintf(stderr, "Wrote %s (%dx%d)\n", outPath.c_str(), w, h);
        return 0;
    }

    QMainWindow window;
    window.setWindowTitle("DWG Viewer");
    auto *viewer = new ViewerWidget(&window);
    window.setCentralWidget(viewer);

    QToolBar *toolbar = window.addToolBar("Main");
    toolbar->setMovable(false);
    QAction *openAction = toolbar->addAction(
        window.style()->standardIcon(QStyle::SP_DialogOpenButton), "Open...");
    openAction->setShortcut(QKeySequence::Open);
    QObject::connect(openAction, &QAction::triggered, &window, [&window, viewer]() {
        const QString path = QFileDialog::getOpenFileName(
            &window, "Open Drawing", QString(),
            "Drawing files (*.dwg *.dxf);;All files (*)");
        if (path.isEmpty()) return;
        loadIntoViewer(window, *viewer, path);
    });

    window.resize(1000, 700);
    window.show();

    // A file argument is optional: with none, the window opens empty and
    // the user picks a file via the toolbar's Open action instead.
    if (argc >= 2) {
        loadIntoViewer(window, *viewer, QString::fromLocal8Bit(argv[1]));
    }

    return app.exec();
}
