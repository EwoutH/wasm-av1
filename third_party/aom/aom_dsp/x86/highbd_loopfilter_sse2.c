/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <emmintrin.h>  // SSE2

#include "./aom_dsp_rtcd.h"
#include "aom_dsp/x86/lpf_common_sse2.h"
#include "aom_ports/emmintrin_compat.h"
#include "aom_ports/mem.h"

static AOM_FORCE_INLINE void pixel_clamp(const __m128i *min, const __m128i *max,
                                         __m128i *pixel) {
  *pixel = _mm_min_epi16(*pixel, *max);
  *pixel = _mm_max_epi16(*pixel, *min);
}

static AOM_FORCE_INLINE __m128i abs_diff16(__m128i a, __m128i b) {
  return _mm_or_si128(_mm_subs_epu16(a, b), _mm_subs_epu16(b, a));
}

static INLINE void get_limit(const uint8_t *bl, const uint8_t *l,
                             const uint8_t *t, int bd, __m128i *blt,
                             __m128i *lt, __m128i *thr, __m128i *t80_out) {
  const int shift = bd - 8;
  const __m128i zero = _mm_setzero_si128();

  __m128i x = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)bl), zero);
  *blt = _mm_slli_epi16(x, shift);

  x = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)l), zero);
  *lt = _mm_slli_epi16(x, shift);

  x = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)t), zero);
  *thr = _mm_slli_epi16(x, shift);

  *t80_out = _mm_set1_epi16(1 << (bd - 1));
}

static INLINE void get_limit_dual(
    const uint8_t *_blimit0, const uint8_t *_limit0, const uint8_t *_thresh0,
    const uint8_t *_blimit1, const uint8_t *_limit1, const uint8_t *_thresh1,
    int bd, __m128i *blt_out, __m128i *lt_out, __m128i *thr_out,
    __m128i *t80_out) {
  const int shift = bd - 8;
  const __m128i zero = _mm_setzero_si128();

  __m128i x0 =
      _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_blimit0), zero);
  __m128i x1 =
      _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_blimit1), zero);
  x0 = _mm_unpacklo_epi64(x0, x1);
  *blt_out = _mm_slli_epi16(x0, shift);

  x0 = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_limit0), zero);
  x1 = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_limit1), zero);
  x0 = _mm_unpacklo_epi64(x0, x1);
  *lt_out = _mm_slli_epi16(x0, shift);

  x0 = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_thresh0), zero);
  x1 = _mm_unpacklo_epi8(_mm_load_si128((const __m128i *)_thresh1), zero);
  x0 = _mm_unpacklo_epi64(x0, x1);
  *thr_out = _mm_slli_epi16(x0, shift);

  *t80_out = _mm_set1_epi16(1 << (bd - 1));
}

static INLINE void load_highbd_pixel(const uint16_t *s, int size, int pitch,
                                     __m128i *p, __m128i *q) {
  int i;
  for (i = 0; i < size; i++) {
    p[i] = _mm_loadu_si128((__m128i *)(s - (i + 1) * pitch));
    q[i] = _mm_loadu_si128((__m128i *)(s + i * pitch));
  }
}

static INLINE void highbd_hev_mask(const __m128i *p0q0, const __m128i *p1q1,
                                   const __m128i *t, __m128i *abs_p1p0,
                                   __m128i *hev) {
  *abs_p1p0 = abs_diff16(*p1q1, *p0q0);
  __m128i abs_q1q0 = _mm_srli_si128(*abs_p1p0, 8);
  __m128i h = _mm_max_epi16(*abs_p1p0, abs_q1q0);
  h = _mm_subs_epu16(h, *t);

  const __m128i ffff = _mm_set1_epi16(0xFFFF);
  const __m128i zero = _mm_setzero_si128();
  *hev = _mm_xor_si128(_mm_cmpeq_epi16(h, zero), ffff);
  // replicate for the further "merged variables" usage
  *hev = _mm_unpacklo_epi64(*hev, *hev);
}

static INLINE void highbd_filter_mask(const __m128i *p, const __m128i *q,
                                      const __m128i *l, const __m128i *bl,
                                      __m128i *mask) {
  __m128i abs_p0q0 = abs_diff16(p[0], q[0]);
  __m128i abs_p1q1 = abs_diff16(p[1], q[1]);
  abs_p0q0 = _mm_adds_epu16(abs_p0q0, abs_p0q0);
  abs_p1q1 = _mm_srli_epi16(abs_p1q1, 1);

  const __m128i zero = _mm_setzero_si128();
  const __m128i one = _mm_set1_epi16(1);
  const __m128i ffff = _mm_set1_epi16(0xFFFF);
  __m128i max = _mm_subs_epu16(_mm_adds_epu16(abs_p0q0, abs_p1q1), *bl);
  max = _mm_xor_si128(_mm_cmpeq_epi16(max, zero), ffff);
  max = _mm_and_si128(max, _mm_adds_epu16(*l, one));

  int i;
  for (i = 1; i < 4; ++i) {
    max = _mm_max_epi16(max, abs_diff16(p[i], p[i - 1]));
    max = _mm_max_epi16(max, abs_diff16(q[i], q[i - 1]));
  }
  max = _mm_subs_epu16(max, *l);
  *mask = _mm_cmpeq_epi16(max, zero);  // return ~mask
}

static INLINE void flat_mask_internal(const __m128i *th, const __m128i *p,
                                      const __m128i *q, int bd, int start,
                                      int end, __m128i *flat) {
  int i;
  __m128i max =
      _mm_max_epi16(abs_diff16(q[start], q[0]), abs_diff16(p[start], p[0]));

  for (i = start + 1; i < end; ++i) {
    max = _mm_max_epi16(max, abs_diff16(p[i], p[0]));
    max = _mm_max_epi16(max, abs_diff16(q[i], q[0]));
  }

  __m128i ft;
  if (bd == 8)
    ft = _mm_subs_epu16(max, *th);
  else if (bd == 10)
    ft = _mm_subs_epu16(max, _mm_slli_epi16(*th, 2));
  else  // bd == 12
    ft = _mm_subs_epu16(max, _mm_slli_epi16(*th, 4));

  const __m128i zero = _mm_setzero_si128();
  *flat = _mm_cmpeq_epi16(ft, zero);
}

// Note:
//  Access p[3-1], p[0], and q[3-1], q[0]
static INLINE void highbd_flat_mask4(const __m128i *th, const __m128i *p,
                                     const __m128i *q, __m128i *flat, int bd) {
  // check the distance 1,2,3 against 0
  flat_mask_internal(th, p, q, bd, 1, 4, flat);
}

// Note:
//  access p[6-4], p[0], and q[6-4], q[0]
static INLINE void highbd_flat_mask4_13(const __m128i *th, const __m128i *p,
                                        const __m128i *q, __m128i *flat,
                                        int bd) {
  flat_mask_internal(th, p, q, bd, 4, 7, flat);
}

// Note:
//  access p[7-4], p[0], and q[7-4], q[0]
static INLINE void highbd_flat_mask5(const __m128i *th, const __m128i *p,
                                     const __m128i *q, __m128i *flat, int bd) {
  flat_mask_internal(th, p, q, bd, 4, 8, flat);
}

static AOM_FORCE_INLINE void highbd_filter4_sse2(__m128i *p1p0, __m128i *q1q0,
                                                 __m128i *hev, __m128i *mask,
                                                 __m128i *qs1qs0,
                                                 __m128i *ps1ps0, __m128i *t80,
                                                 int bd) {
  const __m128i zero = _mm_setzero_si128();
  const __m128i one = _mm_set1_epi16(1);
  const __m128i pmax =
      _mm_subs_epi16(_mm_subs_epi16(_mm_slli_epi16(one, bd), one), *t80);
  const __m128i pmin = _mm_subs_epi16(zero, *t80);

  const __m128i t3t4 = _mm_set_epi16(3, 3, 3, 3, 4, 4, 4, 4);
  __m128i ps1ps0_work, qs1qs0_work, work;
  __m128i filt, filter2filter1, filter2filt, filter1filt;

  ps1ps0_work = _mm_subs_epi16(*p1p0, *t80);
  qs1qs0_work = _mm_subs_epi16(*q1q0, *t80);

  work = _mm_subs_epi16(ps1ps0_work, qs1qs0_work);
  pixel_clamp(&pmin, &pmax, &work);
  filt = _mm_and_si128(_mm_srli_si128(work, 8), *hev);

  filt = _mm_subs_epi16(filt, work);
  filt = _mm_subs_epi16(filt, work);
  filt = _mm_subs_epi16(filt, work);
  // (aom_filter + 3 * (qs0 - ps0)) & mask
  pixel_clamp(&pmin, &pmax, &filt);
  filt = _mm_and_si128(filt, *mask);
  filt = _mm_unpacklo_epi64(filt, filt);

  filter2filter1 = _mm_adds_epi16(filt, t3t4); /* signed_short_clamp */
  pixel_clamp(&pmin, &pmax, &filter2filter1);
  filter2filter1 = _mm_srai_epi16(filter2filter1, 3); /* >> 3 */

  filt = _mm_unpacklo_epi64(filter2filter1, filter2filter1);

  // filt >> 1
  filt = _mm_adds_epi16(filt, one);
  filt = _mm_srai_epi16(filt, 1);
  filt = _mm_andnot_si128(*hev, filt);

  filter2filt = _mm_unpackhi_epi64(filter2filter1, filt);
  filter1filt = _mm_unpacklo_epi64(filter2filter1, filt);

  qs1qs0_work = _mm_subs_epi16(qs1qs0_work, filter1filt);
  ps1ps0_work = _mm_adds_epi16(ps1ps0_work, filter2filt);

  pixel_clamp(&pmin, &pmax, &qs1qs0_work);
  pixel_clamp(&pmin, &pmax, &ps1ps0_work);

  *qs1qs0 = _mm_adds_epi16(qs1qs0_work, *t80);
  *ps1ps0 = _mm_adds_epi16(ps1ps0_work, *t80);
}

