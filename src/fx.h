// Mystik's effects, after the voice at the host's rate: Drive, Crush, a ping-pong Echo with Absorb in its loop, Space
// (a spring-like tank), Volume and a soft ceiling. Each one costs nothing while it is off, and Echo and Space stop
// working once their loops have gone quiet.
#pragma once

#include "dsp.h"
#include "params.h"

#include <vector>

namespace mystik
{
struct FxKnobs
{
    float drive = 0, crush = 0, echo = 0.12f, repeats = 0.4f, absorb = 0.45f, space = 0.18f, spaceDecay = 0.5f, volume = 0.7f;
    int echoTime = 7;
};

class Effects
{
public:
    void prepare (double hostRate)
    {
        rate = (float) hostRate;
        // The echo's line: at least 2.6 s, a power of two for a masked wrap.
        size_t n = 1;
        while ((double) n < 2.6 * hostRate)
            n <<= 1;
        if (echoL.size() != n)
            echoL.assign (n, 0.0f), echoR.assign (n, 0.0f);
        mask = (uint32_t) n - 1;
        // The tank's lines: 37.1 and 41.7 ms (never in step), each under a power of two.
        springLength[0] = std::max (1, (int) std::lround (0.0371 * hostRate));
        springLength[1] = std::max (1, (int) std::lround (0.0417 * hostRate));
        size_t m = 1;
        while ((int) m <= springLength[1] + 2)
            m <<= 1;
        for (auto& line : spring)
            line.assign (m, 0.0f);
        springMask = (uint32_t) m - 1;
        dcCoef = 1.0f - 2.0f * (float) M_PI * 8.0f / rate;
        springHp = onePoleCoeff (2.0f * (float) M_PI * 150.0f / rate);
        springLp = onePoleCoeff (2.0f * (float) M_PI * 5500.0f / rate);
        reset();
    }

    void reset()
    {
        std::fill (echoL.begin(), echoL.end(), 0.0f);
        std::fill (echoR.begin(), echoR.end(), 0.0f);
        for (auto& line : spring)
            std::fill (line.begin(), line.end(), 0.0f);
        for (int k = 0; k < 8; ++k)
            ap1[k] = ap2[k] = v2 { 0.0f, 0.0f };
        write = springWrite = 0;
        echoQuiet = springQuiet = 1u << 30;
        absorbL = absorbR = 0.0f;
        hpState = lpState = v2 { 0.0f, 0.0f };
        dcIn = dcOut = 0.0f;
        held = 0.0f, holdPhase = 1.0f;
        delay = -1.0f;
    }

    void set (const FxKnobs& k)
    {
        knobs = k;
        driveGain = 1.0f + 7.0f * k.drive * k.drive;
        driveMakeup = 1.0f / std::sqrt (driveGain);
        crushStep = std::pow (1500.0f / rate, k.crush);
        crushLevels = std::exp2 (16.0f - 13.0f * k.crush);
        feedback = 1.05f * k.repeats;
        absorbCoef = onePoleCoeff (2.0f * (float) M_PI * 14000.0f * std::pow (600.0f / 14000.0f, k.absorb) / rate);
        springFeedback = 0.35f + 0.57f * k.spaceDecay;
        volumeGain = k.volume * k.volume * 1.4f;
        retime();
    }

    void tempo (double bpm)
    {
        if (bpm != tempoBpm)
            tempoBpm = bpm, retime();
    }

