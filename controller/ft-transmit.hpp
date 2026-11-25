#pragma once

#include "ft_mode.hpp"

#include <QByteArray>
#include <QString>

QByteArray encodeFtAudio(FtMode mode, const QString& message, int audioHz, int level);
