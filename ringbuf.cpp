#include <iostream>
#include <queue>
#include <deque>

#include "ringbuf.hpp"

using namespace std;

template <class T>
Ringbuf<T>::Ringbuf(int maxsize) :
    _deleter(this),
    _q(),
    _live(0),
    _maxsize(maxsize)
{
    pthread_mutex_init(&this->_q_lock, NULL);
    pthread_mutex_init(&this->_live_lock, NULL);
}

template <class T>
Ringbuf<T>::~Ringbuf() {
    pthread_mutex_destroy(&this->_q_lock);
    pthread_mutex_destroy(&this->_live_lock);
}

template <class T>
shared_ptr<T> Ringbuf<T>::push(T* t) {
    // Is there room?  (This tries to make room if not...)
    bool can = can_push();
    if (!can)
        return shared_ptr<T>();
    //cout << "Creating shared_ptr..." << endl;
    shared_ptr<T> p(t, _deleter);
    pthread_mutex_lock(&this->_live_lock);
    _live++;
    pthread_mutex_unlock(&this->_live_lock);
    pthread_mutex_lock(&this->_q_lock);
    _q.push_back(p);
    pthread_mutex_unlock(&this->_q_lock);
    //cout << "Now " << _live << " objects are live" << endl;
    return p;
}

template <class T>
shared_ptr<T> Ringbuf<T>::pop() {
    //cout << "Popping..." << endl;
    shared_ptr<T> p;
    pthread_mutex_lock(&this->_q_lock);
    if (_q.empty()) {
        //cout << "Pop: empty" << endl;
    } else {
        p = _q.pop_front();
        //cout << "Pop: returning " << *p << endl;
    }
    pthread_mutex_unlock(&this->_q_lock);
    return p;
}

template <class T>
vector<shared_ptr<T> > Ringbuf<T>::snapshot(bool (*testFunc)(const shared_ptr<T>)) {
    vector<shared_ptr<T> > vec;
    pthread_mutex_lock(&this->_q_lock);
    for (auto it = _q.begin(); it != _q.end(); it++) {
        if (!testFunc || testFunc(*it)) {
	    vec.push_back(*it);
        }
    }
    pthread_mutex_unlock(&this->_q_lock);
    return vec;
}

template <class T>
void Ringbuf<T>::dropping(shared_ptr<T> t) {}

// Called by the RingbufDeleter when a shared_ptr is deleted
template <class T>
void Ringbuf<T>::deleted(T* t) {
    //cout << "Deleting object: " << *t << endl;
    pthread_mutex_lock(&this->_live_lock);
    _live--;
    pthread_mutex_unlock(&this->_live_lock);
    //cout << "Now " << _live << " objects are live" << endl;
    delete t;
}

template <class T>
bool Ringbuf<T>::can_push() {
    while (1) {
        pthread_mutex_lock(&this->_live_lock);
        size_t nlive = _live;
        pthread_mutex_unlock(&this->_live_lock);
        if (nlive < _maxsize)
            break;
        //cout << "Push: _live >= _maxsize." << " (" << nlive << " >= " << _maxsize << ")" << endl;
        pthread_mutex_lock(&this->_q_lock);

        if (_q.empty()) {
            //cout << "Ring buffer empty but still too many live elements -- push fails" << endl;
            pthread_mutex_unlock(&this->_q_lock);
            return false;
        }
        //cout << "Dropping an element..." << endl;
        shared_ptr<T> p = _q.front();
        _q.pop_front();
        pthread_mutex_unlock(&this->_q_lock);
        dropping(p);
        p.reset();
        //cout << "Now " << _live << " live" << endl;
    }
    return true;
}

// Helper class that is the Deleter for our shared_ptr<frames>.  Calls
// Ringbuf.deleted() to track number of live frames.
template <class T>
RingbufDeleter<T>::RingbufDeleter(Ringbuf<T>* rb) : _ringbuf(rb) {}

template <class T>
void RingbufDeleter<T>::operator()(T* t) {
    //cout << "RingbufDelete::operator() called." << endl;
    _ringbuf->deleted(t);
}



////////////////////////// How we'd use this ring buffer in L1 //////////////////////////


#include "ch_frb_io.hpp"
using namespace ch_frb_io;

std::ostream& operator<<(std::ostream& s, const assembled_chunk& ch) {
    s << "assembled_chunk(beam " << ch.beam_id << ", ichunk " << ch.ichunk << ")";
    return s;
}

class L1Ringbuf;

class AssembledChunkRingbuf : public Ringbuf<assembled_chunk> {

public:
    AssembledChunkRingbuf(int binlevel, L1Ringbuf* parent, int maxsize) :
        Ringbuf<assembled_chunk>(maxsize),
        _binlevel(binlevel),
        _parent(parent)
    {}

    virtual ~AssembledChunkRingbuf() {}

protected:
    // my time-binning. level: 0 = original intensity stream; 1 =
    // binned x 2, 2 = binned x 4.
    int _binlevel;
    L1Ringbuf* _parent;

    // Called when the given frame *t* is being dropped off the buffer
    // to free up some space for a new frame.
    virtual void dropping(shared_ptr<assembled_chunk> t);

};

class L1Ringbuf {
    friend class AssembledChunkRingbuf;

    static const size_t Nbins = 4;

public:
    L1Ringbuf() :
        _q(),
        _rb(),
        _dropped()
    {
        // Create the ring buffer objects for each time binning
        // (0 = native rate, 1 = binned by 2, ...)
        for (size_t i=0; i<Nbins; i++)
            _rb.push_back(shared_ptr<AssembledChunkRingbuf>
                          (new AssembledChunkRingbuf(i, this, 4)));
        // Fill the "_dropped" array with empty shared_ptrs.
        for (size_t i=0; i<Nbins-1; i++)
            _dropped.push_back(shared_ptr<assembled_chunk>());
    }

