#ifndef __COMMON_H__
#define __COMMON_H__

#include "defs.h"
#include "unionfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 *
 **/
GNode*
common_get_local_session_data();

/**
 * flushes local session
 **/
void
common_clear_local_session();

/**
 * brings over local session conf to main config
 **/
void
commmon_copy_local_to_main();

/**
 * sets system context for operation (i.e. hack for unionfs implementation)
 **/
void
common_set_context(char *cpath, char *dpath);

/**
 * sets system parent context for operation (i.e. hack for unionfs
 * implementation)
 **/
void
common_set_parent_context(char *cpath, char *dpath);

/**
 *
 **/
void
common_commit_copy_to_live_config(GNode *root_node, boolean suppress_piecewise_copy, boolean test_mode);

/**
 *
 **/
void
common_commit_clean_temp_config(GNode *root_node, boolean test_mode);

#ifdef __cplusplus
}
#endif

#endif //__COMMON_H__
