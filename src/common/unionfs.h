#ifndef __UNIONFS_HH__
#define __UNIONFS_HH__

#include <glib-2.0/glib.h>
#include <stdio.h>
#include "defs.h"
#include "../cli_val.h"
#include "../cli_objects.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UNSAVED_FILE ".unsaved"
#define DEF_FILE "def"
#define WHITEOUT_FILE ".wh.__dir_opaque"
#define DELETED_NODE ".wh."

#define MAX_LENGTH_DIR_PATH 4096
#define MAX_LENGTH_HELP_STR 4096

boolean
value_exists(const char *path);

struct PriData {
  unsigned long _pri;
  gchar **_tok_str;
};


struct ValueData
{
  boolean _leaf;
  NODE_OPERATION _state;
};

#ifdef __cplusplus
}
#endif

#endif //__UNIONFS_HH__
