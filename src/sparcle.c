// \file sparcle.c
// \brief Main source module of sparcle.
//
// Copyright (C) 2016, Hattori, Hiroki all rights reserved.
// This program released under BSD3 license.
//
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <ucontext.h>
#include <unistd.h>
#include "sparcle.h"

/** \addtogroup Scheduler
 * @{ */
_Thread_local static ucontext_t native_context_;
_Thread_local sparcle_scheduler_t sparcle_current_scheduler = NULL;
_Thread_local sparcle_thread_t sparcle_current_thread = NULL;

struct tag_sparcle_scheduler {
  atomic_uintptr_t idle_threads_;
  atomic_uintptr_t exhausted_threads_;

  size_t max_workers_;
  atomic_size_t num_workers_;
  atomic_size_t num_threads_;

  size_t default_thread_stack_size_;
};
/** @} */

/** \ingroup Thread */
struct tag_sparcle_thread {
  sparcle_thread_t next_thread_;
  sparcle_thread_state_t state_;
  ucontext_t context_;
  sparcle_scheduler_t scheduler_;
  sparcle_wait_queue_t* wait_queue_;
  sparcle_mutex_t* mutex_;
  void* retval_;
};



/* **************************************************************** */
/** \addtogroup Scheduler
 * @{ */

/* スケジューラを作成する
 * @param num_workers ワーカの最大数
 * @return 新しいスケジューラ
 */
sparcle_scheduler_t sparcle_scheduler_new(size_t num_workers) {
  sparcle_scheduler_t p = malloc(sizeof(struct tag_sparcle_scheduler));
  atomic_init(&p->idle_threads_, 0);
  atomic_init(&p->exhausted_threads_, 0);
  p->max_workers_ = num_workers;
  atomic_init(&p->num_workers_, 0);
  atomic_init(&p->num_threads_, 0);
  p->default_thread_stack_size_ = 4000;
  return p;
}


void sparcle_scheduler_delete(sparcle_scheduler_t sched) {
  assert(sparcle_current_scheduler != sched);

  while (atomic_load(&(sched->num_workers_)) > 0)
    usleep(100000);

  free(sched);
}





/** コンテキストスイッチを行う
 * @param x 切り替え先のスレッド。
 *          NULLならワーカーのネイティブのコンテキストへ戻る。
 *
 *  1. 現在のコンテキストを保存する
 *  2. 新しいスレッド x のコンテキストにスイッチする。
 *     xがNULLなら、ネイティブスレッドの本来のコンテキストへスイッチする。
 *  3. sparcle_current_threadを更新する
 *  4. 新しいスレッドのwait_queue_, next_thread_をNULLにし、ステートをRUNにする
 *  5. 元のスレッドが
 *     a.待ちに入るならwait_queueへ繋ぐ。
 *       この時、wait_queue_lockが指定されていればその書き込みロックを解除する
 *     b.suspend又はexitするなら放置
 *     c.いずれでもなければexchaustedへ繋ぐ
 *  事前条件
 *    A. 切り替え先スレッドxはNULLか、または現在のスレッドと同じスケジューラに属している必要がある
 *    B. 待ちに入るスレッドは予めthread構造体のwait_queue_を設定して置く
 *    C. suspend/exitするスレッドは予めステートを設定しておく
 *    D. 自分自身へはスイッチできない
 */
static void sparcle_switch_context(sparcle_thread_t x) {
  assert(x == NULL || x->scheduler_ == sparcle_current_scheduler);
  assert(sparcle_current_thread != x);

  _Thread_local static sparcle_thread_t prev_thread;
  prev_thread = sparcle_current_thread;
  sparcle_current_thread = x; 
  swapcontext((sparcle_current_thread == NULL? &native_context_ : &prev_thread->context_),
              (x == NULL? &native_context_ : &x->context_) );

  // 他のスレッドから戻って来た
  sparcle_current_thread->state_ = SPARCLE_THREAD_STATE_RUN;
  sparcle_current_thread->next_thread_ = NULL;
  sparcle_current_thread->wait_queue_ = NULL;
  sparcle_current_thread->mutex_ = NULL;

  if (prev_thread != NULL) {
    if (prev_thread->state_ == SPARCLE_THREAD_STATE_EXIT) {
      free(prev_thread->context_.uc_stack.ss_sp);
      prev_thread->context_.uc_stack.ss_sp = NULL;
    } else if (prev_thread->wait_queue_ != NULL) {
      prev_thread->state_ = SPARCLE_THREAD_STATE_WAIT;
      while (! atomic_compare_exchange_weak(&prev_thread->wait_queue_->q_,
                                            (uintptr_t*)&prev_thread->next_thread_,
                                            (uintptr_t)prev_thread) ) ; // spin
    } else if (prev_thread->mutex_ != NULL) {
      if (atomic_flag_test_and_set(&prev_thread->mutex_->flag_)) {
        while (! atomic_compare_exchange_weak(&sparcle_current_scheduler->exhausted_threads_,
                                              (uintptr_t*)&prev_thread->next_thread_,
                                              (uintptr_t)prev_thread) ) ; // spin
      } else {
        prev_thread->state_ = SPARCLE_THREAD_STATE_WAIT;
        while (! atomic_compare_exchange_weak(&prev_thread->mutex_->wait_queue_.q_,
                                              (uintptr_t*)&prev_thread->next_thread_,
                                              (uintptr_t)prev_thread) ) ; // spin
      }
    } else if (prev_thread->state_ != SPARCLE_THREAD_STATE_SUSPEND &&
               prev_thread->state_ != SPARCLE_THREAD_STATE_EXIT) {
      while (! atomic_compare_exchange_weak(&sparcle_current_scheduler->exhausted_threads_,
                                            (uintptr_t*)&prev_thread->next_thread_,
                                            (uintptr_t)prev_thread) ) ; // spin
    }
  }
}