    /*
     Tries to enqueue an assembled_chunk.  If no space can be
     allocated, returns false.  The ring buffer now assumes ownership
     of the assembled_chunk.
     */
    bool push(assembled_chunk* ch) {
        shared_ptr<assembled_chunk> p = _rb[0]->push(ch);
        if (!p)
            return false;
        _q.push_back(p);
        return true;
    }

    /*
     Returns the next assembled_chunk for downstream processing.
     */
    shared_ptr<assembled_chunk> pop() {
        if (_q.empty())
            return shared_ptr<assembled_chunk>();
        shared_ptr<assembled_chunk> p = _q.front();
        _q.pop_front();
        return p;
    }

    /*
     Prints a report of the assembled_chunks currently queued.
     */
    void print() {
        cout << "L1 ringbuf:" << endl;
        cout << "  downstream: [ ";
        for (auto it = _q.begin(); it != _q.end(); it++) {
            cout << (*it)->ichunk << " ";
        }
        cout << "];" << endl;
        for (size_t i=0; i<Nbins; i++) {
            vector<shared_ptr<assembled_chunk> > v = _rb[i]->snapshot(NULL);
            cout << "  binning " << i << ": [ ";
            for (auto it = v.begin(); it != v.end(); it++) {
                cout << (*it)->ichunk << " ";
            }
            cout << "]" << endl;
            if (i < Nbins-1) {
                cout << "  dropped " << i << ": ";
                if (_dropped[i])
                    cout << _dropped[i]->ichunk << endl;
                else
                    cout << "none" << endl;
            }
        }
    }
    
protected:
    // The queue for downstream
    deque<shared_ptr<assembled_chunk> > _q;

    // The ring buffers for each time-binning.  Length fixed at Nbins.
    vector<shared_ptr<AssembledChunkRingbuf> > _rb;

    // The assembled_chunks that have been dropped from the ring
    // buffers and are waiting for a pair to be time-downsampled.
    // Length fixed at Nbins-1.
    vector<shared_ptr<assembled_chunk> > _dropped;

    // Called from the AssembledChunkRingbuf objects when a chunk is
    // about to be dropped from one binning level of the ringbuf.  If
    // the chunk does not have a partner waiting (in _dropped), then
    // it is saved in _dropped.  Otherwise, the two chunks are merged
    // into one new chunk and added to the next binning level's
    // ringbuf.
    void dropping(int binlevel, shared_ptr<assembled_chunk> ch) {
        cout << "Bin level " << binlevel << " dropping a chunk" << endl;
        if (binlevel >= (int)(Nbins-1))
            return;

        if (_dropped[binlevel]) {
            cout << "Now have 2 dropped chunks from bin level " << binlevel << endl;
            // FIXME -- bin down
            assembled_chunk* binned = new assembled_chunk(ch->beam_id, ch->nupfreq, ch->nt_per_packet, ch->fpga_counts_per_sample, _dropped[binlevel]->ichunk);
            // push onto _rb[level+1]
            _rb[binlevel+1]->push(binned);
            _dropped[binlevel].reset();
        } else {
            // Keep this one until its partner arrives!
            cout << "Saving as _dropped" << binlevel << endl;
            _dropped[binlevel] = ch;
        }
    }

};

// after L1Ringbuf has been declared...
void AssembledChunkRingbuf::dropping(shared_ptr<assembled_chunk> t) {
    _parent->dropping(_binlevel, t);
}



int main() {

    L1Ringbuf rb;

    int beam = 77;
    int nupfreq = 4;
    int nt_per = 16;
    int fpga_per = 400;

    assembled_chunk* ch;
    //ch = assembled_chunk::make(4, nupfreq, nt_per, fpga_per, 42);

    std::random_device rd;
    std::mt19937 rng(rd());
    rng.seed(42);
    std::uniform_int_distribution<> rando(0,1);

    for (int i=0; i<100; i++) {
        ch = new assembled_chunk(beam, nupfreq, nt_per, fpga_per, i);
        rb.push(ch);

        cout << "Pushed " << i << endl;
        rb.print();
        cout << endl;

        // downstream thread consumes with a lag of 2...
        if (i >= 2) {
            // Randomly consume 0 to 2 chunks
            if (rando(rng)) {
                cout << "Downstream consumes a chunk" << endl;
                rb.pop();
            }
            if (rando(rng)) {
                cout << "Downstream consumes a chunk" << endl;
                rb.pop();
            }
        }
    }

    cout << "End state:" << endl;
    rb.print();
    cout << endl;


}


/*
int main() {
    cout << "Creating ringbuf..." << endl;
    Ringbuf<int> rb(4);

    int a = 42;
    int b = 43;
    int c = 44;

    cout << "Pushing" << endl;
    rb.push(&a);
    cout << "Pushing" << endl;
    rb.push(&b);
    cout << "Pushing" << endl;
    rb.push(&c);

    cout << "Popping" << endl;
    shared_ptr<int> p1 = rb.pop();
    cout << "Popping" << endl;
    shared_ptr<int> p2 = rb.pop();
    cout << "Dropping" << endl;
    p1.reset();
    cout << endl;

    int d = 45;
    int e = 46;
    int f = 47;
    int g = 48;

    cout << "Pushing d..." << endl;
    shared_ptr<int> pd = rb.push(&d);

    cout << endl;
    cout << "Pushing e..." << endl;
    shared_ptr<int> pe = rb.push(&e);

    cout << endl;
    cout << "Pushing f..." << endl;
    shared_ptr<int> pf = rb.push(&f);

    cout << endl;
    cout << "Pushing g..." << endl;
    rb.push(&g);

    cout << "Done" << endl;

}
 */

