#include "runtime.h"
#include <curl/curl.h>

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_status = CURLE_FAILED_INIT;

static void initialize_curl(void) {
    curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
}

int curl_runtime_init(void) {
    if (pthread_once(&curl_once, initialize_curl) != 0) return -1;
    return curl_status == CURLE_OK ? 0 : -1;
}
