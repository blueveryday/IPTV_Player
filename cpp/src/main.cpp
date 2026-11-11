#include "mainwindow.h"
#include "util.h"
#include "widgets.h"

#include <QApplication>
#include <QFont>
#include <QStyleFactory>
#include <clocale>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    // libmpv 要求 LC_NUMERIC 为 "C"
    std::setlocale(LC_NUMERIC, "C");
    QApplication::setApplicationName(iptv::kAppTitle);
    if (QStyleFactory::keys().contains("Fusion", Qt::CaseInsensitive))
        app.setStyle(QStyleFactory::create("Fusion"));
    QFont f = app.font();
    f.setFamilies({"Microsoft YaHei UI", "Noto Sans CJK SC", "WenQuanYi Micro Hei", f.family()});
    f.setPointSize(9);
    app.setFont(f);
    app.setStyleSheet(iptv::darkStyleSheet());

    iptv::MainWindow w;
    w.show();
    return app.exec();
}
