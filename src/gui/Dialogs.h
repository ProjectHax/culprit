// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <QString>

class QWidget;

namespace culprit::Dialogs {

// Themed replacements for QMessageBox / QInputDialog that get Culprit's title
// bar when integrated title bars are active.
bool question(QWidget* parent, const QString& title, const QString& text, const QString& yes = {}, const QString& no = {});
void warning(QWidget* parent, const QString& title, const QString& text);
bool getInt(QWidget* parent, const QString& title, const QString& label, int value, int min, int max, int* result);

} // namespace culprit::Dialogs
