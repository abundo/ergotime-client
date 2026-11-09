#pragma once

#include "domain.hpp"
#include "settings.hpp"
#include "store.hpp"

#include <QWidget>

#include <optional>

class QTableWidget;

// Returns nullopt when the user cancels.
std::optional<Config> showSettingsDialog(QWidget *parent, const Config &cfg);

// Returns true when a report was saved or deleted.
bool showReportDialog(QWidget *parent, Store &store, const Report *existing,
                      const QDate &defaultDate);

// Project column stays one line. The comment column edits as several lines.
void configureProjectCommentTable(QTableWidget *table);
