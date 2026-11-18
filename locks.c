#include <pthread.h>
#include "locks.h"

// Simple lock interface.
void mutex_init(struct mutex *lock)
{
    pthread_mutex_init(&lock->lock,NULL);
}

void mutex_lock(struct mutex *lock)
{
    pthread_mutex_lock(&lock->lock);
}

void mutex_unlock(struct mutex *lock)
{
    pthread_mutex_unlock(&lock->lock);
}

void mutex_destroy(struct mutex *lock)
{
    pthread_mutex_destroy(&lock->lock);
}

// Read-write lock interface.
void rwmutex_init(struct rwmutex *rwlock)
{
    pthread_mutex_init(&rwlock->mutex,NULL);
    
    pthread_cond_init(&rwlock->readers_cv,NULL);
    pthread_cond_init(&rwlock->writers_cv,NULL);

    rwlock->active_readers=0;
    rwlock->active_writers=0;
    rwlock->waiting_writers=0;
}

void rwmutex_read_lock(struct rwmutex *rwlock)
{
    pthread_mutex_lock(&rwlock->mutex);

    while(rwlock->active_writers>0 || rwlock->waiting_writers>0){
        pthread_cond_wait(&rwlock->readers_cv,&rwlock->mutex);
    }
    rwlock->active_readers++;

    pthread_mutex_unlock(&rwlock->mutex);
}

void rwmutex_write_lock(struct rwmutex *rwlock)
{
    pthread_mutex_lock(&rwlock->mutex);

    rwlock->waiting_writers++;

    while(rwlock->active_readers>0 || rwlock->active_writers>0){
        pthread_cond_wait(&rwlock->writers_cv,&rwlock->mutex);
    }
    
    rwlock->waiting_writers--;
    rwlock->active_writers=1;

    pthread_mutex_unlock(&rwlock->mutex);
}

void rwmutex_read_unlock(struct rwmutex *rwlock)
{
    pthread_mutex_lock(&rwlock->mutex);

    rwlock->active_readers--;

    if(rwlock->active_readers == 0 && rwlock->waiting_writers>0){
        pthread_cond_signal(&rwlock->writers_cv);
    }
    pthread_mutex_unlock(&rwlock->mutex);
}

void rwmutex_write_unlock(struct rwmutex *rwlock)
{
    pthread_mutex_lock(&rwlock->mutex);
    
    rwlock->active_writers=0;

    if(rwlock->waiting_writers>0){
        pthread_cond_signal(&rwlock->writers_cv);
    }else{
        pthread_cond_broadcast(&rwlock->readers_cv);
    }
    pthread_mutex_unlock(&rwlock->mutex);
}

void rwmutex_destroy(struct rwmutex *rwlock)
{
    pthread_cond_destroy(&rwlock->readers_cv);
    pthread_cond_destroy(&rwlock->writers_cv);
    pthread_mutex_destroy(&rwlock->mutex);
}

