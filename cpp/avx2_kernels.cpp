#include <iomanip>
#include <immintrin.h>
#include "ch_frb_io_internals.hpp"

using namespace std;

namespace ch_frb_io {
#if 0
};  // pacify emacs c-mode!
#endif


template<unsigned int N> inline void _vstr8_partial(stringstream &ss, __m256i x, bool hexflag);
template<unsigned int N> inline void _vstr32_partial(stringstream &ss, __m256i x, bool hexflag);
template<unsigned int N> inline void _vstr_partial(stringstream &ss, __m256 x);

template<> inline void _vstr8_partial<0>(stringstream &ss, __m256i x, bool hex) { return; }
template<> inline void _vstr32_partial<0>(stringstream &ss, __m256i x, bool hex) { return; }
template<> inline void _vstr_partial<0>(stringstream &ss, __m256 x) { return; }

template<unsigned int N> 
inline void _vstr8_partial(stringstream &ss, __m256i x, bool hexflag) 
{
    _vstr8_partial<N-1>(ss, x, hexflag);
    if (hexflag)
	ss << " " << setfill('0') << setw(2) << hex << uint32_t(uint8_t(_mm256_extract_epi8(x,N-1)));
    else
	ss << " " << int32_t(_mm256_extract_epi8(x,N-1));
}

template<unsigned int N>
inline void _vstr32_partial(stringstream &ss, __m256i x, bool hexflag) 
{
    _vstr32_partial<N-1>(ss, x, hexflag);
    if (hexflag)
	ss << " " << setfill('0') << setw(8) << hex << uint32_t(_mm256_extract_epi32(x,N-1));
    else
	ss << " " << _mm256_extract_epi32(x,N-1);
}


inline string _vstr8(__m256i x, bool hexflag=true)
{
    stringstream ss;
    ss << "[";
    _vstr8_partial<32> (ss, x, hexflag);
    ss << " ]";
    return ss.str();
}

inline string _vstr32(__m256i x, bool hexflag=false)
{
    stringstream ss;
    ss << "[";
    _vstr32_partial<8> (ss, x, hexflag);
    ss << " ]";
    return ss.str();
}


// -------------------------------------------------------------------------------------------------


inline void _unpack(__m256i &out0, __m256i &out1, __m256i &out2, __m256i &out3, __m256i x)
{
    // FIXME is there a better way to initialize this?
    static const __m256i ctl0 = _mm256_set_epi8(15,11,7,3,14,10,6,2,13,9,5,1,12,8,4,0,
						15,11,7,3,14,10,6,2,13,9,5,1,12,8,4,0);

    // 4-by-4 transpose within each 128-bit lane
    x = _mm256_shuffle_epi8(x, ctl0);

    __m256i y0 = _mm256_and_si256(x, _mm256_set1_epi32(0xff));
    __m256i y1 = _mm256_and_si256(x, _mm256_set1_epi32(0xff00));
    __m256i y2 = _mm256_and_si256(x, _mm256_set1_epi32(0xff0000));
    __m256i y3 = _mm256_and_si256(x, _mm256_set1_epi32(0xff000000));

    y1 = _mm256_srli_epi32(y1, 8);
    y2 = _mm256_srli_epi32(y2, 16);
    y3 = _mm256_srli_epi32(y3, 24);

    out0 = _mm256_permute2f128_si256(y0, y1, 0x20);
    out1 = _mm256_permute2f128_si256(y2, y3, 0x20);
    out2 = _mm256_permute2f128_si256(y0, y1, 0x31);
    out3 = _mm256_permute2f128_si256(y2, y3, 0x31);
}


inline void _set_weights(float *wtp, __m256i x, __m256i i0, __m256i i254, __m256 f0, __m256 f1)
{
    __m256i gt0 = _mm256_cmpgt_epi32(x, i0);
    __m256i gt254 = _mm256_cmpgt_epi32(x, i254);
    __m256i valid = _mm256_andnot_si256(gt0, gt254);

    _mm256_storeu_ps(wtp, _mm256_blendv_ps(f1, f0, valid));
}


inline void _kernel32(float *intp, float *wtp, __m256i data, __m256 scale0, __m256 scale1, __m256 offset0, __m256 offset1)
{
    __m256i in0, in1, in2, in3;
    _unpack(in0, in1, in2, in3, data);
    
    _mm256_storeu_ps(intp, scale0 * _mm256_cvtepi32_ps(in0) + offset0);
    _mm256_storeu_ps(intp+8, scale0 * _mm256_cvtepi32_ps(in1) + offset0);
    _mm256_storeu_ps(intp+16, scale1 * _mm256_cvtepi32_ps(in2) + offset1);
    _mm256_storeu_ps(intp+24, scale1 * _mm256_cvtepi32_ps(in3) + offset1);

    __m256i i0 = _mm256_set1_epi32(0);
    __m256i i254 = _mm256_set1_epi32(254);
    __m256i f0 = _mm256_set1_ps(0.0);
    __m256i f1 = _mm256_set1_ps(1.0);

    _set_weights(wtp, in0, i0, i254, f0, f1);
    _set_weights(wtp+8, in1, i0, i254, f0, f1);
    _set_weights(wtp+16, in2, i0, i254, f0, f1);
    _set_weights(wtp+24, in3, i0, i254, f0, f1);
}


