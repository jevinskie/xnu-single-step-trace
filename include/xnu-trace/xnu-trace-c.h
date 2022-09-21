#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <frida-gum.h>

typedef void *stalker_t;

__attribute__((visibility("default"))) stalker_t
create_stalker(const char *log_dir_path, int symbolicate, int compression_level, int stream);

__attribute__((visibility("default"))) void destroy_stalker(stalker_t stalker);

__attribute__((visibility("default"))) void stalker_follow_me(stalker_t stalker);

__attribute__((visibility("default"))) void stalker_follow_thread(stalker_t stalker,
                                                                  GumThreadId thread_id);

__attribute__((visibility("default"))) void stalker_unfollow_me(stalker_t stalker);

__attribute__((visibility("default"))) void stalker_unfollow_thread(stalker_t stalker,
                                                                    GumThreadId thread_id);

#ifdef __cplusplus
}
#endif