    // In: the voice in `left` (`silentIn`: all zeros). Out: both sides.
    void process (float* left, float* right, uint32_t frames, bool silentIn)
    {
        // Nothing in, nothing left ringing (and no Crush, whose hold keeps time): silence at no cost.
        if (silentIn && dcOut == 0.0f && dcIn == 0.0f && echoQuiet > mask && springQuiet > springMask && knobs.crush <= 0.0f)
        {
            if (right != nullptr)
                std::memset (right, 0, sizeof (float) * frames);
            return;
        }
        // The fold's bias leaves DC: an 8 Hz high-pass first (its tail ends, exactly, under -200 dB).
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float x = left[i];
            const float y = x - dcIn + dcCoef * dcOut;
            dcOut = (x == 0.0f && std::fabs (y) < 1e-10f) ? 0.0f : y; // sample by sample, whatever the block
            dcIn = x;
            left[i] = dcOut;
        }
        if (knobs.drive > 0.0f)
            for (uint32_t i = 0; i < frames; ++i)
                left[i] = tanhFast (left[i] * driveGain) * driveMakeup;
        if (knobs.crush > 0.0f)
            for (uint32_t i = 0; i < frames; ++i)
            {
                holdPhase += crushStep;
                if (holdPhase >= 1.0f)
                {
                    holdPhase -= std::floor (holdPhase);
                    held = std::rint (left[i] * crushLevels) / crushLevels;
                }
                left[i] = held;
            }
        // Echo and Space make the two sides; without them the right side is the left.
        const bool echoOn = ! (knobs.echo <= 0.0f && echoQuiet > mask), tankOn = ! (knobs.space <= 0.0f && springQuiet > springMask);
        if (! echoOn && ! tankOn)
        {
            for (uint32_t i = 0; i < frames; ++i)
                left[i] = ceiling (left[i] * volumeGain);
            if (right != nullptr)
                std::memcpy (right, left, sizeof (float) * frames);
            return;
        }
        if (right != nullptr)
        {
            std::memcpy (right, left, sizeof (float) * frames);
            if (echoOn) echo (left, right, frames);
            if (tankOn) tank (left, right, frames);
            for (uint32_t i = 0; i < frames; ++i)
                left[i] = ceiling (left[i] * volumeGain), right[i] = ceiling (right[i] * volumeGain);
            return;
        }
        // A mono host: the right side is worked out in scratch and left behind.
        for (uint32_t at = 0; at < frames; at += scratchSize)
        {
            const uint32_t n = std::min<uint32_t> (scratchSize, frames - at);
            std::memcpy (scratch, left + at, sizeof (float) * n);
            if (echoOn) echo (left + at, scratch, n);
            if (tankOn) tank (left + at, scratch, n);
            for (uint32_t i = 0; i < n; ++i)
                left[at + i] = ceiling (left[at + i] * volumeGain);
        }
    }

    // All sound off: the tails go too.
    void silence() { reset(); }
    // A choke: the drum stops at once, so the DC blocker's memory of it goes too (or it would thump on); echoes ring on.
    void cut() { dcIn = dcOut = 0.0f; }

    bool quiet() const { return echoQuiet > mask && springQuiet > springMask; }

private:
    // Linear below 0.9, then rounds off toward 1.
    static float ceiling (float y)
    {
        const float a = std::fabs (y);
        return a <= 0.9f ? y : std::copysign (0.9f + 0.1f * tanhFast ((a - 0.9f) * 10.0f), y);
    }

    void retime()
    {
        const double beats = echoTimeBeats[std::max (0, std::min (knobs.echoTime, 11))];
        echoTarget = (float) std::min ((double) mask - 4.0, std::max (1.0, beats * 60.0 / std::max (1.0, tempoBpm) * rate));
        if (delay < 0.0f)
            delay = echoTarget;
    }