static INLINE void highbd_filter4_dual_sse2(__m128i *p, __m128i *q, __m128i *ps,
                                            __m128i *qs, const __m128i *mask,
                                            const __m128i *th, int bd,

                                            __m128i *t80) {
  __m128i ps0 = _mm_subs_epi16(p[0], *t80);
  __m128i ps1 = _mm_subs_epi16(p[1], *t80);
  __m128i qs0 = _mm_subs_epi16(q[0], *t80);
  __m128i qs1 = _mm_subs_epi16(q[1], *t80);
  const __m128i one = _mm_set1_epi16(1);
  const __m128i pmax =
      _mm_subs_epi16(_mm_subs_epi16(_mm_slli_epi16(one, bd), one), *t80);

  const __m128i zero = _mm_setzero_si128();
  const __m128i pmin = _mm_subs_epi16(zero, *t80);
  __m128i filter = _mm_subs_epi16(ps1, qs1);
  pixel_clamp(&pmin, &pmax, &filter);

  // hev_filter
  __m128i hev;
  const __m128i abs_p1p0 = abs_diff16(p[1], p[0]);
  const __m128i abs_q1q0 = abs_diff16(q[1], q[0]);
  __m128i h = _mm_max_epi16(abs_p1p0, abs_q1q0);
  h = _mm_subs_epu16(h, *th);
  const __m128i ffff = _mm_cmpeq_epi16(h, h);
  hev = _mm_xor_si128(_mm_cmpeq_epi16(h, zero), ffff);

  filter = _mm_and_si128(filter, hev);

  const __m128i x = _mm_subs_epi16(qs0, ps0);
  filter = _mm_adds_epi16(filter, x);
  filter = _mm_adds_epi16(filter, x);
  filter = _mm_adds_epi16(filter, x);
  pixel_clamp(&pmin, &pmax, &filter);
  filter = _mm_and_si128(filter, *mask);
  const __m128i t3 = _mm_set1_epi16(3);
  const __m128i t4 = _mm_set1_epi16(4);
  __m128i filter1 = _mm_adds_epi16(filter, t4);
  __m128i filter2 = _mm_adds_epi16(filter, t3);
  pixel_clamp(&pmin, &pmax, &filter1);
  pixel_clamp(&pmin, &pmax, &filter2);
  filter1 = _mm_srai_epi16(filter1, 3);
  filter2 = _mm_srai_epi16(filter2, 3);
  qs0 = _mm_subs_epi16(qs0, filter1);
  pixel_clamp(&pmin, &pmax, &qs0);
  ps0 = _mm_adds_epi16(ps0, filter2);
  pixel_clamp(&pmin, &pmax, &ps0);
  qs[0] = _mm_adds_epi16(qs0, *t80);
  ps[0] = _mm_adds_epi16(ps0, *t80);
  filter = _mm_adds_epi16(filter1, one);
  filter = _mm_srai_epi16(filter, 1);
  filter = _mm_andnot_si128(hev, filter);
  qs1 = _mm_subs_epi16(qs1, filter);
  pixel_clamp(&pmin, &pmax, &qs1);
  ps1 = _mm_adds_epi16(ps1, filter);
  pixel_clamp(&pmin, &pmax, &ps1);
  qs[1] = _mm_adds_epi16(qs1, *t80);
  ps[1] = _mm_adds_epi16(ps1, *t80);
}

static AOM_FORCE_INLINE void highbd_lpf_internal_14_sse2(
    __m128i *p, __m128i *q, __m128i *pq, const unsigned char *blt,
    const unsigned char *lt, const unsigned char *thr, int bd) {
  int i;
  __m128i blimit, limit, thresh;
  __m128i t80;
  get_limit(blt, lt, thr, bd, &blimit, &limit, &thresh, &t80);

  __m128i mask;
  highbd_filter_mask(p, q, &limit, &blimit, &mask);

  __m128i flat, flat2;
  const __m128i one = _mm_set1_epi16(1);
  highbd_flat_mask4(&one, p, q, &flat, bd);
  highbd_flat_mask4_13(&one, p, q, &flat2, bd);

  flat = _mm_and_si128(flat, mask);
  flat2 = _mm_and_si128(flat2, flat);

  // replicate for the further "merged variables" usage
  flat = _mm_unpacklo_epi64(flat, flat);
  flat2 = _mm_unpacklo_epi64(flat2, flat2);

  __m128i ps0ps1, qs0qs1, p1p0, q1q0;

  // filters - hev and filter4
  __m128i hevhev;
  __m128i abs_p1p0;
  for (i = 0; i < 6; i++) {
    pq[i] = _mm_unpacklo_epi64(p[i], q[i]);
  }

  highbd_hev_mask(&pq[0], &pq[1], &thresh, &abs_p1p0, &hevhev);

  p1p0 = _mm_unpacklo_epi64(p[0], p[1]);
  q1q0 = _mm_unpacklo_epi64(q[0], q[1]);
  highbd_filter4_sse2(&p1p0, &q1q0, &hevhev, &mask, &qs0qs1, &ps0ps1, &t80, bd);

  // flat and wide flat calculations
  __m128i flat_p[3], flat_q[3], flat_pq[3];
  __m128i flat2_p[6], flat2_q[6];
  __m128i flat2_pq[6];

  {
    const __m128i eight = _mm_set1_epi16(8);
    const __m128i four = _mm_set1_epi16(4);
    __m128i sum_p = _mm_add_epi16(p[5], _mm_add_epi16(p[4], p[3]));
    __m128i sum_q = _mm_add_epi16(q[5], _mm_add_epi16(q[4], q[3]));

    __m128i sum_lp = _mm_add_epi16(p[0], _mm_add_epi16(p[2], p[1]));
    sum_p = _mm_add_epi16(sum_p, sum_lp);

    __m128i sum_lq = _mm_add_epi16(q[0], _mm_add_epi16(q[2], q[1]));
    sum_q = _mm_add_epi16(sum_q, sum_lq);

    sum_p = _mm_add_epi16(eight, _mm_add_epi16(sum_p, sum_q));
    sum_lp = _mm_add_epi16(four, _mm_add_epi16(sum_lp, sum_lq));

    flat2_p[0] = _mm_add_epi16(sum_p, _mm_add_epi16(_mm_add_epi16(p[6], p[0]),
                                                    _mm_add_epi16(p[1], q[0])));
    flat2_q[0] = _mm_add_epi16(sum_p, _mm_add_epi16(_mm_add_epi16(q[6], q[0]),
                                                    _mm_add_epi16(p[0], q[1])));

    flat_p[0] = _mm_add_epi16(sum_lp, _mm_add_epi16(p[3], p[0]));
    flat_q[0] = _mm_add_epi16(sum_lp, _mm_add_epi16(q[3], q[0]));
    __m128i sum_p6 = _mm_add_epi16(p[6], p[6]);
    __m128i sum_q6 = _mm_add_epi16(q[6], q[6]);
    __m128i sum_p3 = _mm_add_epi16(p[3], p[3]);
    __m128i sum_q3 = _mm_add_epi16(q[3], q[3]);

    sum_q = _mm_sub_epi16(sum_p, p[5]);
    sum_p = _mm_sub_epi16(sum_p, q[5]);

    flat2_p[1] = _mm_add_epi16(
        sum_p,
        _mm_add_epi16(sum_p6, _mm_add_epi16(p[1], _mm_add_epi16(p[2], p[0]))));
    flat2_q[1] = _mm_add_epi16(
        sum_q,
        _mm_add_epi16(sum_q6, _mm_add_epi16(q[1], _mm_add_epi16(q[0], q[2]))));

    sum_lq = _mm_sub_epi16(sum_lp, p[2]);
    sum_lp = _mm_sub_epi16(sum_lp, q[2]);

    flat_p[1] = _mm_add_epi16(sum_lp, _mm_add_epi16(sum_p3, p[1]));
    flat_q[1] = _mm_add_epi16(sum_lq, _mm_add_epi16(sum_q3, q[1]));

    flat_pq[0] = _mm_srli_epi16(_mm_unpacklo_epi64(flat_p[0], flat_q[0]), 3);
    flat_pq[1] = _mm_srli_epi16(_mm_unpacklo_epi64(flat_p[1], flat_q[1]), 3);

    flat2_pq[0] = _mm_srli_epi16(_mm_unpacklo_epi64(flat2_p[0], flat2_q[0]), 4);
    flat2_pq[1] = _mm_srli_epi16(_mm_unpacklo_epi64(flat2_p[1], flat2_q[1]), 4);

    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p3 = _mm_add_epi16(sum_p3, p[3]);
    sum_q3 = _mm_add_epi16(sum_q3, q[3]);
    sum_p = _mm_sub_epi16(sum_p, q[4]);
    sum_q = _mm_sub_epi16(sum_q, p[4]);
    flat2_p[2] = _mm_add_epi16(
        sum_p,
        _mm_add_epi16(sum_p6, _mm_add_epi16(p[2], _mm_add_epi16(p[3], p[1]))));
    flat2_q[2] = _mm_add_epi16(
        sum_q,
        _mm_add_epi16(sum_q6, _mm_add_epi16(q[2], _mm_add_epi16(q[1], q[3]))));
    flat2_pq[2] = _mm_srli_epi16(_mm_unpacklo_epi64(flat2_p[2], flat2_q[2]), 4);

    sum_lp = _mm_sub_epi16(sum_lp, q[1]);
    sum_lq = _mm_sub_epi16(sum_lq, p[1]);
    flat_p[2] = _mm_add_epi16(sum_lp, _mm_add_epi16(sum_p3, p[2]));
    flat_q[2] = _mm_add_epi16(sum_lq, _mm_add_epi16(sum_q3, q[2]));
    flat_pq[2] = _mm_srli_epi16(_mm_unpacklo_epi64(flat_p[2], flat_q[2]), 3);

    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p = _mm_sub_epi16(sum_p, q[3]);
    sum_q = _mm_sub_epi16(sum_q, p[3]);
    flat2_p[3] = _mm_add_epi16(
        sum_p,
        _mm_add_epi16(sum_p6, _mm_add_epi16(p[3], _mm_add_epi16(p[4], p[2]))));
    flat2_q[3] = _mm_add_epi16(
        sum_q,
        _mm_add_epi16(sum_q6, _mm_add_epi16(q[3], _mm_add_epi16(q[2], q[4]))));
    flat2_pq[3] = _mm_srli_epi16(_mm_unpacklo_epi64(flat2_p[3], flat2_q[3]), 4);

    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p = _mm_sub_epi16(sum_p, q[2]);
    sum_q = _mm_sub_epi16(sum_q, p[2]);
    flat2_p[4] = _mm_add_epi16(
        sum_p,
        _mm_add_epi16(sum_p6, _mm_add_epi16(p[4], _mm_add_epi16(p[5], p[3]))));
    flat2_q[4] = _mm_add_epi16(
        sum_q,
        _mm_add_epi16(sum_q6, _mm_add_epi16(q[4], _mm_add_epi16(q[3], q[5]))));
    flat2_pq[4] = _mm_srli_epi16(_mm_unpacklo_epi64(flat2_p[4], flat2_q[4]), 4);

    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p = _mm_sub_epi16(sum_p, q[1]);
    sum_q = _mm_sub_epi16(sum_q, p[1]);
    flat2_p[5] = _mm_add_epi16(
        sum_p,
        _mm_add_epi16(sum_p6, _mm_add_epi16(p[5], _mm_add_epi16(p[6], p[4]))));
    flat2_q[5] = _mm_add_epi16(
        sum_q,
        _mm_add_epi16(sum_q6, _mm_add_epi16(q[5], _mm_add_epi16(q[4], q[6]))));

    flat2_pq[5] = _mm_srli_epi16(_mm_unpacklo_epi64(flat2_p[5], flat2_q[5]), 4);
  }

  // highbd_filter8
  pq[0] = _mm_unpacklo_epi64(ps0ps1, qs0qs1);
  pq[1] = _mm_unpackhi_epi64(ps0ps1, qs0qs1);

  for (i = 0; i < 3; i++) {
    pq[i] = _mm_andnot_si128(flat, pq[i]);
    flat_pq[i] = _mm_and_si128(flat, flat_pq[i]);
    pq[i] = _mm_or_si128(pq[i], flat_pq[i]);
  }

  // highbd_filter16
  for (i = 5; i >= 0; i--) {
    //  p[i] remains unchanged if !(flat2 && flat && mask)
    pq[i] = _mm_andnot_si128(flat2, pq[i]);
    flat2_pq[i] = _mm_and_si128(flat2, flat2_pq[i]);
    //  get values for when (flat2 && flat && mask)
    pq[i] = _mm_or_si128(pq[i], flat2_pq[i]);  // full list of pq values
  }
}

