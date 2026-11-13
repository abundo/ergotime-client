#pragma once

#include <QString>

// Version baked in by scripts/version.sh. A release is exactly X.Y.Z. A
// non-release appends '+' and the git SHA-1, and that suffix selects
// ~/.ergotime2-dev instead of ~/.ergotime2.
QString ergotimeVersion();
bool ergotimeIsReleaseBuild();
QString ergotimeDefaultDataDirName();
