#include <cassert>
#include "ch_frb_io_internals.hpp"

using namespace std;
using namespace ch_frb_io;


inline bool array_contains(const int *arr, int len, int x)
{
    for (int i = 0; i < len; i++)
	if (arr[i] == x)
	    return true;
    return false;
}


inline double intval(int beam_id, int ifreq, int it)
{
    return 0.823*beam_id + 1.319*ifreq + 0.2139*it;
}


// weights are between 0.2 and 1
inline double wtval(int beam_id, int ifreq, int it)
{
    return 0.6 + 0.4 * sin(1.328*beam_id + 2.382*ifreq + 0.883*it);
}


// -------------------------------------------------------------------------------------------------


struct unit_test_instance {
    static constexpr int maxbeams = 8;

    int nbeams = 0;
    int nupfreq = 0;
    int nfreq_coarse_per_packet = 0;
    int nt_per_packet = 0;
    int nt_per_chunk = 0;
    int nt_tot = 0;
    int nt_maxlag = 0;
    int fpga_counts_per_sample = 0;
    uint64_t initial_t0 = 0;
    float wt_cutoff = 0.0;

    vector<int> recv_beam_ids;
    vector<int> send_beam_ids;
    vector<int> send_freq_ids;
    int send_stride;

    // not protected by lock
    pthread_t consumer_threads[maxbeams];    
    shared_ptr<ch_frb_io::intensity_network_stream> istream;

    pthread_mutex_t tpos_lock;
    pthread_cond_t cond_tpos_changed;
    uint64_t consumer_tpos[maxbeams];

    unit_test_instance(std::mt19937 &rng);
    ~unit_test_instance();

    void show() const;
};


unit_test_instance::unit_test_instance(std::mt19937 &rng)
{
    const int nfreq_coarse = ch_frb_io::constants::nfreq_coarse;

    this->nbeams = randint(rng, 1, maxbeams+1);
    this->nupfreq = randint(rng, 1, 17);
    this->nfreq_coarse_per_packet = 1 << randint(rng,0,5);
    
    // nt_max = max possible nt_per_packet (before max packet size is exceeded)
    int header_nbytes = 24 + 2*nbeams + 2*nfreq_coarse_per_packet + 8*nbeams*nfreq_coarse_per_packet;
    int nbytes_per_nt = nbeams * nfreq_coarse_per_packet * nupfreq;
    int nt_max = (ch_frb_io::constants::max_output_udp_packet_size - header_nbytes) / nbytes_per_nt;
    nt_max = min(512, nt_max);

    assert(nt_max >= 3);
    this->nt_per_packet = randint(rng, nt_max/2+1, nt_max+1);

    // FIXME increase nt_tot
    this->nt_per_chunk = nt_per_packet * randint(rng,1,10);
    this->nt_tot = nt_per_chunk * randint(rng,1,10);
    this->nt_maxlag = 4 * ch_frb_io::constants::nt_per_assembled_chunk;

    this->fpga_counts_per_sample = randint(rng, 1, 1025);
    this->initial_t0 = randint(rng, 0, 4097);
    this->wt_cutoff = uniform_rand(rng, 0.3, 0.7);

    // Clunky way of generating random beam_ids
    this->recv_beam_ids.resize(nbeams);
    for (int i = 0; i < nbeams; i++) {
	do {
	    recv_beam_ids[i] = randint(rng, 0, ch_frb_io::constants::max_allowed_beam_id);
	} while (array_contains(&recv_beam_ids[0], i, recv_beam_ids[i]));
    }

    // To slightly strengthen the test, we permute the receiver beams relative to sender
    this->send_beam_ids = recv_beam_ids;
    std::shuffle(send_beam_ids.begin(), send_beam_ids.end(), rng);

    // Randomly permute frequencies, just to strengthen the test
    this->send_freq_ids.resize(nfreq_coarse);
    for (int i = 0; i < nfreq_coarse; i++)
	send_freq_ids[i] = i;
    std::shuffle(send_freq_ids.begin(), send_freq_ids.end(), rng);

    this->send_stride = randint(rng, nt_per_chunk, 2*nt_per_chunk+1);

    xpthread_mutex_init(&this->tpos_lock);
    xpthread_cond_init(&this->cond_tpos_changed);
    
    for (int ithread = 0; ithread < nbeams; ithread++)
	this->consumer_tpos[ithread] = initial_t0;

    this->show();

    // FIXME revisit carefully
    int nt_lag = (nt_maxlag + 2 * ch_frb_io::constants::nt_per_assembled_chunk);
    int npackets_needed = ((nt_lag + nt_per_packet - 1) / nt_per_packet) * (ch_frb_io::constants::nfreq_coarse /nfreq_coarse_per_packet);
    int nbytes_per_packet = header_nbytes + nbytes_per_nt * nt_per_packet;
    int nbytes_needed = nbytes_per_packet * npackets_needed;
    int npackets_alloc = ch_frb_io::constants::unassembled_ringbuf_capacity * ch_frb_io::constants::max_unassembled_packets_per_list;
    int nbytes_alloc = ch_frb_io::constants::unassembled_ringbuf_capacity * ch_frb_io::constants::max_unassembled_nbytes_per_list;

    if ((npackets_alloc < npackets_needed) || (nbytes_alloc < nbytes_needed)) {
	cout << "nt_lag=" << nt_lag << endl
	     << "npackets_needed=" << npackets_needed << endl
	     << "nbytes_per_packet=" << nbytes_per_packet << endl
	     << "nbytes_needed=" << nbytes_needed << endl
	     << "npackets_alloc=" << npackets_alloc << endl
	     << "nbytes_alloc=" << nbytes_alloc << endl
	     << "  Fatal: unassembled_packet_buf is underallocated" << endl;

	exit(1);
    }
}