    void echo (float* left, float* right, uint32_t frames)
    {
        const float mix = knobs.echo * knobs.echo;
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float x = left[i];
            // Quiet in and out for a whole line's length: everything in it is silence; stop until sound comes.
            if (x == 0.0f && echoQuiet > mask)
                continue;
            delay += (echoTarget - delay) * 0.0005f; // a tape's glide to a new time
            const float back = (float) write - delay;
            const float floorBack = std::floor (back);
            const uint32_t at = (uint32_t) (int32_t) floorBack & mask;
            const float frac = back - floorBack;
            const float tl = echoL[at] + (echoL[(at + 1) & mask] - echoL[at]) * frac;
            const float tr = echoR[at] + (echoR[(at + 1) & mask] - echoR[at]) * frac;
            absorbL += (tl - absorbL) * absorbCoef;
            absorbR += (tr - absorbR) * absorbCoef;
            // Ping-pong: the dry sound enters on the left, each side feeds the other.
            echoL[write] = tanhFast (x + feedback * absorbR);
            echoR[write] = tanhFast (feedback * absorbL);
            write = (write + 1) & mask;
            left[i] = x + mix * tl;
            right[i] = x + mix * tr;
            echoQuiet = std::fmax (std::fabs (x), std::fmax (std::fabs (tl), std::fabs (tr))) < 1e-6f ? echoQuiet + 1 : 0;
            if (echoQuiet > mask)
            {
                std::fill (echoL.begin(), echoL.end(), 0.0f); // exactly silent from here
                std::fill (echoR.begin(), echoR.end(), 0.0f);
                absorbL = absorbR = 0.0f;
            }
        }
    }

    // Space: per side a feedback delay through eight stretched allpasses (H = (a + z^-2) / (1 + a z^-2), one multiply
    // each in lattice form): their dispersion smears each echo into a chirp, as a spring does. A high-pass at 150 Hz in,
    // a low-pass at 5.5 kHz in the loop. Left and right run as the two lanes of one vector.
    void tank (float* left, float* right, uint32_t frames)
    {
        const float mix = knobs.space * knobs.space * 1.6f;
        constexpr float coefficient = 0.62f;
        const v2 fb = { springFeedback, springFeedback }, hp = { springHp, springHp }, lp = { springLp, springLp };
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float dryL = left[i], dryR = right[i];
            const float x = 0.5f * (dryL + dryR);
            if (x == 0.0f && springQuiet > springMask)
                continue;
            const v2 in = { x, x };
            hpState += (in - hpState) * hp;
            v2 s = (in - hpState) + fb * lpState;
            for (int k = 0; k < 8; ++k)
            {
                const v2 v = s - coefficient * ap2[k];
                s = coefficient * v + ap2[k];
                ap2[k] = ap1[k], ap1[k] = v;
            }
            spring[0][springWrite] = s[0];
            spring[1][springWrite] = s[1];
            const v2 tap = { spring[0][(springWrite - (uint32_t) springLength[0]) & springMask], spring[1][(springWrite - (uint32_t) springLength[1]) & springMask] };
            lpState += (tap - lpState) * lp;
            springWrite = (springWrite + 1) & springMask;
            left[i] = dryL + mix * tap[0];
            right[i] = dryR + mix * tap[1];
            const float loud = std::fmax (std::fmax (std::fabs (x), std::fmax (std::fabs (tap[0]), std::fabs (tap[1]))),
                                          std::fmax (std::fabs (lpState[0]), std::fabs (lpState[1])));
            springQuiet = loud < 1e-6f ? springQuiet + 1 : 0;
            if (springQuiet > springMask)
            {
                for (auto& line : spring)
                    std::fill (line.begin(), line.end(), 0.0f);
                for (int k = 0; k < 8; ++k)
                    ap1[k] = ap2[k] = v2 { 0.0f, 0.0f };
                hpState = lpState = v2 { 0.0f, 0.0f };
            }
        }
    }

    FxKnobs knobs;
    float rate = 48000.0f;
    double tempoBpm = 120.0;
    // Drive, Crush.
    float driveGain = 1, driveMakeup = 1, crushStep = 1, crushLevels = 65536, held = 0, holdPhase = 1;
    // DC.
    float dcCoef = 0.999f, dcIn = 0, dcOut = 0;
    // Echo.
    std::vector<float> echoL, echoR;
    uint32_t mask = 0, write = 0, echoQuiet = 1u << 30;
    float delay = -1, echoTarget = 1, feedback = 0, absorbCoef = 1, absorbL = 0, absorbR = 0;
    // Space.
    std::vector<float> spring[2];
    int springLength[2] { 1, 1 };
    uint32_t springMask = 0, springWrite = 0, springQuiet = 1u << 30;
    v2 ap1[8] {}, ap2[8] {}, hpState {}, lpState {};
    float springHp = 0, springLp = 0, springFeedback = 0.5f;
    static constexpr uint32_t scratchSize = 256;
    float scratch[scratchSize] {};
    float volumeGain = 1;
};
} // namespace mystik
