/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
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

#include <private/plugins/crossover.h>
#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/common/bits.h>
#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/dsp/dsp.h>
#include <lsp-plug.in/dsp-units/misc/envelope.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/shared/debug.h>
#include <lsp-plug.in/shared/id_colors.h>
#include <lsp-plug.in/stdlib/math.h>

#define BUFFER_SIZE             0x400U

namespace lsp
{
    namespace plugins
    {
        //-------------------------------------------------------------------------
        // Plugin factory
        inline namespace
        {
            typedef struct plugin_settings_t
            {
                const meta::plugin_t   *metadata;
                uint8_t                 mode;
            } plugin_settings_t;

            static const meta::plugin_t *plugins[] =
            {
                &meta::crossover_mono,
                &meta::crossover_stereo,
                &meta::crossover_lr,
                &meta::crossover_ms
            };

            static const plugin_settings_t plugin_settings[] =
            {
                { &meta::crossover_mono,        crossover::XOVER_MONO       },
                { &meta::crossover_stereo,      crossover::XOVER_STEREO     },
                { &meta::crossover_lr,          crossover::XOVER_LR         },
                { &meta::crossover_ms,          crossover::XOVER_MS         },

                { NULL, 0 }
            };

            static plug::Module *plugin_factory(const meta::plugin_t *meta)
            {
                for (const plugin_settings_t *s = plugin_settings; s->metadata != NULL; ++s)
                    if (s->metadata == meta)
                        return new crossover(s->metadata, s->mode);
                return NULL;
            }

            static plug::Factory factory(plugin_factory, plugins, 4);
        } /* inline namespace */

        //-------------------------------------------------------------------------
        crossover::crossover(const meta::plugin_t *metadata, size_t mode): plug::Module(metadata)
        {
            nMode           = mode;
            nOpMode         = meta::crossover_metadata::CROSS_CLASSIC;
            vChannels       = NULL;
            vAnalyze[0]     = NULL;
            vAnalyze[1]     = NULL;
            vAnalyze[2]     = NULL;
            vAnalyze[3]     = NULL;
            fInGain         = GAIN_AMP_0_DB;
            fOutGain        = GAIN_AMP_0_DB;
            fZoom           = GAIN_AMP_0_DB;
            bMSOut          = false;
            bSMApply        = true;

            pData           = NULL;
            vFreqs          = NULL;
            vCurve          = NULL;
            vIndexes        = NULL;
            pIDisplay       = NULL;

            pBypass         = NULL;
            pOpMode         = NULL;
            pSMApply        = NULL;
            pInGain         = NULL;
            pOutGain        = NULL;
            pReactivity     = NULL;
            pShiftGain      = NULL;
            pZoom           = NULL;
            pMSOut          = NULL;
        }

        crossover::~crossover()
        {
            do_destroy();
        }

        void crossover::init(plug::IWrapper *wrapper, plug::IPort **ports)
        {
            // Initialize plugin
            plug::Module::init(wrapper, ports);

            // Determine number of channels
            size_t channels         = (nMode == XOVER_MONO) ? 1 : 2;
            size_t sz_channels      = align_size(channels * sizeof(channel_t), DEFAULT_ALIGN);
            size_t mesh_size        = align_size(meta::crossover_metadata::MESH_POINTS * sizeof(float), DEFAULT_ALIGN);
            size_t ind_size         = align_size(meta::crossover_metadata::MESH_POINTS * sizeof(uint32_t), DEFAULT_ALIGN);

            size_t to_alloc         = sz_channels +
                                      mesh_size             + // vFreqs
                                      ind_size              + // vIndexes
                                      channels * (
                                          2 * mesh_size                           +                             // vTr (both complex and real)
                                          mesh_size                               +                             // vFc (real only)
                                          BUFFER_SIZE * sizeof(float) * 4         +                             // vInAnalyze, vOutAnalyze, vBuffer, vResult
                                          BUFFER_SIZE * sizeof(float) * meta::crossover_metadata::BANDS_MAX +   // band.vResult
                                          meta::crossover_metadata::BANDS_MAX * mesh_size * 2 +                 // band.vTr
                                          meta::crossover_metadata::BANDS_MAX * mesh_size                       // band.vFc
                                      );

            // Initialize analyzer
            size_t an_cid           = 0;
            if (!sAnalyzer.init(2*channels, meta::crossover_metadata::FFT_RANK,
                                MAX_SAMPLE_RATE, meta::crossover_metadata::REFRESH_RATE))
                return;

            sAnalyzer.set_rank(meta::crossover_metadata::FFT_RANK);
            sAnalyzer.set_activity(false);
            sAnalyzer.set_envelope(dspu::envelope::PINK_NOISE);
            sAnalyzer.set_window(meta::crossover_metadata::FFT_WINDOW);
            sAnalyzer.set_rate(meta::crossover_metadata::REFRESH_RATE);

            // Allocate memory
            uint8_t *ptr    = alloc_aligned<uint8_t>(pData, to_alloc);
            if (ptr == NULL)
                return;
            lsp_guard_assert(uint8_t *save   = ptr);

            // Assign pointers
            vChannels       = advance_ptr_bytes<channel_t>(ptr, sz_channels);       // Audio channels
            vFreqs          = advance_ptr_bytes<float>(ptr, mesh_size);           // Graph frequencies
            vIndexes        = advance_ptr_bytes<uint32_t>(ptr, ind_size);

            // Initialize channels
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c        = &vChannels[i];

                c->sBypass.construct();
                c->sXOver.construct();
                c->sFFTXOver.construct();

                if (!c->sXOver.init(meta::crossover_metadata::BANDS_MAX, BUFFER_SIZE))
                    return;

                for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                {
                    xover_band_t *b     = &c->vBands[j];

                    c->sXOver.set_handler(j, process_band, this, c);                // Bind channel as a handler

                    b->sDelay.construct();

                    b->vOut             = NULL;

                    b->vResult          = advance_ptr_bytes<float>(ptr, BUFFER_SIZE * sizeof(float));
                    b->vTr              = advance_ptr_bytes<float>(ptr, mesh_size * 2);           // Transfer buffer
                    b->vFc              = advance_ptr_bytes<float>(ptr, mesh_size);           // Frequency chart

                    b->bSolo            = false;
                    b->bMute            = false;
                    b->bActive          = false;
                    b->fGain            = GAIN_AMP_0_DB;
                    b->fOutLevel        = 0.0f;
                    b->bSyncCurve       = false;

                    b->pSolo            = NULL;
                    b->pMute            = NULL;
                    b->pPhase           = NULL;
                    b->pGain            = NULL;
                    b->pDelay           = NULL;
                    b->pOutLevel        = NULL;
                    b->pFreqEnd         = NULL;
                    b->pOut             = NULL;
                    b->pAmpGraph        = NULL;
                }

                for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX-1; ++j)
                {
                    xover_split_t *s    = &c->vSplit[j];

                    s->nBand            = j+1;
                    s->nSlope           = 0;
                    s->fFreq            = 0.0f;
                    s->pSlope           = NULL;
                    s->pFreq            = NULL;
                }

