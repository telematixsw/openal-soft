/*
 * 2-channel UHJ Encoder
 *
 * Copyright (c) Chris Robinson <chris.kcat@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"

#include <array>
#include <cstring>
#include <inttypes.h>
#include <memory>
#include <stddef.h>
#include <string>
#include <utility>
#include <vector>

#include "almalloc.h"
#include "alspan.h"
#include "math_defs.h"
#include "opthelpers.h"
#include "phase_shifter.h"
#include "vector.h"

#include "sndfile.h"

#include "win_main_utf8.h"


namespace {

struct SndFileDeleter {
    void operator()(SNDFILE *sndfile) { sf_close(sndfile); }
};
using SndFilePtr = std::unique_ptr<SNDFILE,SndFileDeleter>;


using uint = unsigned int;

constexpr uint BufferLineSize{1024};

using FloatBufferLine = std::array<float,BufferLineSize>;
using FloatBufferSpan = al::span<float,BufferLineSize>;


struct UhjEncoder {
    constexpr static size_t sFilterDelay{1024};

    /* Delays and processing storage for the unfiltered signal. */
    alignas(16) std::array<float,BufferLineSize+sFilterDelay> mS{};
    alignas(16) std::array<float,BufferLineSize+sFilterDelay> mD{};

    /* History for the FIR filter. */
    alignas(16) std::array<float,sFilterDelay*2 - 1> mWXHistory{};

    alignas(16) std::array<float,BufferLineSize + sFilterDelay*2> mTemp{};

    void encode(const FloatBufferSpan LeftOut, const FloatBufferSpan RightOut,
        const FloatBufferLine *InSamples, const size_t SamplesToDo);

    DEF_NEWDEL(UhjEncoder)
};

const PhaseShifterT<UhjEncoder::sFilterDelay*2> PShift{};


/* Encoding UHJ from B-Format is done as:
 *
 * S = 0.9396926*W + 0.1855740*X
 * D = j(-0.3420201*W + 0.5098604*X) + 0.6554516*Y
 *
 * Left = (S + D)/2.0
 * Right = (S - D)/2.0
 * T = j(-0.1432*W + 0.6512*X) - 0.7071068*Y
 * Q = 0.9772*Z
 *
 * where j is a wide-band +90 degree phase shift. T is excluded from 2-channel
 * output, and Q is excluded from 2- and 3-channel output.
 */
void UhjEncoder::encode(const FloatBufferSpan LeftOut, const FloatBufferSpan RightOut,
    const FloatBufferLine *InSamples, const size_t SamplesToDo)
{
    float *RESTRICT left{al::assume_aligned<16>(LeftOut.data())};
    float *RESTRICT right{al::assume_aligned<16>(RightOut.data())};

    const float *RESTRICT winput{al::assume_aligned<16>(InSamples[0].data())};
    const float *RESTRICT xinput{al::assume_aligned<16>(InSamples[1].data())};
    const float *RESTRICT yinput{al::assume_aligned<16>(InSamples[2].data())};

    /* Combine the previously delayed S/D signal with the input. */

    /* S = 0.9396926*W + 0.1855740*X */
    auto miditer = mS.begin() + sFilterDelay;
    std::transform(winput, winput+SamplesToDo, xinput, miditer,
        [](const float w, const float x) noexcept -> float
        { return 0.9396926f*w + 0.1855740f*x; });

    /* D = 0.6554516*Y */
    auto sideiter = mD.begin() + sFilterDelay;
    std::transform(yinput, yinput+SamplesToDo, sideiter,
        [](const float y) noexcept -> float { return 0.6554516f*y; });

    /* D += j(-0.3420201*W + 0.5098604*X) */
    auto tmpiter = std::copy(mWXHistory.cbegin(), mWXHistory.cend(), mTemp.begin());
    std::transform(winput, winput+SamplesToDo, xinput, tmpiter,
        [](const float w, const float x) noexcept -> float
        { return -0.3420201f*w + 0.5098604f*x; });
    std::copy_n(mTemp.cbegin()+SamplesToDo, mWXHistory.size(), mWXHistory.begin());
    PShift.processAccum({mD.data(), SamplesToDo}, mTemp.data());

    /* Left = (S + D)/2.0 */
    for(size_t i{0};i < SamplesToDo;i++)
        left[i] = (mS[i] + mD[i]) * 0.5f;
    /* Right = (S - D)/2.0 */
    for(size_t i{0};i < SamplesToDo;i++)
        right[i] = (mS[i] - mD[i]) * 0.5f;

    /* Copy the future samples to the front for next time. */
    std::copy(mS.cbegin()+SamplesToDo, mS.cbegin()+SamplesToDo+sFilterDelay, mS.begin());
    std::copy(mD.cbegin()+SamplesToDo, mD.cbegin()+SamplesToDo+sFilterDelay, mD.begin());
}


