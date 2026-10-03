#pragma once

#include <QByteArray>
#include <QString>

// OptiFine is only distributed through optifine.net's ad-gated pages. A download takes two steps:
//
//   GET optifine.net/adloadx?f=<file>   -> HTML containing  downloadx?f=<file>&x=<token>
//   GET optifine.net/downloadx?f=...    -> the jar
//
// The token rotates over time, so the page has to be fetched right before the download. BMCLAPI
// mirrors most releases at a static URL and is used when the page can't be scraped; it lacks
// some old versions (e.g. 1.6.4), so it is the fallback rather than the primary source.
namespace OptiFine {

/** True for release (OptiFine_1.12.2_HD_U_F5.jar) and preview (preview_OptiFine_1.20.1_HD_U_I5_pre4.jar) jars. */
bool isOptiFineFile(const QString& fileName);

/** The ad page for `fileName` that carries the current download token. */
QString downloadPageUrl(const QString& fileName);

/** Extracts the tokenized download URL from a downloadPageUrl() response; empty if there is none. */
QString parseDownloadPage(const QByteArray& html);

/** The BMCLAPI mirror URL for `fileName`; empty if the name isn't an OptiFine jar. */
QString mirrorUrl(const QString& fileName);

}  // namespace OptiFine