unit_test_instance::~unit_test_instance()
{
    pthread_mutex_destroy(&tpos_lock);
    pthread_cond_destroy(&cond_tpos_changed);
}


void unit_test_instance::show() const
{
    cout << "nbeams=" << nbeams << endl
	 << "nupfreq=" << nupfreq << endl
	 << "nfreq_coarse_per_packet=" << nfreq_coarse_per_packet << endl
	 << "nt_per_packet=" << nt_per_packet << endl
	 << "nt_per_chunk=" << nt_per_chunk << endl
	 << "nt_tot=" << nt_tot << endl;
}


// -------------------------------------------------------------------------------------------------


struct consumer_thread_context {
    shared_ptr<ch_frb_io::intensity_beam_assembler> assembler;
    shared_ptr<unit_test_instance> tp;
    int ithread = 0;

    pthread_mutex_t lock;
    pthread_cond_t cond_running;
    bool is_running = false;

    consumer_thread_context(const shared_ptr<ch_frb_io::intensity_beam_assembler> &assembler_, const shared_ptr<unit_test_instance> &tp_, int ithread_)
	: assembler(assembler_), tp(tp_), ithread(ithread_)
    {
	xpthread_mutex_init(&lock);
	xpthread_cond_init(&cond_running);
    }
};


static void *consumer_thread_main(void *opaque_arg)
{
    consumer_thread_context *context = reinterpret_cast<consumer_thread_context *> (opaque_arg);

    //
    // Note: the consumer thread startup logic works like this:
    //
    //   - parent thread puts a context struct on its stack, in spawn_consumer_thread()
    //   - parent thread calls pthread_create() to spawn consumer thread
    //   - parent thread blocks waiting for consumer thread to set context->is_running
    //   - when parent thread unblocks, spawn_consumer_thread() removes and the context struct becomes invalid
    //
    // Therefore, the consumer thread is only allowed to access the context struct _before_
    // setting context->is_running to unblock the parent thread.  The first thing we do is
    // extract all members of the context struct so we don't need to access it again.
    //
    shared_ptr<ch_frb_io::intensity_beam_assembler> assembler = context->assembler;
    shared_ptr<unit_test_instance> tp = context->tp;
    int ithread = context->ithread;
    
    // Now we can set context->is_running and unblock the parent thread.
    pthread_mutex_lock(&context->lock);
    context->is_running = true;
    pthread_cond_broadcast(&context->cond_running);
    pthread_mutex_unlock(&context->lock);
    
    uint64_t tpos = 0;
    bool tpos_initialized = false;

    for (;;) {
	shared_ptr<assembled_chunk> chunk;
	bool alive = assembler->get_assembled_chunk(chunk);

	if (!alive)
	    return NULL;

	assert(chunk->nupfreq == tp->nupfreq);
	assert(chunk->fpga_counts_per_sample == tp->fpga_counts_per_sample);
	assert(chunk->beam_id == assembler->beam_id);

	if (tpos_initialized)
	    assert(chunk->chunk_t0 == tpos);

	// tpos = expected chunk_t0 in the next assembled_chunk.
	tpos = chunk->chunk_t0 + ch_frb_io::constants::nt_per_assembled_chunk;
	tpos_initialized = true;

	pthread_mutex_lock(&tp->tpos_lock);
	tp->consumer_tpos[ithread] = chunk->chunk_t0;
	pthread_cond_broadcast(&tp->cond_tpos_changed);
	pthread_mutex_unlock(&tp->tpos_lock);
	
	// more chunk processing will go here
    }
}


