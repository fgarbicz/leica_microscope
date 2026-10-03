#pragma once
// A Leica .lif made by hand for the tests (iotest, uitest): everything LAS X
// puts in one file, with pixel values that say where they came from.
//
//   synthetic.lif
//     Folder A/
//       stack          13 x 7, two 12-bit channels (Red, Green), 3 z, 2 tiles,
//                      2 time points; confocal settings; tiles at (0,0) and (10,1)
//     Empty folder/    (no images: left out of the tree)
//     camera: 10x/0.25 4 x 3, 8-bit BGR, camera settings and a time stamp
//     broken           claims 100 x 100 pixels in 36 bytes (left out)

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>

#include <cstdint>

namespace liftest {

constexpr int W = 13, H = 7, Z = 3, M = 2, T = 2, C = 2;
// channel inside z inside tile inside time, as a confocal stores a tile scan
constexpr qint64 kPlane = qint64(W) * H * 2;
constexpr qint64 kCInc = kPlane, kZInc = C * kPlane, kMInc = Z * kZInc, kTInc = M * kMInc;

// The sample at (x, y) of channel c, slice z, time t, tile m.
inline uint16_t value(int c, int z, int t, int m, int x, int y)
{
    return uint16_t(c * 1000 + z * 300 + t * 100 + m * 50 + x * 3 + y);
}

// A .lif from an XML header and named memory blocks, the way LAS X lays one
// out (version 1 files have 32-bit block sizes).
inline QByteArray buildLif(const QString &xml, const QList<QPair<QString, QByteArray>> &blocks, bool version1 = false)
{
    QByteArray out;
    const auto u32 = [&](quint32 v) {
        for (int k = 0; k < 4; ++k)
            out.append(char((v >> (8 * k)) & 0xff));
    };
    const auto u64 = [&](quint64 v) {
        for (int k = 0; k < 8; ++k)
            out.append(char((v >> (8 * k)) & 0xff));
    };
    const auto utf16 = [&](const QString &s) {
        for (const QChar c : s) {
            out.append(char(c.unicode() & 0xff));
            out.append(char(c.unicode() >> 8));
        }
    };
    u32(0x70);
    u32(quint32(1 + 4 + xml.size() * 2));
    out.append(char(0x2A));
    u32(quint32(xml.size()));
    utf16(xml);
    for (const auto &b : blocks) {
        u32(0x70);
        u32(quint32(1 + (version1 ? 4 : 8) + 1 + 4 + b.first.size() * 2));
        out.append(char(0x2A));
        if (version1)
            u32(quint32(b.second.size()));
        else
            u64(quint64(b.second.size()));
        out.append(char(0x2A));
        u32(quint32(b.first.size()));
        utf16(b.first);
        out.append(b.second);
    }
    return out;
}

inline QString dimXml(int id, int n, qint64 inc, const QString &len, const QString &unit = QStringLiteral("m"))
{
    return QStringLiteral("<DimensionDescription DimID=\"%1\" NumberOfElements=\"%2\" Origin=\"0\" Length=\"%3\" "
                          "Unit=\"%4\" BitInc=\"0\" BytesInc=\"%5\"/>")
        .arg(id)
        .arg(n)
        .arg(len, unit)
        .arg(inc);
}

inline QString chanXml(int bits, qint64 inc, const QString &lut)
{
    return QStringLiteral("<ChannelDescription DataType=\"0\" ChannelTag=\"0\" Resolution=\"%1\" LUTName=\"%2\" "
                          "BytesInc=\"%3\" BitInc=\"0\"/>")
        .arg(bits)
        .arg(lut)
        .arg(inc);
}

// When the camera image was taken: 2024-01-02 03:04:05 UTC.
inline QDateTime syntheticTime() { return QDateTime::fromMSecsSinceEpoch(1704164645000LL); }

// The header XML; fills the pixel data of the stack and of the camera image.
inline QString syntheticXml(QByteArray &stack, QByteArray &camera)
{
    stack = QByteArray(int(T * kTInc), 0);
    for (int t = 0; t < T; ++t)
        for (int m = 0; m < M; ++m)
            for (int z = 0; z < Z; ++z)
                for (int c = 0; c < C; ++c)
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x) {
                            const qint64 o = t * kTInc + m * kMInc + z * kZInc + c * kCInc + (qint64(y) * W + x) * 2;
                            const uint16_t v = value(c, z, t, m, x, y);
                            stack[int(o)] = char(v & 0xff);
                            stack[int(o + 1)] = char(v >> 8);
                        }
    camera = QByteArray(4 * 3 * 3, 0);
    for (int i = 0; i < 12; ++i) {
        camera[3 * i] = char(10 + i);      // blue
        camera[3 * i + 1] = char(100 + i); // green
        camera[3 * i + 2] = char(200 + i); // red
    }
    const quint64 fileTime = quint64(syntheticTime().toMSecsSinceEpoch() + 11644473600000LL) * 10000ULL;
    return QStringLiteral("<LMSDataContainerHeader Version=\"2\"><Element Name=\"synthetic.lif\"><Data><Experiment/></Data>"
                          "<Memory Size=\"0\" MemoryBlockID=\"MemBlock_0\"/><Children>"
                          "<Element Name=\"Folder A\"><Data/><Memory Size=\"0\" MemoryBlockID=\"MemBlock_1\"/><Children>"
                          "<Element Name=\"stack\" UniqueID=\"abc\"><Data><Image>"
                          "<Attachment Name=\"TileScanInfo\" FlipX=\"0\" FlipY=\"0\" SwapXY=\"0\">"
                          "<Tile FieldX=\"0\" FieldY=\"0\" PosX=\"0.0\" PosY=\"0.0\"/>"
                          "<Tile FieldX=\"1\" FieldY=\"0\" PosX=\"0.000005\" PosY=\"0.0000005\"/></Attachment>"
                          "<ImageDescription><Channels>")
           + chanXml(12, 0, QStringLiteral("Red")) + chanXml(12, kCInc, QStringLiteral("Green"))
           + QStringLiteral("</Channels><Dimensions>")
           // the x axis spans W - 1 pixels of 0.5 um
           + dimXml(1, W, 2, QStringLiteral("6.000000e-06")) + dimXml(2, H, W * 2, QStringLiteral("3.000000e-06"))
           + dimXml(3, Z, kZInc, QStringLiteral("4.000000e-06")) + dimXml(10, M, kMInc, QStringLiteral("0"), QString())
           + dimXml(4, T, kTInc, QStringLiteral("2.500000e+00"), QStringLiteral("s"))
           + QStringLiteral("</Dimensions></ImageDescription>"
                            "<Attachment Name=\"HardwareSetting\" Software=\"LAS X 9\" SystemTypeName=\"TCS SP8\" "
                            "DataSourceTypeName=\"Confocal\"><ATLConfocalSettingDefinition ObjectiveName=\"HC PL APO 63x/1.40 OIL\" "
                            "Magnification=\"63\" NumericalAperture=\"1.4\" Zoom=\"2\" Pinhole=\"0.0001\" PinholeAiry=\"1\">"
                            "<DetectorList><Detector Name=\"HyD 1\" IsActive=\"1\" DyeName=\"Leica/DAPI\"/>"
                            "<Detector Name=\"PMT 2\" IsActive=\"0\"/><Detector Name=\"HyD 3\" IsActive=\"1\"/></DetectorList>"
                            "</ATLConfocalSettingDefinition></Attachment>"
                            "</Image></Data><Memory Size=\"%1\" MemoryBlockID=\"MemBlock_2\"/></Element>"
                            "<Element Name=\"Empty folder\"><Data/><Memory Size=\"0\" MemoryBlockID=\"MemBlock_3\"/></Element>"
                            "</Children></Element>")
                 .arg(stack.size())
           + QStringLiteral("<Element Name=\"camera: 10x/0.25\"><Data><Image><ImageDescription><Channels>")
           + chanXml(8, 0, QStringLiteral("Blue")) + chanXml(8, 1, QStringLiteral("Green"))
           + chanXml(8, 2, QStringLiteral("Red")) + QStringLiteral("</Channels><Dimensions>")
           + dimXml(1, 4, 3, QStringLiteral("3.000000e-06")) + dimXml(2, 3, 12, QStringLiteral("2.000000e-06"))
           + QStringLiteral("</Dimensions></ImageDescription><TimeStampList NumberOfTimeStamps=\"1\">%1 </TimeStampList>"
                            "<Attachment Name=\"HardwareSetting\" Software=\"LAS X 3.7\" SystemTypeName=\"AF 6000\" "
                            "DataSourceTypeName=\"Camera\"><ATLCameraSettingDefinition ObjectiveName=\"N PLAN    10x/0.25 DRY \" "
                            "Magnification=\"10\" NumericalAperture=\"0.25\" MicroscopeModel=\"DM2000\" BinningText=\"2.3MP\">"
                            "<WideFieldChannelConfigurator CameraName=\"DMC6200-1\"><WideFieldChannelInfo Active=\"1\" "
                            "ExposureTime=\"0.01\"/></WideFieldChannelConfigurator></ATLCameraSettingDefinition></Attachment>"
                            "</Image></Data><Memory Size=\"36\" MemoryBlockID=\"MemBlock_4\"/></Element>")
                 .arg(QString::number(fileTime, 16))
           // claims 100 x 100 pixels in 36 bytes
           + QStringLiteral("<Element Name=\"broken\"><Data><Image><ImageDescription><Channels>")
           + chanXml(8, 0, QStringLiteral("Gray")) + QStringLiteral("</Channels><Dimensions>")
           + dimXml(1, 100, 1, QStringLiteral("0")) + dimXml(2, 100, 100, QStringLiteral("0"))
           + QStringLiteral("</Dimensions></ImageDescription></Image></Data><Memory Size=\"36\" MemoryBlockID=\"MemBlock_5\"/>"
                            "</Element></Children></Element></LMSDataContainerHeader>");
}

inline QByteArray syntheticLif(const QString &xml, const QByteArray &stack, const QByteArray &camera)
{
    return buildLif(xml, {{QStringLiteral("MemBlock_0"), QByteArray()},
                          {QStringLiteral("MemBlock_1"), QByteArray()},
                          {QStringLiteral("MemBlock_2"), stack},
                          {QStringLiteral("MemBlock_3"), QByteArray()},
                          {QStringLiteral("MemBlock_4"), camera},
                          {QStringLiteral("MemBlock_5"), camera}});
}

} // namespace liftest
