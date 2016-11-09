// Copyright (c) 2014 Cesanta Software
// All rights reserved
//
// This example demostrates how to send arbitrary files to the client.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#include "mongoose.h"
#include <pthread.h>
#include <curl/curl.h>

#define USE_ATOMIC
#include <stdatomic.h>

#if defined( USE_ATOMIC)
static atomic_int               s_received_signal;
#define READ_ATOMIC_D(X)        atomic_load( &X)
#define WRITE_ATOMIC_D(X,Y)     do { atomic_store( &X, Y); } while(0);
#define INCREMENT_ATOMIC_D(X)   atomic_fetch_add( &X, 1)
#define DECREMENT_ATOMIC_D(X)   atomic_fetch_sub( &X, 1)
#define READ_ATOMIC(X)          atomic_load( &thisChannel->X)
#define WRITE_ATOMIC(X,Y)       do { atomic_store( &thisChannel->X, Y); } while(0);
#define DECREMENT_ATOMIC(X)     atomic_fetch_sub( &thisChannel->X, 1)
#define INCREMENT_ATOMIC(X)     atomic_fetch_add( &thisChannel->X, 1)
#endif


#define max(X,Y)      ((X)>(Y) ? (X):(Y))
#define M3U8_EXPIRY   4
#define TS_EXPIRY     60

typedef struct _files {
    void            *next;
    pthread_t       thread;
    time_t          first_usage;
    time_t          start_time;
#if defined( USE_ATOMIC)
    atomic_int      downloaded;
#else
    int             downloaded;
#endif
    int             length;
    char            *name;
    char            *remote_name;
    char            *filename;
    uint8_t         *data;
    int             serving;
    int             max_serving;
} FILES;

#define VERBOSE_CURL    1

static int              VERBOSE             = 1;
static pthread_mutex_t  files_mutex;
static FILES            *receiving_files;
static const char       *server             = "http://62.77.130.133:56789";
static const char       *userAgent          = "Caching HLS server (c) 2015 joolzg@gardnersweden.com";

static void signal_handler(int sig_num) {
  signal(sig_num, signal_handler);
  WRITE_ATOMIC_D( s_received_signal, sig_num);
}

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
size_t realsize = size * nmemb;
FILES *myReply = (FILES *)userp;

    pthread_mutex_lock( &files_mutex);
    if( myReply->data) {
        myReply->data = realloc( myReply->data, realsize+myReply->length+1);
    }
    else {
        myReply->data = malloc( realsize+1);
        myReply->length = 0;
    }
    memcpy( myReply->data+myReply->length, contents, realsize);
    pthread_mutex_unlock( &files_mutex);
    myReply->length += realsize;

    return realsize;
}

static void *startPullingSegment( void *_thisOne)
{
FILES *thisOne = _thisOne;
CURL *curl;
FILE *fp;

    pthread_detach( pthread_self());
    curl = curl_easy_init();
    if( curl) {
    CURLcode res;

        if( VERBOSE>1) {
            printf( "Pulling Seg %s\r\n", thisOne->remote_name); fflush( stdout);
        }
        curl_easy_setopt(curl, CURLOPT_URL, thisOne->remote_name);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)thisOne);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, VERBOSE_CURL);
        res = curl_easy_perform(curl);
        pthread_mutex_lock( &files_mutex);
        fp = fopen( thisOne->filename, "wb+");
        if( fp) {
            fwrite( thisOne->data, 1, thisOne->length, fp);
            fclose( fp);
        }
        WRITE_ATOMIC_D( thisOne->downloaded, 1);
        pthread_mutex_unlock( &files_mutex);
        if( VERBOSE) {
            printf( "Pulled  Seg %s\t%d\t%d\r\n", thisOne->remote_name, thisOne->length, res); fflush( stdout);
        }
        curl_easy_cleanup(curl);
    }

    return NULL;
}

static FILES *findFileInMemory( const char *filename)
{
FILES * first;

    pthread_mutex_lock( &files_mutex);
    first = receiving_files;
    while( first) {
        if( first->data && !strcmp( filename, first->name)) {
            first->serving++;
            first->max_serving = max( first->max_serving, first->serving);
            pthread_mutex_unlock( &files_mutex);
            return first;
        }
        first = first->next;
    }
    pthread_mutex_unlock( &files_mutex);

    return NULL;
}

static void *localFree( FILES *whichOne, int locked)
{
time_t now;
void *next;

    time( &now);
    printf( "File %s being cleared, %ld seconds old, number of sessions %d\r\n", whichOne->name, now-whichOne->start_time, whichOne->max_serving);
    if( !locked) {
        pthread_mutex_lock( &files_mutex);
    }
    free( whichOne->data);
    free( whichOne->name);
    free( whichOne->remote_name);
    unlink( whichOne->filename);
    free( whichOne->filename);
    if( whichOne==receiving_files) {
        receiving_files = whichOne->next;
        free( whichOne);
    }
    else {
    FILES *ptr = receiving_files;

        while( ptr) {
            if( ptr->next==whichOne) {
                ptr->next = whichOne->next;
                free( whichOne);
                break;
            }
            ptr = ptr->next;
        }
    }
    next = receiving_files;
    if( !locked) {
        pthread_mutex_unlock( &files_mutex);
    }

    return next;
}

