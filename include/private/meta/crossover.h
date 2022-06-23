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

#ifndef PRIVATE_META_CROSSOVER_H_
#define PRIVATE_META_CROSSOVER_H_

#include <lsp-plug.in/plug-fw/meta/types.h>
#include <lsp-plug.in/plug-fw/const.h>
#include <lsp-plug.in/dsp-units/misc/windows.h>

namespace lsp
{
    namespace meta
    {
        struct crossover_metadata
        {
            // Maximum supported number of bands
            static const size_t         BANDS_MAX               = 8;
            static const size_t         SLOPE_DFL               = 2;

            // In/out gain
            static constexpr float          IN_GAIN_DFL         = GAIN_AMP_0_DB;
            static constexpr float          OUT_GAIN_DFL        = GAIN_AMP_0_DB;

            // Makeup gain for each band
            static constexpr float          MAKEUP_MIN          = GAIN_AMP_M_60_DB;
            static constexpr float          MAKEUP_MAX          = GAIN_AMP_P_60_DB;
            static constexpr float          MAKEUP_DFL          = GAIN_AMP_0_DB;
            static constexpr float          MAKEUP_STEP         = 0.05f;

            // Delay for each band
            static constexpr float          DELAY_MIN           = 0.0f;
            static constexpr float          DELAY_MAX           = 1000.0f;
            static constexpr float          DELAY_DFL           = 0.0f;
            static constexpr float          DELAY_STEP          = 0.5f;

            // Split frequency
            static constexpr float          SPLIT_FREQ_MIN      = 10.0f;
            static constexpr float          SPLIT_FREQ_MAX      = 20000.0f;
            static constexpr float          SPLIT_FREQ_DFL      = 1000.0f;
            static constexpr float          SPLIT_FREQ_STEP     = 0.002f;

            // Frequency analysis
            static constexpr float          REACT_TIME_MIN      = 0.000;
            static constexpr float          REACT_TIME_MAX      = 1.000;
            static constexpr float          REACT_TIME_DFL      = 0.200;
            static constexpr float          REACT_TIME_STEP     = 0.001;
            static const size_t         FFT_RANK                = 13;
            static const size_t         FFT_ITEMS               = 1 << FFT_RANK;
            static const size_t         MESH_POINTS             = 640;
            static const size_t         FILTER_MESH_POINTS      = MESH_POINTS + 2;
            static const size_t         FFT_WINDOW              = dspu::windows::HANN;
            static const size_t         REFRESH_RATE            = 20;

            // Output frequency
            static constexpr float          OUT_FREQ_MIN        = 0.0f;
            static constexpr float          OUT_FREQ_MAX        = MAX_SAMPLE_RATE;
            static constexpr float          OUT_FREQ_DFL        = 1000.0f;
            static constexpr float          OUT_FREQ_STEP       = 0.002f;

            // Zoom
            static constexpr float          ZOOM_MIN            = GAIN_AMP_M_18_DB;
            static constexpr float          ZOOM_MAX            = GAIN_AMP_0_DB;
            static constexpr float          ZOOM_DFL            = GAIN_AMP_0_DB;
            static constexpr float          ZOOM_STEP           = 0.0125f;
        };

        extern const meta::plugin_t crossover_mono;
        extern const meta::plugin_t crossover_stereo;
        extern const meta::plugin_t crossover_lr;
        extern const meta::plugin_t crossover_ms;
    } // namespace meta
} // namespace lsp


#endif /* PRIVATE_META_CROSSOVER_H_ */
