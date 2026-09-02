#include <QApplication>
#include <QMainWindow>
#include <QMessageBox>
#include <QImage>
#include <QPainter>
#include <clocale>
#include <cstdio>
#include <cstring>

#include "dwg_document.h"
#include "viewer_widget.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    // QApplication adopts the system locale, but libdxfrw's DXF number
    // parser uses locale-sensitive strtod() (third_party/libdxfrw/src/intern/
    // dxfreader.cpp) and DXF files always use '.' as the decimal separator.
    // On any system locale with a different separator (e.g. pt_BR, most of
    // Europe) this silently breaks parsing of every coordinate/radius/angle,
    // making loadFile() fail. Force the numeric locale back to "C" for
    // parsing, independent of the UI locale.
    std::setlocale(LC_NUMERIC, "C");

    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: %s <file.dxf|file.dwg> [--png out.png WIDTHxHEIGHT]\n",
            argv[0]);
        return 1;
    }

    const std::string path = argv[1];
    DwgDocument doc;
    if (!doc.loadFile(path)) {
        std::fprintf(stderr, "Error: %s\n", doc.errorMessage().c_str());
        return 1;
    }
    std::fprintf(stderr, "Loaded %zu shapes from %s\n", doc.shapes().size(), path.c_str());

    // Headless verification path: render straight to a PNG instead of
    // opening a window. Useful for CI, and for confirming the pipeline
    // works in an environment with no display attached.
    if (argc >= 3 && std::strcmp(argv[2], "--png") == 0) {
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
    window.setWindowTitle(QString::fromStdString("DWG Viewer — " + path));
    auto *viewer = new ViewerWidget(&window);
    viewer->setDocument(std::move(doc));
    window.setCentralWidget(viewer);
    window.resize(1000, 700);
    window.show();

    return app.exec();
}