void aom_highbd_lpf_horizontal_14_sse2(uint16_t *s, int pitch,
                                       const uint8_t *blt, const uint8_t *lt,
                                       const uint8_t *thr, int bd) {
  __m128i p[7], q[7], pq[7];
  int i;

  for (i = 0; i < 7; i++) {
    p[i] = _mm_loadl_epi64((__m128i *)(s - (i + 1) * pitch));
    q[i] = _mm_loadl_epi64((__m128i *)(s + i * pitch));
  }

  highbd_lpf_internal_14_sse2(p, q, pq, blt, lt, thr, bd);

  for (i = 0; i < 6; i++) {
    _mm_storel_epi64((__m128i *)(s - (i + 1) * pitch), pq[i]);
    _mm_storel_epi64((__m128i *)(s + i * pitch), _mm_srli_si128(pq[i], 8));
  }
}

static AOM_FORCE_INLINE void highbd_lpf_internal_14_dual_sse2(
    __m128i *p, __m128i *q, const uint8_t *blt, const uint8_t *lt,
    const uint8_t *thr, int bd) {
  __m128i blimit, limit, thresh, t80;
  get_limit(blt, lt, thr, bd, &blimit, &limit, &thresh, &t80);
  __m128i mask;
  highbd_filter_mask(p, q, &limit, &blimit, &mask);
  __m128i flat, flat2;
  const __m128i one = _mm_set1_epi16(1);
  highbd_flat_mask4(&one, p, q, &flat, bd);
  highbd_flat_mask4_13(&one, p, q, &flat2, bd);
  flat = _mm_and_si128(flat, mask);
  flat2 = _mm_and_si128(flat2, flat);
  __m128i ps[2], qs[2];
  highbd_filter4_dual_sse2(p, q, ps, qs, &mask, &thresh, bd, &t80);
  // flat and wide flat calculations
  __m128i flat_p[3], flat_q[3];
  __m128i flat2_p[6], flat2_q[6];
  {
    const __m128i eight = _mm_set1_epi16(8);
    const __m128i four = _mm_set1_epi16(4);
    __m128i sum_p = _mm_add_epi16(p[5], _mm_add_epi16(p[4], p[3]));
    __m128i sum_q = _mm_add_epi16(q[5], _mm_add_epi16(q[4], q[3]));
    __m128i sum_lp = _mm_add_epi16(p[0], _mm_add_epi16(p[2], p[1]));
    sum_p = _mm_add_epi16(sum_p, sum_lp);
    __m128i sum_lq = _mm_add_epi16(q[0], _mm_add_epi16(q[2], q[1]));
    sum_q = _mm_add_epi16(sum_q, sum_lq);
    sum_p = _mm_add_epi16(eight, _mm_add_epi16(sum_p, sum_q));
    sum_lp = _mm_add_epi16(four, _mm_add_epi16(sum_lp, sum_lq));
    flat2_p[0] = _mm_srli_epi16(
        _mm_add_epi16(sum_p, _mm_add_epi16(_mm_add_epi16(p[6], p[0]),
                                           _mm_add_epi16(p[1], q[0]))),
        4);
    flat2_q[0] = _mm_srli_epi16(
        _mm_add_epi16(sum_p, _mm_add_epi16(_mm_add_epi16(q[6], q[0]),
                                           _mm_add_epi16(p[0], q[1]))),
        4);
    flat_p[0] =
        _mm_srli_epi16(_mm_add_epi16(sum_lp, _mm_add_epi16(p[3], p[0])), 3);
    flat_q[0] =
        _mm_srli_epi16(_mm_add_epi16(sum_lp, _mm_add_epi16(q[3], q[0])), 3);
    __m128i sum_p6 = _mm_add_epi16(p[6], p[6]);
    __m128i sum_q6 = _mm_add_epi16(q[6], q[6]);
    __m128i sum_p3 = _mm_add_epi16(p[3], p[3]);
    __m128i sum_q3 = _mm_add_epi16(q[3], q[3]);
    sum_q = _mm_sub_epi16(sum_p, p[5]);
    sum_p = _mm_sub_epi16(sum_p, q[5]);
    flat2_p[1] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_p, _mm_add_epi16(
                       sum_p6, _mm_add_epi16(p[1], _mm_add_epi16(p[2], p[0])))),
        4);
    flat2_q[1] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_q, _mm_add_epi16(
                       sum_q6, _mm_add_epi16(q[1], _mm_add_epi16(q[0], q[2])))),
        4);
    sum_lq = _mm_sub_epi16(sum_lp, p[2]);
    sum_lp = _mm_sub_epi16(sum_lp, q[2]);
    flat_p[1] =
        _mm_srli_epi16(_mm_add_epi16(sum_lp, _mm_add_epi16(sum_p3, p[1])), 3);
    flat_q[1] =
        _mm_srli_epi16(_mm_add_epi16(sum_lq, _mm_add_epi16(sum_q3, q[1])), 3);
    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p3 = _mm_add_epi16(sum_p3, p[3]);
    sum_q3 = _mm_add_epi16(sum_q3, q[3]);
    sum_p = _mm_sub_epi16(sum_p, q[4]);
    sum_q = _mm_sub_epi16(sum_q, p[4]);
    flat2_p[2] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_p, _mm_add_epi16(
                       sum_p6, _mm_add_epi16(p[2], _mm_add_epi16(p[3], p[1])))),
        4);
    flat2_q[2] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_q, _mm_add_epi16(
                       sum_q6, _mm_add_epi16(q[2], _mm_add_epi16(q[1], q[3])))),
        4);
    sum_lp = _mm_sub_epi16(sum_lp, q[1]);
    sum_lq = _mm_sub_epi16(sum_lq, p[1]);
    flat_p[2] =
        _mm_srli_epi16(_mm_add_epi16(sum_lp, _mm_add_epi16(sum_p3, p[2])), 3);
    flat_q[2] =
        _mm_srli_epi16(_mm_add_epi16(sum_lq, _mm_add_epi16(sum_q3, q[2])), 3);
    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p = _mm_sub_epi16(sum_p, q[3]);
    sum_q = _mm_sub_epi16(sum_q, p[3]);
    flat2_p[3] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_p, _mm_add_epi16(
                       sum_p6, _mm_add_epi16(p[3], _mm_add_epi16(p[4], p[2])))),
        4);
    flat2_q[3] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_q, _mm_add_epi16(
                       sum_q6, _mm_add_epi16(q[3], _mm_add_epi16(q[2], q[4])))),
        4);
    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p = _mm_sub_epi16(sum_p, q[2]);
    sum_q = _mm_sub_epi16(sum_q, p[2]);
    flat2_p[4] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_p, _mm_add_epi16(
                       sum_p6, _mm_add_epi16(p[4], _mm_add_epi16(p[5], p[3])))),
        4);
    flat2_q[4] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_q, _mm_add_epi16(
                       sum_q6, _mm_add_epi16(q[4], _mm_add_epi16(q[3], q[5])))),
        4);
    sum_p6 = _mm_add_epi16(sum_p6, p[6]);
    sum_q6 = _mm_add_epi16(sum_q6, q[6]);
    sum_p = _mm_sub_epi16(sum_p, q[1]);
    sum_q = _mm_sub_epi16(sum_q, p[1]);
    flat2_p[5] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_p, _mm_add_epi16(
                       sum_p6, _mm_add_epi16(p[5], _mm_add_epi16(p[6], p[4])))),
        4);
    flat2_q[5] = _mm_srli_epi16(
        _mm_add_epi16(
            sum_q, _mm_add_epi16(
                       sum_q6, _mm_add_epi16(q[5], _mm_add_epi16(q[4], q[6])))),
        4);
  }
  // highbd_filter8
  p[2] = _mm_andnot_si128(flat, p[2]);
  //  p2 remains unchanged if !(flat && mask)
  flat_p[2] = _mm_and_si128(flat, flat_p[2]);
  //  when (flat && mask)
  p[2] = _mm_or_si128(p[2], flat_p[2]);  // full list of p2 values
  q[2] = _mm_andnot_si128(flat, q[2]);
  flat_q[2] = _mm_and_si128(flat, flat_q[2]);
  q[2] = _mm_or_si128(q[2], flat_q[2]);  // full list of q2 values
  int i;
  for (i = 1; i >= 0; i--) {
    ps[i] = _mm_andnot_si128(flat, ps[i]);
    flat_p[i] = _mm_and_si128(flat, flat_p[i]);
    p[i] = _mm_or_si128(ps[i], flat_p[i]);
    qs[i] = _mm_andnot_si128(flat, qs[i]);
    flat_q[i] = _mm_and_si128(flat, flat_q[i]);
    q[i] = _mm_or_si128(qs[i], flat_q[i]);
  }
  // highbd_filter16
  for (i = 5; i >= 0; i--) {
    //  p[i] remains unchanged if !(flat2 && flat && mask)
    p[i] = _mm_andnot_si128(flat2, p[i]);
    flat2_p[i] = _mm_and_si128(flat2, flat2_p[i]);
    //  get values for when (flat2 && flat && mask)
    p[i] = _mm_or_si128(p[i], flat2_p[i]);  // full list of p values
    q[i] = _mm_andnot_si128(flat2, q[i]);
    flat2_q[i] = _mm_and_si128(flat2, flat2_q[i]);
    q[i] = _mm_or_si128(q[i], flat2_q[i]);
  }
}

