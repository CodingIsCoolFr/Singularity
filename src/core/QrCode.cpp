#include "core/QrCode.h"

#include <QPainter>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace QrCode {

namespace {

// Level M, from the QR specification, indexed by version. Index 0 is unused.
constexpr int EccPerBlock[41] = {
    -1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
    26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28};
constexpr int EccBlocks[41] = {
    -1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16,
    17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49};

// Level M is 0 in the two format bits.
constexpr int FormatBitsM = 0;

int rawDataModules(int version)
{
    int result = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const int alignCount = version / 7 + 2;
        result -= (25 * alignCount - 10) * alignCount - 55;
        if (version >= 7)
            result -= 36;
    }
    return result;
}

int dataCodewords(int version)
{
    return rawDataModules(version) / 8 - EccPerBlock[version] * EccBlocks[version];
}

bool bit(long value, int index)
{
    return ((value >> index) & 1) != 0;
}

// Multiplication in GF(2^8) with the QR polynomial 0x11D.
std::uint8_t gfMultiply(std::uint8_t x, std::uint8_t y)
{
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<std::uint8_t>(z);
}

std::vector<std::uint8_t> reedSolomonDivisor(int degree)
{
    std::vector<std::uint8_t> result(static_cast<size_t>(degree));
    result.back() = 1;
    std::uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (size_t j = 0; j < result.size(); ++j) {
            result[j] = gfMultiply(result[j], root);
            if (j + 1 < result.size())
                result[j] ^= result[j + 1];
        }
        root = gfMultiply(root, 0x02);
    }
    return result;
}

std::vector<std::uint8_t> reedSolomonRemainder(const std::vector<std::uint8_t> &data,
                                               const std::vector<std::uint8_t> &divisor)
{
    std::vector<std::uint8_t> result(divisor.size());
    for (std::uint8_t value : data) {
        const std::uint8_t factor = value ^ result.front();
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < result.size(); ++i)
            result[i] ^= gfMultiply(divisor[i], factor);
    }
    return result;
}

class Builder
{
public:
    explicit Builder(int version)
        : m_version(version)
        , m_size(version * 4 + 17)
        , m_modules(static_cast<size_t>(m_size), std::vector<bool>(static_cast<size_t>(m_size)))
        , m_function(static_cast<size_t>(m_size), std::vector<bool>(static_cast<size_t>(m_size)))
    {
    }

    Matrix build(const std::vector<std::uint8_t> &data)
    {
        drawFunctionPatterns();
        drawCodewords(addEccAndInterleave(data));

        // Try every mask and keep the one that scores lowest, as the
        // specification asks. A code with big blocks of one colour or
        // shapes that look like the corner markers is harder to read.
        int bestMask = 0;
        long bestPenalty = LONG_MAX;
        for (int mask = 0; mask < 8; ++mask) {
            applyMask(mask);
            drawFormatBits(mask);
            const long penalty = penaltyScore();
            if (penalty < bestPenalty) {
                bestPenalty = penalty;
                bestMask = mask;
            }
            applyMask(mask);   // XOR again undoes it
        }
        applyMask(bestMask);
        drawFormatBits(bestMask);

        Matrix out(m_size, QVector<bool>(m_size));
        for (int y = 0; y < m_size; ++y)
            for (int x = 0; x < m_size; ++x)
                out[y][x] = m_modules[y][x];
        return out;
    }

private:
    void setFunction(int x, int y, bool dark)
    {
        m_modules[y][x] = dark;
        m_function[y][x] = true;
    }