                c->vIn              = NULL;
                c->vOut             = NULL;
                c->vInAnalyze       = advance_ptr_bytes<float>(ptr, BUFFER_SIZE * sizeof(float));
                c->vOutAnalyze      = advance_ptr_bytes<float>(ptr, BUFFER_SIZE * sizeof(float));
                c->vBuffer          = advance_ptr_bytes<float>(ptr, BUFFER_SIZE * sizeof(float));
                c->vResult          = advance_ptr_bytes<float>(ptr, BUFFER_SIZE * sizeof(float));
                c->vTr              = advance_ptr_bytes<float>(ptr, mesh_size * 2);
                c->vFc              = advance_ptr_bytes<float>(ptr, mesh_size);

                c->nAnInChannel     = an_cid++;
                c->nAnOutChannel    = an_cid++;

                vAnalyze[c->nAnInChannel]   = c->vInAnalyze;
                vAnalyze[c->nAnOutChannel]  = c->vOutAnalyze;

                c->bSyncCurve       = false;
                c->fInLevel         = 0.0f;
                c->fOutLevel        = 0.0f;

                c->pIn              = NULL;
                c->pOut             = NULL;
                c->pFftIn           = NULL;
                c->pFftInSw         = NULL;
                c->pFftOut          = NULL;
                c->pFftOutSw        = NULL;
                c->pAmpGraph        = NULL;
                c->pInLvl           = NULL;
                c->pOutLvl          = NULL;
            }

            lsp_assert(ptr <= &save[to_alloc]);

            // Bind ports
            size_t port_id              = 0;

            // Input ports
            lsp_trace("Binding input ports");
            for (size_t i=0; i<channels; ++i)
                BIND_PORT(vChannels[i].pIn);

            // Input ports
            lsp_trace("Binding output ports");
            for (size_t i=0; i<channels; ++i)
                BIND_PORT(vChannels[i].pOut);

            // Bind
            lsp_trace("Binding band outputs");
            if (channels < 2)
            {
                for (size_t i=0; i<meta::crossover_metadata::BANDS_MAX; ++i)
                    BIND_PORT(vChannels[0].vBands[i].pOut);
            }
            else
            {
                for (size_t i=0; i<meta::crossover_metadata::BANDS_MAX; ++i)
                {
                    BIND_PORT(vChannels[0].vBands[i].pOut);
                    BIND_PORT(vChannels[1].vBands[i].pOut);
                }
            }

            // Bind bypass
            lsp_trace("Binding common ports");
            BIND_PORT(pBypass);
            BIND_PORT(pOpMode);
            BIND_PORT(pSMApply);
            BIND_PORT(pInGain);
            BIND_PORT(pOutGain);
            BIND_PORT(pReactivity);
            BIND_PORT(pShiftGain);
            BIND_PORT(pZoom);

            if ((nMode == XOVER_LR) || (nMode == XOVER_MS))
            {
                SKIP_PORT("Separate channels link");
                SKIP_PORT("Processor selector");
            }

            if (nMode == XOVER_MS)
                BIND_PORT(pMSOut);

            // Bind channel ports
            lsp_trace("Binding channel ports");

            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c    = &vChannels[i];

