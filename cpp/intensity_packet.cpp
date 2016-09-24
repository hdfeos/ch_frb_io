#include <cassert>
#include <iostream>
#include "ch_frb_io_internals.hpp"

using namespace std;

namespace ch_frb_io {
#if 0
};  // pacify emacs c-mode!
#endif


// Returns true if packet is good, false if bad.
//
// Explicitly, the following checks are performed:
//   - protocol version == 1
//   - dimensions (nbeams, nfreq_coarse, nupfreq, ntsamp) are not large enough to lead to integer overflows
//   - packet and data byte counts are correct
//   - coarse_freq_ids are in range
//   - ntsamp is a power of two
//   - nbeams, nfreq_coarse, nupfreq, ntsamp, fpga_counts_per_sample are all > 0
//   - fpga_count is a multiple of (fpga_counts_per_sample * ntsamp)

bool intensity_packet::read(const uint8_t *src, int src_nbytes)
{
    if (_unlikely(src_nbytes < 24))
	return false;
    if (_unlikely(src_nbytes > constants::max_input_udp_packet_size))
	return false;

    memcpy(this, src, 24);

    if (_unlikely(protocol_version != 1))
	return false;
    if (_unlikely(!is_power_of_two(ntsamp)))
	return false;
    if (_unlikely(fpga_counts_per_sample == 0))
	return false;

    // Note conversions to uint64_t, to prevent integer overflow
    uint64_t fpga_counts_per_packet = uint64_t(fpga_counts_per_sample) * uint64_t(ntsamp);
    if (_unlikely(fpga_count % fpga_counts_per_packet != 0))
	return false;
	
    uint64_t n1 = uint64_t(nbeams);
    uint64_t n2 = uint64_t(nfreq_coarse);
    uint64_t n3 = uint64_t(nupfreq);
    uint64_t n4 = uint64_t(ntsamp);

    // Expected header, data size
    uint64_t nh = 24 + 2*n1 + 2*n2 + 8*n1*n2;
    uint64_t nd = n1 * n2 * n3 * n4;

    if (_unlikely(uint64_t(src_nbytes) != nh+nd))
	return false;
    if (_unlikely(uint64_t(data_nbytes) != nd))
	return false;

    this->beam_ids = (uint16_t *) (src + 24);
    this->freq_ids = (uint16_t *) (src + 24 + 2*n1);
    this->scales = (float *) (src + 24 + 2*n1 + 2*n2);
    this->offsets = (float *) (src + 24 + 2*n1 + 2*n2 + 4*n1*n2);
    this->data = (uint8_t *) (src + nh);

    for (int i = 0; i < nfreq_coarse; i++)
	if (_unlikely(freq_ids[i] >= constants::nfreq_coarse))
	    return false;

    return true;
}


void intensity_packet::encode(uint8_t *dst, const float *intensity, const float *weights, int beam_stride, int freq_stride, float wt_cutoff)
{
    int nb = this->nbeams;
    int nf = this->nfreq_coarse;
    int nu = this->nupfreq;
    int nt = this->ntsamp;

    memcpy(dst, this, 24);
    memcpy(dst + 24, this->beam_ids, 2*nb);
    memcpy(dst + 24 + 2*nb, this->freq_ids, 2*nf);

    scales = (float *) (dst + 24 + 2*nb + 2*nf);
    offsets = (float *) (dst + 24 + 2*nb + 2*nf + 4*nb*nf);
    data = dst + 24 + 2*nb + 2*nf + 8*nb*nf;

    for (int b = 0; b < nb; b++) {
	for (int f = 0; f < nf; f++) {
	    uint8_t *sub_data = data + (b*nf+f) * (nu*nt);
	    const float *sub_int = intensity + b*beam_stride + f*nu*freq_stride;
	    const float *sub_wt = weights + b*beam_stride + f*nu*freq_stride;

	    float acc0 = 0.0;
	    float acc1 = 0.0;
	    float acc2 = 0.0;

	    for (int u = 0; u < nu; u++) {
		for (int t = 0; t < nt; t++) {
		    float x = sub_int[u*freq_stride+t];
		    float w = (sub_wt[u*freq_stride+t] >= wt_cutoff) ? 1.0 : 0.0;

		    acc0 += w;
		    acc1 += w * x;
		    acc2 += w * x * x;
		}
	    }
		    
	    if (acc0 <= 0.0) {
		this->scales[b*nf+f] = 1.0;
		this->offsets[b*nf+f] = 0.0;
		memset(sub_data, 0, nu*nt);
		continue;
	    }

	    float mean = acc1/acc0;
	    float var = acc2/acc0 - mean*mean;
    
	    // Since we use single precision, the numerical error in 'var' is approx (1.0e-7 * mean*mean),
	    // so we need to regulate values of 'var' which are of this order or smaller.  The radiometer
	    // equation predicts that the ideal variance is (1.0e-3 * mean*mean) or more, depending on
	    // the correlator configuration.  We choose a regulator which is safely in between these values.

	    var = max(var, float(1.0e-5) * mean*mean);

	    float scale = sqrt(var) / 25.;
	    float offset = -128.*scale + mean;   // 0x80 -> mean

	    this->scales[b*nf+f] = scale;
	    this->offsets[b*nf+f] = offset;

	    for (int u = 0; u < nu; u++) {
		for (int t = 0; t < nt; t++) {
		    float x = sub_int[u*freq_stride+t];
		    float w = (sub_wt[u*freq_stride+t] >= wt_cutoff) ? 1.0 : 0.0;

		    x = w * (x - offset) / scale;
		    x = min(x, float(255.));
		    x = max(x, float(0.));
		    sub_data[u*nt+t] = uint8_t(x+0.5);   // round to nearest integer
		}
	    }
	}
    }
}


int intensity_packet::find_freq_id(int freq_id) const
{
    for (int i = 0; i < this->nfreq_coarse; i++)
	if (this->freq_ids[i] == freq_id)
	    return i;
    return -1;
}


bool intensity_packet::contains_freq_id(int freq_id) const
{
    int i = this->find_freq_id(freq_id);
    return (i >= 0);
}


void test_packet_encoding()
{
    intensity_packet p;

    char *p0 = (char *) &p;
    assert((char *) &p.data_nbytes == p0+4);
    assert((char *) &p.fpga_counts_per_sample == p0+6);
    assert((char *) &p.fpga_count == p0+8);
    assert((char *) &p.nbeams == p0+16);
    assert((char *) &p.nfreq_coarse == p0+18);
    assert((char *) &p.nupfreq == p0+20);
    assert((char *) &p.ntsamp == p0+22);

    cerr << "test_packet_encoding(): success\n";
}


}   // namespace ch_frb_io