    std::vector<int> alignmentPositions() const
    {
        if (m_version == 1)
            return {};
        const int count = m_version / 7 + 2;
        const int step = (m_version == 32) ? 26 : (m_version * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
        std::vector<int> result;
        for (int i = 0, pos = m_size - 7; i < count - 1; ++i, pos -= step)
            result.insert(result.begin(), pos);
        result.insert(result.begin(), 6);
        return result;
    }

    void drawFinder(int x, int y)
    {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int distance = std::max(std::abs(dx), std::abs(dy));
                const int xx = x + dx;
                const int yy = y + dy;
                if (xx >= 0 && xx < m_size && yy >= 0 && yy < m_size)
                    setFunction(xx, yy, distance != 2 && distance != 4);
            }
        }
    }

    void drawAlignment(int x, int y)
    {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                setFunction(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }

    void drawFunctionPatterns()
    {
        for (int i = 0; i < m_size; ++i) {
            setFunction(6, i, i % 2 == 0);
            setFunction(i, 6, i % 2 == 0);
        }

        drawFinder(3, 3);
        drawFinder(m_size - 4, 3);
        drawFinder(3, m_size - 4);

        const std::vector<int> align = alignmentPositions();
        const int count = static_cast<int>(align.size());
        for (int i = 0; i < count; ++i) {
            for (int j = 0; j < count; ++j) {
                // The three corners already hold finder patterns.
                if ((i == 0 && j == 0) || (i == 0 && j == count - 1) || (i == count - 1 && j == 0))
                    continue;
                drawAlignment(align[i], align[j]);
            }
        }

        drawFormatBits(0);   // reserves the area; the real mask is drawn later
        drawVersion();
    }

    void drawFormatBits(int mask)
    {
        const int data = FormatBitsM << 3 | mask;
        int remainder = data;
        for (int i = 0; i < 10; ++i)
            remainder = (remainder << 1) ^ ((remainder >> 9) * 0x537);
        const int bits = (data << 10 | remainder) ^ 0x5412;

        for (int i = 0; i <= 5; ++i)
            setFunction(8, i, bit(bits, i));
        setFunction(8, 7, bit(bits, 6));
        setFunction(8, 8, bit(bits, 7));
        setFunction(7, 8, bit(bits, 8));
        for (int i = 9; i < 15; ++i)
            setFunction(14 - i, 8, bit(bits, i));

        for (int i = 0; i < 8; ++i)
            setFunction(m_size - 1 - i, 8, bit(bits, i));
        for (int i = 8; i < 15; ++i)
            setFunction(8, m_size - 15 + i, bit(bits, i));
        setFunction(8, m_size - 8, true);   // always dark
    }

    void drawVersion()
    {
        if (m_version < 7)
            return;
        int remainder = m_version;
        for (int i = 0; i < 12; ++i)
            remainder = (remainder << 1) ^ ((remainder >> 11) * 0x1F25);
        const long bits = static_cast<long>(m_version) << 12 | remainder;
        for (int i = 0; i < 18; ++i) {
            const bool dark = bit(bits, i);
            const int a = m_size - 11 + i % 3;
            const int b = i / 3;
            setFunction(a, b, dark);
            setFunction(b, a, dark);
        }
    }

    std::vector<std::uint8_t> addEccAndInterleave(const std::vector<std::uint8_t> &data) const
    {
        const int blocks = EccBlocks[m_version];
        const int eccLength = EccPerBlock[m_version];
        const int rawCodewords = rawDataModules(m_version) / 8;
        const int shortBlocks = blocks - rawCodewords % blocks;
        const int shortLength = rawCodewords / blocks;

        std::vector<std::vector<std::uint8_t>> pieces;
        const std::vector<std::uint8_t> divisor = reedSolomonDivisor(eccLength);
        for (int i = 0, k = 0; i < blocks; ++i) {
            const int length = shortLength - eccLength + (i < shortBlocks ? 0 : 1);
            std::vector<std::uint8_t> piece(data.begin() + k, data.begin() + k + length);
            k += length;
            const std::vector<std::uint8_t> ecc = reedSolomonRemainder(piece, divisor);
            if (i < shortBlocks)
                piece.push_back(0);
            piece.insert(piece.end(), ecc.begin(), ecc.end());
            pieces.push_back(std::move(piece));
        }

        std::vector<std::uint8_t> result;
        for (size_t i = 0; i < pieces.front().size(); ++i) {
            for (size_t j = 0; j < pieces.size(); ++j) {
                // The padding byte added to short blocks is not sent.
                if (i != static_cast<size_t>(shortLength - eccLength) || j >= static_cast<size_t>(shortBlocks))
                    result.push_back(pieces[j][i]);
            }
        }
        return result;
    }

    void drawCodewords(const std::vector<std::uint8_t> &data)
    {
        size_t i = 0;
        const size_t total = data.size() * 8;
        for (int right = m_size - 1; right >= 1; right -= 2) {
            if (right == 6)
                right = 5;
            for (int vert = 0; vert < m_size; ++vert) {
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? m_size - 1 - vert : vert;
                    if (!m_function[y][x] && i < total) {
                        m_modules[y][x] = bit(data[i >> 3], 7 - static_cast<int>(i & 7));
                        ++i;
                    }
                }
            }
        }
    }

    void applyMask(int mask)
    {
        for (int y = 0; y < m_size; ++y) {
            for (int x = 0; x < m_size; ++x) {
                bool invert = false;
                switch (mask) {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                case 7: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (invert && !m_function[y][x])
                    m_modules[y][x] = !m_modules[y][x];
            }
        }
    }

    bool dark(int x, int y) const { return m_modules[y][x]; }

    // The four penalty rules from the specification.
    long penaltyScore() const
    {
        long result = 0;

        // 1: runs of five or more of one colour, across and down.
        for (int pass = 0; pass < 2; ++pass) {
            for (int a = 0; a < m_size; ++a) {
                int run = 0;
                bool colour = false;
                for (int b = 0; b < m_size; ++b) {
                    const bool here = pass == 0 ? dark(b, a) : dark(a, b);
                    if (b == 0 || here != colour) {
                        if (run >= 5)
                            result += 3 + (run - 5);
                        colour = here;
                        run = 1;
                    } else {
                        ++run;
                    }
                }
                if (run >= 5)
                    result += 3 + (run - 5);
            }
        }

        // 2: every 2x2 block of one colour.
        for (int y = 0; y < m_size - 1; ++y) {
            for (int x = 0; x < m_size - 1; ++x) {
                const bool c = dark(x, y);
                if (c == dark(x + 1, y) && c == dark(x, y + 1) && c == dark(x + 1, y + 1))
                    result += 3;
            }
        }

        // 3: anything that looks like a finder pattern, 1:1:3:1:1 with four
        // light modules on one side.
        static const bool patternA[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
        static const bool patternB[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
        for (int pass = 0; pass < 2; ++pass) {
            for (int a = 0; a < m_size; ++a) {
                for (int b = 0; b + 11 <= m_size; ++b) {
                    bool matchA = true;
                    bool matchB = true;
                    for (int k = 0; k < 11 && (matchA || matchB); ++k) {
                        const bool here = pass == 0 ? dark(b + k, a) : dark(a, b + k);
                        matchA = matchA && here == patternA[k];
                        matchB = matchB && here == patternB[k];
                    }
                    if (matchA)
                        result += 40;
                    if (matchB)
                        result += 40;
                }
            }
        }

        // 4: how far the dark share is from half.
        int darkCount = 0;
        for (int y = 0; y < m_size; ++y)
            for (int x = 0; x < m_size; ++x)
                darkCount += dark(x, y) ? 1 : 0;
        const int total = m_size * m_size;
        const int k = (std::abs(darkCount * 20 - total * 10) + total - 1) / total - 1;
        result += static_cast<long>(std::max(0, k)) * 10;

        return result;
    }

    int m_version;
    int m_size;
    std::vector<std::vector<bool>> m_modules;
    std::vector<std::vector<bool>> m_function;
};

} // namespace

Matrix encode(const QByteArray &data)
{
    const int length = static_cast<int>(data.size());

    int version = 1;
    for (; version <= 40; ++version) {
        const int countBits = version <= 9 ? 8 : 16;
        if (length >= (1 << countBits))
            continue;
        if (4 + countBits + 8 * length <= dataCodewords(version) * 8)
            break;
    }
    if (version > 40)
        return {};

    // Byte mode: the mode, the length, then the bytes themselves.
    std::vector<bool> bits;
    const auto append = [&bits](unsigned value, int count) {
        for (int i = count - 1; i >= 0; --i)
            bits.push_back(((value >> i) & 1) != 0);
    };
    append(0x4, 4);
    append(static_cast<unsigned>(length), version <= 9 ? 8 : 16);
    for (char c : data)
        append(static_cast<std::uint8_t>(c), 8);

    // A terminator of up to four zeros, then whole bytes, then the two
    // alternating pad bytes the specification names.
    const size_t capacity = static_cast<size_t>(dataCodewords(version)) * 8;
    append(0, static_cast<int>(std::min<size_t>(4, capacity - bits.size())));
    append(0, static_cast<int>((8 - bits.size() % 8) % 8));
    for (std::uint8_t pad = 0xEC; bits.size() < capacity; pad ^= 0xEC ^ 0x11)
        append(pad, 8);

    std::vector<std::uint8_t> codewords(bits.size() / 8);
    for (size_t i = 0; i < bits.size(); ++i)
        codewords[i >> 3] |= static_cast<std::uint8_t>(bits[i] ? 1 : 0) << (7 - (i & 7));

    return Builder(version).build(codewords);
}

QImage render(const Matrix &modules, int moduleSize)
{
    if (modules.isEmpty() || moduleSize <= 0)
        return {};

    constexpr int QuietZone = 4;
    const int count = static_cast<int>(modules.size());
    const int side = (count + QuietZone * 2) * moduleSize;

    QImage image(side, side, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    for (int y = 0; y < count; ++y) {
        for (int x = 0; x < count; ++x) {
            if (modules[y][x]) {
                painter.drawRect((x + QuietZone) * moduleSize, (y + QuietZone) * moduleSize, moduleSize,
                                 moduleSize);
            }
        }
    }
    return image;
}

} // namespace QrCode