                if ((i > 0) && (nMode == XOVER_STEREO))
                    c->pAmpGraph            = NULL;
                else
                {
                    SKIP_PORT("Filter curves switch"); // Skip filter curves switch
                    SKIP_PORT("Graph curves switch"); // Skip graph curves switch
                    BIND_PORT(c->pAmpGraph);
                }
            }

            lsp_trace("Binding meters");
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c    = &vChannels[i];

                BIND_PORT(c->pFftInSw);
                BIND_PORT(c->pFftOutSw);
                BIND_PORT(c->pFftIn);
                BIND_PORT(c->pFftOut);
                BIND_PORT(c->pInLvl);
                BIND_PORT(c->pOutLvl);
            }

            // Split frequencies
            lsp_trace("Binding split frequencies");
            for (size_t i=0; i<channels; ++i)
            {
                for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX-1; ++j)
                {
                    xover_split_t *s    = &vChannels[i].vSplit[j];

                    if ((i > 0) && (nMode == XOVER_STEREO))
                    {
                        xover_split_t *sc   = &vChannels[0].vSplit[j];
                        s->pSlope           = sc->pSlope;
                        s->pFreq            = sc->pFreq;
                    }
                    else
                    {
                        BIND_PORT(s->pSlope);
                        BIND_PORT(s->pFreq);
                    }
                }
            }

            // Bands
            lsp_trace("Binding band controllers");
            for (size_t i=0; i<channels; ++i)
            {
                for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                {
                    xover_band_t *b     = &vChannels[i].vBands[j];

                    if ((i > 0) && (nMode == XOVER_STEREO))
                    {
                        xover_band_t *sb    = &vChannels[0].vBands[j];
                        b->pSolo            = sb->pSolo;
                        b->pMute            = sb->pMute;
                        b->pPhase           = sb->pPhase;
                        b->pGain            = sb->pGain;
                        b->pDelay           = sb->pDelay;
                        b->pFreqEnd         = sb->pFreqEnd;
                        b->pAmpGraph        = NULL;
                    }
                    else
                    {
                        BIND_PORT(b->pSolo);
                        BIND_PORT(b->pMute);
                        BIND_PORT(b->pPhase);
                        BIND_PORT(b->pGain);
                        BIND_PORT(b->pDelay);
                        SKIP_PORT("Hue"); // Skip hue settings
                        BIND_PORT(b->pFreqEnd);
                        BIND_PORT(b->pAmpGraph);
                    }
                }
            }

            // Band meters
            lsp_trace("Binding band meters");
            for (size_t i=0; i<meta::crossover_metadata::BANDS_MAX; ++i)
            {
                for (size_t j=0; j<channels; ++j)
                {
                    xover_band_t *b     = &vChannels[j].vBands[i];
                    BIND_PORT(b->pOutLevel);
                }
            }

            lsp_trace("Initialization done");
        }

        void crossover::destroy()
        {
            Module::destroy();
            do_destroy();
        }

        void crossover::do_destroy()
        {
            // Determine number of channels
            size_t channels     = (nMode == XOVER_MONO) ? 1 : 2;

            // Destroy channels
            if (vChannels != NULL)
            {
                for (size_t i=0; i<channels; ++i)
                {
                    channel_t *c    = &vChannels[i];

                    c->sXOver.destroy();
                    c->sFFTXOver.destroy();
                    c->vBuffer      = NULL;
                    c->vTr          = NULL;

                    for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                    {
                        xover_band_t *b = &c->vBands[j];
                        b->sDelay.destroy();
                    }
                }

                vChannels       = NULL;
            }

            if (pIDisplay != NULL)
            {
                pIDisplay->destroy();
                pIDisplay   = NULL;
            }

            // Destroy data
            if (pData != NULL)
                free_aligned(pData);

            // Destroy analyzer
            sAnalyzer.destroy();
        }

        inline dspu::crossover_mode_t crossover::crossover_mode(size_t slope)
        {
            return dspu::CROSS_MODE_BT;
        }

        inline size_t crossover::crossover_slope(size_t slope)
        {
            return slope;
        }

        inline float crossover::fft_crossover_slope(size_t slope)
        {
            return (slope == dspu::CROSS_SLOPE_LR2) ? -12.0f : -24.0f * (slope - 1.0f);
        }

        int crossover::compare_splits(const void *a1, const void *a2, void *data)
        {
            const xover_split_t * const * s1 = reinterpret_cast<const xover_split_t * const *>(a1);
            const xover_split_t * const * s2 = reinterpret_cast<const xover_split_t * const *>(a2);
            if ((*s1)->fFreq < (*s2)->fFreq)
                return -1;
            else if ((*s1)->fFreq > (*s2)->fFreq)
                return 1;
            return 0;
        }

        void crossover::update_settings()
        {
            // Determine number of channels
            size_t channels     = (nMode == XOVER_MONO) ? 1 : 2;
            size_t fft_channels = 0;
            bool sync           = false;
            bool redraw         = false;

            // Update analyzer settings
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c    = &vChannels[i];

                // Update analyzer settings
                sAnalyzer.enable_channel(c->nAnInChannel, c->pFftInSw->value() >= 0.5f);
                sAnalyzer.enable_channel(c->nAnOutChannel, c->pFftOutSw->value() >= 0.5f);

                if (sAnalyzer.channel_active(c->nAnInChannel))
                    fft_channels ++;
                if (sAnalyzer.channel_active(c->nAnOutChannel))
                    fft_channels ++;
            }

            // Update analyzer parameters
            sAnalyzer.set_reactivity(pReactivity->value());
            if (pShiftGain != NULL)
                sAnalyzer.set_shift(pShiftGain->value() * 100.0f);
            sAnalyzer.set_activity(fft_channels > 0);

            // Update analyzer
            if (sAnalyzer.needs_reconfiguration())
            {
                sAnalyzer.reconfigure();
                sAnalyzer.get_frequencies(vFreqs, vIndexes, SPEC_FREQ_MIN, SPEC_FREQ_MAX, meta::crossover_metadata::MESH_POINTS);
                sync            = true;
            }

            // Operating mode
            size_t op_mode      = pOpMode->value();
            if (nOpMode != op_mode)
            {
                nOpMode         = op_mode;
                sync            = true;
            }

            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c            = &vChannels[i];
                bool csync              = false;
                c->sBypass.set_bypass(pBypass->value() >= 0.5f);
                bool solo               = false;

                // Configure split points
                for (size_t i=0; i<meta::crossover_metadata::BANDS_MAX-1; ++i)
                {
                    xover_split_t *sp   = &c->vSplit[i];
                    sp->nBand           = i + 1;
                    sp->nSlope          = sp->pSlope->value();
                    sp->fFreq           = sp->pFreq->value();
                }

                if (nOpMode == meta::crossover_metadata::CROSS_CLASSIC)
                {
                    dspu::Crossover *xc     = &c->sXOver;

                    // Configure split points for crossover
                    for (size_t i=0; i<meta::crossover_metadata::BANDS_MAX-1; ++i)
                    {
                        xover_split_t *sp   = &c->vSplit[i];

                        xc->set_frequency(i, sp->fFreq);
                        xc->set_slope(i, crossover_slope(sp->nSlope));
                        xc->set_mode(i, crossover_mode(sp->nSlope));
                    }

                    // Configure bands (step 1):
                    for (size_t i=0; i<meta::crossover_metadata::BANDS_MAX; ++i)
                    {
                        xover_band_t *b     = &c->vBands[i];
                        size_t delay        = dspu::millis_to_samples(fSampleRate, b->pDelay->value());
                        float gain          = b->pGain->value();

                        b->sDelay.set_delay(delay);

                        b->bSolo            = b->pSolo->value() >= 0.5f;
                        if ((i > 0) && (c->vSplit[i-1].pSlope->value() <= 0))
                            b->bSolo            = false;
                        b->bMute            = b->pMute->value() >= 0.5f;
                        b->fGain            = (b->pPhase->value() >= 0.5f) ? -GAIN_AMP_0_DB : GAIN_AMP_0_DB;
                        b->bActive          = (i == 0) || (c->vSplit[i-1].nSlope > 0);
                        solo                = solo || b->bSolo;

                        xc->set_gain(i, gain);
                    }

                    // Reconfigure the crossover
                    csync   = (sync) || (xc->needs_reconfiguration());
                    xc->reconfigure();

                    // Output band parameters and update sync curve flag
                    for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                    {
                        xover_band_t *b     = &c->vBands[j];
                        b->pFreqEnd->set_value(xc->get_band_end(j));
                        if (csync)
                        {
                            // Get frequency response for band
                            xc->freq_chart(j, b->vTr, vFreqs, meta::crossover_metadata::MESH_POINTS);
                            dsp::pcomplex_mod(b->vFc, b->vTr, meta::crossover_metadata::MESH_POINTS);

                            b->bSyncCurve       = true;
                        }
                    }
                }
                else
                {
                    dspu::FFTCrossover *xf  = &c->sFFTXOver;

                    // Form the list of splits
                    for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                        c->vBands[j].bActive    = j <= 0;
                    size_t num_sp   = 0;
                    xover_split_t *sp[meta::crossover_metadata::BANDS_MAX];
                    for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX-1; ++j)
                        if (c->vSplit[j].nSlope > 0)
                        {
                            xover_split_t *xsp              = &c->vSplit[j];
                            sp[num_sp++]                    = xsp;
                            c->vBands[xsp->nBand].bActive   = true;
                        }
                    if (num_sp > 1)
                        lsp::qsort_r(sp, num_sp, sizeof(xover_split_t *), compare_splits, NULL);
                    for (size_t i=0; i<=num_sp; ++i)
                    {
                        size_t band = (i > 0) ? sp[i-1]->nBand : 0;
                        xover_band_t *b     = &c->vBands[band];

                        if (i > 0)
                        {
                            xf->enable_hpf(band, true);
                            xf->set_hpf_frequency(band, sp[i-1]->fFreq);
                            xf->set_hpf_slope(band, fft_crossover_slope(sp[i-1]->nSlope));
                        }
                        else
                            xf->disable_hpf(band);

                        if (i < num_sp)
                        {
                            xf->enable_lpf(band, true);
                            xf->set_lpf_frequency(band, sp[i]->fFreq);
                            xf->set_lpf_slope(band, fft_crossover_slope(sp[i]->nSlope));
                            b->pFreqEnd->set_value(sp[i]->fFreq);
                        }
                        else
                        {
                            xf->disable_lpf(band);
                            b->pFreqEnd->set_value(fSampleRate * 0.5f);
                        }
                    }

                    // Configure bands (step 1):
                    for (size_t i=0; i<meta::crossover_metadata::BANDS_MAX; ++i)
                    {
                        xover_band_t *b     = &c->vBands[i];
                        size_t delay        = dspu::millis_to_samples(fSampleRate, b->pDelay->value());
                        float gain          = b->pGain->value();

                        b->sDelay.set_delay(delay);

                        b->bSolo            = b->pSolo->value() >= 0.5f;
                        if ((i > 0) && (c->vSplit[i-1].pSlope->value() <= 0))
                            b->bSolo            = false;
                        b->bMute            = b->pMute->value() >= 0.5f;
                        b->fGain            = (b->pPhase->value() >= 0.5f) ? -GAIN_AMP_0_DB : GAIN_AMP_0_DB;
                        solo                = solo || b->bSolo;

                        // Do we have hi-pass filter?
                        xf->enable_band(i, b->bActive);
                        xf->set_gain(i, gain);
                    }

                    // Reconfigure the crossover
                    csync   = (sync) || (xf->needs_update());
                    xf->update_settings();

                    if (csync)
                    {
                        // Output band parameters and update sync curve flag
                        for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                        {
                            xover_band_t *b     = &c->vBands[j];

                            // Get frequency response for band
                            xf->freq_chart(j, b->vFc, vFreqs, meta::crossover_metadata::MESH_POINTS);
                            b->bSyncCurve       = true;
                        }
                    }
                }

                // Configure bands (step 2):
                for (size_t i=0; i<meta::crossover_metadata::BANDS_MAX; ++i)
                {
                    xover_band_t *b     = &c->vBands[i];
                    if ((solo) && (!b->bSolo))
                        b->bMute            = true;
                }

                if (csync)
                {
                    // Compute amplitude response for the whole crossover
                    dsp::copy(c->vFc, c->vBands[0].vFc, meta::crossover_metadata::MESH_POINTS);
                    for (size_t j=1; j<meta::crossover_metadata::BANDS_MAX; ++j)
                    {
                        xover_band_t *b     = &c->vBands[j];
                        if (b->bActive)
                            dsp::add2(c->vFc, b->vFc, meta::crossover_metadata::MESH_POINTS);
                    }

                    c->bSyncCurve       = true;
                }

                // Request for redraw
                if ((csync) && (pWrapper != NULL))
                    redraw              = true;
            }

            // Global parameters
            fInGain         = pInGain->value();
            fOutGain        = pOutGain->value();
            fZoom           = pZoom->value();
            bMSOut          = (pMSOut != NULL) ? pMSOut->value() >= 0.5f : false;
            bSMApply        = pSMApply->value() >= 0.5f;

            // Report latency
            set_latency(
                (nOpMode == meta::crossover_metadata::CROSS_CLASSIC) ? 0
                : vChannels[0].sFFTXOver.latency());

            if (redraw)
                pWrapper->query_display_draw();
        }

        void crossover::update_sample_rate(long sr)
        {
            // Determine number of channels
            size_t channels     = (nMode == XOVER_MONO) ? 1 : 2;
            size_t max_delay    = dspu::millis_to_samples(sr, meta::crossover_metadata::DELAY_MAX);
            size_t fft_rank     = select_fft_rank(sr);

            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c        = &vChannels[i];

                c->sBypass.init(sr);
                c->sXOver.set_sample_rate(sr);

                // Need to re-initialize FFT crossover?
                if (fft_rank != c->sFFTXOver.rank())
                {
                    c->sFFTXOver.init(fft_rank, meta::crossover_metadata::BANDS_MAX);
                    for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                        c->sFFTXOver.set_handler(j, process_band, this, c);
                    c->sFFTXOver.set_rank(fft_rank);
                    c->sFFTXOver.set_phase(float(i) / float(channels));
                }
                c->sFFTXOver.set_sample_rate(sr);

                // Initialize delay line for each band
                for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                {
                    xover_band_t *b     = &c->vBands[j];
                    b->sDelay.init(max_delay);
                }
            }

            sAnalyzer.set_sample_rate(sr);
        }

        size_t crossover::select_fft_rank(size_t sample_rate)
        {
            const size_t k = (sample_rate + meta::crossover_metadata::FFT_XOVER_FREQ_MIN/2) / meta::crossover_metadata::FFT_XOVER_FREQ_MIN;
            const size_t n = int_log2(k);
            return meta::crossover_metadata::FFT_XOVER_RANK_MIN + n;
        }

        void crossover::ui_activated()
        {
            // Determine number of channels
            size_t channels     = (nMode == XOVER_MONO) ? 1 : 2;

            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c    = &vChannels[i];
                c->bSyncCurve   = true;

                for (size_t i=0; i<meta::crossover_metadata::BANDS_MAX; ++i)
                {
                    xover_band_t *xb    = &c->vBands[i];
                    xb->bSyncCurve      = true;
                }
            }
        }

        void crossover::process_band(void *object, void *subject, size_t band, const float *data, size_t sample, size_t count)
        {
            crossover *self         = static_cast<crossover *>(object);
            channel_t *c            = static_cast<channel_t *>(subject);
            xover_band_t *b         = &c->vBands[band];

            // Process signal of the band
            b->sDelay.process(&b->vResult[sample], data, b->fGain, count);
            if (!b->bMute)
                dsp::add2(&c->vResult[sample], &b->vResult[sample], count);
            else if (self->bSMApply)
                dsp::fill_zero(&b->vResult[sample], count);
        }

        void crossover::process(size_t samples)
        {
            // Determine number of channels
            size_t channels     = (nMode == XOVER_MONO) ? 1 : 2;

            // Prepare pointers
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c        = &vChannels[i];
                c->vIn              = c->pIn->buffer<float>();
                c->vOut             = c->pOut->buffer<float>();

                c->fInLevel         = 0.0f;
                c->fOutLevel        = 0.0f;

                for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                {
                    xover_band_t *b     = &c->vBands[j];
                    b->fOutLevel        = 0.0f;
                    b->vOut             = b->pOut->buffer<float>();
                }
            }

            // Process samples
            while (samples > 0)
            {
                size_t to_do        = lsp_min(samples, BUFFER_SIZE);

                // Apply input gain and M/S transform (if required)
                if (nMode == XOVER_MS)
                {
                    vChannels[0].fInLevel   = lsp_max(vChannels[0].fInLevel, dsp::abs_max(vChannels[0].vIn, to_do) * fInGain);
                    vChannels[1].fInLevel   = lsp_max(vChannels[1].fInLevel, dsp::abs_max(vChannels[1].vIn, to_do) * fInGain);

                    dsp::lr_to_ms(vChannels[0].vInAnalyze, vChannels[1].vInAnalyze, vChannels[0].vIn, vChannels[1].vIn, to_do);
                    dsp::mul_k3(vChannels[0].vBuffer, vChannels[0].vInAnalyze, fInGain, to_do);
                    dsp::mul_k3(vChannels[1].vBuffer, vChannels[1].vInAnalyze, fInGain, to_do);
                    dsp::fill_zero(vChannels[0].vResult, to_do);
                    dsp::fill_zero(vChannels[1].vResult, to_do);
                }
                else if (channels > 1)
                {
                    vChannels[0].fInLevel   = lsp_max(vChannels[0].fInLevel, dsp::abs_max(vChannels[0].vIn, to_do) * fInGain);
                    vChannels[1].fInLevel   = lsp_max(vChannels[1].fInLevel, dsp::abs_max(vChannels[1].vIn, to_do) * fInGain);

                    dsp::copy(vChannels[0].vInAnalyze, vChannels[0].vIn, to_do);
                    dsp::copy(vChannels[1].vInAnalyze, vChannels[1].vIn, to_do);
                    dsp::mul_k3(vChannels[0].vBuffer, vChannels[0].vInAnalyze, fInGain, to_do);
                    dsp::mul_k3(vChannels[1].vBuffer, vChannels[1].vInAnalyze, fInGain, to_do);
                    dsp::fill_zero(vChannels[0].vResult, to_do);
                    dsp::fill_zero(vChannels[1].vResult, to_do);
                }
                else
                {
                    vChannels[0].fInLevel   = lsp_max(vChannels[0].fInLevel, dsp::abs_max(vChannels[0].vIn, to_do) * fInGain);

                    dsp::copy(vChannels[0].vInAnalyze,  vChannels[0].vIn, to_do);
                    dsp::mul_k3(vChannels[0].vBuffer, vChannels[0].vInAnalyze, fInGain, to_do);
                    dsp::fill_zero(vChannels[0].vResult, to_do);
                }

                // Call the crossovers
                for (size_t i=0; i<channels; ++i)
                {
                    channel_t *c        = &vChannels[i];
                    if (nOpMode == meta::crossover_metadata::CROSS_CLASSIC)
                        c->sXOver.process(c->vBuffer, to_do);
                    else
                        c->sFFTXOver.process(c->vBuffer, to_do);
                }

                // Output signal of each band to output buffers
                for (size_t i=0; i<channels; ++i)
                {
                    channel_t *c        = &vChannels[i];

                    for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                    {
                        xover_band_t *b     = &c->vBands[j];
                        if (b->bActive)
                        {
                            b->fOutLevel        = lsp_max(b->fOutLevel, dsp::abs_max(b->vResult, to_do));
                            dsp::copy(b->vOut, b->vResult, to_do);
                        }
                        else
                            dsp::fill_zero(b->vOut, to_do);
                    }
                }

                // Post-process and route signal to outputs via bypasses
                if (nMode == XOVER_MS)
                {
                    dsp::copy(vChannels[0].vOutAnalyze, vChannels[0].vResult, to_do);
                    dsp::copy(vChannels[1].vOutAnalyze, vChannels[1].vResult, to_do);

                    if (!bMSOut)
                        dsp::ms_to_lr(vChannels[0].vResult, vChannels[1].vResult, vChannels[0].vResult, vChannels[1].vResult, to_do);

                    dsp::mul_k2(vChannels[0].vResult, fOutGain, to_do);
                    dsp::mul_k2(vChannels[1].vResult, fOutGain, to_do);

                    vChannels[0].fOutLevel  = lsp_max(vChannels[0].fOutLevel, dsp::abs_max(vChannels[0].vResult, to_do));
                    vChannels[1].fOutLevel  = lsp_max(vChannels[1].fOutLevel, dsp::abs_max(vChannels[1].vResult, to_do));

                    vChannels[0].sBypass.process(vChannels[0].vOut, vChannels[0].vIn, vChannels[0].vResult, to_do);
                    vChannels[1].sBypass.process(vChannels[1].vOut, vChannels[1].vIn, vChannels[1].vResult, to_do);
                }
                else if (channels > 1)
                {
                    dsp::copy(vChannels[0].vOutAnalyze, vChannels[0].vResult, to_do);
                    dsp::copy(vChannels[1].vOutAnalyze, vChannels[1].vResult, to_do);

                    dsp::mul_k2(vChannels[0].vResult, fOutGain, to_do);
                    dsp::mul_k2(vChannels[1].vResult, fOutGain, to_do);

                    vChannels[0].fOutLevel  = lsp_max(vChannels[0].fOutLevel, dsp::abs_max(vChannels[0].vResult, to_do));
                    vChannels[1].fOutLevel  = lsp_max(vChannels[1].fOutLevel, dsp::abs_max(vChannels[1].vResult, to_do));

                    vChannels[0].sBypass.process(vChannels[0].vOut, vChannels[0].vIn, vChannels[0].vResult, to_do);
                    vChannels[1].sBypass.process(vChannels[1].vOut, vChannels[1].vIn, vChannels[1].vResult, to_do);
                }
                else
                {
                    dsp::copy(vChannels[0].vOutAnalyze, vChannels[0].vResult, to_do);

                    dsp::mul_k2(vChannels[0].vResult, fOutGain, to_do);
                    vChannels[0].fOutLevel  = lsp_max(vChannels[0].fOutLevel, dsp::abs_max(vChannels[0].vResult, to_do));

                    vChannels[0].sBypass.process(vChannels[0].vOut, vChannels[0].vIn, vChannels[0].vResult, to_do);
                }

                // Call the analyzer
                sAnalyzer.process(vAnalyze, to_do);

                // Update pointers
                for (size_t i=0; i<channels; ++i)
                {
                    channel_t *c        = &vChannels[i];
                    c->vIn             += to_do;
                    c->vOut            += to_do;

                    for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                    {
                        xover_band_t *b     = &c->vBands[j];
                        b->vOut            += to_do;
                    }
                }

                samples            -= to_do;
            }

            //---------------------------------------------------------------------
            // Output meters and graphs
            plug::mesh_t *mesh;

            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c        = &vChannels[i];

                c->pInLvl->set_value(c->fInLevel);
                c->pOutLvl->set_value(c->fOutLevel);

                // Output transfer function of the mesh
                mesh        = ((c->bSyncCurve) && (c->pAmpGraph != NULL)) ? c->pAmpGraph->buffer<plug::mesh_t>() : NULL;
                if ((mesh != NULL) && (mesh->isEmpty()))
                {
                    dsp::copy(mesh->pvData[0], vFreqs, meta::crossover_metadata::MESH_POINTS);
                    dsp::copy(mesh->pvData[1], c->vFc, meta::crossover_metadata::MESH_POINTS);
                    mesh->data(2, meta::crossover_metadata::MESH_POINTS);

                    c->bSyncCurve       = false;
                }

                // Sync output for each band
                for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                {
                    xover_band_t *b     = &c->vBands[j];
                    b->pOutLevel->set_value(b->fOutLevel);

                    // Pass transfer function of the band
                    mesh        = ((b->bSyncCurve) && (b->pAmpGraph != NULL)) ? b->pAmpGraph->buffer<plug::mesh_t>() : NULL;
                    if ((mesh != NULL) && (mesh->isEmpty()))
                    {
                        float *x = mesh->pvData[0];
                        float *y = mesh->pvData[1];

                        // Fill mesh
                        dsp::copy(&x[2], vFreqs, meta::crossover_metadata::MESH_POINTS);
                        dsp::copy(&y[2], b->vFc, meta::crossover_metadata::MESH_POINTS);

                        // Add extra points
                        x[0]    = SPEC_FREQ_MIN*0.5f;
                        x[1]    = x[0];
                        y[0]    = 0.0f;
                        y[1]    = y[2];
                        x      += meta::crossover_metadata::MESH_POINTS + 2;
                        y      += meta::crossover_metadata::MESH_POINTS + 2;
                        x[0]    = SPEC_FREQ_MAX*2.0f;
                        x[1]    = x[0];
                        y[0]    = y[-1];
                        y[1]    = 0.0f;

                        // Mark mesh as synchronized
                        mesh->data(2, meta::crossover_metadata::MESH_POINTS + 4);

                        b->bSyncCurve       = false;
                    }
                }

                // Output spectrum analysis for input channel
                mesh        = ((sAnalyzer.channel_active(c->nAnInChannel)) && (c->pFftIn != NULL)) ? c->pFftIn->buffer<plug::mesh_t>() : NULL;
                if ((mesh != NULL) && (mesh->isEmpty()))
                {
                    // Add extra points
                    mesh->pvData[0][0] = SPEC_FREQ_MIN*0.5f;
                    mesh->pvData[0][meta::crossover_metadata::MESH_POINTS+1] = SPEC_FREQ_MAX * 2.0f;
                    mesh->pvData[1][0] = 0.0f;
                    mesh->pvData[1][meta::crossover_metadata::MESH_POINTS+1] = 0.0f;

                    dsp::copy(&mesh->pvData[0][1], vFreqs, meta::crossover_metadata::MESH_POINTS);
                    sAnalyzer.get_spectrum(c->nAnInChannel, &mesh->pvData[1][1], vIndexes, meta::crossover_metadata::MESH_POINTS);

                    // Mark mesh containing data
                    mesh->data(2, meta::crossover_metadata::MESH_POINTS + 2);
                }

                // Output spectrum analysis for output channel
                mesh        = ((sAnalyzer.channel_active(c->nAnOutChannel)) && (c->pFftOut != NULL)) ? c->pFftOut->buffer<plug::mesh_t>() : NULL;
                if ((mesh != NULL) && (mesh->isEmpty()))
                {
                    dsp::copy(mesh->pvData[0], vFreqs, meta::crossover_metadata::MESH_POINTS);
                    sAnalyzer.get_spectrum(c->nAnOutChannel, mesh->pvData[1], vIndexes, meta::crossover_metadata::MESH_POINTS);

                    // Mark mesh containing data
                    mesh->data(2, meta::crossover_metadata::MESH_POINTS);
                }
            }
        }

        bool crossover::inline_display(plug::ICanvas *cv, size_t width, size_t height)
        {
            // Check proportions
            if (height > (M_RGOLD_RATIO * width))
                height  = M_RGOLD_RATIO * width;

            // Init canvas
            if (!cv->init(width, height))
                return false;
            width   = cv->width();
            height  = cv->height();

            // Clear background
            bool bypassing = vChannels[0].sBypass.bypassing();
            cv->set_color_rgb((bypassing) ? CV_DISABLED : CV_BACKGROUND);
            cv->paint();

            // Draw axis
            cv->set_line_width(1.0);

            // "-72 db / (:zoom ** 3)" max="24 db * :zoom"

            float miny  = logf(GAIN_AMP_M_72_DB / dsp::ipowf(fZoom, 3));
            float maxy  = logf(GAIN_AMP_P_24_DB * fZoom);

            float zx    = 1.0f/SPEC_FREQ_MIN;
            float zy    = dsp::ipowf(fZoom, 3)/GAIN_AMP_M_72_DB;
            float dx    = width/logf(SPEC_FREQ_MAX/SPEC_FREQ_MIN);
            float dy    = height/(miny-maxy);

            // Draw vertical lines
            cv->set_color_rgb(CV_YELLOW, 0.5f);
            for (float i=100.0f; i<SPEC_FREQ_MAX; i *= 10.0f)
            {
                float ax = dx*(logf(i*zx));
                cv->line(ax, 0, ax, height);
            }

            // Draw horizontal lines
            cv->set_color_rgb(CV_WHITE, 0.5f);
            for (float i=GAIN_AMP_M_72_DB; i<GAIN_AMP_P_24_DB; i *= GAIN_AMP_P_12_DB)
            {
                float ay = height + dy*(logf(i*zy));
                cv->line(0, ay, width, ay);
            }

            // Allocate buffer: f, x, y, tr
            size_t xwidth       = width + 4;
            pIDisplay           = core::IDBuffer::reuse(pIDisplay, 4, xwidth);
            core::IDBuffer *b   = pIDisplay;
            if (b == NULL)
                return false;

            // Initialize mesh
            size_t channels = ((nMode == XOVER_MONO) || (nMode == XOVER_STEREO)) ? 1 : 2;
            static uint32_t c_colors[] = {
                    CV_MIDDLE_CHANNEL, CV_MIDDLE_CHANNEL,
                    CV_MIDDLE_CHANNEL, CV_MIDDLE_CHANNEL,
                    CV_LEFT_CHANNEL, CV_RIGHT_CHANNEL,
                    CV_MIDDLE_CHANNEL, CV_SIDE_CHANNEL
                   };

            bool aa = cv->set_anti_aliasing(true);
            cv->set_line_width(2);

            // Initialize frequency list
            float delta     = float(meta::crossover_metadata::MESH_POINTS) / float(width);
            for (size_t i=0; i<width; ++i)
            {
                size_t idx      = i * delta;
                b->v[0][i+2]    = vFreqs[idx];
            }

            b->v[0][0]          = SPEC_FREQ_MIN*0.5f;
            b->v[0][1]          = SPEC_FREQ_MIN*0.5f;
            b->v[0][width+2]    = SPEC_FREQ_MAX*2.0f;
            b->v[0][width+3]    = SPEC_FREQ_MAX*2.0f;

            // Draw curves
            uint32_t color;
            Color col(CV_MESH);

            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c    = &vChannels[i];

                // Draw the filter curve for each band
                for (size_t j=0; j<meta::crossover_metadata::BANDS_MAX; ++j)
                {
                    xover_band_t *xb    = &c->vBands[j];
                    if (!xb->bActive)
                        continue;

                    for (size_t k=0; k<width; ++k)
                    {
                        size_t idx      = k * delta;
                        b->v[3][k+2]    = xb->vFc[idx];
                    }
                    b->v[3][0]          = 0.0f;
                    b->v[3][1]          = b->v[3][2];
                    b->v[3][width+2]    = b->v[3][width+1];
                    b->v[3][width+3]    = 0.0f;

                    dsp::fill(b->v[1], 0.0f, xwidth);
                    dsp::fill(b->v[2], height, xwidth);
                    dsp::axis_apply_log1(b->v[1], b->v[0], zx, dx, xwidth);
                    dsp::axis_apply_log1(b->v[2], b->v[3], zy, dy, xwidth);

                    col.hue(float(j) / float(meta::crossover_metadata::BANDS_MAX));
                    color = (bypassing || !(active())) ? CV_SILVER : col.rgb24();
                    Color stroke(color), fill(color, 0.75f);
                    cv->draw_poly(b->v[1], b->v[2], xwidth, stroke, fill);
                }

                // Draw overall curve for each channel
                for (size_t k=0; k<width; ++k)
                {
                    size_t idx      = k * delta;
                    b->v[3][k+2]    = c->vFc[idx];
                }
                b->v[3][0]          = 0.0f;
                b->v[3][1]          = b->v[3][2];
                b->v[3][width+2]    = b->v[3][width+1];
                b->v[3][width+3]    = 0.0f;

                dsp::fill(b->v[1], 0.0f, xwidth);
                dsp::fill(b->v[2], height, xwidth);
                dsp::axis_apply_log1(b->v[1], b->v[0], zx, dx, xwidth);
                dsp::axis_apply_log1(b->v[2], b->v[3], zy, dy, xwidth);

                uint32_t color = (bypassing || !(active())) ? CV_SILVER : c_colors[nMode*2 + i];
                cv->set_color_rgb(color);
                cv->draw_lines(b->v[1], b->v[2], xwidth);
            }
            cv->set_anti_aliasing(aa);

            return true;
        }

        void crossover::dump(dspu::IStateDumper *v) const
        {
            plug::Module::dump(v);

            // Determine number of channels
            size_t channels     = (nMode == XOVER_MONO) ? 1 : 2;

            v->write_object("sAnalyzer", &sAnalyzer);
            v->write("nMode", nMode);
            v->write("nOpMode", nOpMode);

            v->begin_array("vChannels", vChannels, channels);
            {
                for (size_t i=0; i<channels; ++i)
                {
                    const channel_t *c = &vChannels[i];

                    v->begin_object(c, sizeof(channel_t));
                    {
                        v->write_object("sBypasss", &c->sBypass);
                        v->write_object("sXOver", &c->sXOver);
                        v->write_object("sFFTXOver", &c->sFFTXOver);

                        v->begin_array("vSplit", c->vSplit, meta::crossover_metadata::BANDS_MAX-1);
                        {
                            for (size_t i=0; i<(meta::crossover_metadata::BANDS_MAX-1); ++i)
                            {
                                const xover_split_t *sp = &c->vSplit[i];

                                v->begin_object(sp, sizeof(xover_split_t));
                                {
                                    v->write("nBand", sp->nBand);
                                    v->write("nSlope", sp->nSlope);
                                    v->write("fFreq", sp->fFreq);
                                    v->write("pSlope", sp->pSlope);
                                    v->write("pFreq", sp->pFreq);
                                }
                                v->end_object();
                            }
                        }
                        v->end_array();

                        v->begin_array("vBands", c->vBands, meta::crossover_metadata::BANDS_MAX);
                        {
                            for (size_t i=0; i<meta::crossover_metadata::BANDS_MAX; ++i)
                            {
                                const xover_band_t *b = &c->vBands[i];

                                v->begin_object(v, sizeof(xover_band_t));
                                {
                                    v->write_object("sDelay", &b->sDelay);

                                    v->write("vOut", b->vOut);
                                    v->write("vResult", b->vResult);
                                    v->write("vTr", b->vTr);
                                    v->write("vFc", b->vFc);

                                    v->write("bSolo", b->bSolo);
                                    v->write("bMute", b->bMute);
                                    v->write("fGain", b->fGain);
                                    v->write("fOutLevel", b->fOutLevel);
                                    v->write("bSyncCurve", b->bSyncCurve);

                                    v->write("pSolo", b->pSolo);
                                    v->write("pMute", b->pMute);
                                    v->write("pPhase", b->pPhase);
                                    v->write("pGain", b->pGain);
                                    v->write("pDelay", b->pDelay);
                                    v->write("pOutLevel", b->pOutLevel);
                                    v->write("pFreqEnd", b->pFreqEnd);
                                    v->write("pOut", b->pOut);
                                    v->write("pAmpGraph", b->pAmpGraph);
                                }
                                v->end_object();
                            }
                        }
                        v->end_array();

                        v->write("vIn", c->vIn);
                        v->write("vOut", c->vOut);
                        v->write("vInAnalyze", c->vInAnalyze);
                        v->write("vOutAnalyze", c->vOutAnalyze);
                        v->write("vBuffer", c->vBuffer);
                        v->write("vResult", c->vResult);
                        v->write("vTr", c->vTr);
                        v->write("vFc", c->vFc);

                        v->write("nAnInChannel", c->nAnInChannel);
                        v->write("nAnOutChannel", c->nAnOutChannel);
                        v->write("bSyncCurve", c->bSyncCurve);
                        v->write("fInLevel", c->fInLevel);
                        v->write("fOutLevel", c->fOutLevel);

                        v->write("pIn", c->pIn);
                        v->write("pOut", c->pOut);
                        v->write("pFftIn", c->pFftIn);
                        v->write("pFftInSw", c->pFftInSw);
                        v->write("pFftOut", c->pFftOut);
                        v->write("pFftOutSw", c->pFftOutSw);
                        v->write("pAmpGraph", c->pAmpGraph);
                        v->write("pInLvl", c->pInLvl);
                        v->write("pOutLvl", c->pOutLvl);
                    }
                    v->end_object();
                }
            }
            v->end_array();

            v->writev("vAnalyze", vAnalyze, 4);

            v->write("fInGain", fInGain);
            v->write("fOutGain", fOutGain);
            v->write("fZoom", fZoom);
            v->write("bMSOut", bMSOut);
            v->write("bSMApply", bSMApply);

            v->write("pData", pData);
            v->write("vFreqs", vFreqs);
            v->write("vCurve", vCurve);
            v->write("vIndexes", vIndexes);
            v->write("pIDisplay", pIDisplay);

            v->write("pBypass", pBypass);
            v->write("pOpMode", pOpMode);
            v->write("pSMApply", pSMApply);
            v->write("pInGain", pInGain);
            v->write("pOutGain", pOutGain);
            v->write("pReactivity", pReactivity);
            v->write("pShiftGain", pShiftGain);
            v->write("pZoom", pZoom);
            v->write("pMSOut", pMSOut);
        }

    } /* namespace plugins */
} /* namespace lsp */


