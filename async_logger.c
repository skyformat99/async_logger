//include(system)
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
//include(custom)
#include "async_logger.h"

//config
#define SYSLOG_WI_POOL_SIZE     64
#define SYSLOG_WI_DATA_LEN      2048

//type
typedef enum {
    SYSLOG_ITEM_ASYNC_WRITE,
    SYSLOG_ITEM_ASYNC_EXIT
} item_types;

struct syslog_wi
{
    item_types item_type;
    char data[SYSLOG_WI_DATA_LEN];
    int len;
    int priority;
    struct syslog_wi *next;
};

#define SYSLOG_WI_NOWAIT        0
#define SYSLOG_WI_WAIT          1

//data
static pthread_mutex_t syslog_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int syslog_queue_inited = 0;
static pthread_t syslog_queue;
static pthread_cond_t syslog_queue_cond;
static pthread_mutex_t syslog_queue_mutex;
static pthread_cond_t syslog_wi_free_cond;
static pthread_mutex_t syslog_wi_free_mutex;

static int syslog_dropped_items;

static struct syslog_wi syslog_wi_pool[SYSLOG_WI_POOL_SIZE];
static struct syslog_wi *syslog_wi_free;
static struct syslog_wi *syslog_wi_queue, *syslog_wi_queue_tail;

static int current_log_level = LOG_LVL_INFO;

//method
static char * level_to_str(int lvl){
  switch(lvl){
	case LOG_LVL_ERR: return "ERR";
	case LOG_LVL_WARN: return "WARN";
	case LOG_LVL_INFO: return "INFO";
	case LOG_LVL_DEBUG: return "DEBUG";
	default: return "UNKOWN";
  }
}

