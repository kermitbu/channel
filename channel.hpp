#pragma once

#include <stdlib.h>
#include <stddef.h>
#include <atomic>
#include <chrono>
#include "assert.h"

namespace kmt{

template <typename T> class atomic_pointer
{
public:
	inline atomic_pointer() {
		_ptr = nullptr;
	}

	inline void set(T *ptr_) {
		_ptr = ptr_;
	}

	inline T *xchg(T *val_) {
		return _ptr.exchange(val_, std::memory_order_acq_rel);
	}

	inline T *cas(T *cmp_, T *val_){
		_ptr.compare_exchange_strong(cmp_, val_, std::memory_order_acq_rel);
		return cmp_;
	}

private:
	std::atomic<T *> _ptr;
};

template <typename T, int N> class queue
{
public:
	inline queue() {
		_begin_chunk = allocate_chunk();
		assert(_begin_chunk);
		_begin_pos = 0;
		_back_chunk = nullptr;
		_back_pos = 0;
		_end_chunk = _begin_chunk;
		_end_pos = 0;
	}

	inline ~queue() {
		while (true) {
			if (_begin_chunk == _end_chunk) {
				free(_begin_chunk);
				break;
			}
			chunk_t *o = _begin_chunk;
			_begin_chunk = _begin_chunk->next;
			free(o);
		}

		chunk_t *sc = _spare_chunk.xchg(nullptr);
		free(sc);
	}

	inline T &front() {
		return _begin_chunk->values[_begin_pos];
	}

	inline T &back() { 
		return _back_chunk->values[_back_pos];
	}

	inline void push(){
		_back_chunk = _end_chunk;
		_back_pos = _end_pos;

		if (++_end_pos != N)
			return;

		chunk_t *sc = _spare_chunk.xchg(nullptr);
		if (sc) {
			_end_chunk->next = sc;
			sc->prev = _end_chunk;
		}
		else {
			_end_chunk->next = allocate_chunk();
			assert(_end_chunk->next);
			_end_chunk->next->prev = _end_chunk;
		}
		_end_chunk = _end_chunk->next;
		_end_pos = 0;
	}

	inline void pop() {
		if (++_begin_pos == N) {
			chunk_t *o = _begin_chunk;
			_begin_chunk = _begin_chunk->next;
			_begin_chunk->prev = nullptr;
			_begin_pos = 0;

			chunk_t *cs = _spare_chunk.xchg(o);
			free(cs);
		}
	}

private:
	struct chunk_t {
		T values[N];
		chunk_t *prev;
		chunk_t *next;
	};

	inline chunk_t *allocate_chunk(){
		return (chunk_t *)malloc(sizeof(chunk_t));
	}

	chunk_t *_begin_chunk;
	int _begin_pos;
	chunk_t *_back_chunk;
	int _back_pos;
	chunk_t *_end_chunk;
	int _end_pos;

	atomic_pointer<chunk_t> _spare_chunk;

	queue(const queue &);
	const queue &operator= (const queue &);
};

////////////////////////////////////////////////////////////////////

template <typename T, int N> class pipeline final {
  public:
    inline pipeline () {
        _queue.push ();
        _r = _w = _f = &_queue.back ();
        _c.set (&_queue.back ());
    }

    inline virtual ~pipeline () {
	}

    inline void write (const T &value_) {
        _queue.back () = value_;
        _queue.push ();
        _f = &_queue.back ();
    }

    inline bool flush () {
        if (_w == _f)
            return true;

        if (_c.cas (_w, _f) != _w) {
            _c.set (_f);
            _w = _f;
            return false;
        }
		
		_w = _f;
        return true;
    }

    inline bool check_read () {
        if (&_queue.front () != _r && _r)
            return true;

        _r = _c.cas (&_queue.front (), nullptr);

        if (&_queue.front () == _r || !_r)
            return false;

		return true;
    }
	
	inline bool read (T *value_)
    {
        if (!check_read ())
            return false;

        *value_ = _queue.front ();
        _queue.pop ();
        return true;
    }

  protected:
    queue<T, N> _queue;

	T *_w;

    T *_r;

	T *_f;

    atomic_pointer<T> _c;

    pipeline (const pipeline &);
    const pipeline &operator= (const pipeline &);
};

//////////////////////////////////////////////////////
const std::chrono::microseconds timeout(int64_t ms) {
	return std::chrono::microseconds(ms);
}

const struct end_t {
}end;


template <typename T, int N> class channel {

private:
	pipeline<T, N> pipe_;
	std::chrono::microseconds timeout_;

public:
	friend channel& operator<<(channel& out, const T& x) {
		out.pipe_.write(x);
		return out;
	}

	friend channel& operator>>(channel& in, T& x) {
		while (!in.pipe_.read(&x));
		return in;
	}

	friend channel& operator<<(channel& out, const std::chrono::microseconds& x) {
		return out;
	}
	
	friend channel& operator<<(channel& out, const end_t& x) {
		out.pipe_.flush();
		return out;
	}
};

}