void sparcle_yield() {
  assert(sparcle_is_worker_thread());
  assert(sparcle_current_thread == NULL ||
         sparcle_current_thread->scheduler_ == sparcle_current_scheduler);

  sparcle_thread_t x = (sparcle_thread_t)atomic_load(&sparcle_current_scheduler->idle_threads_);
retry:;
  if (x == NULL) {
    x = (sparcle_thread_t)atomic_exchange(
            &sparcle_current_scheduler->exhausted_threads_,
            (uintptr_t)NULL);
    if (x == NULL) {
      // 切り替えるべきスレッドが無くて、かつ今のスレッドも待つ訳では無いのなら
      // 何もせずに呼び出し元へ戻る
      if (sparcle_current_thread->state_ != SPARCLE_THREAD_STATE_SUSPEND &&
          sparcle_current_thread->wait_queue_ == NULL &&
          sparcle_current_thread->mutex_ == NULL)
        return;
    }
    atomic_store(&sparcle_current_scheduler->idle_threads_, (uintptr_t)x->next_thread_);
  } else if (! atomic_compare_exchange_weak(&sparcle_current_scheduler->idle_threads_,
                                            (uintptr_t*)&x, (uintptr_t)x->next_thread_))
    goto retry;

  sparcle_switch_context(x);
}




static void* sparcle_worker_main(void* sched) {
  assert(sparcle_current_scheduler == NULL);
  assert(sparcle_current_thread == NULL);

  sparcle_current_scheduler = sched;
  for (;;) {
    sparcle_yield();
    usleep(100000);
  }

  return NULL;
}



void sparcle_scheduler_add_thread(sparcle_thread_t x) {
  assert(x != NULL);
  assert(x->next_thread_ == NULL);

  sparcle_scheduler_t sched = x->scheduler_;
  assert(sched != NULL);

  x->next_thread_= (sparcle_thread_t)atomic_load(&sched->idle_threads_);
  while (! atomic_compare_exchange_weak(&sched->idle_threads_,
                                        (uintptr_t*)&x->next_thread_, (uintptr_t)x))
    ; // spin

  atomic_fetch_add(&sched->num_threads_, 1);
  {
    size_t n = atomic_load(&sched->num_workers_);
    while (n < sched->max_workers_) {
      if (atomic_compare_exchange_strong(&sched->num_workers_, &n, n + 1)) {
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 16384);
        pthread_create(&th, NULL, sparcle_worker_main, sched);
        pthread_attr_destroy(&attr);
      }
    }
  }
}





/** @} */
/* **************************************************************** */
/** \addtogroup Thread
 * @{ */

static void sparcle_thread_entry(void* (*f)(void*), void* arg) {
  assert(sparcle_is_worker_thread());
  assert(sparcle_current_thread != NULL);
  sparcle_thread_exit(f(arg));
}


/*
 *
 */
int sparcle_thread_new(
        sparcle_scheduler_t sched,
        sparcle_thread_t* thread, sparcle_attr_t const* attr,
        void* (*startup_routine)(void*), void* arg) {
  sparcle_thread_t x = malloc(sizeof(struct tag_sparcle_thread));
  x->next_thread_ = NULL;
  x->scheduler_ = sched;
  x->state_ = SPARCLE_THREAD_STATE_IDLE;
  x->wait_queue_ = NULL;
  x->mutex_ = NULL;

  x->context_.uc_stack.ss_sp = malloc(512);
  x->context_.uc_stack.ss_size = 512;
  x->context_.uc_link = NULL;
  makecontext(&x->context_, (void (*)())sparcle_thread_entry, 2, startup_routine, arg);

  sparcle_scheduler_add_thread(x);
  *thread = x;
  return 0;
}