static void spawn_consumer_thread(const shared_ptr<ch_frb_io::intensity_beam_assembler> &assembler, const shared_ptr<unit_test_instance> &tp, int ithread)
{
    consumer_thread_context context(assembler, tp, ithread);

    int err = pthread_create(&tp->consumer_threads[ithread], NULL, consumer_thread_main, &context);
    if (err)
	throw runtime_error(string("pthread_create() failed to create consumer thread: ") + strerror(errno));

    pthread_mutex_lock(&context.lock);
    while (!context.is_running)
	pthread_cond_wait(&context.cond_running, &context.lock);
    pthread_mutex_unlock(&context.lock);
}


static void spawn_all_receive_threads(const shared_ptr<unit_test_instance> &tp)
{
    vector<shared_ptr<ch_frb_io::intensity_beam_assembler> > assemblers;

    for (int ithread = 0; ithread < tp->nbeams; ithread++) {
	auto assembler = ch_frb_io::intensity_beam_assembler::make(tp->recv_beam_ids[ithread]);
	spawn_consumer_thread(assembler, tp, ithread);
	assemblers.push_back(assembler);
    }

    tp->istream = intensity_network_stream::make(assemblers, ch_frb_io::constants::default_udp_port);
}


// -------------------------------------------------------------------------------------------------


static void send_data(const shared_ptr<unit_test_instance> &tp)
{
    const int nbeams = tp->nbeams;
    const int nupfreq = tp->nupfreq;
    const int nfreq_coarse = ch_frb_io::constants::nfreq_coarse;
    const int nt_chunk = tp->nt_per_chunk;
    const int nchunks = tp->nt_tot / tp->nt_per_chunk;
    const int stride = tp->send_stride;
    const int s2 = nupfreq * stride;
    const int s3 = nfreq_coarse * s2;

    string dstname = "127.0.0.1:" + to_string(ch_frb_io::constants::default_udp_port);

    // spawns network thread
    auto ostream = intensity_network_ostream::make(dstname, tp->send_beam_ids, tp->send_freq_ids, tp->nupfreq,
						   tp->nt_per_chunk, tp->nfreq_coarse_per_packet, 
						   tp->nt_per_packet, tp->fpga_counts_per_sample,
						   tp->wt_cutoff, 0.0);

    vector<float> intensity(nbeams * s3, 0.0);
    vector<float> weights(nbeams * s3, 0.0);

    for (int ichunk = 0; ichunk < nchunks; ichunk++) {
	int chunk_t0 = tp->initial_t0 + ichunk * nt_chunk;   // start of chunk

	for (int ibeam = 0; ibeam < nbeams; ibeam++) {
	    int beam_id = tp->send_beam_ids[ibeam];

	    for (int ifreq_coarse = 0; ifreq_coarse < nfreq_coarse; ifreq_coarse++) {
		int coarse_freq_id = tp->send_freq_ids[ifreq_coarse];

		for (int iupfreq = 0; iupfreq < nupfreq; iupfreq++) {
		    int ifreq_logical = coarse_freq_id * nupfreq + iupfreq;
		    
		    int row_start = ibeam*s3 + ifreq_coarse*s2 + iupfreq*stride;
		    float *i_row = &intensity[row_start];
		    float *w_row = &weights[row_start];

		    for (int it = 0; it < nt_chunk; it++) {
			i_row[it] = intval(beam_id, ifreq_logical, chunk_t0 + it);
			w_row[it] = wtval(beam_id, ifreq_logical, chunk_t0 + it);
		    }
		}
	    }
	}

	// Wait for consumer threads if necessary
	pthread_mutex_lock(&tp->tpos_lock);
	for (int i = 0; i < nbeams; i++) {
	    while (tp->consumer_tpos[i] + tp->nt_maxlag < chunk_t0)
		pthread_cond_wait(&tp->cond_tpos_changed, &tp->tpos_lock);
	}
	pthread_mutex_unlock(&tp->tpos_lock);
	
	uint64_t fpga_count = (tp->initial_t0 + ichunk * nt_chunk) * tp->fpga_counts_per_sample;
	ostream->send_chunk(&intensity[0], &weights[0], stride, fpga_count, true);
    }

    // joins network thread
    ostream->end_stream(true);
}


// -------------------------------------------------------------------------------------------------


int main(int argc, char **argv)
{
    std::random_device rd;
    std::mt19937 rng(rd());

    for (int iouter = 0; iouter < 100; iouter++) {
	auto tp = make_shared<unit_test_instance> (rng);

	spawn_all_receive_threads(tp);
	tp->istream->start_stream();

	send_data(tp);	
	tp->istream->end_stream(true);  // join_threads=true

	for (int ibeam = 0; ibeam < tp->nbeams; ibeam++) {
	    int err = pthread_join(tp->consumer_threads[ibeam], NULL);
	    if (err)
		throw runtime_error("pthread_join() failed");
	}
    }

    return 0;
}