void aom_highbd_lpf_horizontal_14_dual_sse2(uint16_t *s, int pitch,
                                            const uint8_t *_blimit,
                                            const uint8_t *_limit,
                                            const uint8_t *_thresh, int bd) {
  __m128i p[7], q[7];
  int i;
  load_highbd_pixel(s, 7, pitch, p, q);

  highbd_lpf_internal_14_dual_sse2(p, q, _blimit, _limit, _thresh, bd);
  for (i = 0; i < 6; i++) {
    _mm_store_si128((__m128i *)(s - (i + 1) * pitch), p[i]);
    _mm_store_si128((__m128i *)(s + i * pitch), q[i]);
  }
}

static AOM_FORCE_INLINE void highbd_lpf_internal_6_sse2(
    __m128i *p2, __m128i *p1, __m128i *p0, __m128i *q0, __m128i *q1,
    __m128i *q2, __m128i *p1p0_out, __m128i *q1q0_out, const uint8_t *_blimit,
    const uint8_t *_limit, const uint8_t *_thresh, int bd) {
  const __m128i zero = _mm_setzero_si128();
  __m128i blimit, limit, thresh;
  __m128i mask, hev, flat;
  __m128i q2p2, q1p1, q0p0, p1q1, p0q0;
  __m128i p1p0, q1q0, ps1ps0, qs1qs0;
  __m128i flat_p1p0, flat_q0q1;

  q2p2 = _mm_unpacklo_epi64(*p2, *q2);
  q1p1 = _mm_unpacklo_epi64(*p1, *q1);
  q0p0 = _mm_unpacklo_epi64(*p0, *q0);

  p1q1 = _mm_shuffle_epi32(q1p1, _MM_SHUFFLE(1, 0, 3, 2));
  p0q0 = _mm_shuffle_epi32(q0p0, _MM_SHUFFLE(1, 0, 3, 2));

  __m128i abs_p1q1, abs_p0q0, abs_p1p0, work;

  const __m128i four = _mm_set1_epi16(4);
  __m128i t80;
  const __m128i one = _mm_set1_epi16(0x1);
  const __m128i ffff = _mm_cmpeq_epi16(one, one);

  get_limit(_blimit, _limit, _thresh, bd, &blimit, &limit, &thresh, &t80);

  // filter_mask and hev_mask
  highbd_hev_mask(&p0q0, &p1q1, &thresh, &abs_p1p0, &hev);

  abs_p0q0 = abs_diff16(q0p0, p0q0);
  abs_p1q1 = abs_diff16(q1p1, p1q1);

  abs_p0q0 = _mm_adds_epu16(abs_p0q0, abs_p0q0);
  abs_p1q1 = _mm_srli_epi16(abs_p1q1, 1);
  mask = _mm_subs_epu16(_mm_adds_epu16(abs_p0q0, abs_p1q1), blimit);
  mask = _mm_xor_si128(_mm_cmpeq_epi16(mask, zero), ffff);
  // mask |= (abs(*p0 - *q0) * 2 + abs(*p1 - *q1) / 2  > blimit) * -1;
  // So taking maximums continues to work:
  mask = _mm_and_si128(mask, _mm_adds_epu16(limit, one));
  mask = _mm_max_epi16(abs_p1p0, mask);
  // mask |= (abs(*p1 - *p0) > limit) * -1;
  // mask |= (abs(*q1 - *q0) > limit) * -1;

  work = abs_diff16(q2p2, q1p1);

  mask = _mm_max_epi16(work, mask);
  mask = _mm_max_epi16(mask, _mm_srli_si128(mask, 8));
  mask = _mm_subs_epu16(mask, limit);
  mask = _mm_cmpeq_epi16(mask, zero);

  // flat_mask
  flat = _mm_max_epi16(abs_diff16(q2p2, q0p0), abs_p1p0);
  flat = _mm_max_epi16(flat, _mm_srli_si128(flat, 8));

  if (bd == 8)
    flat = _mm_subs_epu16(flat, one);
  else if (bd == 10)
    flat = _mm_subs_epu16(flat, _mm_slli_epi16(one, 2));
  else  // bd == 12
    flat = _mm_subs_epu16(flat, _mm_slli_epi16(one, 4));

  flat = _mm_cmpeq_epi16(flat, zero);
  flat = _mm_and_si128(
      flat, mask);  // flat & mask
                    // replicate for the further "merged variables" usage
  flat = _mm_unpacklo_epi64(flat, flat);

  {
    __m128i workp_a, workp_b, workp_shft0, workp_shft1;

    // op1
    workp_a = _mm_add_epi16(_mm_add_epi16(*p0, *p0),
                            _mm_add_epi16(*p1, *p1));  // *p0 *2 + *p1 * 2
    workp_a = _mm_add_epi16(_mm_add_epi16(workp_a, four),
                            *p2);  // *p2 + *p0 * 2 + *p1 * 2 + 4

    workp_b = _mm_add_epi16(_mm_add_epi16(*p2, *p2), *q0);
    workp_shft0 = _mm_add_epi16(
        workp_a, workp_b);  // *p2 * 3 + *p1 * 2 + *p0 * 2 + *q0 + 4

    // op0
    workp_b = _mm_add_epi16(_mm_add_epi16(*q0, *q0), *q1);  // *q0 * 2 + *q1
    workp_a =
        _mm_add_epi16(workp_a,
                      workp_b);  // *p2 + *p0 * 2 + *p1 * 2 + *q0 * 2 + *q1 + 4

    flat_p1p0 = _mm_srli_epi16(_mm_unpacklo_epi64(workp_a, workp_shft0), 3);

    // oq0
    workp_a = _mm_sub_epi16(_mm_sub_epi16(workp_a, *p2),
                            *p1);  // *p0 * 2 + *p1  + *q0 * 2 + *q1 + 4
    workp_b = _mm_add_epi16(*q1, *q2);
    workp_shft0 = _mm_add_epi16(
        workp_a, workp_b);  // *p0 * 2 + *p1  + *q0 * 2 + *q1 * 2 + *q2 + 4

    // oq1
    workp_a = _mm_sub_epi16(_mm_sub_epi16(workp_shft0, *p1),
                            *p0);  // *p0   + *q0 * 2 + *q1 * 2 + *q2 + 4
    workp_b = _mm_add_epi16(*q2, *q2);
    workp_shft1 = _mm_add_epi16(
        workp_a, workp_b);  // *p0  + *q0 * 2 + *q1 * 2 + *q2 * 3 + 4

    flat_q0q1 = _mm_srli_epi16(_mm_unpacklo_epi64(workp_shft0, workp_shft1), 3);
  }
  // lp filter
  {
    p1p0 = _mm_unpacklo_epi64(q0p0, q1p1);
    q1q0 = _mm_unpackhi_epi64(q0p0, q1p1);

    highbd_filter4_sse2(&p1p0, &q1q0, &hev, &mask, &qs1qs0, &ps1ps0, &t80, bd);
  }

  qs1qs0 = _mm_andnot_si128(flat, qs1qs0);
  q1q0 = _mm_and_si128(flat, flat_q0q1);
  *q1q0_out = _mm_or_si128(qs1qs0, q1q0);

  ps1ps0 = _mm_andnot_si128(flat, ps1ps0);
  p1p0 = _mm_and_si128(flat, flat_p1p0);
  *p1p0_out = _mm_or_si128(ps1ps0, p1p0);
}

void aom_highbd_lpf_horizontal_6_sse2(uint16_t *s, int p,
                                      const uint8_t *_blimit,
                                      const uint8_t *_limit,
                                      const uint8_t *_thresh, int bd) {
  __m128i p2, p1, p0, q0, q1, q2, p1p0_out, q1q0_out;

  p2 = _mm_loadl_epi64((__m128i *)(s - 3 * p));
  p1 = _mm_loadl_epi64((__m128i *)(s - 2 * p));
  p0 = _mm_loadl_epi64((__m128i *)(s - 1 * p));
  q0 = _mm_loadl_epi64((__m128i *)(s + 0 * p));
  q1 = _mm_loadl_epi64((__m128i *)(s + 1 * p));
  q2 = _mm_loadl_epi64((__m128i *)(s + 2 * p));

  highbd_lpf_internal_6_sse2(&p2, &p1, &p0, &q0, &q1, &q2, &p1p0_out, &q1q0_out,
                             _blimit, _limit, _thresh, bd);

  _mm_storel_epi64((__m128i *)(s - 2 * p), _mm_srli_si128(p1p0_out, 8));
  _mm_storel_epi64((__m128i *)(s - 1 * p), p1p0_out);
  _mm_storel_epi64((__m128i *)(s + 0 * p), q1q0_out);
  _mm_storel_epi64((__m128i *)(s + 1 * p), _mm_srli_si128(q1q0_out, 8));
}