void sparcle_thread_exit(void* retval) {
  assert(sparcle_is_worker_thread());
  assert(sparcle_current_thread != NULL);

  sparcle_current_thread->retval_ = retval;
  sparcle_current_thread->state_ = SPARCLE_THREAD_STATE_EXIT;

  sparcle_yield();
}


/** 現在のスレッドをサスペンドさせる
 * サスペンド状態とは、いかなる待機キューに繋がらない状態での休眠を表す。
 * 待機キューに繋がっていないので、明示的に起床させない限り永遠に休眠する。
 */
void sparcle_thread_suspend() {
  assert(sparcle_current_thread != NULL);
  sparcle_current_thread->state_ = SPARCLE_THREAD_STATE_SUSPEND;
  sparcle_yield();
}


/** サスペンド状態のスレッドを起床させる
 * @param x 起床させるスレッド
 */
void sparcle_thread_wakeup(sparcle_thread_t x) {
  assert(x->next_thread_ = NULL);
  x->state_ = SPARCLE_THREAD_STATE_IDLE;
  while (! atomic_compare_exchange_weak(&sparcle_current_scheduler->exhausted_threads_,
                                        (uintptr_t*)&x->next_thread_,
                                        (uintptr_t)x) ) ; // spin
}


/** @} */
/* **************************************************************** */
/** \addtogroup wait_queue Wait queue
 * @{ */


void sparcle_wait_queue_wait(sparcle_wait_queue_t* q) {
  assert(sparcle_is_worker_thread());
  assert(sparcle_current_thread != NULL);
  // まだcurrent_threadが走行中なのでキューに入れるわけにはいかない。
  // でないと別のワーカーがまだ走行中のスレッドを起床させてしまう。
  // 対象のキューをwait_queue_に指定しておくと、yieldが繋いでくれる。
  sparcle_current_thread->wait_queue_ = q;
  sparcle_yield();
}



sparcle_thread_t sparcle_wait_queue_signal(sparcle_wait_queue_t* q) {
  sparcle_thread_t x = (sparcle_thread_t)atomic_load(&q->q_);
  while (x != NULL) {
    if (atomic_compare_exchange_weak(&q->q_, (uintptr_t*)&x, (uintptr_t)x->next_thread_) ) {
      x->next_thread_ = NULL;
      x->wait_queue_ = NULL;
      sparcle_thread_wakeup(x);
      return x;
    }
  }
  return NULL; 
}



void sparcle_wait_queue_broadcast(sparcle_wait_queue_t* q) {
  sparcle_thread_t x = (sparcle_thread_t)atomic_exchange(&q->q_, (uintptr_t)NULL);
  while (x != NULL) {
    sparcle_thread_t y = x->next_thread_;
    x->next_thread_ = NULL;
    x->wait_queue_ = NULL;
    sparcle_thread_wakeup(x);
    x = y;
  }
}

/** @} */
/* **************************************************************** */
/** \addtogroup Mutex
 * @{ */

int sparcle_mutex_lock(sparcle_mutex_t *mutex) {
  assert(sparcle_is_worker_thread());
  assert(sparcle_current_thread != NULL);

  while (atomic_flag_test_and_set(&mutex->flag_)) {
    sparcle_current_thread->mutex_ = mutex;
    sparcle_yield();
  }
  return 0;
}


int sparcle_mutex_unlock(sparcle_mutex_t *mutex) {
  assert(sparcle_is_worker_thread());
  if (sparcle_wait_queue_signal(&mutex->wait_queue_) == 0) {
    atomic_flag_clear(&mutex->flag_);
    sparcle_wait_queue_signal(&mutex->wait_queue_);
  }
  return 0;
}


/** @} */
/* **************************************************************** */
/** \addtogroup condition_variable Condition variable
 * @{ */

int sparcle_cond_wait(sparcle_cond_t* cond, sparcle_mutex_t* mutex) {
  sparcle_mutex_unlock(mutex);
  do {
    sparcle_wait_queue_wait(&cond->wait_queue_);
  } while (sparcle_mutex_trylock(mutex));
  return 0;
}


/** @} */
/* **************************************************************** */
/** \addtogroup Semaphore
 * @{ */

int sparcle_semaphore_get(sparcle_semaphore_t* semaphore, size_t n) {
  unsigned int x = atomic_load(&semaphore->current_);
  while (x >= n) {
    if (atomic_compare_exchange_strong(&semaphore->current_, &x, x - n))
      return 0;
  }
  sparcle_mutex_lock(&semaphore->mutex_);
  for (;;) {
    x = atomic_load(&semaphore->current_);
    while (x >= n) {
      if (atomic_compare_exchange_weak(&semaphore->current_, &x, x - n)) {
        sparcle_mutex_unlock(&semaphore->mutex_);
        return 0;
      }
    }
    sparcle_cond_wait(&semaphore->cond_, &semaphore->mutex_);
  }
}



/** @} */
/* vim: ts=8 sw=2 tw=0 cindent
 */

