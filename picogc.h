/* 
 * Copyright 2012 Kazuho Oku
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the author.
 * 
 */
#ifndef picogc_h
#define picogc_h

extern "C" {
#include <stdint.h>
}
#include <cstddef>
#include <cassert>
#include <vector>

namespace picogc {
  
  enum {
    FLAG_MARKED = 1,
    FLAG_HAS_GC_MEMBERS = 2,
    FLAG_MASK = 3
  };
  
  class gc;
  class gc_root;
  class gc_object;
  
  struct config {
    size_t gc_interval_bytes_;
    config(size_t gc_interval_bytes = 8192 * 1024)
      : gc_interval_bytes_(gc_interval_bytes) {}
    static config default_;
  };
  
  struct gc_stats {
    size_t on_stack;
    size_t slowly_marked;
    size_t not_collected;
    size_t collected;
    gc_stats() : on_stack(0), slowly_marked(0), not_collected(0), collected(0)
    {}
  };
  
  struct gc_emitter {
    virtual ~gc_emitter() {}
    virtual void gc_start(gc*) {}
    virtual void gc_end(gc*, const gc_stats&) {}
    virtual void mark_start(gc*) {}
    virtual void mark_end(gc*) {}
    virtual void sweep_start(gc*) {}
    virtual void sweep_end(gc*) {}
    static gc_emitter default_;
  };
  
  template <typename T> class local {
    T* obj_;
  public:
    local() : obj_(NULL) {}
    local(const local<T>& x) : obj_(x.obj_) {}
    local(T* obj);
    local& operator=(const local<T>& x) { obj_ = x.obj_; return *this; }
    local& operator=(T* obj);
    operator T*() const { return obj_; }
    T* operator->() const { return obj_; }
  };
  
  class scope {
    static gc* top_;
    gc* prev_;
    std::vector<gc_object*>::size_type frame_;
  public:
    scope(gc* gc = top_);
    ~scope();
    template <typename T> local<T>& close(local<T>& l);
    static gc* top() {
      assert(top_ != NULL);
      return top_;
    }
  };
  
  class gc {
    friend class scope;
    gc_root* roots_;
    std::vector<gc_object*> stack_;
    gc_object* obj_head_;
    std::vector<gc_object*> pending_;
    size_t bytes_allocated_since_gc_;
    config* config_;
    gc_emitter* emitter_;
  public:
    gc(config* conf = &config::default_);
    ~gc();
    void* allocate(size_t sz);
    void trigger_gc();
    void _register(gc_object* obj, bool has_gc_members);
    void mark(gc_object* obj);
    void _register(gc_root* root);
    void _unregister(gc_root* root);
    void _register_local(gc_object* o) {
      stack_.push_back(o);
    }
    gc_emitter* emitter() { return emitter_; }
    void emitter(gc_emitter* emitter) { emitter_ = emitter; }
  protected:
    void _setup_roots(gc_stats& stats);
    void _mark(gc_stats& stats);
    void _sweep(gc_stats& stats);
  };
  
  class gc_root {
    friend class gc;
    gc_object* obj_;
    gc_root* prev_;
    gc_root* next_;
  public:
    gc_root(gc_object* obj) : obj_(obj) {
      scope::top()->_register(this);
    }
    ~gc_root() {
      scope::top()->_unregister(this);
    }
    gc_object* operator*() { return obj_; }
  };
  
  class gc_object {
    friend class gc;
    intptr_t next_;
    gc_object(const gc_object&); // = delete;
    gc_object& operator=(const gc_object&); // = delete;
  protected:
    gc_object(bool has_gc_members = true);
    virtual ~gc_object() {}
    virtual void gc_mark(picogc::gc* gc) {}
  public:
    static void* operator new(size_t sz);
  };

  template <typename T> local<T>::local(T* obj) : obj_(obj)
  {
    gc* gc = scope::top();
    gc->_register_local(obj);
  }
  
  template <typename T> local<T>& local<T>::operator=(T* obj)
  {
    if (obj_ != obj) {
      gc* gc = scope::top();
      gc->_register_local(obj);
      obj_ = obj;
    }
    return *this;
  }
  
  inline scope::scope(gc* gc) : prev_(top_), frame_(gc->stack_.size())
  {
    top_ = gc;
  }
  
  inline scope::~scope()
  {
    top_->stack_.resize(frame_);
    top_ = prev_;
  }
  
  template <typename T> local<T>& scope::close(local<T>& obj) {
    gc_object* o = obj;
    // destruct the frame, and push the returning value on the prev frame
    top_->stack_[frame_] = o;
    top_->stack_.resize(++frame_);
    return obj;
  }
  
  inline void* gc::allocate(size_t sz)
  {
    bytes_allocated_since_gc_ += sz;
    if (bytes_allocated_since_gc_ >= config_->gc_interval_bytes_) {
      trigger_gc();
      bytes_allocated_since_gc_ = 0;
    }
    void* p = ::operator new(sz);
    memset(p, 0, sz); // GC might walk through the object during construction
    return p;
  }
  
  inline void gc::_register(gc_object* obj, bool has_gc_members)
  {
    obj->next_ = reinterpret_cast<intptr_t>(obj_head_)
      | (has_gc_members ? FLAG_HAS_GC_MEMBERS : 0);
    obj_head_ = obj;
    // NOTE: not marked
  }
  
  inline void gc::mark(gc_object* obj)
  {
    if (obj == NULL)
      return;
    // return if already marked
    if ((obj->next_ & FLAG_MARKED) != 0)
      return;
    // mark
    obj->next_ |= FLAG_MARKED;
    // push to the mark stack
    if ((obj->next_ & FLAG_HAS_GC_MEMBERS) != 0)
      pending_.push_back(obj);
  }

  inline gc_object::gc_object(bool has_gc_members)
  {
    gc* gc = scope::top();
    // protect the object by first registering the object to the local list and then to the GC chain
    gc->_register_local(this);
    gc->_register(this, has_gc_members);
  }
  
  inline void* gc_object::operator new(size_t sz)
  {
    return scope::top()->allocate(sz);
  }
  
}

#endif
