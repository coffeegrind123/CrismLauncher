#include "OptiFineUtils.h"

#include <QRegularExpression>
#include <QUrl>

namespace OptiFine {

namespace {
const QString s_baseUrl = QStringLiteral("https://optifine.net/");
const QString s_mirrorUrl = QStringLiteral("https://bmclapi2.bangbang93.com/optifine/%1/%2/%3");

// OptiFine_<mc>_<type>_<patch>.jar, e.g. OptiFine_1.12.2_HD_U_F5.jar -> 1.12.2, HD_U, F5
const QRegularExpression s_release(QStringLiteral("^OptiFine_([0-9][0-9a-z.-]*)_(HD(?:_U)?)_([A-Z][0-9]+)\\.jar$"));

// preview_OptiFine_<mc>_<type>_<patch>.jar, where BMCLAPI folds the target release into the type,
// e.g. preview_OptiFine_1.20.1_HD_U_I5_pre4.jar -> 1.20.1, HD_U_I5, pre4. <mc> may be a snapshot (21w08b).
const QRegularExpression s_preview(QStringLiteral("^preview_OptiFine_([0-9][0-9a-z.-]*)_(HD(?:_U)?_[A-Z][0-9]+)_(pre[0-9]+)\\.jar$"));

const QRegularExpression s_downloadLink(QStringLiteral("downloadx\\?f=[^\"'\\s<>]+"));
}  // namespace

bool isOptiFineFile(const QString& fileName)
{
    return s_release.match(fileName).hasMatch() || s_preview.match(fileName).hasMatch();
}

QString downloadPageUrl(const QString& fileName)
{
    return s_baseUrl + "adloadx?f=" + QString::fromUtf8(QUrl::toPercentEncoding(fileName));
}

QString parseDownloadPage(const QByteArray& html)
{
    const auto match = s_downloadLink.match(QString::fromLatin1(html));
    if (!match.hasMatch()) {
        return {};
    }

    auto link = match.captured(0);
    link.replace("&amp;", "&");
    return s_baseUrl + link;
}

QString mirrorUrl(const QString& fileName)
{
    auto match = s_release.match(fileName);
    if (!match.hasMatch()) {
        match = s_preview.match(fileName);
    }
    if (!match.hasMatch()) {
        return {};
    }

    // BMCLAPI keys the first release of a minor version as x.y.0
    auto mcVersion = match.captured(1);
    if (mcVersion == "1.8" || mcVersion == "1.9") {
        mcVersion += ".0";
    }

    return s_mirrorUrl.arg(mcVersion, match.captured(2), match.captured(3));
}

}  // namespace OptiFine
