#pragma once

#include <QDir>
#include <QStandardPaths>

inline QString ftAdifPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .absoluteFilePath(QStringLiteral("kmcontrol/kmcontrol-backend/ft4-ft8.adi"));
}
