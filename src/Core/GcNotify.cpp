/*
 * Copyright (c) 2026 Aron van Ammers
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "GcNotify.h"
#include "Context.h"

#include <QDebug>

static void
logged(const QString &title, const QString &text)
{
    if (text.isEmpty()) qWarning().noquote() << title;
    else qWarning().noquote() << title + ": " + text;
}

void
GcNotify::critical(QWidget *parent, const QString &title, const QString &text)
{
    if (GlobalContext::isHeadless()) logged(title, text);
    else QMessageBox::critical(parent, title, text);
}

void
GcNotify::warning(QWidget *parent, const QString &title, const QString &text)
{
    if (GlobalContext::isHeadless()) logged(title, text);
    else QMessageBox::warning(parent, title, text);
}

void
GcNotify::message(QMessageBox::Icon icon, const QString &text, const QString &informative)
{
    if (GlobalContext::isHeadless()) {
        logged(text, informative);
        return;
    }
    QMessageBox msgBox;
    msgBox.setIcon(icon);
    msgBox.setText(text);
    if (!informative.isEmpty()) msgBox.setInformativeText(informative);
    msgBox.exec();
}
