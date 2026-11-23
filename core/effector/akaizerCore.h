#ifndef EFFECTOR_AKAIZERCORE_H_
#define EFFECTOR_AKAIZERCORE_H_

// Akaizer timestretch core - the TIMESTRETCH routine of Ben Burchett's Akaizer v1.2,
// as ported line for line in Twisted Timestretch (TwistedGenetics/Twisted-Timestretch,
// Source/Engine/Akaizer.cpp), reworked to run in steps on the Tracker:
//  - no heap: reads the input and writes the output through channel pointers,
//    emulating the original's interleaved 16-bit words (stereo quirks included)
//  - the optional sampler-style repitch is worked out on the fly, value for value
//    the same as the plugin's repitch buffer
//  - processes a few grains per call, so the Sample Editor stays responsive
//
// The original, for reference (indices are 16-bit words, interleaved):
//
//     L  = CycleLength * 2 * Channels               ' grain length
//     C  = FpI4(Channels * SampleRate / CycleFreq)  ' output hop
//     nOut = FpI4(nIn * Stretch / 100)
//     S  = FpI4(nIn / (nOut / C))                   ' input hop
//     For k = 0 To FpI4(nOut / C - 1)
//         For j = 0 To L - 1
//             If S*k + j > UBound(In)  Then Exit For
//             If C*k + j > UBound(Out) Then Exit For
//             g = Sin((j + 1) * 3.141592654 / L)
//             Out(C*k + j) = FpI4(In(S*k + j) * g + Out(C*k + j))   ' clipped to 16-bit
//         Next
//     Next

#include <math.h>
#include <stdint.h>

namespace akaizer
{
// VB6 __vbaFpI4 / CInt rounding: nearest, ties to even
inline int64_t roundHalfEven(double x)
{
    const double f = floor(x);
    const double d = x - f;
    int64_t fi = (int64_t)f;
    if (d > 0.5)
    {
        return fi + 1;
    }
    if (d < 0.5)
    {
        return fi;
    }
    return (fi % 2 == 0) ? fi : fi + 1;
}

// cycle frequency the original uses when it isn't set by hand
inline int linkedFrequency(int cycleLength)
{
    if (cycleLength <= 0)
    {
        return 0;
    }
    return (int)roundHalfEven(41000.0 / (double)cycleLength);
}

inline int16_t clamp16(int64_t v)
{
    if (v > 32767)
    {
        return 32767;
    }
    if (v < -32768)
    {
        return -32768;
    }
    return (int16_t)v;
}

struct Settings
{
    double stretchPercent = 100.0; // 25 .. 2000
    int cycleLength = 800;         // 20 .. 2000 samples
    int cycleFrequency = 0;        // 20 .. 2050 cps, 0 = linked to the cycle length
    double semitones = 0.0;        // repitch before stretching, the final length is kept
};

class Core
{
  public:
    static constexpr int kMaxChannels = 2;

    // Work out the sizes. Returns the output length in frames (0 = nothing to do).
    uint32_t setup(const int16_t* const* in,
                   int channels,
                   uint32_t inFrames,
                   double sampleRate,
                   const Settings& s)
    {
        done = true;
        outFrameCount = 0;

        if (channels < 1 || channels > kMaxChannels || inFrames == 0 || sampleRate <= 0.0 ||
            s.cycleLength <= 0 || s.stretchPercent <= 0.0)
        {
            return 0;
        }

        int freq = s.cycleFrequency > 0 ? s.cycleFrequency : linkedFrequency(s.cycleLength);
        if (freq <= 0)
        {
            return 0;
        }

        ch = channels;
        for (int c = 0; c < ch; c++)
        {
            src[c] = in[c];
        }
        srcFrames = inFrames;

        const int64_t origWords = (int64_t)inFrames * ch;
        double stretchPct = s.stretchPercent;

        // repitch (varispeed) first, exactly like the plugin's render()
        repitch = fabs(s.semitones) >= 1.0e-9;
        if (repitch)
        {
            ratio = pow(2.0, s.semitones / 12.0);
            int64_t outF = (int64_t)llround((double)inFrames / ratio);
            pitchedFrames = outF < 1 ? 1 : outF;
            nIn = pitchedFrames * ch;
            stretchPct = s.stretchPercent * ((double)origWords / (double)nIn);
        }
        else
        {
            ratio = 1.0;
            pitchedFrames = inFrames;
            nIn = origWords;
        }

        const double factor = stretchPct / 100.0;
        nOut = roundHalfEven((double)nIn * factor);
        L = (int64_t)s.cycleLength * 2 * ch;
        C = roundHalfEven((double)ch * sampleRate / (double)freq);

        if (nOut <= 0 || C <= 0)
        {
            return 0;
        }

        S = roundHalfEven((double)nIn / ((double)nOut / (double)C));
        kMax = roundHalfEven((double)nOut / (double)C - 1.0);
        k = 0;

        outFrameCount = (uint32_t)(nOut / ch);
        done = outFrameCount == 0;
        return outFrameCount;
    }