static AOM_FORCE_INLINE void highbd_lpf_internal_8_sse2(
    __m128i *p3, __m128i *q3, __m128i *p2, __m128i *q2, __m128i *p1,
    __m128i *q1, __m128i *p0, __m128i *q0, __m128i *q1q0_out, __m128i *p1p0_out,
    const unsigned char *_blimit, const unsigned char *_limit,
    const unsigned char *_thresh, int bd) {
  const __m128i zero = _mm_setzero_si128();
  __m128i blimit, limit, thresh;
  __m128i mask, hev, flat;
  __m128i q2p2, q1p1, q0p0, p1q1, p0q0, q3p3;
  __m128i p1p0, q1q0, ps1ps0, qs1qs0;
  __m128i work_a, op2, oq2, flat_p1p0, flat_q0q1;

  q3p3 = _mm_unpacklo_epi64(*p3, *q3);
  q2p2 = _mm_unpacklo_epi64(*p2, *q2);
  q1p1 = _mm_unpacklo_epi64(*p1, *q1);
  q0p0 = _mm_unpacklo_epi64(*p0, *q0);

  p1q1 = _mm_shuffle_epi32(q1p1, _MM_SHUFFLE(1, 0, 3, 2));
  p0q0 = _mm_shuffle_epi32(q0p0, _MM_SHUFFLE(1, 0, 3, 2));

  __m128i abs_p1q1, abs_p0q0, abs_p1p0, work;

  const __m128i four = _mm_set1_epi16(4);
  __m128i t80;
  const __m128i one = _mm_set1_epi16(0x1);
  const __m128i ffff = _mm_cmpeq_epi16(one, one);

  get_limit(_blimit, _limit, _thresh, bd, &blimit, &limit, &thresh, &t80);

  // filter_mask and hev_mask
  highbd_hev_mask(&p0q0, &p1q1, &thresh, &abs_p1p0, &hev);

  abs_p0q0 = abs_diff16(q0p0, p0q0);
  abs_p1q1 = abs_diff16(q1p1, p1q1);

  abs_p0q0 = _mm_adds_epu16(abs_p0q0, abs_p0q0);
  abs_p1q1 = _mm_srli_epi16(abs_p1q1, 1);
  mask = _mm_subs_epu16(_mm_adds_epu16(abs_p0q0, abs_p1q1), blimit);
  mask = _mm_xor_si128(_mm_cmpeq_epi16(mask, zero), ffff);
  // mask |= (abs(*p0 - q0) * 2 + abs(*p1 - q1) / 2  > blimit) * -1;
  // So taking maximums continues to work:
  mask = _mm_and_si128(mask, _mm_adds_epu16(limit, one));
  mask = _mm_max_epi16(abs_p1p0, mask);
  // mask |= (abs(*p1 - *p0) > limit) * -1;
  // mask |= (abs(q1 - q0) > limit) * -1;

  work = _mm_max_epi16(abs_diff16(q2p2, q1p1), abs_diff16(q3p3, q2p2));
  mask = _mm_max_epi16(work, mask);
  mask = _mm_max_epi16(mask, _mm_srli_si128(mask, 8));
  mask = _mm_subs_epu16(mask, limit);
  mask = _mm_cmpeq_epi16(mask, zero);

  // flat_mask4
  flat = _mm_max_epi16(abs_diff16(q2p2, q0p0), abs_diff16(q3p3, q0p0));
  flat = _mm_max_epi16(abs_p1p0, flat);
  flat = _mm_max_epi16(flat, _mm_srli_si128(flat, 8));

  if (bd == 8)
    flat = _mm_subs_epu16(flat, one);
  else if (bd == 10)
    flat = _mm_subs_epu16(flat, _mm_slli_epi16(one, 2));
  else  // bd == 12
    flat = _mm_subs_epu16(flat, _mm_slli_epi16(one, 4));

  flat = _mm_cmpeq_epi16(flat, zero);
  flat = _mm_and_si128(
      flat, mask);  // flat & mask
                    // replicate for the further "merged variables" usage
  flat = _mm_unpacklo_epi64(flat, flat);

  {
    __m128i workp_a, workp_b, workp_shft0, workp_shft1;
    // Added before shift for rounding part of ROUND_POWER_OF_TWO

    // o*p2
    workp_a = _mm_add_epi16(_mm_add_epi16(*p3, *p3), _mm_add_epi16(*p2, *p1));
    workp_a = _mm_add_epi16(_mm_add_epi16(workp_a, four), *p0);
    workp_b = _mm_add_epi16(_mm_add_epi16(*q0, *p2), *p3);
    op2 = _mm_srli_epi16(_mm_add_epi16(workp_a, workp_b), 3);

    // o*p1
    workp_b = _mm_add_epi16(_mm_add_epi16(*q0, *q1), *p1);
    workp_shft0 = _mm_add_epi16(workp_a, workp_b);

    // o*p0
    workp_a = _mm_add_epi16(_mm_sub_epi16(workp_a, *p3), *q2);
    workp_b = _mm_add_epi16(_mm_sub_epi16(workp_b, *p1), *p0);
    workp_shft1 = _mm_add_epi16(workp_a, workp_b);

    flat_p1p0 = _mm_srli_epi16(_mm_unpacklo_epi64(workp_shft1, workp_shft0), 3);

    // oq0
    workp_a = _mm_add_epi16(_mm_sub_epi16(workp_a, *p3), *q3);
    workp_b = _mm_add_epi16(_mm_sub_epi16(workp_b, *p0), *q0);
    workp_shft0 = _mm_add_epi16(workp_a, workp_b);

    // oq1
    workp_a = _mm_add_epi16(_mm_sub_epi16(workp_a, *p2), *q3);
    workp_b = _mm_add_epi16(_mm_sub_epi16(workp_b, *q0), *q1);
    workp_shft1 = _mm_add_epi16(workp_a, workp_b);

    flat_q0q1 = _mm_srli_epi16(_mm_unpacklo_epi64(workp_shft0, workp_shft1), 3);

    // oq2
    workp_a = _mm_add_epi16(_mm_sub_epi16(workp_a, *p1), *q3);
    workp_b = _mm_add_epi16(_mm_sub_epi16(workp_b, *q1), *q2);
    oq2 = _mm_srli_epi16(_mm_add_epi16(workp_a, workp_b), 3);
  }

  // lp filter
  {
    p1p0 = _mm_unpacklo_epi64(q0p0, q1p1);
    q1q0 = _mm_unpackhi_epi64(q0p0, q1p1);

    highbd_filter4_sse2(&p1p0, &q1q0, &hev, &mask, &qs1qs0, &ps1ps0, &t80, bd);
  }

  qs1qs0 = _mm_andnot_si128(flat, qs1qs0);
  q1q0 = _mm_and_si128(flat, flat_q0q1);
  *q1q0_out = _mm_or_si128(qs1qs0, q1q0);

  ps1ps0 = _mm_andnot_si128(flat, ps1ps0);
  p1p0 = _mm_and_si128(flat, flat_p1p0);
  *p1p0_out = _mm_or_si128(ps1ps0, p1p0);

  work_a = _mm_andnot_si128(flat, *q2);
  *q2 = _mm_and_si128(flat, oq2);
  *q2 = _mm_or_si128(work_a, *q2);

  work_a = _mm_andnot_si128(flat, *p2);
  *p2 = _mm_and_si128(flat, op2);
  *p2 = _mm_or_si128(work_a, *p2);
}

static AOM_FORCE_INLINE void highbd_lpf_internal_8_dual_sse2(
    __m128i *p3, __m128i *q3, __m128i *p2, __m128i *q2, __m128i *p1,
    __m128i *q1, __m128i *p0, __m128i *q0, const unsigned char *_blimit0,
    const unsigned char *_limit0, const unsigned char *_thresh0,
    const unsigned char *_blimit1, const unsigned char *_limit1,
    const unsigned char *_thresh1, int bd) {
  __m128i blimit0, limit0, thresh0;
  __m128i t80;
  __m128i mask, flat;
  __m128i work_a, op2, oq2, op1, op0, oq0, oq1;
  __m128i abs_p1q1, abs_p0q0, work0, work1, work2;

  const __m128i zero = _mm_setzero_si128();
  const __m128i four = _mm_set1_epi16(4);
  const __m128i one = _mm_set1_epi16(0x1);
  const __m128i ffff = _mm_cmpeq_epi16(one, one);

  get_limit_dual(_blimit0, _limit0, _thresh0, _blimit1, _limit1, _thresh1, bd,
                 &blimit0, &limit0, &thresh0, &t80);

  abs_p0q0 = abs_diff16(*p0, *q0);
  abs_p1q1 = abs_diff16(*p1, *q1);

  abs_p0q0 = _mm_adds_epu16(abs_p0q0, abs_p0q0);
  abs_p1q1 = _mm_srli_epi16(abs_p1q1, 1);
  mask = _mm_subs_epu16(_mm_adds_epu16(abs_p0q0, abs_p1q1), blimit0);
  mask = _mm_xor_si128(_mm_cmpeq_epi16(mask, zero), ffff);
  // mask |= (abs(*p0 - q0) * 2 + abs(*p1 - q1) / 2  > blimit) * -1;

  // So taking maximums continues to work:
  mask = _mm_and_si128(mask, _mm_adds_epu16(limit0, one));

  work0 = _mm_max_epi16(abs_diff16(*p3, *p2), abs_diff16(*p2, *p1));
  work1 =
      _mm_max_epi16(abs_diff16(*p1, *p0), abs_diff16(*q1, *q0));  // tbu 4 flat
  work0 = _mm_max_epi16(work0, work1);
  work2 = _mm_max_epi16(abs_diff16(*q2, *q1), abs_diff16(*q2, *q3));
  work2 = _mm_max_epi16(work2, work0);
  mask = _mm_max_epi16(work2, mask);

  mask = _mm_subs_epu16(mask, limit0);
  mask = _mm_cmpeq_epi16(mask, zero);

  flat = _mm_max_epi16(abs_diff16(*p2, *p0), abs_diff16(*q2, *q0));
  flat = _mm_max_epi16(work1, flat);
  work0 = _mm_max_epi16(abs_diff16(*p3, *p0), abs_diff16(*q3, *q0));
  flat = _mm_max_epi16(work0, flat);

  if (bd == 8)
    flat = _mm_subs_epu16(flat, one);
  else if (bd == 10)
    flat = _mm_subs_epu16(flat, _mm_slli_epi16(one, 2));
  else  // bd == 12
    flat = _mm_subs_epu16(flat, _mm_slli_epi16(one, 4));

  flat = _mm_cmpeq_epi16(flat, zero);
  flat = _mm_and_si128(flat, mask);  // flat & mask

  {
    __m128i workp_a, workp_b;
    // Added before shift for rounding part of ROUND_POWER_OF_TWO

    // o*p2
    workp_a = _mm_add_epi16(_mm_add_epi16(*p3, *p3), _mm_add_epi16(*p2, *p1));
    workp_a = _mm_add_epi16(_mm_add_epi16(workp_a, four), *p0);
    workp_b = _mm_add_epi16(_mm_add_epi16(*q0, *p2), *p3);
    op2 = _mm_srli_epi16(_mm_add_epi16(workp_a, workp_b), 3);

    // o*p1
    workp_b = _mm_add_epi16(_mm_add_epi16(*q0, *q1), *p1);
    op1 = _mm_srli_epi16(_mm_add_epi16(workp_a, workp_b), 3);

    // o*p0
    workp_a = _mm_add_epi16(_mm_sub_epi16(workp_a, *p3), *q2);
    workp_b = _mm_add_epi16(_mm_sub_epi16(workp_b, *p1), *p0);
    op0 = _mm_srli_epi16(_mm_add_epi16(workp_a, workp_b), 3);

    // oq0
    workp_a = _mm_add_epi16(_mm_sub_epi16(workp_a, *p3), *q3);
    workp_b = _mm_add_epi16(_mm_sub_epi16(workp_b, *p0), *q0);
    oq0 = _mm_srli_epi16(_mm_add_epi16(workp_a, workp_b), 3);

    // oq1
    workp_a = _mm_add_epi16(_mm_sub_epi16(workp_a, *p2), *q3);
    workp_b = _mm_add_epi16(_mm_sub_epi16(workp_b, *q0), *q1);
    oq1 = _mm_srli_epi16(_mm_add_epi16(workp_a, workp_b), 3);

    // oq2
    workp_a = _mm_add_epi16(_mm_sub_epi16(workp_a, *p1), *q3);
    workp_b = _mm_add_epi16(_mm_sub_epi16(workp_b, *q1), *q2);
    oq2 = _mm_srli_epi16(_mm_add_epi16(workp_a, workp_b), 3);
  }

  // lp filter
  __m128i ps[2], qs[2], p[2], q[2];
  {
    p[0] = *p0;
    p[1] = *p1;
    q[0] = *q0;
    q[1] = *q1;
    // filter_mask and hev_mask
    highbd_filter4_dual_sse2(p, q, ps, qs, &mask, &thresh0, bd, &t80);
  }

  qs[0] = _mm_andnot_si128(flat, qs[0]);
  oq0 = _mm_and_si128(flat, oq0);
  *q0 = _mm_or_si128(qs[0], oq0);

  qs[1] = _mm_andnot_si128(flat, qs[1]);
  oq1 = _mm_and_si128(flat, oq1);
  *q1 = _mm_or_si128(qs[1], oq1);

  ps[0] = _mm_andnot_si128(flat, ps[0]);
  op0 = _mm_and_si128(flat, op0);
  *p0 = _mm_or_si128(ps[0], op0);

  ps[1] = _mm_andnot_si128(flat, ps[1]);
  op1 = _mm_and_si128(flat, op1);
  *p1 = _mm_or_si128(ps[1], op1);

  work_a = _mm_andnot_si128(flat, *q2);
  *q2 = _mm_and_si128(flat, oq2);
  *q2 = _mm_or_si128(work_a, *q2);

  work_a = _mm_andnot_si128(flat, *p2);
  *p2 = _mm_and_si128(flat, op2);
  *p2 = _mm_or_si128(work_a, *p2);
}