struct SpeakerPos {
    int mChannelID;
    float mAzimuth;
    float mElevation;
};

/* Azimuth is counter-clockwise. */
const SpeakerPos StereoMap[2]{
    { SF_CHANNEL_MAP_LEFT,  Deg2Rad( 30.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_RIGHT, Deg2Rad(-30.0f), Deg2Rad(0.0f) },
}, QuadMap[4]{
    { SF_CHANNEL_MAP_LEFT,       Deg2Rad(  45.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_RIGHT,      Deg2Rad( -45.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_REAR_LEFT,  Deg2Rad( 135.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_REAR_RIGHT, Deg2Rad(-135.0f), Deg2Rad(0.0f) },
}, X51Map[6]{
    { SF_CHANNEL_MAP_LEFT,       Deg2Rad(  30.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_RIGHT,      Deg2Rad( -30.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_CENTER,     Deg2Rad(   0.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_LFE, 0.0f, 0.0f },
    { SF_CHANNEL_MAP_SIDE_LEFT,  Deg2Rad( 110.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_SIDE_RIGHT, Deg2Rad(-110.0f), Deg2Rad(0.0f) },
}, X51RearMap[6]{
    { SF_CHANNEL_MAP_LEFT,       Deg2Rad(  30.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_RIGHT,      Deg2Rad( -30.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_CENTER,     Deg2Rad(   0.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_LFE, 0.0f, 0.0f },
    { SF_CHANNEL_MAP_REAR_LEFT,  Deg2Rad( 110.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_REAR_RIGHT, Deg2Rad(-110.0f), Deg2Rad(0.0f) },
}, X71Map[8]{
    { SF_CHANNEL_MAP_LEFT,       Deg2Rad(  30.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_RIGHT,      Deg2Rad( -30.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_CENTER,     Deg2Rad(   0.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_LFE, 0.0f, 0.0f },
    { SF_CHANNEL_MAP_REAR_LEFT,  Deg2Rad( 150.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_REAR_RIGHT, Deg2Rad(-150.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_SIDE_LEFT,  Deg2Rad(  90.0f), Deg2Rad(0.0f) },
    { SF_CHANNEL_MAP_SIDE_RIGHT, Deg2Rad( -90.0f), Deg2Rad(0.0f) },
}, X714Map[12]{
    { SF_CHANNEL_MAP_LEFT,       Deg2Rad(  30.0f), Deg2Rad( 0.0f) },
    { SF_CHANNEL_MAP_RIGHT,      Deg2Rad( -30.0f), Deg2Rad( 0.0f) },
    { SF_CHANNEL_MAP_CENTER,     Deg2Rad(   0.0f), Deg2Rad( 0.0f) },
    { SF_CHANNEL_MAP_LFE, 0.0f, 0.0f },
    { SF_CHANNEL_MAP_REAR_LEFT,  Deg2Rad( 150.0f), Deg2Rad( 0.0f) },
    { SF_CHANNEL_MAP_REAR_RIGHT, Deg2Rad(-150.0f), Deg2Rad( 0.0f) },
    { SF_CHANNEL_MAP_SIDE_LEFT,  Deg2Rad(  90.0f), Deg2Rad( 0.0f) },
    { SF_CHANNEL_MAP_SIDE_RIGHT, Deg2Rad( -90.0f), Deg2Rad( 0.0f) },
    { SF_CHANNEL_MAP_TOP_FRONT_LEFT,  Deg2Rad(  45.0f), Deg2Rad(35.0f) },
    { SF_CHANNEL_MAP_TOP_FRONT_RIGHT, Deg2Rad( -45.0f), Deg2Rad(35.0f) },
    { SF_CHANNEL_MAP_TOP_REAR_LEFT,   Deg2Rad( 135.0f), Deg2Rad(35.0f) },
    { SF_CHANNEL_MAP_TOP_REAR_RIGHT,  Deg2Rad(-135.0f), Deg2Rad(35.0f) },
};

inline std::array<float,4> GenCoeffs(float x /*+front*/, float y /*+left*/, float z /*+up*/)
{
    /* Coefficients are +3dB of FuMa. */
    std::array<float,4> coeffs;
    coeffs[0] = 1.0f;
    coeffs[1] = 1.41421356237f * x;
    coeffs[2] = 1.41421356237f * y;
    coeffs[3] = 1.41421356237f * z;
    return coeffs;
}

} // namespace


int main(int argc, char **argv)
{
    if(argc < 2 || std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)
    {
        printf("Usage: %s <infile...>\n\n", argv[0]);
        return 1;
    }

    size_t num_files{0}, num_encoded{0};
    for(int fidx{1};fidx < argc;++fidx)
    {
        ++num_files;

        std::string outname{argv[fidx]};
        size_t lastslash{outname.find_last_of('/')};
        if(lastslash != std::string::npos)
            outname.erase(0, lastslash+1);
        size_t extpos{outname.find_last_of('.')};
        if(extpos != std::string::npos)
            outname.resize(extpos);
        outname += ".uhj.flac";

        SF_INFO ininfo{};
        SndFilePtr infile{sf_open(argv[fidx], SFM_READ, &ininfo)};
        if(!infile)
        {
            fprintf(stderr, "Failed to open %s\n", argv[fidx]);
            continue;
        }
        printf("Converting %s to %s...\n", argv[fidx], outname.c_str());

        /* Work out the channel map, preferably using the actual channel map
         * from the file/format, but falling back to assuming WFX order.
         *
         * TODO: Map indices when the channel order differs from the virtual
         * speaker position maps.
         */
        al::span<const SpeakerPos> spkrs;
        auto chanmap = std::vector<int>(static_cast<uint>(ininfo.channels), SF_CHANNEL_MAP_INVALID);
        if(sf_command(infile.get(), SFC_GET_CHANNEL_MAP_INFO, chanmap.data(),
            ininfo.channels*int{sizeof(int)}) == SF_TRUE)
        {
            static const std::array<int,2> stereomap{{SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT}};
            static const std::array<int,4> quadmap{{SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT,
                SF_CHANNEL_MAP_REAR_LEFT, SF_CHANNEL_MAP_REAR_RIGHT}};
            static const std::array<int,6> x51map{{SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT,
                SF_CHANNEL_MAP_CENTER, SF_CHANNEL_MAP_LFE,
                SF_CHANNEL_MAP_SIDE_LEFT, SF_CHANNEL_MAP_SIDE_RIGHT}};
            static const std::array<int,6> x51rearmap{{SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT,
                SF_CHANNEL_MAP_CENTER, SF_CHANNEL_MAP_LFE,
                SF_CHANNEL_MAP_REAR_LEFT, SF_CHANNEL_MAP_REAR_RIGHT}};
            static const std::array<int,8> x71map{{SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT,
                SF_CHANNEL_MAP_CENTER, SF_CHANNEL_MAP_LFE,
                SF_CHANNEL_MAP_REAR_LEFT, SF_CHANNEL_MAP_REAR_RIGHT,
                SF_CHANNEL_MAP_SIDE_LEFT, SF_CHANNEL_MAP_SIDE_RIGHT}};
            static const std::array<int,12> x714map{{SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT,
                SF_CHANNEL_MAP_CENTER, SF_CHANNEL_MAP_LFE,
                SF_CHANNEL_MAP_REAR_LEFT, SF_CHANNEL_MAP_REAR_RIGHT,
                SF_CHANNEL_MAP_SIDE_LEFT, SF_CHANNEL_MAP_SIDE_RIGHT,
                SF_CHANNEL_MAP_TOP_FRONT_LEFT, SF_CHANNEL_MAP_TOP_FRONT_RIGHT,
                SF_CHANNEL_MAP_TOP_REAR_LEFT, SF_CHANNEL_MAP_TOP_REAR_RIGHT}};
            static const std::array<int,3> ambi2dmap{{SF_CHANNEL_MAP_AMBISONIC_B_W,
                SF_CHANNEL_MAP_AMBISONIC_B_X, SF_CHANNEL_MAP_AMBISONIC_B_Y}};
            static const std::array<int,4> ambi3dmap{{SF_CHANNEL_MAP_AMBISONIC_B_W,
                SF_CHANNEL_MAP_AMBISONIC_B_X, SF_CHANNEL_MAP_AMBISONIC_B_Y,
                SF_CHANNEL_MAP_AMBISONIC_B_Z}};

            auto match_chanmap = [](const al::span<int> a, const al::span<const int> b) -> bool
            {
                return a.size() == b.size()
                    && std::mismatch(a.begin(), a.end(), b.begin(), b.end()).first == a.end();
            };
            if(match_chanmap(chanmap, stereomap))
                spkrs = StereoMap;
            else if(match_chanmap(chanmap, quadmap))
                spkrs = QuadMap;
            else if(match_chanmap(chanmap, x51map))
                spkrs = X51Map;
            else if(match_chanmap(chanmap, x51rearmap))
                spkrs = X51RearMap;
            else if(match_chanmap(chanmap, x71map))
                spkrs = X71Map;
            else if(match_chanmap(chanmap, x714map))
                spkrs = X714Map;
            else if(match_chanmap(chanmap, ambi2dmap) || match_chanmap(chanmap, ambi3dmap))
            {
                /* Do nothing. */
            }
            else
            {
                std::string mapstr;
                if(chanmap.size() > 0)
                {
                    mapstr = std::to_string(chanmap[0]);
                    for(int idx : al::span<int>{chanmap}.subspan<1>())
                    {
                        mapstr += ',';
                        mapstr += std::to_string(idx);
                    }
                }
                fprintf(stderr, " ... %zu channels not supported (map: %s)\n", chanmap.size(),
                    mapstr.c_str());
                continue;
            }
        }
        else if(ininfo.channels == 2)
        {
            fprintf(stderr, " ... assuming WFX order stereo\n");
            spkrs = StereoMap;
        }
        else if(ininfo.channels == 6)
        {
            fprintf(stderr, " ... assuming WFX order 5.1\n");
            spkrs = X51Map;
        }
        else if(ininfo.channels == 8)
        {
            fprintf(stderr, " ... assuming WFX order 7.1\n");
            spkrs = X71Map;
        }
        else
        {
            fprintf(stderr, " ... unmapped %d-channel audio not supported\n", ininfo.channels);
            continue;
        }

        SF_INFO outinfo{};
        outinfo.frames = ininfo.frames;
        outinfo.samplerate = ininfo.samplerate;
        outinfo.channels = 2;
        outinfo.format = SF_FORMAT_PCM_24 | SF_FORMAT_FLAC;
        SndFilePtr outfile{sf_open(outname.c_str(), SFM_WRITE, &outinfo)};
        if(!outfile)
        {
            fprintf(stderr, " ... failed to create %s\n", outname.c_str());
            continue;
        }

        auto encoder = std::make_unique<UhjEncoder>();
        auto splbuf = al::vector<FloatBufferLine, 16>(static_cast<uint>(ininfo.channels+9));
        auto ambmem = al::span<FloatBufferLine,4>{&splbuf[0], 4};
        auto encmem = al::span<FloatBufferLine,2>{&splbuf[4], 2};
        auto srcmem = al::span<float,BufferLineSize>{splbuf[6].data(), BufferLineSize};
        auto outmem = al::span<float,BufferLineSize*2>{splbuf[7].data(), BufferLineSize*2};

        /* A number of initial samples need to be skipped to cut the lead-in
         * from the all-pass filter delay. The same number of samples need to
         * be fed through the encoder after reaching the end of the input file
         * to ensure none of the original input is lost.
         */
        size_t total_wrote{0};
        size_t LeadIn{UhjEncoder::sFilterDelay};
        sf_count_t LeadOut{UhjEncoder::sFilterDelay};
        while(LeadIn > 0 || LeadOut > 0)
        {
            auto inmem = splbuf[9].data();
            auto sgot = sf_readf_float(infile.get(), inmem, BufferLineSize);

            sgot = std::max<sf_count_t>(sgot, 0);
            if(sgot < BufferLineSize)
            {
                const sf_count_t remaining{std::min(BufferLineSize - sgot, LeadOut)};
                std::fill_n(inmem + sgot*ininfo.channels, remaining*ininfo.channels, 0.0f);
                sgot += remaining;
                LeadOut -= remaining;
            }

            for(auto&& buf : ambmem)
                buf.fill(0.0f);

            auto got = static_cast<size_t>(sgot);
            if(spkrs.empty())
            {
                /* B-Format is already in the correct order. It just needs a
                 * +3dB boost.
                 */
                constexpr float scale{1.41421356237f};
                const size_t chans{std::min<size_t>(static_cast<uint>(ininfo.channels), 4u)};
                for(size_t c{0};c < chans;++c)
                {
                    for(size_t i{0};i < got;++i)
                        ambmem[c][i] = inmem[i*static_cast<uint>(ininfo.channels)] * scale;
                    ++inmem;
                }
            }
            else for(auto&& spkr : spkrs)
            {
                /* Skip LFE. Or mix directly into W? Or W+X? */
                if(spkr.mChannelID == SF_CHANNEL_MAP_LFE)
                {
                    ++inmem;
                    continue;
                }

                for(size_t i{0};i < got;++i)
                    srcmem[i] = inmem[i * static_cast<uint>(ininfo.channels)];
                ++inmem;

                const auto coeffs = GenCoeffs(
                    std::cos(spkr.mAzimuth) * std::cos(spkr.mElevation),
                    std::sin(spkr.mAzimuth) * std::cos(spkr.mElevation),
                    std::sin(spkr.mElevation));
                for(size_t c{0};c < 4;++c)
                {
                    for(size_t i{0};i < got;++i)
                        ambmem[c][i] += srcmem[i] * coeffs[c];
                }
            }

            encoder->encode(encmem[0], encmem[1], ambmem.data(), got);
            if(LeadIn >= got)
            {
                LeadIn -= got;
                continue;
            }

            got -= LeadIn;
            for(size_t c{0};c < 2;++c)
            {
                constexpr float max_val{8388607.0f / 8388608.0f};
                auto clamp = [](float v, float mn, float mx) noexcept
                { return std::min(std::max(v, mn), mx); };
                for(size_t i{0};i < got;++i)
                    outmem[i*2 + c] = clamp(encmem[c][LeadIn+i], -1.0f, max_val);
            }
            LeadIn = 0;

            sf_count_t wrote{sf_writef_float(outfile.get(), outmem.data(),
                static_cast<sf_count_t>(got))};
            if(wrote < 0)
                fprintf(stderr, " ... failed to write samples: %d\n", sf_error(outfile.get()));
            else
                total_wrote += static_cast<size_t>(wrote);
        }
        printf(" ... wrote %zu samples (%" PRId64 ").\n", total_wrote, int64_t{ininfo.frames});
        ++num_encoded;
    }
    if(num_encoded == 0)
        fprintf(stderr, "Failed to encode any input files\n");
    else if(num_encoded < num_files)
        fprintf(stderr, "Encoded %zu of %zu files\n", num_encoded, num_files);
    else
        printf("Encoded %s%zu file%s\n", (num_encoded > 1) ? "all " : "", num_encoded,
            (num_encoded == 1) ? "" : "s");
    return 0;
}
