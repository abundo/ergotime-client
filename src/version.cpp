#include "version.hpp"

#include <QStringList>

namespace ergotime {
extern const char version[];
}

QString ergotimeVersion() { return QString::fromUtf8(ergotime::version); }

bool ergotimeIsReleaseBuild() {
  const QString version = ergotimeVersion();
  if (version.contains(QLatin1Char('+'))) {
    return false;
  }
  const QStringList parts = version.split(QLatin1Char('.'));
  if (parts.size() != 3) {
    return false;
  }
  for (const QString &part : parts) {
    if (part.isEmpty()) {
      return false;
    }
    for (const QChar c : part) {
      if (!c.isDigit()) {
        return false;
      }
    }
  }
  return true;
}

QString ergotimeDefaultDataDirName() {
  return ergotimeIsReleaseBuild() ? QStringLiteral(".ergotime2")
                                  : QStringLiteral(".ergotime2-dev");
}
