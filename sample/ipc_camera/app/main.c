#include "../include/app_context.h"
#include "../include/platform.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static AppContext *g_context;

static void handle_exit_signal(int signal_number)
{
    (void)signal_number;

    if (g_context != NULL) {
        g_context->exit_requested = 1;
    }
}

static int install_signal_handlers(void)
{
    if (signal(SIGINT, handle_exit_signal) == SIG_ERR) {
        perror("signal(SIGINT)");
        return -1;
    }

    if (signal(SIGTERM, handle_exit_signal) == SIG_ERR) {
        perror("signal(SIGTERM)");
        return -1;
    }

    return 0;
}

int main(void)
{
    int exit_code = EXIT_FAILURE;
    AppContext context = {0};

    g_context = &context;

    if (install_signal_handlers() != 0) {
        goto cleanup;
    }

    if (platform_init(&context) != 0) {
        goto cleanup;
    }

    puts("sample_demo is running; press Ctrl+C to exit");
    while (!context.exit_requested) {
        sleep(1);
    }

    exit_code = EXIT_SUCCESS;

cleanup:
    if (platform_deinit(&context) != 0) {
        exit_code = EXIT_FAILURE;
    }
    g_context = NULL;
    return exit_code;
}

