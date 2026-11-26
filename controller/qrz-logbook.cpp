#include "qrz-logbook.hpp"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>

namespace
{
constexpr int uploadTimeoutMs = 10000;
}

QrzLogbook::QrzLogbook(const QString& apiKey, const QUrl& endpoint)
    : m_apiKey(apiKey.trimmed()), m_endpoint(endpoint)
{
}

void QrzLogbook::insert(const QString& call, const QString& stationCall, const QByteArray& adif)
{
    if (!enabled())
    {
        return;
    }

    QNetworkRequest request(m_endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("User-Agent", "kmcontrol/1.0 (" + stationCall.toLatin1() + ')');
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    const auto body = "KEY=" + QUrl::toPercentEncoding(m_apiKey) + "&ACTION=INSERT&ADIF=" +
        QUrl::toPercentEncoding(QString::fromLatin1(adif.trimmed()));
    auto* reply = m_network.post(request, body);
    QTimer::singleShot(uploadTimeoutMs, reply, [reply] {
        if (reply->isRunning())
        {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, call] {
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        auto form = reply->isReadable() ? QString::fromUtf8(reply->readAll().trimmed()) : QString{};
        reply->deleteLater();

        // QRZ responds with form-encoded RESULT and REASON fields.
        form.replace(QLatin1Char('+'), QStringLiteral("%20"));
        const QUrlQuery result(form);
        const auto code = result.queryItemValue(QStringLiteral("RESULT"), QUrl::FullyDecoded);
        const auto reason = result.queryItemValue(QStringLiteral("REASON"), QUrl::FullyDecoded);
        const bool success = error == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300;
        if (success && code == QStringLiteral("OK"))
        {
            emit status(call, QStringLiteral("uploaded"), QStringLiteral("QSO an QRZ.com übertragen."));
        }
        else if (success && code == QStringLiteral("FAIL") && reason.contains(QStringLiteral("duplicate"), Qt::CaseInsensitive))
        {
            emit status(call, QStringLiteral("duplicate"), QStringLiteral("QSO ist bereits im QRZ-Logbuch vorhanden."));
        }
        else if (httpStatus == 401 || httpStatus == 403 || code == QStringLiteral("AUTH") ||
                 reason.contains(QStringLiteral("key"), Qt::CaseInsensitive))
        {
            emit status(call, QStringLiteral("error"), QStringLiteral("QRZ.com hat den Logbuch-Zugriff abgelehnt."));
        }
        else
        {
            emit status(call, QStringLiteral("error"), QStringLiteral("QRZ-Upload nicht bestätigt. QSO im ADIF-Logbuch gespeichert."));
        }
    });
    emit status(call, QStringLiteral("pending"), QStringLiteral("QSO wird an QRZ.com übertragen …"));
}