static void *clearThread( void *nul)
{
    while( !nul && !READ_ATOMIC_D( s_received_signal)) {
    FILES * first;
    time_t now;
    int cnt = 0;

        pthread_mutex_lock( &files_mutex);
        first = receiving_files;
        while( first) {
            first = first->next;
            cnt++;
        }
        pthread_mutex_unlock( &files_mutex);
        if( cnt) {
            printf( "Cache Clearance %d\r\n", cnt);
            time( &now);
            pthread_mutex_lock( &files_mutex);
            first = receiving_files;
            while( first) {
    //          printf( "File %s serving %d, %d seconds old\r\n", first->name, first->serving, now-first->first_usage);
                if( !first->serving && now>=first->first_usage) {
                FILES *ptr = first->next;

                    first = localFree( first, 1);
                }
                else {
                    first = first->next;
                }
            }
            pthread_mutex_unlock( &files_mutex);
        }
        sleep( M3U8_EXPIRY-1);
  }

  return NULL;
}

static FILES *localNewFile( const char *filename, const char *directory, char *isM3U8)
{
FILES * first;
char remote[256];
int error;

    strcpy( remote, server);
    strcat( remote, "/");
    strcat( remote, directory);

    pthread_mutex_lock( &files_mutex);
    first = calloc( 1, sizeof( FILES));
    first->next = receiving_files;
    receiving_files = first;

    first->serving++;
    first->max_serving = 1;
    time( &first->first_usage);
    time( &first->start_time);
    first->first_usage += isM3U8 ? M3U8_EXPIRY:TS_EXPIRY;
    first->name = strdup( filename);
    first->remote_name = strdup( remote);
    {
    char *template = strdup( isM3U8 ? "/tmp/m3u8.XXXXXX" : "/tmp/ts.XXXXXX");

        first->filename = mktemp( template);
        printf( "filename is %s\r\n", first->filename);
    }
    error = pthread_create( &first->thread, NULL, startPullingSegment, (void *)first);
    pthread_mutex_unlock( &files_mutex);
    if( error)
        printf( "Pthread create error %d\r\n", error);

    return first;
}

static int ev_handler(struct mg_connection *conn, enum mg_event ev) {
  const char *host;
  char *isM3U8;
  int l;
  int filename = 0;
  int directory = -1;

    switch (ev) {
        case MG_REQUEST:
            host = mg_get_header(conn, "Host");
            if( VERBOSE>1) {
                printf("[%s] [%s] [%s]\n", conn->request_method, conn->uri, host == NULL ? "" : host);
            }

            l = 0;
            while( conn->uri[l]) {
                if( conn->uri[l]=='/') {
                    directory = filename;
                    filename = l+1;
                }
                l++;
            }

            printf( "directory = '%s'\r\n", conn->uri + directory);
            printf( "filename  = '%s'\r\n", conn->uri + filename);

            if( (isM3U8 = strstr( conn->uri, ".m3u8")) || strstr( conn->uri, ".ts")) {
            FILES *whichOne = findFileInMemory( conn->uri + filename);

                if( !whichOne) {
                    whichOne = localNewFile( conn->uri + filename, conn->uri + directory, isM3U8);
                }
                if( whichOne) {
                    while( !READ_ATOMIC_D( whichOne->downloaded)) {
                        usleep(10000);
                    };
                    mg_send_file(conn, whichOne->filename, isM3U8 ? "Content-Type: application/x-mpegURL\n" : "Content-Type: video/MP2T\n");  // Also could be a dir, or CGI
                    pthread_mutex_lock( &files_mutex);
                    --whichOne->serving;
                    pthread_mutex_unlock( &files_mutex);
                }
                else if( 1) {

                }
                else if( whichOne) {
                int pos = 0;

                    while( 1) {
                    int left = whichOne->length-pos;

                        pthread_mutex_lock( &files_mutex);
                        if( whichOne->downloaded && !left) {
                            --whichOne->serving;
                            pthread_mutex_unlock( &files_mutex);
                            return MG_MORE; // It is important to return MG_MORE after mg_send_file!
                        }
                        else if( pos<whichOne->length) {
                            mg_send_data( conn, whichOne->data + pos, left);
                            pos += left;
                        }
                        else if( !whichOne->downloaded) {
                            pthread_mutex_unlock( &files_mutex);
                            usleep( 1000);
                            pthread_mutex_lock( &files_mutex);
                        }
                        pthread_mutex_unlock( &files_mutex);
                    }
                }
            }
            else {
                mg_send_file(conn, conn->uri + filename, NULL);  // Also could be a dir, or CGI
            }

            return MG_MORE; // It is important to return MG_MORE after mg_send_file!

        case MG_AUTH:
            return MG_TRUE;

        default:
            return MG_FALSE;
    }
}

int main(void) {
  struct mg_server *server;
  int error;
  pthread_t thread;

  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);

  pthread_mutex_init( &files_mutex, NULL);

  /* Must initialize libcurl before any threads are started */
  curl_global_init(CURL_GLOBAL_ALL);

  error = pthread_create( &thread, NULL, clearThread, (void *)NULL);
  if( error)
    printf( "Pthread create error %d\r\n", error);
  else
    pthread_detach( thread);

  server = mg_create_server(NULL, ev_handler);
  mg_set_option(server, "listening_port", "2014");

  printf("Starting on port %s\n", mg_get_option(server, "listening_port"));
  while( !READ_ATOMIC_D( s_received_signal))
    mg_poll_server(server, 1000);
  printf("Closing down     %s\n", mg_get_option(server, "listening_port"));
  mg_destroy_server(&server);

  pthread_mutex_lock( &files_mutex);
  while( localFree( receiving_files, 1)) {
  }
  pthread_mutex_unlock( &files_mutex);

  curl_global_cleanup();

  return 0;
}
