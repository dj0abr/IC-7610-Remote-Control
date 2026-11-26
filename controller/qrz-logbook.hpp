#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

class QrzLogbook : public QObject
{
    Q_OBJECT

public:
    explicit QrzLogbook(const QString& apiKey,
                       const QUrl& endpoint = QUrl(QStringLiteral("https://logbook.qrz.com/api")));
    bool enabled() const { return !m_apiKey.isEmpty(); }
    void insert(const QString& call, const QString& stationCall, const QByteArray& adif);

signals:
    void status(const QString& call, const QString& state, const QString& text);

private:
    const QString m_apiKey;
    const QUrl m_endpoint;
    QNetworkAccessManager m_network;
};
