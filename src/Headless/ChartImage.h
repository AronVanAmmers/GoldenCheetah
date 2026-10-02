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

#ifndef _GC_Headless_ChartImage_h
#define _GC_Headless_ChartImage_h 1

//
// What every command that draws a chart shares (ChartCommands.cpp): the
// image options (--as, --width, --height, --title, and --dark where the
// chart can switch) and the result that carries the image.
//

#include "HeadlessCommand.h"

#include <QSize>

namespace Headless {

QList<ParamSpec> imageParams(bool dark = true);
QSize imageSize(const CommandRequest &request);

// the image (empty when it could not be drawn, error then says why) as the
// result's payload, its format and size added to data
CommandResult imageResult(const QByteArray &bytes, const QString &error, const CommandRequest &request,
                          const QString &name, QJsonObject data);

} // namespace Headless

#endif
