/*
 * Copyright 2022 Yonggang Luo
 * SPDX-License-Identifier: MIT
 */

#include "u_call_once.h"

struct util_call_once_context_t
{
   const void *data;
   util_call_once_data_func func;
};

/* thread_local removed for 10.6 cross-build: unsupported on macOS < 10.7.
 * Plain static is a theoretical race if two different once_flags initialize
 * concurrently. In practice Mesa's call_once paths are not concurrent.
 * For a proper fix: pthread_key_create/pthread_getspecific. */
static struct util_call_once_context_t call_once_context;

static void
util_call_once_data_slow_once(void)
{
   struct util_call_once_context_t *once_context = &call_once_context;
   once_context->func(once_context->data);
}

void
util_call_once_data_slow(once_flag *once, util_call_once_data_func func, const void *data)
{
   struct util_call_once_context_t *once_context = &call_once_context;
   once_context->data = data;
   once_context->func = func;
   call_once(once, util_call_once_data_slow_once);
}
