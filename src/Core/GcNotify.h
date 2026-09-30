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

#ifndef _GC_GcNotify_h
#define _GC_GcNotify_h 1

#include <QString>
#include <QMessageBox>

class QWidget;

//
// Telling the user about a problem from code that also runs without a
// window, for the command line and the REST server. In the GUI these are
// the usual message boxes. Headless there is nobody to click a box away and
// it would block forever, so the message is logged instead.
//
// Only for notifications: code that behaves differently without a user
// (asking a question, waiting, downloading) checks isHeadless() itself.
//
namespace GcNotify
{
    // as QMessageBox::critical and QMessageBox::warning
    void critical(QWidget *parent, const QString &title, const QString &text);
    void warning(QWidget *parent, const QString &title, const QString &text);

    // a box with an icon, a text and an informative text, as the file
    // readers and writers show
    void message(QMessageBox::Icon icon, const QString &text, const QString &informative = QString());
}

#endif