inline void _kernel128(float *intp, float *wtp, const uint8_t *src, const float *scalep, const float *offsetp)
{
    __m256 scale = _mm256_loadu_ps(scalep);
    __m256 offset = _mm256_loadu_ps(offsetp);
    __m256 scale0, offset0;

    scale0 = _mm256_permute2f128_ps(scale, scale, 0x00);
    offset0 = _mm256_permute2f128_ps(offset, offset, 0x00);

    _kernel32(intp, wtp, 
	      _mm256_loadu_si256((const __m256i *) (src)),
	      _mm256_shuffle_ps(scale0, scale0, 0x00), 
	      _mm256_shuffle_ps(scale0, scale0, 0x55),
	      _mm256_shuffle_ps(offset0, offset0, 0x00), 
	      _mm256_shuffle_ps(offset0, offset0, 0x55));

    _kernel32(intp + 32, wtp + 32, 
	      _mm256_loadu_si256((const __m256i *) (src + 32)),
	      _mm256_shuffle_ps(scale0, scale0, 0xaa), 
	      _mm256_shuffle_ps(scale0, scale0, 0xff),
	      _mm256_shuffle_ps(offset0, offset0, 0xaa), 
	      _mm256_shuffle_ps(offset0, offset0, 0xff));


    scale0 = _mm256_permute2f128_ps(scale, scale, 0x11);
    offset0 = _mm256_permute2f128_ps(offset, offset, 0x11);

    _kernel32(intp + 64, wtp + 64,
	      _mm256_loadu_si256((const __m256i *) (src + 64)),
	      _mm256_shuffle_ps(scale0, scale0, 0x00), 
	      _mm256_shuffle_ps(scale0, scale0, 0x55),
	      _mm256_shuffle_ps(offset0, offset0, 0x00), 
	      _mm256_shuffle_ps(offset0, offset0, 0x55));

    _kernel32(intp + 96, wtp + 96, 
	      _mm256_loadu_si256((const __m256i *) (src + 96)),
	      _mm256_shuffle_ps(scale0, scale0, 0xaa), 
	      _mm256_shuffle_ps(scale0, scale0, 0xff),
	      _mm256_shuffle_ps(offset0, offset0, 0xaa), 
	      _mm256_shuffle_ps(offset0, offset0, 0xff));
}


inline void _kernel(float *intp, float *wtp, const uint8_t *src, const float *scalep, const float *offsetp)
{
    constexpr int n = constants::nt_per_assembled_chunk / 128;

    for (int i = 0; i < n; i++)
	_kernel128(intp + i*128, wtp + i*128, src + i*128, scalep + i*8, offsetp + i*8);
}


#if 0
void assembled_chunk::decode(float *intensity, float *weights, int stride) const
{
    if ((nt_per_packet != 16) || (constants::nt_per_assembled_chunk % 128))
	throw runtime_error("ch_frb_io: can't use this kernel");
    if (stride < constants::nt_per_assembled_chunk)
	throw runtime_error("ch_frb_io: bad stride passed to assembled_chunk::decode()");

    for (int if_coarse = 0; if_coarse < constants::nfreq_coarse; if_coarse++) {
	const float *scales_f = this->scales + if_coarse * nt_coarse;
	const float *offsets_f = this->offsets + if_coarse * nt_coarse;
	
	for (int if_fine = if_coarse*nupfreq; if_fine < (if_coarse+1)*nupfreq; if_fine++) {
	    const uint8_t *src_f = this->data + if_fine * constants::nt_per_assembled_chunk;
	    float *int_f = intensity + if_fine * stride;
	    float *wt_f = weights + if_fine * stride;

	    _kernel(int_f, wt_f, src_f, scales_f, offsets_f);
	}
    }    
}
#endif


// -------------------------------------------------------------------------------------------------


void peek_at_avx2_kernels()
{
    uint8_t v[32];
    for (int i = 0; i < 32; i++)
	v[i] = i+1;

    __m256i x = _mm256_loadu_si256((const __m256i *) v);

    __m256i y0, y1, y2, y3;
    _unpack(y0, y1, y2, y3, x);

    cout << _vstr8(x,false) << endl
	 << _vstr32(y0,false) << endl
	 << _vstr32(y1,false) << endl
	 << _vstr32(y2,false) << endl
	 << _vstr32(y3,false) << endl;
}


}  // namespace ch_frb_io
