// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/Dialogs.h"

#include "gui/ThemeManager.h"
#include "gui/widgets/WindowChrome.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace culprit::Dialogs {

namespace {

enum class Icon { Question, Warning };

// Round badge with "?" or "!" drawn in the accent / warning colour.
class Badge : public QWidget {
public:
    explicit Badge(Icon icon) : icon_(icon) { setFixedSize(36, 36); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const ThemeTokens& t = ThemeManager::instance().tokens();
        QColor c = icon_ == Icon::Warning ? t.warning : t.accent;
        QColor bg = c;
        bg.setAlpha(t.dark ? 55 : 35);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawEllipse(rect().adjusted(1, 1, -1, -1));
        QFont f = font();
        f.setBold(true);
        f.setPixelSize(20);
        p.setFont(f);
        p.setPen(c);
        p.drawText(rect(), Qt::AlignCenter, icon_ == Icon::Warning ? QStringLiteral("!") : QStringLiteral("?"));
    }

private:
    Icon icon_;
};

QHBoxLayout* messageRow(Icon icon, const QString& text)
{
    auto* row = new QHBoxLayout;
    row->setSpacing(14);
    row->addWidget(new Badge(icon), 0, Qt::AlignTop);
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setMinimumWidth(360);
    row->addWidget(label, 1);
    return row;
}

QHBoxLayout* buttonRow(ChromeDialog& dlg, const QString& okText, const QString& cancelText)
{
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    if (!cancelText.isEmpty()) {
        auto* cancel = new QPushButton(cancelText);
        QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
        row->addWidget(cancel);
    }
    auto* ok = new QPushButton(okText);
    ok->setObjectName(QStringLiteral("primary"));
    ok->setDefault(true);
    QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    row->addWidget(ok);
    return row;
}

} // namespace

bool question(QWidget* parent, const QString& title, const QString& text, const QString& yes, const QString& no)
{
    ChromeDialog dlg(parent, title);
    dlg.body()->addLayout(messageRow(Icon::Question, text));
    dlg.body()->addSpacing(6);
    dlg.body()->addLayout(buttonRow(dlg, yes.isEmpty() ? QObject::tr("Yes") : yes, no.isEmpty() ? QObject::tr("No") : no));
    return dlg.exec() == QDialog::Accepted;
}

void warning(QWidget* parent, const QString& title, const QString& text)
{
    ChromeDialog dlg(parent, title);
    dlg.body()->addLayout(messageRow(Icon::Warning, text));
    dlg.body()->addSpacing(6);
    dlg.body()->addLayout(buttonRow(dlg, QObject::tr("OK"), QString()));
    dlg.exec();
}

bool getInt(QWidget* parent, const QString& title, const QString& label, int value, int min, int max, int* result)
{
    ChromeDialog dlg(parent, title);
    auto* l = new QLabel(label);
    l->setWordWrap(true);
    l->setMinimumWidth(360);
    auto* spin = new QSpinBox;
    spin->setRange(min, max);
    spin->setValue(value);
    dlg.body()->addWidget(l);
    dlg.body()->addWidget(spin);
    dlg.body()->addSpacing(6);
    dlg.body()->addLayout(buttonRow(dlg, QObject::tr("OK"), QObject::tr("Cancel")));
    spin->setFocus();
    spin->selectAll();
    if (dlg.exec() != QDialog::Accepted)
        return false;
    *result = spin->value();
    return true;
}

} // namespace culprit::Dialogs