void aom_highbd_lpf_horizontal_8_sse2(uint16_t *s, int p,
                                      const uint8_t *_blimit,
                                      const uint8_t *_limit,
                                      const uint8_t *_thresh, int bd) {
  __m128i p2, p1, p0, q0, q1, q2, p3, q3;
  __m128i q1q0, p1p0;

  p3 = _mm_loadl_epi64((__m128i *)(s - 4 * p));
  q3 = _mm_loadl_epi64((__m128i *)(s + 3 * p));
  p2 = _mm_loadl_epi64((__m128i *)(s - 3 * p));
  q2 = _mm_loadl_epi64((__m128i *)(s + 2 * p));
  p1 = _mm_loadl_epi64((__m128i *)(s - 2 * p));
  q1 = _mm_loadl_epi64((__m128i *)(s + 1 * p));
  p0 = _mm_loadl_epi64((__m128i *)(s - 1 * p));
  q0 = _mm_loadl_epi64((__m128i *)(s + 0 * p));

  highbd_lpf_internal_8_sse2(&p3, &q3, &p2, &q2, &p1, &q1, &p0, &q0, &q1q0,
                             &p1p0, _blimit, _limit, _thresh, bd);

  _mm_storel_epi64((__m128i *)(s - 3 * p), p2);
  _mm_storel_epi64((__m128i *)(s - 2 * p), _mm_srli_si128(p1p0, 8));
  _mm_storel_epi64((__m128i *)(s - 1 * p), p1p0);
  _mm_storel_epi64((__m128i *)(s + 0 * p), q1q0);
  _mm_storel_epi64((__m128i *)(s + 1 * p), _mm_srli_si128(q1q0, 8));
  _mm_storel_epi64((__m128i *)(s + 2 * p), q2);
}

void aom_highbd_lpf_horizontal_8_dual_sse2(
    uint16_t *s, int p, const uint8_t *_blimit0, const uint8_t *_limit0,
    const uint8_t *_thresh0, const uint8_t *_blimit1, const uint8_t *_limit1,
    const uint8_t *_thresh1, int bd) {
  __m128i p2, p1, p0, q0, q1, q2, p3, q3;

  p3 = _mm_loadu_si128((__m128i *)(s - 4 * p));
  q3 = _mm_loadu_si128((__m128i *)(s + 3 * p));
  p2 = _mm_loadu_si128((__m128i *)(s - 3 * p));
  q2 = _mm_loadu_si128((__m128i *)(s + 2 * p));
  p1 = _mm_loadu_si128((__m128i *)(s - 2 * p));
  q1 = _mm_loadu_si128((__m128i *)(s + 1 * p));
  p0 = _mm_loadu_si128((__m128i *)(s - 1 * p));
  q0 = _mm_loadu_si128((__m128i *)(s + 0 * p));

  highbd_lpf_internal_8_dual_sse2(&p3, &q3, &p2, &q2, &p1, &q1, &p0, &q0,
                                  _blimit0, _limit0, _thresh0, _blimit1,
                                  _limit1, _thresh1, bd);

  _mm_storeu_si128((__m128i *)(s - 3 * p), p2);
  _mm_storeu_si128((__m128i *)(s - 2 * p), p1);
  _mm_storeu_si128((__m128i *)(s - 1 * p), p0);
  _mm_storeu_si128((__m128i *)(s + 0 * p), q0);
  _mm_storeu_si128((__m128i *)(s + 1 * p), q1);
  _mm_storeu_si128((__m128i *)(s + 2 * p), q2);
}

static AOM_FORCE_INLINE void highbd_lpf_internal_4_sse2(
    __m128i *p1, __m128i *p0, __m128i *q0, __m128i *q1, __m128i *q1q0_out,
    __m128i *p1p0_out, const uint8_t *_blimit, const uint8_t *_limit,
    const uint8_t *_thresh, int bd) {
  __m128i blimit, limit, thresh;
  __m128i mask, hev, flat;
  __m128i p1p0, q1q0;

  const __m128i zero = _mm_setzero_si128();

  __m128i abs_p0q0, abs_p1q1, abs_p1p0, abs_q1q0;

  const __m128i ffff = _mm_cmpeq_epi16(zero, zero);
  const __m128i one = _mm_set1_epi16(1);

  __m128i t80;
  get_limit(_blimit, _limit, _thresh, bd, &blimit, &limit, &thresh, &t80);

  p1p0 = _mm_unpacklo_epi64(*p0, *p1);
  q1q0 = _mm_unpacklo_epi64(*q0, *q1);

  abs_p1p0 = abs_diff16(*p1, *p0);
  abs_q1q0 = abs_diff16(*q1, *q0);

  abs_p0q0 = abs_diff16(p1p0, q1q0);
  abs_p1q1 = _mm_srli_si128(abs_p0q0, 8);

  // filter_mask and hev_mask
  flat = _mm_max_epi16(abs_p1p0, abs_q1q0);
  hev = _mm_subs_epu16(flat, thresh);
  hev = _mm_xor_si128(_mm_cmpeq_epi16(hev, zero), ffff);

  abs_p0q0 = _mm_adds_epu16(abs_p0q0, abs_p0q0);
  abs_p1q1 = _mm_srli_epi16(abs_p1q1, 1);
  mask = _mm_subs_epu16(_mm_adds_epu16(abs_p0q0, abs_p1q1), blimit);
  mask = _mm_xor_si128(_mm_cmpeq_epi16(mask, zero), ffff);
  // mask |= (abs(*p0 - *q0) * 2 + abs(*p1 - *q1) / 2  > blimit) * -1;
  // So taking maximums continues to work:
  mask = _mm_and_si128(mask, _mm_adds_epu16(limit, one));
  mask = _mm_max_epi16(flat, mask);

  mask = _mm_subs_epu16(mask, limit);
  mask = _mm_cmpeq_epi16(mask, zero);

  mask = _mm_unpacklo_epi64(mask, mask);
  hev = _mm_unpacklo_epi64(hev, hev);

  highbd_filter4_sse2(&p1p0, &q1q0, &hev, &mask, q1q0_out, p1p0_out, &t80, bd);
}

static AOM_FORCE_INLINE void highbd_lpf_internal_4_dual_sse2(
    __m128i *p1, __m128i *p0, __m128i *q0, __m128i *q1, __m128i *ps,
    __m128i *qs, const uint8_t *_blimit0, const uint8_t *_limit0,
    const uint8_t *_thresh0, const uint8_t *_blimit1, const uint8_t *_limit1,
    const uint8_t *_thresh1, int bd) {
  __m128i blimit0, limit0, thresh0;
  __m128i mask, flat;
  __m128i p[2], q[2];

  const __m128i zero = _mm_setzero_si128();
  __m128i abs_p0q0 = abs_diff16(*q0, *p0);
  __m128i abs_p1q1 = abs_diff16(*q1, *p1);

  __m128i abs_p1p0 = abs_diff16(*p1, *p0);
  __m128i abs_q1q0 = abs_diff16(*q1, *q0);

  const __m128i ffff = _mm_cmpeq_epi16(abs_p1p0, abs_p1p0);
  const __m128i one = _mm_set1_epi16(1);

  __m128i t80;

  get_limit_dual(_blimit0, _limit0, _thresh0, _blimit1, _limit1, _thresh1, bd,
                 &blimit0, &limit0, &thresh0, &t80);

  // filter_mask and hev_mask
  flat = _mm_max_epi16(abs_p1p0, abs_q1q0);

  abs_p0q0 = _mm_adds_epu16(abs_p0q0, abs_p0q0);
  abs_p1q1 = _mm_srli_epi16(abs_p1q1, 1);

  mask = _mm_subs_epu16(_mm_adds_epu16(abs_p0q0, abs_p1q1), blimit0);
  mask = _mm_xor_si128(_mm_cmpeq_epi16(mask, zero), ffff);
  // mask |= (abs(*p0 - *q0) * 2 + abs(*p1 - *q1) / 2  > blimit) * -1;
  // So taking maximums continues to work:
  mask = _mm_and_si128(mask, _mm_adds_epu16(limit0, one));
  mask = _mm_max_epi16(flat, mask);

  mask = _mm_subs_epu16(mask, limit0);
  mask = _mm_cmpeq_epi16(mask, zero);

  p[0] = *p0;
  p[1] = *p1;
  q[0] = *q0;
  q[1] = *q1;

  highbd_filter4_dual_sse2(p, q, ps, qs, &mask, &thresh0, bd, &t80);
}

