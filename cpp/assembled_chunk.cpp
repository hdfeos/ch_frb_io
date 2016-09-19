#include <iostream>
#include "ch_frb_io_internals.hpp"

using namespace std;

namespace ch_frb_io {
#if 0
};  // pacify emacs c-mode!
#endif


assembled_chunk::assembled_chunk(int beam_id_, int nupfreq_, int nt_per_packet_, int fpga_counts_per_sample_, uint64_t chunk_t0_)
    : beam_id(beam_id_), 
      nupfreq(nupfreq_), 
      nt_per_packet(nt_per_packet_),
      fpga_counts_per_sample(fpga_counts_per_sample_), 
      chunk_t0(chunk_t0_),
      chunk_t1(chunk_t0_ + constants::nt_per_assembled_chunk)
{
    if ((beam_id < 0) || (beam_id > constants::max_allowed_beam_id))
	throw runtime_error("assembled_chunk constructor: bad beam_id argument");
    if ((nupfreq <= 0) || (nupfreq > constants::max_allowed_nupfreq))
	throw runtime_error("assembled_chunk constructor: bad nupfreq argument");
    if ((nt_per_packet <= 0) || !is_power_of_two(nt_per_packet) || (nt_per_packet > constants::nt_per_assembled_chunk))
	throw runtime_error("assembled_chunk constructor: bad nt_per_packet argument");
    if ((fpga_counts_per_sample <= 0) || (fpga_counts_per_sample > constants::max_allowed_fpga_counts_per_sample))
	throw runtime_error("assembled_chunk constructor: bad fpga_counts_per_sample argument");

    this->data = aligned_alloc<uint8_t> (constants::nfreq_coarse * nupfreq * constants::nt_per_assembled_chunk);

    this->nt_coarse = constants::nt_per_assembled_chunk / nt_per_packet;
    this->scales = aligned_alloc<float> (constants::nfreq_coarse * nt_coarse);
    this->offsets = aligned_alloc<float> (constants::nfreq_coarse * nt_coarse);
}


assembled_chunk::~assembled_chunk()
{
    free(data);
    free(scales);
    free(offsets);
}


void assembled_chunk::add_packet(const intensity_packet &packet)
{
    // Offset relative to beginning of packet
    int t0 = packet.fpga_count / uint64_t(fpga_counts_per_sample) - chunk_t0;

#if 1
    // FIXME remove later?
    bool bad = ((packet.nbeams != 1) ||
		(packet.nupfreq != this->nupfreq) ||
		(packet.ntsamp != this->nt_per_packet) ||
		(packet.fpga_counts_per_sample != this->fpga_counts_per_sample) ||
		(packet.fpga_count % (fpga_counts_per_sample * nt_per_packet)) ||
		(packet.beam_ids[0] != this->beam_id) ||
		(t0 < 0) ||
		(t0 + nt_per_packet > constants::nt_per_assembled_chunk));

    if (_unlikely(bad))
	throw runtime_error("ch_frb_io: internal error in assembled_chunk::add_packet()");
#endif

    for (int f = 0; f < packet.nfreq_coarse; f++) {
	int coarse_freq_id = packet.freq_ids[f];

	this->scales[coarse_freq_id*nt_coarse + (t0/nt_per_packet)] = packet.scales[f];
	this->offsets[coarse_freq_id*nt_coarse + (t0/nt_per_packet)] = packet.offsets[f];

	for (int u = 0; u < nupfreq; u++) {
	    memcpy(data + (coarse_freq_id*nupfreq + u) * constants::nt_per_assembled_chunk + t0, 
		   packet.data + (f*nupfreq + u) * nt_per_packet,
		   nt_per_packet);
	}
    }
}


void assembled_chunk::decode(float *intensity, float *weights, int stride) const
{
    if (stride < constants::nt_per_assembled_chunk)
	throw runtime_error("ch_frb_io: bad stride passed to assembled_chunk::decode()");

    for (int if_coarse = 0; if_coarse < constants::nfreq_coarse; if_coarse++) {
	const float *scales_f = this->scales + if_coarse * nt_coarse;
	const float *offsets_f = this->offsets + if_coarse * nt_coarse;
	
	for (int if_fine = if_coarse*nupfreq; if_fine < (if_coarse+1)*nupfreq; if_fine++) {
	    const uint8_t *src_f = this->data + if_fine * constants::nt_per_assembled_chunk;
	    float *int_f = intensity + if_fine * stride;
	    float *wt_f = weights + if_fine * stride;

	    for (int it_coarse = 0; it_coarse < nt_coarse; it_coarse++) {
		float scale = scales_f[it_coarse];
		float offset = offsets_f[it_coarse];
		
		for (int it_fine = it_coarse*nt_per_packet; it_fine < (it_coarse+1)*nt_per_packet; it_fine++) {
		    float x = float(src_f[it_fine]);
		    int_f[it_fine] = scale*x + offset;
		    wt_f[it_fine] = ((x*(255.-x)) > 0.5) ? 1.0 : 0.0;  // FIXME ugh
		}
	    }
	}
    }
}


}  // namespace ch_frb_io