    uint32_t getOutputFrames() const
    {
        return outFrameCount;
    }

    // Output buffers must hold getOutputFrames() frames and be zeroed before the first step().
    void setOutput(int16_t* const* out)
    {
        for (int c = 0; c < ch; c++)
        {
            dst[c] = out[c];
        }
    }

    // Lay down grains until at least maxWords output words have been covered.
    // Returns the number of output frames finished in this call; done() says when it's over.
    uint32_t step(int64_t maxWords)
    {
        if (done)
        {
            return 0;
        }

        // the literal typed into the VB source; kept for bit-faithfulness
        const double piVB = 3.141592654;
        const int64_t ubIn = nIn - 1;
        const int64_t ubOut = nOut - 1;
        const int64_t startK = k;
        int64_t covered = 0;

        while (k <= kMax && covered < maxWords)
        {
            const int64_t ib = S * k;
            const int64_t ob = C * k;
            for (int64_t j = 0; j <= L - 1; ++j)
            {
                if (ib + j > ubIn)
                {
                    break;
                }
                if (ob + j > ubOut)
                {
                    break;
                }
                const double g = sin((double)(j + 1) * piVB / (double)L);
                const int64_t ow = ob + j;
                const int oc = (int)(ow % ch);
                const int64_t of = ow / ch;
                if (of >= (int64_t)outFrameCount)
                {
                    continue; // odd last word of a stereo file: no frame to put it in
                }
                const int16_t cur = dst[oc][of];
                const int64_t v =
                  roundHalfEven((double)inputWord(ib + j) * g + (double)cur);
                dst[oc][of] = clamp16(v);
            }
            covered += C;
            k++;
        }

        if (k > kMax)
        {
            done = true;
        }

        // progress, in output frames
        const int64_t total = (int64_t)outFrameCount;
        int64_t from = (startK * C) / ch;
        int64_t to = done ? total : (k * C) / ch;
        from = from > total ? total : from;
        to = to > total ? total : to;
        return (uint32_t)(to > from ? to - from : 0);
    }

    bool isDone() const
    {
        return done;
    }

  private:
    // word w of the (repitched) interleaved input
    int16_t inputWord(int64_t w) const
    {
        const int c = (int)(w % ch);
        const int64_t f = w / ch;

        if (!repitch)
        {
            return src[c][f];
        }

        // 4-point, 3rd-order Hermite, as in the plugin's repitch()
        const double pos = (double)f * ratio;
        const int64_t i0 = (int64_t)floor(pos);
        const double t = pos - (double)i0;
        const double xm1 = at(i0 - 1, c), x0 = at(i0, c), x1 = at(i0 + 1, c), x2 = at(i0 + 2, c);
        const double c1 = 0.5 * (x1 - xm1);
        const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
        const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
        double v = ((c3 * t + c2) * t + c1) * t + x0;
        v = round(v);
        if (v < -32768.0)
        {
            v = -32768.0;
        }
        if (v > 32767.0)
        {
            v = 32767.0;
        }
        return (int16_t)v;
    }

    double at(int64_t f, int c) const
    {
        if (f < 0)
        {
            f = 0;
        }
        if (f > (int64_t)srcFrames - 1)
        {
            f = (int64_t)srcFrames - 1;
        }
        return (double)src[c][f];
    }

    const int16_t* src[kMaxChannels] = { nullptr, nullptr };
    int16_t* dst[kMaxChannels] = { nullptr, nullptr };
    int ch = 1;
    uint32_t srcFrames = 0;
    bool repitch = false;
    double ratio = 1.0;
    int64_t pitchedFrames = 0;
    int64_t nIn = 0, nOut = 0, L = 0, C = 0, S = 0, kMax = 0, k = 0;
    uint32_t outFrameCount = 0;
    bool done = true;
};
} // namespace akaizer

#endif /* EFFECTOR_AKAIZERCORE_H_ */
