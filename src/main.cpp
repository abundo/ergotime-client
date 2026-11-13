#include "mainwindow.hpp"

#include "version.hpp"

#include <QApplication>

#include <exception>
#include <iostream>

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("ErgoTime"));
  QApplication::setApplicationVersion(ergotimeVersion());
  QApplication::setOrganizationName(QStringLiteral("Abundo AB"));
  QApplication::setOrganizationDomain(QStringLiteral("abundo.se"));
  QApplication::setDesktopFileName(QStringLiteral("ergotime"));
  QApplication::setQuitOnLastWindowClosed(false);
  installLogSink();

  try {
    Settings settings;
    const Config cfg = settings.get();
    qInfo() << "config" << cfg.configPath << "db" << cfg.database;
    Store store(cfg.database);
    Engine engine(store, settings);
    Tracker tracker(store, cfg.idleTimeoutMs());
    MainWindow window(settings, store, engine, tracker);
    const int code = app.exec();
    return code;
  } catch (const std::exception &ex) {
    std::cerr << "fatal: " << ex.what() << '\n';
    return 1;
  }
}