void aom_highbd_lpf_horizontal_4_sse2(uint16_t *s, int p,
                                      const uint8_t *_blimit,
                                      const uint8_t *_limit,
                                      const uint8_t *_thresh, int bd) {
  __m128i p1p0, q1q0;
  __m128i p1 = _mm_loadl_epi64((__m128i *)(s - 2 * p));
  __m128i p0 = _mm_loadl_epi64((__m128i *)(s - 1 * p));
  __m128i q0 = _mm_loadl_epi64((__m128i *)(s - 0 * p));
  __m128i q1 = _mm_loadl_epi64((__m128i *)(s + 1 * p));

  highbd_lpf_internal_4_sse2(&p1, &p0, &q0, &q1, &q1q0, &p1p0, _blimit, _limit,
                             _thresh, bd);

  _mm_storel_epi64((__m128i *)(s - 2 * p), _mm_srli_si128(p1p0, 8));
  _mm_storel_epi64((__m128i *)(s - 1 * p), p1p0);
  _mm_storel_epi64((__m128i *)(s + 0 * p), q1q0);
  _mm_storel_epi64((__m128i *)(s + 1 * p), _mm_srli_si128(q1q0, 8));
}

void aom_highbd_lpf_horizontal_4_dual_sse2(
    uint16_t *s, int p, const uint8_t *_blimit0, const uint8_t *_limit0,
    const uint8_t *_thresh0, const uint8_t *_blimit1, const uint8_t *_limit1,
    const uint8_t *_thresh1, int bd) {
  __m128i p1 = _mm_loadu_si128((__m128i *)(s - 2 * p));
  __m128i p0 = _mm_loadu_si128((__m128i *)(s - 1 * p));
  __m128i q0 = _mm_loadu_si128((__m128i *)(s - 0 * p));
  __m128i q1 = _mm_loadu_si128((__m128i *)(s + 1 * p));
  __m128i ps[2], qs[2];
  highbd_lpf_internal_4_dual_sse2(&p1, &p0, &q0, &q1, ps, qs, _blimit0, _limit0,
                                  _thresh0, _blimit1, _limit1, _thresh1, bd);

  _mm_storeu_si128((__m128i *)(s - 2 * p), ps[1]);
  _mm_storeu_si128((__m128i *)(s - 1 * p), ps[0]);
  _mm_storeu_si128((__m128i *)(s + 0 * p), qs[0]);
  _mm_storeu_si128((__m128i *)(s + 1 * p), qs[1]);
}

void aom_highbd_lpf_vertical_4_sse2(uint16_t *s, int p, const uint8_t *blimit,
                                    const uint8_t *limit, const uint8_t *thresh,
                                    int bd) {
  __m128i x0, x1, x2, x3, d0, d1, d2, d3, d4, d5, d6, d7;
  __m128i p1p0, q1q0;
  __m128i p1, q1;

  x0 = _mm_loadu_si128((__m128i *)(s - 4 + 0 * p));
  x1 = _mm_loadu_si128((__m128i *)(s - 4 + 1 * p));
  x2 = _mm_loadu_si128((__m128i *)(s - 4 + 2 * p));
  x3 = _mm_loadu_si128((__m128i *)(s - 4 + 3 * p));

  highbd_transpose4x8_8x4_sse2(&x0, &x1, &x2, &x3, &d0, &d1, &d2, &d3, &d4, &d5,
                               &d6, &d7);

  highbd_lpf_internal_4_sse2(&d2, &d3, &d4, &d5, &q1q0, &p1p0, blimit, limit,
                             thresh, bd);

  // transpose from 8x4 to 4x8
  p1 = _mm_srli_si128(p1p0, 8);
  q1 = _mm_srli_si128(q1q0, 8);
  highbd_transpose8x8_low_sse2(&d0, &d1, &p1, &p1p0, &q1q0, &q1, &d6, &d7, &d0,
                               &d1, &d2, &d3);

  _mm_storeu_si128((__m128i *)(s - 4 + 0 * p), d0);
  _mm_storeu_si128((__m128i *)(s - 4 + 1 * p), d1);
  _mm_storeu_si128((__m128i *)(s - 4 + 2 * p), d2);
  _mm_storeu_si128((__m128i *)(s - 4 + 3 * p), d3);
}

void aom_highbd_lpf_vertical_4_dual_sse2(
    uint16_t *s, int p, const uint8_t *blimit0, const uint8_t *limit0,
    const uint8_t *thresh0, const uint8_t *blimit1, const uint8_t *limit1,
    const uint8_t *thresh1, int bd) {
  __m128i x0, x1, x2, x3, x4, x5, x6, x7;
  __m128i d0, d1, d2, d3, d4, d5, d6, d7;
  __m128i ps[2], qs[2];

  x0 = _mm_loadu_si128((__m128i *)(s - 4 + 0 * p));
  x1 = _mm_loadu_si128((__m128i *)(s - 4 + 1 * p));
  x2 = _mm_loadu_si128((__m128i *)(s - 4 + 2 * p));
  x3 = _mm_loadu_si128((__m128i *)(s - 4 + 3 * p));
  x4 = _mm_loadu_si128((__m128i *)(s - 4 + 4 * p));
  x5 = _mm_loadu_si128((__m128i *)(s - 4 + 5 * p));
  x6 = _mm_loadu_si128((__m128i *)(s - 4 + 6 * p));
  x7 = _mm_loadu_si128((__m128i *)(s - 4 + 7 * p));

  highbd_transpose8x8_sse2(&x0, &x1, &x2, &x3, &x4, &x5, &x6, &x7, &d0, &d1,
                           &d2, &d3, &d4, &d5, &d6, &d7);

  highbd_lpf_internal_4_dual_sse2(&d2, &d3, &d4, &d5, ps, qs, blimit0, limit0,
                                  thresh0, blimit1, limit1, thresh1, bd);

  highbd_transpose8x8_sse2(&d0, &d1, &ps[1], &ps[0], &qs[0], &qs[1], &d6, &d7,
                           &x0, &x1, &x2, &x3, &x4, &x5, &x6, &x7);

  _mm_storeu_si128((__m128i *)(s - 4 + 0 * p), x0);
  _mm_storeu_si128((__m128i *)(s - 4 + 1 * p), x1);
  _mm_storeu_si128((__m128i *)(s - 4 + 2 * p), x2);
  _mm_storeu_si128((__m128i *)(s - 4 + 3 * p), x3);
  _mm_storeu_si128((__m128i *)(s - 4 + 4 * p), x4);
  _mm_storeu_si128((__m128i *)(s - 4 + 5 * p), x5);
  _mm_storeu_si128((__m128i *)(s - 4 + 6 * p), x6);
  _mm_storeu_si128((__m128i *)(s - 4 + 7 * p), x7);
}

void aom_highbd_lpf_vertical_6_sse2(uint16_t *s, int p, const uint8_t *blimit,
                                    const uint8_t *limit, const uint8_t *thresh,
                                    int bd) {
  __m128i d0, d1, d2, d3, d4, d5;
  __m128i p2, p1, p0, q0, q1, q2;
  __m128i p1p0, q1q0;

  p2 = _mm_loadu_si128((__m128i *)((s - 3) + 0 * p));
  p1 = _mm_loadu_si128((__m128i *)((s - 3) + 1 * p));
  p0 = _mm_loadu_si128((__m128i *)((s - 3) + 2 * p));
  q0 = _mm_loadu_si128((__m128i *)((s - 3) + 3 * p));
  q1 = _mm_setzero_si128();
  q2 = _mm_setzero_si128();

  highbd_transpose6x6_sse2(&p2, &p1, &p0, &q0, &q1, &q2, &d0, &d1, &d2, &d3,
                           &d4, &d5);

  highbd_lpf_internal_6_sse2(&d0, &d1, &d2, &d3, &d4, &d5, &p1p0, &q1q0, blimit,
                             limit, thresh, bd);

  p0 = _mm_srli_si128(p1p0, 8);
  q0 = _mm_srli_si128(q1q0, 8);

  highbd_transpose6x6_sse2(&d0, &p0, &p1p0, &q1q0, &q0, &d5, &d0, &d1, &d2, &d3,
                           &d4, &d5);

  _mm_storel_epi64((__m128i *)((s - 3) + 0 * p), d0);
  *(int *)((s - 3) + 0 * p + 4) = _mm_cvtsi128_si32(_mm_srli_si128(d0, 8));
  _mm_storel_epi64((__m128i *)((s - 3) + 1 * p), d1);
  *(int *)((s - 3) + 1 * p + 4) = _mm_cvtsi128_si32(_mm_srli_si128(d0, 12));
  _mm_storel_epi64((__m128i *)((s - 3) + 2 * p), d2);
  *(int *)((s - 3) + 2 * p + 4) = _mm_cvtsi128_si32(_mm_srli_si128(d2, 8));
  _mm_storel_epi64((__m128i *)((s - 3) + 3 * p), d3);
  *(int *)((s - 3) + 3 * p + 4) = _mm_cvtsi128_si32(_mm_srli_si128(d3, 8));
}

void aom_highbd_lpf_vertical_8_sse2(uint16_t *s, int p, const uint8_t *blimit,
                                    const uint8_t *limit, const uint8_t *thresh,
                                    int bd) {
  __m128i d0, d1, d2, d3, d4, d5, d6, d7;
  __m128i p2, p1, p0, p3, q0;
  __m128i q1q0, p1p0;

  p3 = _mm_loadu_si128((__m128i *)((s - 4) + 0 * p));
  p2 = _mm_loadu_si128((__m128i *)((s - 4) + 1 * p));
  p1 = _mm_loadu_si128((__m128i *)((s - 4) + 2 * p));
  p0 = _mm_loadu_si128((__m128i *)((s - 4) + 3 * p));

  highbd_transpose4x8_8x4_sse2(&p3, &p2, &p1, &p0, &d0, &d1, &d2, &d3, &d4, &d5,
                               &d6, &d7);

  // Loop filtering
  highbd_lpf_internal_8_sse2(&d0, &d7, &d1, &d6, &d2, &d5, &d3, &d4, &q1q0,
                             &p1p0, blimit, limit, thresh, bd);

  p0 = _mm_srli_si128(p1p0, 8);
  q0 = _mm_srli_si128(q1q0, 8);

  highbd_transpose8x8_low_sse2(&d0, &d1, &p0, &p1p0, &q1q0, &q0, &d6, &d7, &d0,
                               &d1, &d2, &d3);

  _mm_storeu_si128((__m128i *)(s - 4 + 0 * p), d0);
  _mm_storeu_si128((__m128i *)(s - 4 + 1 * p), d1);
  _mm_storeu_si128((__m128i *)(s - 4 + 2 * p), d2);
  _mm_storeu_si128((__m128i *)(s - 4 + 3 * p), d3);
}

