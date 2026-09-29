#ifndef SAMPLE_DEMO_APP_CONTEXT_H
#define SAMPLE_DEMO_APP_CONTEXT_H

#include <signal.h>

typedef struct AppContext {
    volatile sig_atomic_t exit_requested;
    int mpp_initialized;
} AppContext;

#endif

