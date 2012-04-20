/*
 * Copyright (c) 2011 Joseph Gaeddert
 * Copyright (c) 2011 Virginia Polytechnic Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// spgram (spectral periodogram)
//

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <complex.h>
#include "liquid.internal.h"

struct spgram_s {
    // options
    unsigned int nfft;      // FFT length
    unsigned int M;         // number of input samples in FFT
    unsigned int delay;     // number of samples before FFT taken
    float alpha;            // filter

    windowcf buffer;        // input buffer
    float complex * x;      // pointer to input array (allocated)
    float complex * X;      // output fft (allocated)
    float complex * w;      // tapering window
    float * psd;            // psd (allocated)
    FFT_PLAN p;             // fft plan

    unsigned int num_windows; // number of fft windows accumulated
    unsigned int index;     //
};

// create spgram object
//  _nfft   :   fft size
//  _alpha  :   averaging factor
spgram spgram_create(unsigned int _nfft,
                     float _alpha)
{
    unsigned int _m     = _nfft / 4;    // window size
    unsigned int _delay = _nfft / 8;    // delay between transforms

    return spgram_create_advanced(_nfft, _m, _delay, _alpha);
}


// create spgram object (advanced method)
//  _nfft   :   fft size
//  _m      :   window size
//  _delay  :   number of samples between transforms
//  _alpha  :   averaging factor
spgram spgram_create_advanced(unsigned int _nfft,
                              unsigned int _m,
                              unsigned int _delay,
                              float        _alpha)
{
    // validate input
    if (_nfft < 2) {
        fprintf(stderr,"error: spgram_create_advanced(), fft size must be at least 2\n");
        exit(1);
    } else if (_m > _nfft) {
        fprintf(stderr,"error: spgram_create_advanced(), window size cannot exceed fft size\n");
        exit(1);
    } else if (_delay == 0) {
        fprintf(stderr,"error: spgram_create_advanced(), delay must be greater than zero\n");
        exit(1);
    } else if (_alpha <= 0.0f || _alpha > 1.0f) {
        fprintf(stderr,"error: spgram_create_advanced(), alpha must be in (0,1]\n");
        exit(1);
    }

    spgram q = (spgram) malloc(sizeof(struct spgram_s));

    // input parameters
    q->nfft  = _nfft;
    q->alpha = _alpha;
    q->M     = _m;
    q->delay = _delay;

    q->buffer = windowcf_create(q->M);
    q->x = (float complex*) malloc((q->nfft)*sizeof(float complex));
    q->X = (float complex*) malloc((q->nfft)*sizeof(float complex));
    q->w = (float complex*) malloc((q->M)*sizeof(float complex));
    q->psd = (float*) malloc((q->nfft)*sizeof(float));

    q->p = FFT_CREATE_PLAN(q->nfft, q->x, q->X, FFT_DIR_FORWARD, FFT_METHOD);

    // initialize tapering window, scaled by window length size
    // TODO : scale by window magnitude, FFT size as well
    unsigned int i;
    for (i=0; i<q->M; i++)
        q->w[i] = hamming(i,q->M) / (float)(q->M);

    // clear FFT input
    for (i=0; i<q->nfft; i++)
        q->x[i] = 0.0f;

    // reset the spgram object
    spgram_reset(q);

    return q;
}

// destroy spgram object
void spgram_destroy(spgram _q)
{
    windowcf_destroy(_q->buffer);

    free(_q->x);
    free(_q->X);
    free(_q->w);
    free(_q->psd);
    FFT_DESTROY_PLAN(_q->p);
    free(_q);
}

// resets the internal state of the spgram object
void spgram_reset(spgram _q)
{
    // clear the window buffer
    windowcf_clear(_q->buffer);

    // reset counters
    _q->num_windows = 0;
    _q->index = 0;
}

// push samples into spgram object
//  _q      :   spgram object
//  _x      :   input buffer [size: _n x 1]
//  _n      :   input buffer length
void spgram_push(spgram _q,
                 float complex * _x,
                 unsigned int _n)
{
    // push/write samples
    //windowcf_write(_q->buffer, _x, _n);


    unsigned int i;
    for (i=0; i<_n; i++) {
        windowcf_push(_q->buffer, _x[i]);

        _q->index++;

        if (_q->index == _q->delay) {
            unsigned int j;

            // reset counter
            _q->index = 0;

            // read buffer, copy to FFT input (applying window)
            float complex * rc;
            windowcf_read(_q->buffer, &rc);
            unsigned int k;
            for (k=0; k<_q->M; k++)
                _q->x[k] = rc[k] * _q->w[k];

            // execute fft
            FFT_EXECUTE(_q->p);
            
            // accumulate output
            if (_q->num_windows == 0) {
                // first window; copy internally
                for (j=0; j<_q->nfft; j++)
                    _q->psd[j] = cabsf(_q->X[j]);
            } else {
                // 
                for (j=0; j<_q->nfft; j++)
                    _q->psd[j] = (1.0f - _q->alpha)*_q->psd[j] + _q->alpha*cabsf(_q->X[j]);
            }

            _q->num_windows++;
        }
    }
}

// compute spectral periodogram output
//  _q      :   spgram object
//  _X      :   output spectrum [dB]
void spgram_execute(spgram _q,
                    float * _X)
{
    // check to see if any transforms have been taken
    if (_q->num_windows == 0) {
        // copy zeros to output
        unsigned int i;
        for (i=0; i<_q->nfft; i++)
            _X[i] = 0.0f;

        return;
    }

    // copy shifted PSD contents to output
    unsigned int i;
    unsigned int k;
    unsigned int nfft_2 = _q->nfft / 2;
    for (i=0; i<_q->nfft; i++) {
        k = (i + nfft_2) % _q->nfft;
        _X[i] = 20*log10f(fabsf(_q->psd[k]));
    }
}