void aom_highbd_lpf_vertical_8_dual_sse2(
    uint16_t *s, int p, const uint8_t *blimit0, const uint8_t *limit0,
    const uint8_t *thresh0, const uint8_t *blimit1, const uint8_t *limit1,
    const uint8_t *thresh1, int bd) {
  __m128i x0, x1, x2, x3, x4, x5, x6, x7;
  __m128i d0, d1, d2, d3, d4, d5, d6, d7;

  x0 = _mm_loadu_si128((__m128i *)(s - 4 + 0 * p));
  x1 = _mm_loadu_si128((__m128i *)(s - 4 + 1 * p));
  x2 = _mm_loadu_si128((__m128i *)(s - 4 + 2 * p));
  x3 = _mm_loadu_si128((__m128i *)(s - 4 + 3 * p));
  x4 = _mm_loadu_si128((__m128i *)(s - 4 + 4 * p));
  x5 = _mm_loadu_si128((__m128i *)(s - 4 + 5 * p));
  x6 = _mm_loadu_si128((__m128i *)(s - 4 + 6 * p));
  x7 = _mm_loadu_si128((__m128i *)(s - 4 + 7 * p));

  highbd_transpose8x8_sse2(&x0, &x1, &x2, &x3, &x4, &x5, &x6, &x7, &d0, &d1,
                           &d2, &d3, &d4, &d5, &d6, &d7);

  highbd_lpf_internal_8_dual_sse2(&d0, &d7, &d1, &d6, &d2, &d5, &d3, &d4,
                                  blimit0, limit0, thresh0, blimit1, limit1,
                                  thresh1, bd);

  highbd_transpose8x8_sse2(&d0, &d1, &d2, &d3, &d4, &d5, &d6, &d7, &x0, &x1,
                           &x2, &x3, &x4, &x5, &x6, &x7);

  _mm_storeu_si128((__m128i *)(s - 4 + 0 * p), x0);
  _mm_storeu_si128((__m128i *)(s - 4 + 1 * p), x1);
  _mm_storeu_si128((__m128i *)(s - 4 + 2 * p), x2);
  _mm_storeu_si128((__m128i *)(s - 4 + 3 * p), x3);
  _mm_storeu_si128((__m128i *)(s - 4 + 4 * p), x4);
  _mm_storeu_si128((__m128i *)(s - 4 + 5 * p), x5);
  _mm_storeu_si128((__m128i *)(s - 4 + 6 * p), x6);
  _mm_storeu_si128((__m128i *)(s - 4 + 7 * p), x7);
}

void aom_highbd_lpf_vertical_14_sse2(uint16_t *s, int pitch,
                                     const uint8_t *blimit,
                                     const uint8_t *limit,
                                     const uint8_t *thresh, int bd) {
  __m128i q[7], p[7], pq[7];
  __m128i p6, p5, p4, p3;
  __m128i p6_2, p5_2, p4_2, p3_2;
  __m128i d0, d1, d2, d3;
  __m128i d0_2, d1_2, d2_2, d3_2, d7_2;

  p6 = _mm_loadu_si128((__m128i *)((s - 8) + 0 * pitch));
  p5 = _mm_loadu_si128((__m128i *)((s - 8) + 1 * pitch));
  p4 = _mm_loadu_si128((__m128i *)((s - 8) + 2 * pitch));
  p3 = _mm_loadu_si128((__m128i *)((s - 8) + 3 * pitch));

  highbd_transpose4x8_8x4_sse2(&p6, &p5, &p4, &p3, &d0, &p[6], &p[5], &p[4],
                               &p[3], &p[2], &p[1], &p[0]);

  p6_2 = _mm_loadu_si128((__m128i *)(s + 0 * pitch));
  p5_2 = _mm_loadu_si128((__m128i *)(s + 1 * pitch));
  p4_2 = _mm_loadu_si128((__m128i *)(s + 2 * pitch));
  p3_2 = _mm_loadu_si128((__m128i *)(s + 3 * pitch));

  highbd_transpose4x8_8x4_sse2(&p6_2, &p5_2, &p4_2, &p3_2, &q[0], &q[1], &q[2],
                               &q[3], &q[4], &q[5], &q[6], &d7_2);

  highbd_lpf_internal_14_sse2(p, q, pq, blimit, limit, thresh, bd);

  highbd_transpose8x8_low_sse2(&d0, &p[6], &pq[5], &pq[4], &pq[3], &pq[2],
                               &pq[1], &pq[0], &d0, &d1, &d2, &d3);

  q[0] = _mm_srli_si128(pq[0], 8);
  q[1] = _mm_srli_si128(pq[1], 8);
  q[2] = _mm_srli_si128(pq[2], 8);
  q[3] = _mm_srli_si128(pq[3], 8);
  q[4] = _mm_srli_si128(pq[4], 8);
  q[5] = _mm_srli_si128(pq[5], 8);

  highbd_transpose8x8_low_sse2(&q[0], &q[1], &q[2], &q[3], &q[4], &q[5], &q[6],
                               &d7_2, &d0_2, &d1_2, &d2_2, &d3_2);

  _mm_storeu_si128((__m128i *)(s - 8 + 0 * pitch), d0);
  _mm_storeu_si128((__m128i *)(s - 8 + 1 * pitch), d1);
  _mm_storeu_si128((__m128i *)(s - 8 + 2 * pitch), d2);
  _mm_storeu_si128((__m128i *)(s - 8 + 3 * pitch), d3);

  _mm_storeu_si128((__m128i *)(s + 0 * pitch), d0_2);
  _mm_storeu_si128((__m128i *)(s + 1 * pitch), d1_2);
  _mm_storeu_si128((__m128i *)(s + 2 * pitch), d2_2);
  _mm_storeu_si128((__m128i *)(s + 3 * pitch), d3_2);
}

void aom_highbd_lpf_vertical_14_dual_sse2(uint16_t *s, int pitch,
                                          const uint8_t *blimit,
                                          const uint8_t *limit,
                                          const uint8_t *thresh, int bd) {
  __m128i q[7], p[7];
  __m128i p6, p5, p4, p3, p2, p1, p0, q0;
  __m128i p6_2, p5_2, p4_2, p3_2, p2_2, p1_2, q0_2, p0_2;
  __m128i d0, d7;
  __m128i d0_out, d1_out, d2_out, d3_out, d4_out, d5_out, d6_out, d7_out;

  p6 = _mm_loadu_si128((__m128i *)((s - 8) + 0 * pitch));
  p5 = _mm_loadu_si128((__m128i *)((s - 8) + 1 * pitch));
  p4 = _mm_loadu_si128((__m128i *)((s - 8) + 2 * pitch));
  p3 = _mm_loadu_si128((__m128i *)((s - 8) + 3 * pitch));
  p2 = _mm_loadu_si128((__m128i *)((s - 8) + 4 * pitch));
  p1 = _mm_loadu_si128((__m128i *)((s - 8) + 5 * pitch));
  p0 = _mm_loadu_si128((__m128i *)((s - 8) + 6 * pitch));
  q0 = _mm_loadu_si128((__m128i *)((s - 8) + 7 * pitch));

  highbd_transpose8x8_sse2(&p6, &p5, &p4, &p3, &p2, &p1, &p0, &q0, &d0, &p[6],
                           &p[5], &p[4], &p[3], &p[2], &p[1], &p[0]);

  p6_2 = _mm_loadu_si128((__m128i *)(s + 0 * pitch));
  p5_2 = _mm_loadu_si128((__m128i *)(s + 1 * pitch));
  p4_2 = _mm_loadu_si128((__m128i *)(s + 2 * pitch));
  p3_2 = _mm_loadu_si128((__m128i *)(s + 3 * pitch));
  p2_2 = _mm_loadu_si128((__m128i *)(s + 4 * pitch));
  p1_2 = _mm_loadu_si128((__m128i *)(s + 5 * pitch));
  p0_2 = _mm_loadu_si128((__m128i *)(s + 6 * pitch));
  q0_2 = _mm_loadu_si128((__m128i *)(s + 7 * pitch));

  highbd_transpose8x8_sse2(&p6_2, &p5_2, &p4_2, &p3_2, &p2_2, &p1_2, &p0_2,
                           &q0_2, &q[0], &q[1], &q[2], &q[3], &q[4], &q[5],
                           &q[6], &d7);

  highbd_lpf_internal_14_dual_sse2(p, q, blimit, limit, thresh, bd);

  highbd_transpose8x8_sse2(&d0, &p[6], &p[5], &p[4], &p[3], &p[2], &p[1], &p[0],
                           &d0_out, &d1_out, &d2_out, &d3_out, &d4_out, &d5_out,
                           &d6_out, &d7_out);

  _mm_storeu_si128((__m128i *)(s - 8 + 0 * pitch), d0_out);
  _mm_storeu_si128((__m128i *)(s - 8 + 1 * pitch), d1_out);
  _mm_storeu_si128((__m128i *)(s - 8 + 2 * pitch), d2_out);
  _mm_storeu_si128((__m128i *)(s - 8 + 3 * pitch), d3_out);
  _mm_storeu_si128((__m128i *)(s - 8 + 4 * pitch), d4_out);
  _mm_storeu_si128((__m128i *)(s - 8 + 5 * pitch), d5_out);
  _mm_storeu_si128((__m128i *)(s - 8 + 6 * pitch), d6_out);
  _mm_storeu_si128((__m128i *)(s - 8 + 7 * pitch), d7_out);

  highbd_transpose8x8_sse2(&q[0], &q[1], &q[2], &q[3], &q[4], &q[5], &q[6], &d7,
                           &d0_out, &d1_out, &d2_out, &d3_out, &d4_out, &d5_out,
                           &d6_out, &d7_out);

  _mm_storeu_si128((__m128i *)(s + 0 * pitch), d0_out);
  _mm_storeu_si128((__m128i *)(s + 1 * pitch), d1_out);
  _mm_storeu_si128((__m128i *)(s + 2 * pitch), d2_out);
  _mm_storeu_si128((__m128i *)(s + 3 * pitch), d3_out);
  _mm_storeu_si128((__m128i *)(s + 4 * pitch), d4_out);
  _mm_storeu_si128((__m128i *)(s + 5 * pitch), d5_out);
  _mm_storeu_si128((__m128i *)(s + 6 * pitch), d6_out);
  _mm_storeu_si128((__m128i *)(s + 7 * pitch), d7_out);
}