static int append_time(char * buf)
{
    time_t t;
    struct tm * lt;
    time (&t);
    lt = localtime (&t);
    return sprintf (buf, "%d/%d/%d %d:%d:%d ", 
        lt->tm_year+1900, lt->tm_mon, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
}

static void
syslog_queue_run(void)
{
    struct syslog_wi *wi;

    for (;;) {
        pthread_mutex_lock(&syslog_queue_mutex);
        while (syslog_wi_queue == NULL) {
            pthread_cond_wait(&syslog_queue_cond, &syslog_queue_mutex);
        }
        wi = syslog_wi_queue;
        syslog_wi_queue = wi->next;
        pthread_mutex_unlock(&syslog_queue_mutex);

        /* main work here */
        switch (wi->item_type) {
            case SYSLOG_ITEM_ASYNC_WRITE:
                //syslog(wi->priority, "%s", wi->data);
                //printf("[%s] %s \n",level_to_str(wi->priority), wi->data);
                printf("%s", wi->data);
                break;

            case SYSLOG_ITEM_ASYNC_EXIT:
                return;

            default:
                break;
        }

        /* put wi into syslog_wi_free' tail */
        pthread_mutex_lock(&syslog_wi_free_mutex);

        wi->next = syslog_wi_free;
        syslog_wi_free = wi;

        pthread_cond_signal(&syslog_wi_free_cond);
        pthread_mutex_unlock(&syslog_wi_free_mutex);
    }
}

static int
syslog_queue_init(void)
{
    int i;

    memset(syslog_wi_pool, 0, sizeof(syslog_wi_pool));
    for (i = 0; i < SYSLOG_WI_POOL_SIZE - 1; i++) {
        syslog_wi_pool[i].next = &syslog_wi_pool[i + 1];
    }
    syslog_wi_pool[SYSLOG_WI_POOL_SIZE - 1].next = NULL;

    syslog_wi_free = syslog_wi_pool;
    syslog_wi_queue = NULL;
    syslog_wi_queue_tail = NULL;

    syslog_dropped_items = 0;

    pthread_cond_init(&syslog_queue_cond, NULL);
    pthread_mutex_init(&syslog_queue_mutex, NULL);
    pthread_cond_init(&syslog_wi_free_cond, NULL);
    pthread_mutex_init(&syslog_wi_free_mutex, NULL);

    if (pthread_create(&syslog_queue, NULL, (void *(*)(void *))&syslog_queue_run, NULL) != 0)
        return -1;

    return 0;
}

static struct syslog_wi *
syslog_queue_get_free_item(int wait)
{
    struct syslog_wi *wi;

    pthread_mutex_lock(&syslog_wi_free_mutex);
    while (syslog_wi_free == NULL) {
        /* no free work items, return if no wait is requested */
        if (wait == 0) {
            syslog_dropped_items++;
            pthread_mutex_unlock(&syslog_wi_free_mutex);
            return NULL;
        }
        pthread_cond_wait(&syslog_wi_free_cond, &syslog_wi_free_mutex);
    }

    wi = syslog_wi_free;

    /* move up syslog_wi_free */
    syslog_wi_free = syslog_wi_free->next;
    pthread_mutex_unlock(&syslog_wi_free_mutex);

    return wi;
}

static void
syslog_queue_put_item(struct syslog_wi *wi)
{

    pthread_mutex_lock(&syslog_queue_mutex);

    wi->next = NULL;
    if (syslog_wi_queue == NULL) {
        syslog_wi_queue = wi;
        syslog_wi_queue_tail = wi;
    } else {
        syslog_wi_queue_tail->next = wi;
        syslog_wi_queue_tail = wi;
    }

    /* notify worker thread */
    pthread_cond_signal(&syslog_queue_cond);

    pthread_mutex_unlock(&syslog_queue_mutex);
}

static void
async_logger_atexit(void)
{
    struct syslog_wi *wi;

    if (syslog_queue_inited == 0)
        return;

    /* Wait for the worker thread to exit */
    LOG_INFO("destroy async logger ...");
    wi = syslog_queue_get_free_item(SYSLOG_WI_WAIT);
    wi->item_type = SYSLOG_ITEM_ASYNC_EXIT;
    syslog_queue_put_item(wi);
    pthread_join(syslog_queue, NULL);
}

int
async_logger_init(const char *app, int level /*, int facility*/)
{

    if(syslog_queue_inited) return 0;

    pthread_mutex_lock(&syslog_init_mutex);

    if (syslog_queue_inited == 0) {
        if (syslog_queue_init() != 0) {
            pthread_mutex_unlock(&syslog_init_mutex);
            return -1;
        }
    }
    syslog_queue_inited = 1;

    pthread_mutex_unlock(&syslog_init_mutex);

    current_log_level = level;
    atexit(async_logger_atexit);
    
    LOG_INFO("initialize async logger ...");
    return 0;
}

void
async_log_queue(int priority, const char *format, va_list ap)
{
    struct syslog_wi *wi;
    char *p;
    int s1, s2;

    wi = syslog_queue_get_free_item(SYSLOG_WI_NOWAIT);
    if (wi == NULL)
        return;

    p = wi->data;
    s1 = sizeof(wi->data);
    s2 = vsnprintf(p, s1, format, ap);
    if (s2 >= s1) {
        /* message was truncated */
        s2 = s1 - 1;
        p[s2] = '\0';
    }
    wi->len = s2;
    wi->priority = priority;
    wi->item_type = SYSLOG_ITEM_ASYNC_WRITE;
    syslog_queue_put_item(wi);
}

void async_log(int lvl, const char * func, const char * file, int line, const char *format, ...)
{
       va_list ap ;
       char buf[520];
       int offset = 0;
       if(lvl > current_log_level) return;

       va_start(ap, format);
//%[flags] [width] [.precision] [{h | l | I64 | L}]type
       offset += append_time(buf+offset);
       offset += sprintf(buf+offset, "[%s]", level_to_str(lvl));
       offset += sprintf(buf+offset, "[%d][%s:%d]%s(): %s\n", getpid(), file, line, func, format);

       async_log_queue(lvl, buf, ap);

       va_end(ap); 

       return ;
}

