#pragma once

#include <QByteArray>
#include <QImage>
#include <QVector>

// Makes QR codes, for signing in by scanning one with the Discord phone app.
//
// Written here rather than pulled in: it is one standard algorithm, the only
// input is a short address, and a library for it would be the largest piece
// of third party code in the sign-in path. Byte mode, error correction level
// M, versions 1 to 40, all eight masks scored the standard way.
namespace QrCode {

// Rows of modules, true for dark. Empty if the data does not fit.
using Matrix = QVector<QVector<bool>>;
Matrix encode(const QByteArray &data);

// Dark modules on white with the standard four-module quiet zone. A dark code
// on a light ground is the one every camera app reads; inverted codes are
// still not universal.
QImage render(const Matrix &modules, int moduleSize);

} // namespace QrCode
