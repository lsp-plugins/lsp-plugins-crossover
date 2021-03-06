/*
 * Copyright (C) 2021 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2021 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-plugins-crossover
 * Created on: 3 авг. 2021 г.
 *
 * lsp-plugins-crossover is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-plugins-crossover is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-plugins-crossover. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PRIVATE_PLUGINS_CROSSOVER_H_
#define PRIVATE_PLUGINS_CROSSOVER_H_

#include <lsp-plug.in/plug-fw/plug.h>
#include <lsp-plug.in/plug-fw/core/IDBuffer.h>
#include <lsp-plug.in/dsp-units/ctl/Bypass.h>
#include <lsp-plug.in/dsp-units/util/Analyzer.h>
#include <lsp-plug.in/dsp-units/util/Crossover.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>

#include <private/meta/crossover.h>

namespace lsp
{
    namespace plugins
    {
        /**
         * Crossover Plugin Series
         */
        class crossover: public plug::Module
        {
            public:
                enum c_mode_t
                {
                    XOVER_MONO,
                    XOVER_STEREO,
                    XOVER_LR,
                    XOVER_MS
                };

            protected:
                typedef struct xover_band_t
                {
                    dspu::Delay         sDelay;             // Band delay

                    float              *vOut;               // Output channel pointer
                    float              *vResult;            // Result buffer
                    float              *vTr;                // Transfer function
                    float              *vFc;                // Frequency chart

                    bool                bSolo;              // Soloing
                    bool                bMute;              // Muting
                    float               fGain;              // Gain
                    float               fOutLevel;          // Output signal level
                    bool                bSyncCurve;         // Sync frequency response
                    float               fHue;               // Hue color

                    plug::IPort        *pSolo;              // Soloing
                    plug::IPort        *pMute;              // Muting
                    plug::IPort        *pPhase;             // Phase reversal
                    plug::IPort        *pGain;              // Gain
                    plug::IPort        *pDelay;             // Delay
                    plug::IPort        *pOutLevel;          // Output level of the band
                    plug::IPort        *pFreqEnd;           // Frequency range end
                    plug::IPort        *pOut;               // Output port
                    plug::IPort        *pAmpGraph;          // Amplitude graph
                    plug::IPort        *pHue;               // Hue color
                } xover_band_t;

                typedef struct xover_split_t
                {
                    plug::IPort        *pSlope;             // Slope
                    plug::IPort        *pFreq;              // Split frequency
                } xover_split_t;

                typedef struct channel_t
                {
                    dspu::Bypass        sBypass;            // Bypass
                    dspu::Crossover     sXOver;             // Crossover module

                    xover_split_t       vSplit[meta::crossover_metadata::BANDS_MAX-1];   // Split bands
                    xover_band_t        vBands[meta::crossover_metadata::BANDS_MAX];     // Crossover bands

                    float              *vIn;                // Input buffer
                    float              *vOut;               // Output buffer
                    float              *vInAnalyze;         // Input analysis
                    float              *vOutAnalyze;        // Output analysis
                    float              *vBuffer;            // Common data processing buffer
                    float              *vResult;            // Result buffer
                    float              *vTr;                // Transfer function
                    float              *vFc;                // Frequency chart

                    size_t              nAnInChannel;       // Analyzer channel used for input signal analysis
                    size_t              nAnOutChannel;      // Analyzer channel used for output signal analysis
                    bool                bSyncCurve;         // Sync frequency response curve
                    float               fInLevel;           // Input level meter
                    float               fOutLevel;          // Output level meter

                    plug::IPort        *pIn;                // Input
                    plug::IPort        *pOut;               // Output
                    plug::IPort        *pFftIn;             // Pre-processing FFT analysis data
                    plug::IPort        *pFftInSw;           // Pre-processing FFT analysis control port
                    plug::IPort        *pFftOut;            // Post-processing FFT analysis data
                    plug::IPort        *pFftOutSw;          // Post-processing FFT analysis controlport
                    plug::IPort        *pAmpGraph;          // Crossover amplitude graph
                    plug::IPort        *pInLvl;             // Input level meter
                    plug::IPort        *pOutLvl;            // Output level meter
                } channel_t;

            protected:
                dspu::Analyzer      sAnalyzer;              // Analyzer
                size_t              nMode;                  // Crossover mode
                channel_t          *vChannels;              // Crossover channels
                float              *vAnalyze[4];            // Data analysis buffer
                float               fInGain;                // Input gain
                float               fOutGain;               // Output gain
                float               fZoom;                  // Zoom
                bool                bMSOut;                 // Mid/Side output

                uint8_t            *pData;                  // Aligned data pointer
                float              *vFreqs;                 // Analyzer FFT frequencies
                float              *vCurve;                 // Curve
                uint32_t           *vIndexes;               // Analyzer FFT indexes
                core::IDBuffer     *pIDisplay;              // Inline display buffer

                plug::IPort        *pBypass;                // Bypass port
                plug::IPort        *pInGain;                // Input gain port
                plug::IPort        *pOutGain;               // Output gain port
                plug::IPort        *pReactivity;            // Reactivity
                plug::IPort        *pShiftGain;             // Shift gain port
                plug::IPort        *pZoom;                  // Zoom port
                plug::IPort        *pMSOut;                 // Mid/Side output

            protected:
                static void                             process_band(void *object, void *subject, size_t band, const float *data, size_t sample, size_t count);
                static inline dspu::crossover_mode_t    crossover_mode(size_t slope);
                static inline size_t                    crossover_slope(size_t slope);

            public:
                explicit crossover(const meta::plugin_t *metadata, size_t mode);
                virtual ~crossover();

                virtual void        init(plug::IWrapper *wrapper, plug::IPort **ports);
                virtual void        destroy();

            public:
                virtual void        update_settings();
                virtual void        update_sample_rate(long sr);
                virtual void        ui_activated();

                virtual void        process(size_t samples);
                virtual bool        inline_display(plug::ICanvas *cv, size_t width, size_t height);

                virtual void        dump(dspu::IStateDumper *v) const;
        };
    } // namespace plugins
} // namespace lsp

#endif /* PRIVATE_PLUGINS_CROSSOVER_H_ */
